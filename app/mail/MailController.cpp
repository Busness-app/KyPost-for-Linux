#include "mail/MailController.h"

#include "domain/DevicePairing.h"
#include "domain/KeywordRepository.h"
#include "domain/FolderRepository.h"
#include "domain/MailRepository.h"
#include "stores/SettingsStore.h"
#include "domain/PairingStore.h"
#include "models/KeywordSettings.h"
#include "models/StandardFolder.h"
#include "net/RelayAuth.h"
#include "models/MailFolder.h"
#include "net/FolderClient.h"
#include "net/PgpBootstrapClient.h"
#include "net/PgpRecipientChecker.h"
#include "net/RelayMailSource.h"

#include "domain/PgpComposeState.h"
#include "mail/PgpMessagePresentation.h"
#include "util/ReentrancyGuard.h"

#include <KLocalizedString>

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QTimer>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QMimeType>
#include <QStandardPaths>
#include <QUrl>
#include <QVariantMap>
#include <QSet>
#include <algorithm>

MailController::MailController(MailRepository& mailRepository, RelayMailSource& relayMailSource,
                                KeywordRepository& keywordRepository, PairingStore& pairingStore,
                                FolderRepository& folderRepository, SettingsStore& settingsStore,
                                PgpBootstrapClient& pgpBootstrapClient, PgpRecipientChecker& pgpRecipientChecker,
                                QObject* parent)
    : QObject(parent)
    , m_mailRepository(mailRepository)
    , m_folderRepository(folderRepository)
    , m_settingsStore(settingsStore)
    , m_relayMailSource(relayMailSource)
    , m_keywordRepository(keywordRepository)
    , m_pairingStore(pairingStore)
    , m_pgpBootstrapClient(pgpBootstrapClient)
    , m_pgpRecipientChecker(pgpRecipientChecker)
    , m_model(new EmailListModel(this))
{
}

QObject* MailController::emailModel() const
{
    return m_model;
}

QString MailController::currentFolder() const
{
    return m_currentFolder;
}

QString MailController::selectedKeyword() const
{
    return m_selectedKeyword;
}

QVariantList MailController::keywordTabs() const
{
    const QVector<KeywordTab> tabs = m_keywordRepository.visibleTabs(m_currentFolderEmails);
    QVariantList list;
    list.reserve(tabs.size());
    for (const KeywordTab& tab : tabs) {
        QVariantMap entry;
        entry[QStringLiteral("name")] = tab.name;
        entry[QStringLiteral("count")] = tab.count;
        list.append(entry);
    }
    return list;
}

bool MailController::isBusy() const
{
    return m_isBusy;
}

QString MailController::lastError() const
{
    return m_lastError;
}

bool MailController::pgpCanEncrypt() const
{
    return m_pgpCanEncrypt;
}

bool MailController::pgpCanSign() const
{
    return m_pgpCanSign;
}

bool MailController::pgpHandoffToWebmail() const
{
    return m_pgpHandoffToWebmail;
}

QStringList MailController::pgpKeylessRecipients() const
{
    return m_pgpKeylessRecipients;
}

void MailController::setBusy(bool busy)
{
    if (m_isBusy == busy)
        return;
    m_isBusy = busy;
    emit isBusyChanged();
}

void MailController::setLastError(const QString& error)
{
    if (m_lastError == error)
        return;
    m_lastError = error;
    emit lastErrorChanged();
}

void MailController::applyFilter()
{
    if (m_selectedKeyword.isEmpty()) {
        m_model->setEmails(m_currentFolderEmails);
        return;
    }
    QVector<Email> filtered;
    for (const Email& email : m_currentFolderEmails) {
        if (email.keywords.contains(m_selectedKeyword))
            filtered.append(email);
    }
    m_model->setEmails(filtered);
}

// Read-only pairing lookup for link building. Unlike requirePairing() below
// it sets no lastError and emits nothing: an unpaired client simply has no
// webmail URL to offer, which the caller renders as "no button", not as a
// failure.
QUrl MailController::webmailBaseUrl() const
{
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value())
        return {};
    return QUrl(pairing->serverBaseUrl);
}

bool MailController::requirePairing(QUrl& serverBaseUrl, RelayAuth& auth)
{
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing) {
        setLastError(i18n("Not paired"));
        return false;
    }
    serverBaseUrl = QUrl(pairing->serverBaseUrl);
    auth = RelayAuth{ pairing->deviceId, pairing->deviceSecret };
    return true;
}

// Guarded public entry points delegate to the *Internal bodies below.
//
// The split exists because several of these legitimately call each other
// (deleteFolder -> selectFolder -> refresh, openWebmailDrafts -> saveDraft),
// and a single flag shared by both the outer and inner call would make the
// inner one a no-op -- deleting the current folder would leave the app
// showing a mailbox that no longer exists. Only the QML-facing boundary is
// guarded; internal callers use the unguarded bodies, which is safe because
// the outer guard is already held for the whole chain.
void MailController::selectFolder(const QString& wireFolder)
{
    ReentrancyGuard guard(m_inNetworkCall);
    if (!guard.entered())
        return;
    selectFolderInternal(wireFolder);
}

void MailController::selectFolderInternal(const QString& wireFolder)
{
    if (m_currentFolder != wireFolder) {
        m_currentFolder = wireFolder;
        emit currentFolderChanged();
    }
    if (!m_selectedKeyword.isEmpty()) {
        m_selectedKeyword.clear();
        emit selectedKeywordChanged();
    }
    m_currentFolderEmails = m_mailRepository.cachedEmails(m_currentFolder);
    emit keywordTabsChanged();
    applyFilter();
    refreshInternal(false);
}

void MailController::selectKeyword(const QString& keyword)
{
    if (m_selectedKeyword == keyword)
        return;
    m_selectedKeyword = keyword;
    emit selectedKeywordChanged();
    applyFilter();
}

void MailController::refresh(bool forceFullResync)
{
    ReentrancyGuard guard(m_inNetworkCall);
    if (!guard.entered())
        return;
    refreshInternal(forceFullResync);
}

void MailController::refreshInternal(bool forceFullResync)
{
    setBusy(true);
    const MailFetchOutcome outcome = m_mailRepository.refreshFolder(m_currentFolder, forceFullResync);
    setBusy(false);

    if (outcome.outcome != MailRepositoryOutcome::Success)
        setLastError(outcome.detail.isEmpty() ? i18n("Refresh failed") : outcome.detail);
    else
        setLastError(QString());

    m_currentFolderEmails = m_mailRepository.cachedEmails(m_currentFolder);
    emit keywordTabsChanged();
    applyFilter();
}

bool MailController::performActionCommon(const QStringList& messageIds, const QString& action,
                                          const std::optional<QString>& targetMailbox)
{
    ReentrancyGuard guard(m_inNetworkCall);
    if (!guard.entered())
        return false;

    QUrl serverBaseUrl;
    RelayAuth auth;
    if (!requirePairing(serverBaseUrl, auth))
        return false;

    setBusy(true);
    const ActionResult result =
        m_relayMailSource.performAction(serverBaseUrl, auth, action, messageIds, m_currentFolder, targetMailbox);
    setBusy(false);

    if (result.error.has_value() || !result.ok) {
        setLastError(result.detail.isEmpty() ? i18n("Action failed") : result.detail);
        return false;
    }

    // Optimistic local update (mirrors Android's InboxActivity/
    // EmailDetailActivity swipe-action pattern): drop every messageId the
    // server actually processed from the cached folder emails and re-apply
    // the filter immediately, rather than forcing a full refresh() after
    // every action. Per-message failures reported in result.failed are left
    // in place locally (the server did not act on them) and surfaced via
    // lastError, but the call itself still counts as a success since the
    // server accepted and partially processed the request (ActionResult::ok).
    QSet<QString> failedIds;
    for (const ActionFailure& failure : result.failed)
        failedIds.insert(failure.messageId);

    m_currentFolderEmails.erase(std::remove_if(m_currentFolderEmails.begin(), m_currentFolderEmails.end(),
                                                [&](const Email& email) {
                                                    return messageIds.contains(email.messageId)
                                                        && !failedIds.contains(email.messageId);
                                                }),
                                 m_currentFolderEmails.end());
    emit keywordTabsChanged();
    applyFilter();

    if (!result.failed.isEmpty()) {
        QStringList details;
        for (const ActionFailure& failure : result.failed)
            details << failure.messageId + QStringLiteral(": ") + failure.error;
        setLastError(details.join(QStringLiteral("; ")));
    } else {
        setLastError(QString());
    }
    return true;
}

bool MailController::archiveEmails(const QStringList& messageIds)
{
    return performActionCommon(messageIds, QStringLiteral("archive"), std::nullopt);
}

bool MailController::deleteEmails(const QStringList& messageIds)
{
    return performActionCommon(messageIds, QStringLiteral("delete"), std::nullopt);
}

bool MailController::markSpam(const QStringList& messageIds)
{
    return performActionCommon(messageIds, QStringLiteral("spam"), std::nullopt);
}

bool MailController::moveEmails(const QStringList& messageIds, const QString& targetFolder)
{
    return performActionCommon(messageIds, QStringLiteral("move"), targetFolder);
}

// Shared by sendMail() and saveDraft(): both post the identical request
// body, so reading/encoding attachments must not diverge between them.
// Returns false and sets lastError on the first unreadable file or once the
// running total passes the cap.
bool MailController::readAttachments(const QStringList& paths, QVector<MailAttachmentUpload>& out)
{
    // Matches Android's MAX_ATTACHMENT_BYTES / the backend's own cap.
    static constexpr qint64 kMaxAttachmentBytes = 25LL * 1024 * 1024;

    QMimeDatabase mimeDb;
    out.clear();
    out.reserve(paths.size());
    qint64 totalBytes = 0;
    for (const QString& path : paths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            setLastError(i18n("Could not open attachment: %1", path));
            return false;
        }
        const QByteArray data = file.readAll();
        totalBytes += data.size();
        if (totalBytes > kMaxAttachmentBytes) {
            setLastError(i18n("Attachments exceed the 25 MB limit"));
            return false;
        }
        MailAttachmentUpload upload;
        upload.name = QFileInfo(path).fileName();
        upload.mimeType = mimeDb.mimeTypeForFile(path).name();
        upload.data = data;
        out.append(upload);
    }
    return true;
}

QVariantList MailController::mailFolders() const
{
    static constexpr StandardFolder kFolders[] = {
        StandardFolder::Inbox, StandardFolder::Drafts, StandardFolder::Junk,
        StandardFolder::Sent,  StandardFolder::Trash,  StandardFolder::Archive,
    };

    const auto entry = [](const QString& wireName, int depth, bool deletable, bool isStandard) {
        QVariantMap map;
        map[QStringLiteral("wireName")] = wireName;
        map[QStringLiteral("displayName")] = standardFolderDisplayName(wireName);
        map[QStringLiteral("depth")] = depth;
        map[QStringLiteral("deletable")] = deletable;
        map[QStringLiteral("isStandard")] = isStandard;
        return QVariant::fromValue(map);
    };

    QVariantList list;
    for (StandardFolder folder : kFolders) {
        const QString wireName = standardFolderWireName(folder);
        // Standard mailboxes are never deletable -- the backend rejects it
        // outright (isBuiltinMailbox), so don't offer it.
        list.append(entry(wireName, 0, /*deletable=*/false, /*isStandard=*/true));

        // Children come straight after their parent so the flat list reads
        // as a tree once the delegate indents by `depth`.
        for (const MailFolder& child : m_folderRepository.cachedFolders(wireName))
            list.append(entry(child.path, 1, child.deletable, /*isStandard=*/false));
    }
    return list;
}

void MailController::refreshFolders()
{
    ReentrancyGuard guard(m_inNetworkCall);
    if (!guard.entered())
        return;

    // Only Archive today, matching Android's folder picker: the other five
    // standard mailboxes have no subfolder UI, so listing them would be
    // five extra synchronous round-trips (this call blocks the GUI thread --
    // Phase 6 global constraint 2) for nothing to render.
    const MailFetchOutcome outcome = m_folderRepository.refresh(standardFolderWireName(StandardFolder::Archive));

    // A failure here is deliberately quiet: the sidebar still shows all six
    // standard mailboxes, so the app stays fully usable and there is nothing
    // for the user to act on. NotPaired especially is an ordinary state at
    // startup, not an error worth a toast.
    if (outcome.outcome != MailRepositoryOutcome::Success
        && outcome.outcome != MailRepositoryOutcome::NotPaired) {
        qWarning("Folder refresh failed: %s", qUtf8Printable(outcome.detail));
    }
    emit foldersChanged();
}

bool MailController::createFolder(const QString& parent, const QString& name)
{
    ReentrancyGuard guard(m_inNetworkCall);
    if (!guard.entered())
        return false;

    setBusy(true);
    const FolderRepository::FolderMutationOutcome outcome = m_folderRepository.create(parent, name);
    setBusy(false);

    if (outcome.outcome != MailRepositoryOutcome::Success) {
        setLastError(outcome.detail.isEmpty() ? i18n("Could not create folder") : outcome.detail);
        return false;
    }
    setLastError(QString());
    emit foldersChanged();
    return true;
}

bool MailController::renameFolder(const QString& folder, const QString& name)
{
    ReentrancyGuard guard(m_inNetworkCall);
    if (!guard.entered())
        return false;

    setBusy(true);
    const FolderRepository::FolderMutationOutcome outcome = m_folderRepository.rename(folder, name);
    setBusy(false);

    if (outcome.outcome != MailRepositoryOutcome::Success) {
        setLastError(outcome.detail.isEmpty() ? i18n("Could not rename folder") : outcome.detail);
        return false;
    }
    setLastError(QString());
    // Selecting a folder that no longer exists under its old name would
    // fetch an empty mailbox, so fall back to Inbox when the current
    // selection was the one renamed.
    if (m_currentFolder == folder)
        selectFolderInternal(outcome.folder.isEmpty() ? standardFolderWireName(StandardFolder::Inbox) : outcome.folder);
    emit foldersChanged();
    return true;
}

bool MailController::deleteFolder(const QString& folder)
{
    ReentrancyGuard guard(m_inNetworkCall);
    if (!guard.entered())
        return false;

    setBusy(true);
    const FolderRepository::FolderMutationOutcome outcome = m_folderRepository.remove(folder);
    setBusy(false);

    if (outcome.outcome != MailRepositoryOutcome::Success) {
        setLastError(outcome.detail.isEmpty() ? i18n("Could not delete folder") : outcome.detail);
        return false;
    }
    setLastError(QString());
    if (m_currentFolder == folder)
        selectFolderInternal(standardFolderWireName(StandardFolder::Inbox));
    emit foldersChanged();
    return true;
}

bool MailController::saveDraft(const QString& to, const QString& cc, const QString& bcc, const QString& subject,
                                const QString& body, const QStringList& attachmentFilePaths)
{
    ReentrancyGuard guard(m_inNetworkCall);
    if (!guard.entered())
        return false;
    return saveDraftInternal(to, cc, bcc, subject, body, attachmentFilePaths);
}

bool MailController::saveDraftInternal(const QString& to, const QString& cc, const QString& bcc,
                                        const QString& subject, const QString& body,
                                        const QStringList& attachmentFilePaths)
{
    QUrl serverBaseUrl;
    RelayAuth auth;
    if (!requirePairing(serverBaseUrl, auth))
        return false;

    QVector<MailAttachmentUpload> attachments;
    if (!readAttachments(attachmentFilePaths, attachments))
        return false;

    setBusy(true);
    const SaveDraftResult result = m_relayMailSource.saveDraft(serverBaseUrl, auth, to, cc, bcc, subject, body,
                                                                QStringLiteral("html"), attachments);
    setBusy(false);

    if (result.error.has_value() || !result.ok) {
        setLastError(result.detail.isEmpty() ? i18n("Could not save draft") : result.detail);
        return false;
    }
    setLastError(QString());
    return true;
}

bool MailController::sendMail(const QString& to, const QString& cc, const QString& bcc, const QString& subject,
                               const QString& body, const QStringList& attachmentFilePaths, bool sign, bool encrypt)
{
    // The guard, not the token below, is what actually prevents a second
    // composer's Send from starting an inner send while this one is
    // suspended in HttpClient's nested event loop. The token stays as the
    // second line of defence for the pickup-fallback round trip, which
    // crosses back out to QML and can be confirmed by a different composer.
    ReentrancyGuard guard(m_inNetworkCall);
    if (!guard.entered())
        return false;

    // FIRST statement, before any early return below: PendingSend's own doc
    // comment promises the cached plaintext dies when a fresh send starts,
    // and the guards below (not paired, unreadable/oversized attachment) used
    // to return without honoring that, leaving a previous refusal's payload
    // alive past the composition that made it.
    m_pendingSend = {};

    QUrl serverBaseUrl;
    RelayAuth auth;
    if (!requirePairing(serverBaseUrl, auth))
        return false;

    QVector<MailAttachmentUpload> attachments;
    if (!readAttachments(attachmentFilePaths, attachments))
        return false;

    setBusy(true);
    const QString sendMode = QStringLiteral("html");

    // Minted into a local, and it is this local -- never m_pendingSend.token --
    // that the refusal below emits. sendMail can re-enter: the relay call runs
    // a nested event loop, and Send is only disabled once setBusy(true) lands,
    // so a second composer's Send clicked during requestSendableHtml's async
    // round trip can start an inner sendMail that replaces m_pendingSend. An
    // outer call reading the member back would then emit the INNER call's
    // token beside its OWN recipient list, both instances' ownership flags
    // would be set, and confirming would mail the other composition. Emitting
    // the local means a superseded outer call emits a token that no longer
    // matches, and the confirm is refused instead.
    const quint64 pendingSendToken = m_nextPendingSendToken++;

    // Field order matters -- PendingSend is aggregate-initialized. Eleven
    // fields: valid, to, cc, bcc, subject, body, mode, attachments, sign,
    // encrypt, token.
    m_pendingSend = PendingSend{ true, to, cc, bcc, subject, body, sendMode, attachments, sign, encrypt,
                                 pendingSendToken };

    const SendMailResult result = m_relayMailSource.sendMail(
        serverBaseUrl, auth, to, cc, bcc, subject, body, sendMode, attachments,
        sign, encrypt, /*allowPickupFallback=*/false);
    setBusy(false);

    if (result.pickupFallbackNeeded) {
        // Nothing was delivered; the pending payload stays cached for the
        // confirmed re-send. The server's list is authoritative -- it ran
        // WKD discovery too, and may name an address typed since the last
        // preflight.
        //
        // A refusal that opens a confirmation is not an error to read: clear
        // any stale lastError so a leftover red line doesn't sit under the
        // dialog contradicting it.
        setLastError(QString());
        // Emitted synchronously, inside this invokable, on purpose -- that is
        // what lets a Compose instance tell "this is my send" from "this is
        // the other window's send". See the signal's declaration.
        emit pickupFallbackRequired(pendingSendToken, result.keylessRecipients);
        return false;
    }
    if (result.clientSideNeeded) {
        // Categorical: no re-send from this client can fix it.
        m_pendingSend = {};
        m_pgpHandoffToWebmail = true;
        // One source of truth rather than a compound `visible` expression in
        // QML: a client-custody account must show the handoff block INSTEAD OF
        // the Encrypt/Sign toggles, never both, so clear the two flags that
        // draw them here.
        m_pgpCanEncrypt = false;
        m_pgpCanSign = false;
        emit pgpComposeStateChanged();
        setLastError(i18n("This account's PGP key is held only in your browser, so this app cannot "
                           "encrypt on its behalf. Continue in webmail to send it."));
        return false;
    }
    m_pendingSend = {};
    if (!result.ok) {
        // Same shape as every other failure in this class: prefer the
        // server's own detail, fall back to localized wording.
        setLastError(result.detail.isEmpty() ? i18n("Could not send message") : result.detail);
        return false;
    }
    if (!result.warning.isEmpty())
        emit sendWarning(result.warning);
    setLastError(QString());
    return true;
}

// Re-sends the exact payload the server refused, with allowPickupFallback
// set. Byte-identical by construction: nothing is rebuilt, no file is
// re-read, and the preflight is not re-run. Returns false without sending
// when there is no pending send, or when `token` names a different one, so
// neither a stray confirm nor a confirmation collected in some other
// composer's dialog can mail anything.
bool MailController::confirmPickupFallbackSend(quint64 token)
{
    ReentrancyGuard guard(m_inNetworkCall);
    if (!guard.entered())
        return false;

    if (!m_pendingSend.valid || m_pendingSend.token != token)
        return false;

    QUrl serverBaseUrl;
    RelayAuth auth;
    if (!requirePairing(serverBaseUrl, auth))
        return false;

    const PendingSend pending = m_pendingSend;
    // Cleared before the call, not after: the opt-in is per-message, and a
    // failure here must not leave a payload a second confirm could re-send.
    m_pendingSend = {};

    // Bracketed the same way sendMail()/saveDraft() are. Without it the
    // "Sending…" indicator never appears and Send stays enabled for the whole
    // 30s HttpClient timeout, so a user who sees nothing happen presses Send
    // again -- which takes another 409, another confirmation, and delivers
    // the message twice.
    setBusy(true);
    const SendMailResult result = m_relayMailSource.sendMail(
        serverBaseUrl, auth, pending.to, pending.cc, pending.bcc, pending.subject, pending.body,
        pending.mode, pending.attachments, pending.sign, pending.encrypt,
        /*allowPickupFallback=*/true);
    setBusy(false);

    if (!result.ok) {
        // Same detail-or-localized-fallback shape as every other failure in
        // this class: a 200 carrying {"ok":false} leaves detail empty, and
        // reporting that verbatim would fail silently.
        setLastError(result.detail.isEmpty() ? i18n("Could not send message") : result.detail);
        return false;
    }
    if (!result.warning.isEmpty())
        emit sendWarning(result.warning);
    // The one send that most needs to read as success: clear any error left
    // over from the refusal that opened the confirmation.
    setLastError(QString());
    return true;
}

// Cancelling the dialog drops the cached plaintext rather than holding it
// for a confirm that may never come.
void MailController::discardPendingSend()
{
    m_pendingSend = {};
}

// Called when Compose opens. A failure leaves every control hidden --
// "couldn't check" is never "no PGP".
void MailController::refreshPgpComposeState(bool force)
{
    ReentrancyGuard guard(m_inNetworkCall);
    if (!guard.entered())
        return;

    // Unconditional, ahead of the cache check below: pgpKeylessRecipients is
    // singleton state belonging to whatever was composed last, so a fresh
    // composer that ticks Encrypt would otherwise flash the PREVIOUS
    // message's keyless addresses until this session's debounce lands.
    if (!m_pgpKeylessRecipients.isEmpty()) {
        m_pgpKeylessRecipients.clear();
        emit pgpKeylessRecipientsChanged();
    }

    // At most one bootstrap fetch per session. This is a synchronous network
    // call made from Compose.qml's Component.onCompleted, i.e. a nested event
    // loop while the object tree is still being built; custody mode is fixed
    // at key creation and cannot change within a session, so re-asking on
    // every compose open buys nothing.
    if (m_pgpComposeStateFetched && !force)
        return;

    QUrl serverBaseUrl;
    RelayAuth auth;
    if (!requirePairing(serverBaseUrl, auth))
        return; // not cached: pairing later must still get a real answer

    const PgpBootstrapResult bootstrap = m_pgpBootstrapClient.fetch(serverBaseUrl, auth);
    const PgpComposeState state = bootstrap.ok
        ? pgpComposeStateOf(bootstrap.hasIdentity, bootstrap.protection)
        : pgpComposeStateOf(std::nullopt, std::nullopt);

    // Only a real answer is cached. "Couldn't check" is not a custody mode, so
    // caching a failure would hide the PGP controls for the rest of the
    // session over one transient 503; the next compose open retries instead.
    m_pgpComposeStateFetched = bootstrap.ok;
    m_pgpCanEncrypt = state.canEncrypt;
    m_pgpCanSign = state.canSign;
    m_pgpHandoffToWebmail = state.handoffToWebmail;
    emit pgpComposeStateChanged();
}

// Inline, non-blocking warning only. This is a LOWER BOUND: check reads the
// user's contacts, while the send path also runs WKD/keyserver discovery, so
// an address named here may still be encrypted to successfully. Never
// phrase the result as a prediction, and never let it gate the send -- the
// server's 409 is the gate.
//
// Deliberately NOT bracketed with setBusy(): that would disable Send while
// the user is still typing recipients.
void MailController::preflightRecipients(const QString& to, const QString& cc, const QString& bcc)
{
    // check() below runs a nested QEventLoop (HttpClient::waitForReply), which
    // keeps processing timers -- so Compose.qml's 500ms debounce fires again
    // mid-flight and lands right back here, one frame deeper per 500ms of
    // typing. Results then unwind LIFO, making the LAST write to
    // m_pgpKeylessRecipients the OLDEST request's answer: an inline warning
    // naming an address the user already removed. Dropping the overlapping
    // call is correct here because this is a debounced, advisory lower bound,
    // not a gate -- the next edit restarts the timer anyway.
    if (m_preflightInFlight)
        return;

    QUrl serverBaseUrl;
    RelayAuth auth;
    if (!requirePairing(serverBaseUrl, auth))
        return;

    QStringList addresses;
    for (const QString& field : { to, cc, bcc })
        addresses += field.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (QString& address : addresses)
        address = address.trimmed();
    addresses.removeAll(QString());

    QStringList keyless;
    if (!addresses.isEmpty()) {
        m_preflightInFlight = true;
        const RecipientKeyCheckResult result = m_pgpRecipientChecker.check(serverBaseUrl, auth, addresses);
        m_preflightInFlight = false;
        // A failed preflight shows nothing rather than a false all-clear --
        // and clears whatever it showed before, rather than leaving a stale
        // list standing in for an answer this call did not get.
        if (result.ok)
            keyless = result.keylessRecipients;
    }
    if (keyless == m_pgpKeylessRecipients)
        return;
    m_pgpKeylessRecipients = keyless;
    emit pgpKeylessRecipientsChanged();
}

// Saves the composition to Drafts, then opens webmail there in the user's
// browser -- never an embedded view, which shares no session and would put
// an account-password field inside this app.
//
// Returns false without opening anything if the draft did not save: opening
// a browser onto a draft that is not there loses the user's message. Also
// returns false when the pairing's base URL is not https, since
// webmailMailboxUrl() refuses to build a link from a downgraded base.
bool MailController::openWebmailDrafts(const QString& to, const QString& cc, const QString& bcc,
                                        const QString& subject, const QString& body,
                                        const QStringList& attachmentFilePaths)
{
    ReentrancyGuard guard(m_inNetworkCall);
    if (!guard.entered())
        return false;

    // Pairing is checked first so the two distinct failures read distinctly:
    // webmailMailboxUrl() returns an empty URL for an unpaired client just as
    // it does for a plain-http one, and reporting "paired over an insecure
    // connection" to someone who is not paired at all is simply wrong.
    QUrl serverBaseUrl;
    RelayAuth auth;
    if (!requirePairing(serverBaseUrl, auth)) // sets "Not paired"
        return false;

    const QUrl url = webmailMailboxUrl(webmailBaseUrl(), QStringLiteral("Drafts"));
    // Checked BEFORE saving: a draft saved for a handoff that cannot open
    // leaves the user with a silently duplicated draft and no browser.
    if (url.isEmpty()) {
        setLastError(i18n("This device is paired over an insecure connection, so KyPost cannot open "
                           "webmail for you. Open your mail in a browser to send this message."));
        return false;
    }
    if (!saveDraftInternal(to, cc, bcc, subject, body, attachmentFilePaths))
        return false;
    if (!QDesktopServices::openUrl(url)) {
        setLastError(i18n("Saved to Drafts, but KyPost could not open your browser."));
        return false;
    }
    return true;
}

QVariantList MailController::listAttachments(const QString& mailbox, const QString& messageId)
{
    ReentrancyGuard guard(m_inNetworkCall);
    if (!guard.entered())
        return {};

    QUrl serverBaseUrl;
    RelayAuth auth;
    if (!requirePairing(serverBaseUrl, auth))
        return {};

    setBusy(true);
    const ListAttachmentsResult result = m_relayMailSource.listAttachments(serverBaseUrl, auth, mailbox, messageId);
    setBusy(false);

    if (result.error.has_value()) {
        setLastError(result.detail.isEmpty() ? i18n("Could not list attachments") : result.detail);
        return {};
    }
    setLastError(QString());

    QVariantList list;
    list.reserve(result.attachments.size());
    for (const MailAttachmentInfo& info : result.attachments) {
        QVariantMap entry;
        entry[QStringLiteral("index")] = info.index;
        entry[QStringLiteral("name")] = info.name;
        entry[QStringLiteral("mimeType")] = info.mimeType;
        entry[QStringLiteral("size")] = info.size;
        list.append(entry);
    }
    return list;
}

QString MailController::dedupedFilePath(const QString& directory, const QString& fileName)
{
    const QFileInfo info(fileName);
    const QString baseName = info.completeBaseName();
    const QString suffix = info.suffix();

    // Bounded. The original loop had no cap, so a directory already holding
    // every candidate name (or a filesystem whose exists() keeps answering
    // true) spun forever on the GUI thread.
    static constexpr int kMaxDedupeAttempts = 1000;

    QString candidate = fileName;
    for (int suffixCounter = 1; suffixCounter <= kMaxDedupeAttempts; ++suffixCounter) {
        if (!QFile::exists(directory + QStringLiteral("/") + candidate))
            return directory + QStringLiteral("/") + candidate;
        candidate = suffix.isEmpty()
            ? QStringLiteral("%1 (%2)").arg(baseName).arg(suffixCounter)
            : QStringLiteral("%1 (%2).%3").arg(baseName).arg(suffixCounter).arg(suffix);
    }
    return QString(); // caller reports "could not write"
}

// Creates `path` for writing, failing rather than truncating if something is
// already there.
//
// QFile::open(WriteOnly) happily clobbers an existing file, which turns
// dedupedFilePath()'s exists() check into a check-then-use race: two
// downloads of the same attachment name resolve to the same path (entirely
// reachable given how easily these blocking calls interleave) and one
// silently overwrites the other. QIODevice::NewOnly is Qt's O_EXCL
// equivalent and closes it by construction.
bool MailController::openForExclusiveWrite(QFile& file, const QString& path)
{
    if (path.isEmpty())
        return false;
    file.setFileName(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly);
}

// Writes `data` in full or leaves nothing behind.
//
// Every attachment write used to discard both the mkpath() result and the
// qint64 that write() returns, so a full disk produced a truncated file on
// disk and a "saved" message in the UI.
bool MailController::writeAllOrRemove(QFile& file, const QByteArray& data)
{
    const qint64 written = file.write(data);
    const bool flushed = file.flush();
    file.close();
    if (written == data.size() && flushed)
        return true;
    file.remove();
    return false;
}

bool MailController::downloadAttachment(const QString& mailbox, const QString& messageId, int index,
                                         const QString& suggestedName)
{
    ReentrancyGuard guard(m_inNetworkCall);
    if (!guard.entered())
        return false;

    QUrl serverBaseUrl;
    RelayAuth auth;
    if (!requirePairing(serverBaseUrl, auth))
        return false;

    setBusy(true);
    const DownloadAttachmentResult result =
        m_relayMailSource.downloadAttachment(serverBaseUrl, auth, mailbox, messageId, index);
    setBusy(false);

    if (result.error.has_value()) {
        setLastError(result.detail.isEmpty() ? i18n("Download failed") : result.detail);
        return false;
    }

    // Prefer the caller-supplied name (QML typically already has the
    // attachment's display name from listAttachments()); fall back to
    // whatever filename the download response's Content-Disposition header
    // carried, and finally to a generic name if both are somehow empty.
    QString name = suggestedName.isEmpty() ? result.filename : suggestedName;
    // Both sources are attacker-influenced (an attachment's display name and
    // Content-Disposition filename both originate from the mail message
    // itself), so strip any path component before using it to build a
    // filesystem path below -- QFileInfo::fileName() keeps only the segment
    // after the last '/', which neutralizes a "../../.ssh/authorized_keys"-
    // style name regardless of how many traversal segments it contains.
    name = QFileInfo(name).fileName();
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
        name = QStringLiteral("attachment");

    if (m_settingsStore.hostileLocationProtectionEnabled())
        return openAttachmentEphemerally(name, result.mimeType, result.data);

    const QString downloadDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (!QDir().mkpath(downloadDir)) {
        setLastError(i18n("Could not create the download folder %1", downloadDir));
        return false;
    }
    const QString targetPath = dedupedFilePath(downloadDir, name);

    QFile outFile;
    if (!openForExclusiveWrite(outFile, targetPath)) {
        setLastError(i18n("Could not write attachment to %1", downloadDir));
        return false;
    }
    if (!writeAllOrRemove(outFile, result.data)) {
        setLastError(i18n("The attachment could not be written in full (is the disk full?)."));
        return false;
    }

    setLastError(QString());
    return true;
}

// Attachment types this app is willing to hand to the desktop's own handler.
//
// Deliberately short and deliberately not derived from the filename. The
// ephemeral-open path below invokes QDesktopServices::openUrl on bytes that
// came out of a mail message, and the handler the desktop picks is chosen by
// suffix -- so "Invoice.pdf.desktop", "notes.pdf.html" or any of the other
// double-extension classics used to be enough to get an arbitrary handler
// launched with the user's full session privileges. Worse, this only ever
// happened in Hostile Location Protection mode: the one mode meant for users
// who expect to be attacked was the only one that auto-opened hostile input.
//
// The declared MIME type from the server's attachment listing is the gate,
// and the extension is then forced to match it, so a mismatch cannot smuggle
// a different handler in.
static const QHash<QString, QString>& viewableAttachmentTypes()
{
    static const QHash<QString, QString> kTypes = {
        { QStringLiteral("application/pdf"), QStringLiteral("pdf") },
        { QStringLiteral("image/png"), QStringLiteral("png") },
        { QStringLiteral("image/jpeg"), QStringLiteral("jpg") },
        { QStringLiteral("image/gif"), QStringLiteral("gif") },
        { QStringLiteral("image/webp"), QStringLiteral("webp") },
        { QStringLiteral("text/plain"), QStringLiteral("txt") },
    };
    return kTypes;
}

// Hostile Location Protection: never write an attachment to Downloads.
//
// The nearest Linux equivalent to Android's disk-free ContentProvider pipe
// is a file in XDG_RUNTIME_DIR, which on essentially every current distro is
// a tmpfs mount -- so the bytes stay in RAM rather than reaching persistent
// storage. This is a real improvement but NOT identical to "never touches
// disk": tmpfs pages can be swapped, and a distro that backs
// XDG_RUNTIME_DIR differently would lose the property entirely. The UI says
// "View (temporary)" rather than "Save" so the weaker guarantee is not
// oversold.
bool MailController::openAttachmentEphemerally(const QString& name, const QString& declaredMimeType,
                                                const QByteArray& data)
{
    // Type gate, before a single byte is written. See
    // viewableAttachmentTypes() for why the server-declared type decides and
    // the filename does not.
    const auto typeEntry = viewableAttachmentTypes().constFind(declaredMimeType.trimmed().toLower());
    if (typeEntry == viewableAttachmentTypes().constEnd()) {
        setLastError(i18n("KyPost will not open attachments of this type (%1). Turn off Hostile "
                           "Location Protection to save it and open it yourself.",
                           declaredMimeType.isEmpty() ? i18n("unknown") : declaredMimeType));
        return false;
    }

    // Prefer XDG_RUNTIME_DIR (tmpfs, 0700, per-user) over TempLocation,
    // which is usually /tmp and usually disk-backed and world-traversable.
    QString baseDir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (baseDir.isEmpty())
        baseDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);

    const QString dir = baseDir + QStringLiteral("/kypost-attachments");
    if (!QDir().mkpath(dir)) {
        setLastError(i18n("Could not create a temporary location for the attachment"));
        return false;
    }
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    // Force the extension to the one the allowlisted MIME type implies,
    // rather than keeping whatever the message asked for. Without this,
    // "report.pdf.desktop" declared as application/pdf would pass the gate
    // above and still be launched as a .desktop file.
    const QString safeBaseName = QFileInfo(name).completeBaseName();
    const QString safeName = (safeBaseName.isEmpty() ? QStringLiteral("attachment") : safeBaseName)
        + QLatin1Char('.') + typeEntry.value();

    const QString path = dedupedFilePath(dir, safeName);
    QFile outFile;
    if (!openForExclusiveWrite(outFile, path)) {
        setLastError(i18n("Could not open the attachment"));
        return false;
    }
    // Owner-only before any bytes are written, so the content is never
    // briefly readable by other local users.
    outFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (!writeAllOrRemove(outFile, data)) {
        setLastError(i18n("The attachment could not be written in full (is the disk full?)."));
        return false;
    }

    m_ephemeralAttachments.append(path);
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));

    // There is no reliable signal for "the external viewer closed" -- the
    // handler may be a long-running process that was already open. A timer
    // is the honest fallback: long enough for the viewer to read the file,
    // short enough that it does not linger. It is also deleted on exit (see
    // clearEphemeralAttachments) so a crash-free quit leaves nothing.
    QTimer::singleShot(kEphemeralAttachmentLifetimeMs, this, [this, path]() {
        QFile::remove(path);
        m_ephemeralAttachments.removeAll(path);
    });

    setLastError(QString());
    return true;
}

void MailController::clearEphemeralAttachments()
{
    for (const QString& path : m_ephemeralAttachments)
        QFile::remove(path);
    m_ephemeralAttachments.clear();
}

QVariantMap MailController::findByMessageId(const QString& messageId) const
{
    const std::optional<Email> email = m_mailRepository.findCachedEmail(messageId);
    if (!email.has_value())
        return {};

    QVariantMap map;
    map[QStringLiteral("messageId")] = email->messageId;
    map[QStringLiteral("folder")] = email->folder;
    map[QStringLiteral("sender")] = email->sender;
    map[QStringLiteral("sentTo")] = email->sentTo;
    map[QStringLiteral("cc")] = email->cc;
    map[QStringLiteral("bcc")] = email->bcc;
    map[QStringLiteral("subject")] = email->subject;
    map[QStringLiteral("preview")] = email->preview;
    map[QStringLiteral("body")] = email->body.value_or(QString());
    map[QStringLiteral("label")] = email->label;
    map[QStringLiteral("keywords")] = QVariant::fromValue(email->keywords);
    map[QStringLiteral("status")] = email->status;
    map[QStringLiteral("atUtc")] = email->atUtc;
    map[QStringLiteral("hasAttachments")] = email->hasAttachments;
    map[QStringLiteral("sourceMode")] = email->sourceMode;

    // Everything EmailDetail.qml needs to render the PGP banner without
    // re-deriving the rule. `pgpState` is the raw enum value so QML can
    // switch on it; the two strings are the localized copy for that state.
    const PgpMessageState pgpState = pgpMessageStateOf(*email);
    map[QStringLiteral("pgpState")] = static_cast<int>(pgpState);
    map[QStringLiteral("pgpBannerTitle")] = pgpBannerTitle(pgpState);
    map[QStringLiteral("pgpBannerBody")] = pgpBannerBody(pgpState, email->pgpDecryptError);
    // Empty unless the message is unreadable here AND the pairing has a
    // usable https base URL, so QML can just test for emptiness rather than
    // duplicating the safety rules.
    map[QStringLiteral("webmailUrl")] = pgpState == PgpMessageState::ClientProtected
        ? webmailReadUrl(webmailBaseUrl(), email->folder, email->messageId).toString()
        : QString();
    return map;
}

QVariantList MailController::allKeywordSettings() const
{
    // See the header doc comment: deliberately INBOX's cache, not
    // m_currentFolderEmails.
    const QVector<Email> inboxEmails = m_mailRepository.cachedEmails(QStringLiteral("INBOX"));
    const QVector<KeywordSettings> settings = m_keywordRepository.allSettings(inboxEmails);

    QVariantList list;
    list.reserve(settings.size());
    for (const KeywordSettings& entry : settings) {
        QVariantMap map;
        map[QStringLiteral("keyword")] = entry.keyword;
        map[QStringLiteral("visible")] = entry.visible;
        list.append(map);
    }
    return list;
}

void MailController::setKeywordVisible(const QString& keyword, bool visible)
{
    m_keywordRepository.setVisible(keyword, visible);
    emit keywordTabsChanged();
}

QVariantList MailController::standardFolders() const
{
    static constexpr StandardFolder kFolders[] = {
        StandardFolder::Inbox, StandardFolder::Drafts, StandardFolder::Junk,
        StandardFolder::Sent,  StandardFolder::Trash,  StandardFolder::Archive,
    };

    QVariantList list;
    list.reserve(6);
    for (StandardFolder folder : kFolders) {
        const QString wireName = standardFolderWireName(folder);
        QVariantMap entry;
        entry[QStringLiteral("wireName")] = wireName;
        entry[QStringLiteral("displayName")] = standardFolderDisplayName(wireName);
        list.append(entry);
    }
    return list;
}
