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

#include <KLocalizedString>

#include <QDesktopServices>
#include <QDir>
#include <QFile>
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

void MailController::selectFolder(const QString& wireFolder)
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
    refresh();
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
        selectFolder(outcome.folder.isEmpty() ? standardFolderWireName(StandardFolder::Inbox) : outcome.folder);
    emit foldersChanged();
    return true;
}

bool MailController::deleteFolder(const QString& folder)
{
    setBusy(true);
    const FolderRepository::FolderMutationOutcome outcome = m_folderRepository.remove(folder);
    setBusy(false);

    if (outcome.outcome != MailRepositoryOutcome::Success) {
        setLastError(outcome.detail.isEmpty() ? i18n("Could not delete folder") : outcome.detail);
        return false;
    }
    setLastError(QString());
    if (m_currentFolder == folder)
        selectFolder(standardFolderWireName(StandardFolder::Inbox));
    emit foldersChanged();
    return true;
}

bool MailController::saveDraft(const QString& to, const QString& cc, const QString& bcc, const QString& subject,
                                const QString& body, const QStringList& attachmentFilePaths)
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
    QUrl serverBaseUrl;
    RelayAuth auth;
    if (!requirePairing(serverBaseUrl, auth))
        return false;

    QVector<MailAttachmentUpload> attachments;
    if (!readAttachments(attachmentFilePaths, attachments))
        return false;

    setBusy(true);
    const QString sendMode = QStringLiteral("html");

    // Field order matters -- PendingSend is aggregate-initialized. Ten
    // fields: valid, to, cc, bcc, subject, body, mode, attachments, sign,
    // encrypt.
    m_pendingSend = PendingSend{ true, to, cc, bcc, subject, body, sendMode, attachments, sign, encrypt };

    const SendMailResult result = m_relayMailSource.sendMail(
        serverBaseUrl, auth, to, cc, bcc, subject, body, sendMode, attachments,
        sign, encrypt, /*allowPickupFallback=*/false);
    setBusy(false);

    if (result.pickupFallbackNeeded) {
        // Nothing was delivered; the pending payload stays cached for the
        // confirmed re-send. The server's list is authoritative -- it ran
        // WKD discovery too, and may name an address typed since the last
        // preflight.
        emit pickupFallbackRequired(result.keylessRecipients);
        return false;
    }
    if (result.clientSideNeeded) {
        // Categorical: no re-send from this client can fix it.
        m_pendingSend = {};
        m_pgpHandoffToWebmail = true;
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
// when there is no pending send, so a stray confirm cannot mail anything.
bool MailController::confirmPickupFallbackSend()
{
    if (!m_pendingSend.valid)
        return false;

    QUrl serverBaseUrl;
    RelayAuth auth;
    if (!requirePairing(serverBaseUrl, auth))
        return false;

    const PendingSend pending = m_pendingSend;
    // Cleared before the call, not after: the opt-in is per-message, and a
    // failure here must not leave a payload a second confirm could re-send.
    m_pendingSend = {};

    const SendMailResult result = m_relayMailSource.sendMail(
        serverBaseUrl, auth, pending.to, pending.cc, pending.bcc, pending.subject, pending.body,
        pending.mode, pending.attachments, pending.sign, pending.encrypt,
        /*allowPickupFallback=*/true);

    if (!result.ok) {
        setLastError(result.detail);
        return false;
    }
    if (!result.warning.isEmpty())
        emit sendWarning(result.warning);
    return true;
}

// Cancelling the dialog drops the cached plaintext rather than holding it
// for a confirm that may never come.
void MailController::discardPendingSend()
{
    m_pendingSend = {};
}

// Called once when Compose opens. A failure leaves every control hidden --
// "couldn't check" is never "no PGP".
void MailController::refreshPgpComposeState()
{
    QUrl serverBaseUrl;
    RelayAuth auth;
    if (!requirePairing(serverBaseUrl, auth))
        return;

    const PgpBootstrapResult bootstrap = m_pgpBootstrapClient.fetch(serverBaseUrl, auth);
    const PgpComposeState state = bootstrap.ok
        ? pgpComposeStateOf(bootstrap.hasIdentity, bootstrap.protection)
        : pgpComposeStateOf(std::nullopt, std::nullopt);

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
void MailController::preflightRecipients(const QString& to, const QString& cc, const QString& bcc)
{
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
        const RecipientKeyCheckResult result = m_pgpRecipientChecker.check(serverBaseUrl, auth, addresses);
        // A failed preflight shows nothing rather than a false all-clear.
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
    const QUrl url = webmailMailboxUrl(webmailBaseUrl(), QStringLiteral("Drafts"));
    // Checked BEFORE saving: a draft saved for a handoff that cannot open
    // leaves the user with a silently duplicated draft and no browser.
    if (url.isEmpty()) {
        setLastError(i18n("This device is paired over an insecure connection, so KyPost cannot open "
                           "webmail for you. Open your mail in a browser to send this message."));
        return false;
    }
    if (!saveDraft(to, cc, bcc, subject, body, attachmentFilePaths))
        return false;
    if (!QDesktopServices::openUrl(url)) {
        setLastError(i18n("Saved to Drafts, but KyPost could not open your browser."));
        return false;
    }
    return true;
}

QVariantList MailController::listAttachments(const QString& mailbox, const QString& messageId)
{
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

    QString candidate = fileName;
    int suffixCounter = 1;
    while (QFile::exists(directory + QStringLiteral("/") + candidate)) {
        candidate = suffix.isEmpty()
            ? QStringLiteral("%1 (%2)").arg(baseName).arg(suffixCounter)
            : QStringLiteral("%1 (%2).%3").arg(baseName).arg(suffixCounter).arg(suffix);
        ++suffixCounter;
    }
    return directory + QStringLiteral("/") + candidate;
}

bool MailController::downloadAttachment(const QString& mailbox, const QString& messageId, int index,
                                         const QString& suggestedName)
{
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
        return openAttachmentEphemerally(name, result.data);

    const QString downloadDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QDir().mkpath(downloadDir);
    const QString targetPath = dedupedFilePath(downloadDir, name);

    QFile outFile(targetPath);
    if (!outFile.open(QIODevice::WriteOnly)) {
        setLastError(i18n("Could not write attachment to %1", targetPath));
        return false;
    }
    outFile.write(result.data);
    outFile.close();

    setLastError(QString());
    return true;
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
bool MailController::openAttachmentEphemerally(const QString& name, const QByteArray& data)
{
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

    const QString path = dedupedFilePath(dir, name);
    QFile outFile(path);
    if (!outFile.open(QIODevice::WriteOnly)) {
        setLastError(i18n("Could not open the attachment"));
        return false;
    }
    // Owner-only before any bytes are written, so the content is never
    // briefly readable by other local users.
    outFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    outFile.write(data);
    outFile.close();

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
