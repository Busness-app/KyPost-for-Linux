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
#include "net/NetworkExecutor.h"
#include "net/PgpBootstrapClient.h"
#include "net/PgpPayloadClient.h"
#include "net/PgpResolveClient.h"
#include "net/PgpSendClient.h"
#include "pgp/PgpSendPlanner.h"
#include "pgp/EncryptedMessageReader.h"
#include "pgp/OpenPgpEncryptor.h"
#include "pgp/OpenPgpKeyImporter.h"
#include "pgp/MimeBodyReader.h"
#include "pgp/OpenPgpDecryptor.h"
#include "security/PrivatePath.h"
#include "net/PgpRecipientChecker.h"
#include "net/RelayMailSource.h"

#include "domain/PgpComposeState.h"
#include "mail/PgpMessagePresentation.h"

#include <KLocalizedString>

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QDateTime>
#include <QTimer>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QMimeType>
#include <QStandardPaths>
#include <QUrl>
#include <QVariantMap>
#include <QSet>
#include <algorithm>

namespace {

// Probed once. engineAvailable() stats the gpg installation, and this is
// read while building the map for every message the reader opens.
bool openPgpEngineAvailable()
{
    static const bool available = OpenPgpDecryptor::engineAvailable();
    return available;
}

} // namespace

MailController::MailController(MailRepository& mailRepository, RelayMailSource& relayMailSource,
                                KeywordRepository& keywordRepository, PairingStore& pairingStore,
                                FolderRepository& folderRepository, SettingsStore& settingsStore,
                                PgpBootstrapClient& pgpBootstrapClient, PgpRecipientChecker& pgpRecipientChecker,
                                NetworkExecutor& networkExecutor, QObject* parent)
    : QObject(parent)
    , m_mailRepository(mailRepository)
    , m_folderRepository(folderRepository)
    , m_settingsStore(settingsStore)
    , m_relayMailSource(relayMailSource)
    , m_keywordRepository(keywordRepository)
    , m_pairingStore(pairingStore)
    , m_pgpBootstrapClient(pgpBootstrapClient)
    , m_pgpRecipientChecker(pgpRecipientChecker)
    , m_executor(networkExecutor)
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
    return m_busyDepth > 0;
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

void MailController::pushBusy()
{
    if (++m_busyDepth == 1)
        emit isBusyChanged();
}

void MailController::popBusy()
{
    Q_ASSERT(m_busyDepth > 0);
    if (--m_busyDepth == 0)
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

// No entry point here takes a re-entrancy guard any more. Every request runs
// on the executor thread, so nothing is ever suspended inside a nested event
// loop and there is nothing to re-enter. Taking a guard would now be actively
// wrong: it would be released the instant the method returned, while the
// request was still outstanding -- guarding nothing, while still blocking a
// genuine second call.
void MailController::selectFolder(const QString& wireFolder)
{
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
    reloadCurrentFolder();
    refreshInternal(false);
}

void MailController::reloadCurrentFolder()
{
    m_currentFolderEmails = m_mailRepository.cachedEmails(m_currentFolder);
    emit keywordTabsChanged();
    applyFilter();
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
    refreshInternal(forceFullResync);
}

void MailController::refreshInternal(bool forceFullResync)
{
    // Coalescing, not re-entrancy. Pull-to-refresh, the toolbar button and a
    // folder switch can all land inside one round trip; N of them must cost
    // one follow-up request rather than N queued behind each other on the
    // single executor thread.
    if (m_refreshInFlight) {
        m_refreshPending = true;
        m_refreshPendingFullResync = m_refreshPendingFullResync || forceFullResync;
        return;
    }

    // Phase 1 on this thread: PairingStore caches and is mutated by the
    // credential gate, CursorStore is a QSettings file. Only the request that
    // uses what they produce may leave.
    const std::optional<MailRefreshPlan> plan = m_mailRepository.planRefresh(m_currentFolder, forceFullResync);
    if (!plan.has_value()) {
        setLastError(i18n("Not paired"));
        reloadCurrentFolder();
        return;
    }

    m_refreshInFlight = true;
    pushBusy();
    m_executor.run(
        this, [plan = *plan](HttpClient& http) { return MailRepository::fetchWith(http, plan); },
        [this, plan = *plan](const InboxFetchResult& result) { finishRefresh(plan, result); });
}

void MailController::finishRefresh(const MailRefreshPlan& plan, const InboxFetchResult& result)
{
    m_refreshInFlight = false;
    popBusy();

    // Phase 3 back on this thread: the delta merge writes through EmailDao,
    // whose QSqlDatabase connection was opened here and may not be touched
    // anywhere else.
    const MailFetchOutcome outcome = m_mailRepository.applyRefresh(plan, result);
    if (outcome.outcome == MailRepositoryOutcome::CacheWriteFailed) {
        // Worded here, not in core/ (AGENTS.md 6c). Named separately from the
        // generic failure because the relay answered fine -- the local cache
        // is what refused -- and because the sync cursor was deliberately
        // held back, so this really will be retried rather than skipped.
        setLastError(i18n("Downloaded mail could not be saved to this device. "
                           "Check free disk space; the next refresh will try again."));
    } else if (outcome.outcome == MailRepositoryOutcome::PairingChanged) {
        // Silent, and not an error: this reply belonged to the account that
        // was paired when the request went out, and the repository discarded
        // it rather than write it into the account paired now. "Refresh
        // failed" would be a lie about a refresh that worked, shown to
        // someone who has just successfully paired.
        setLastError(QString());
    } else if (outcome.outcome != MailRepositoryOutcome::Success) {
        setLastError(outcome.detail.isEmpty() ? i18n("Refresh failed") : outcome.detail);
    } else {
        setLastError(QString());
    }

    // m_currentFolder, deliberately -- NOT plan.folder. The user can switch
    // folders while the request is out, and the reply for the folder they
    // left must still be written to the database (it is, above, keyed on
    // plan.folder) without repainting the list with it.
    reloadCurrentFolder();

    // Answer an outstanding notification tap-through, if this was the refresh
    // it was waiting for. Cleared before the lookup so a miss cannot leave it
    // set and hijack the next unrelated refresh.
    if (!m_pendingNotificationMessageId.isEmpty()) {
        const QString pendingId = m_pendingNotificationMessageId;
        m_pendingNotificationMessageId.clear();
        const std::optional<Email> found = m_mailRepository.findCachedEmail(pendingId);
        // The refresh can CREATE the ambiguity: the message was not cached
        // when the notification arrived, and the sync that fetched it found it
        // in more than one mailbox. Same question, so the same answer.
        //
        // NOT an early return. The coalesced-refresh block below still has to
        // run, or a refresh requested while this one was in flight is dropped
        // and the app simply stops syncing until something else asks.
        const QStringList ambiguous =
            found.has_value() ? QStringList() : m_mailRepository.foldersHolding(pendingId);
        if (ambiguous.size() > 1) {
            setLastError(QString());
            emit notificationEmailAmbiguous(pendingId, ambiguous);
        } else {
            if (!found.has_value()) {
                // Worded here rather than left to the roots: this is the one
                // outcome the user cannot act on by waiting, and a blank
                // detail page said nothing at all.
                setLastError(i18n("That message could not be found. It may have been moved or "
                                   "deleted, or it may be in a folder KyPost has not synced."));
            }
            emit notificationEmailResolved(pendingId, found.has_value() ? found->folder : QString());
        }
    }

    if (m_refreshPending) {
        const bool fullResync = m_refreshPendingFullResync;
        m_refreshPending = false;
        m_refreshPendingFullResync = false;
        refreshInternal(fullResync);
    }
}

void MailController::performActionCommon(const QStringList& messageIds, const QString& action,
                                          const std::optional<QString>& targetMailbox)
{
    QUrl serverBaseUrl;
    RelayAuth auth;
    if (!requirePairing(serverBaseUrl, auth)) {
        emit actionCompleted(action, messageIds, false);
        return;
    }

    // Captured at DISPATCH time, not read in the completion handler: the
    // action applies to the mailbox that was on screen when the user
    // triggered it. Reading m_currentFolder when the reply lands would send
    // the request against whatever folder they had switched to since.
    const QString mailbox = m_currentFolder;

    pushBusy();
    m_executor.run(
        this,
        [serverBaseUrl, auth, action, messageIds, mailbox, targetMailbox](HttpClient& http) {
            RelayMailSource source(http);
            return source.performAction(serverBaseUrl, auth, action, messageIds, mailbox, targetMailbox);
        },
        [this, action, messageIds](const ActionResult& result) { finishAction(action, messageIds, result); });
}

void MailController::finishAction(const QString& action, const QStringList& messageIds, const ActionResult& result)
{
    popBusy();

    if (result.error.has_value() || !result.ok) {
        setLastError(result.detail.isEmpty() ? i18n("Action failed") : result.detail);
        emit actionCompleted(action, messageIds, false);
        return;
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

    // Reported as success even when result.failed names individual messages:
    // the server accepted and partially processed the request, and the
    // per-message trouble is in lastError. Same rule the synchronous form
    // used when it returned true here.
    emit actionCompleted(action, messageIds, true);
}

void MailController::archiveEmails(const QStringList& messageIds)
{
    performActionCommon(messageIds, QStringLiteral("archive"), std::nullopt);
}

void MailController::deleteEmails(const QStringList& messageIds)
{
    performActionCommon(messageIds, QStringLiteral("delete"), std::nullopt);
}

void MailController::markSpam(const QStringList& messageIds)
{
    performActionCommon(messageIds, QStringLiteral("spam"), std::nullopt);
}

void MailController::moveEmails(const QStringList& messageIds, const QString& targetFolder)
{
    performActionCommon(messageIds, QStringLiteral("move"), targetFolder);
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
    const std::optional<RelayRequestPlan> plan = m_folderRepository.planRequest();
    if (!plan.has_value())
        return; // unpaired: an ordinary state at startup, not an error

    // Only Archive today, matching Android's folder picker: the other five
    // standard mailboxes have no subfolder UI, so listing them would be five
    // extra round-trips for nothing to render.
    const QString parent = standardFolderWireName(StandardFolder::Archive);

    m_executor.run(
        this,
        [endpoint = plan->endpoint, parent](HttpClient& http) {
            return FolderRepository::listWith(http, endpoint, parent);
        },
        [this, plan = *plan, parent](const FolderListResult& result) {
            const MailFetchOutcome outcome = m_folderRepository.applyList(plan, parent, result);
            // A failure here is deliberately quiet: the sidebar still shows
            // all six standard mailboxes, so the app stays fully usable and
            // there is nothing for the user to act on. PairingChanged is not
            // even that -- it is the correct outcome for a listing that
            // belongs to an account this device no longer has, so it is not
            // worth a log line either.
            if (outcome.outcome != MailRepositoryOutcome::Success
                && outcome.outcome != MailRepositoryOutcome::PairingChanged) {
                qWarning("Folder refresh failed: %s", qUtf8Printable(outcome.detail));
            }
            emit foldersChanged();
        });
}

// The three mutating verbs share everything but which request to make and
// which message to show, so they share a body. `onApplied` runs on this
// thread after the cache has been updated and is where the per-verb
// follow-up (re-selecting a renamed folder, say) goes.
void MailController::runFolderMutation(const QString& failureMessage,
                                        std::function<FolderRepository::FolderMutationFetch(HttpClient&,
                                                                                             const RelayEndpoint&)> work,
                                        std::function<void(const QString& resultingFolder)> onApplied)
{
    const std::optional<RelayRequestPlan> plan = m_folderRepository.planRequest();
    if (!plan.has_value()) {
        setLastError(i18n("Not paired"));
        emit folderMutationCompleted(false);
        return;
    }

    pushBusy();
    m_executor.run(
        this,
        [endpoint = plan->endpoint, work = std::move(work)](HttpClient& http) { return work(http, endpoint); },
        [this, plan = *plan, failureMessage, onApplied = std::move(onApplied)](
            const FolderRepository::FolderMutationFetch& fetched) {
            popBusy();
            const FolderRepository::FolderMutationOutcome outcome =
                m_folderRepository.applyMutation(plan, fetched);
            // The mutation did happen, on the account that is now gone. It is
            // neither a success to celebrate in this account's sidebar nor a
            // failure to blame the user for, so the completion is reported
            // false and no error text is shown.
            if (outcome.outcome == MailRepositoryOutcome::PairingChanged) {
                setLastError(QString());
                emit folderMutationCompleted(false);
                return;
            }
            if (outcome.outcome != MailRepositoryOutcome::Success) {
                setLastError(outcome.detail.isEmpty() ? failureMessage : outcome.detail);
                emit folderMutationCompleted(false);
                return;
            }
            setLastError(QString());
            onApplied(outcome.folder);
            emit foldersChanged();
            emit folderMutationCompleted(true);
        });
}

void MailController::createFolder(const QString& parent, const QString& name)
{
    runFolderMutation(
        i18n("Could not create folder"),
        [parent, name](HttpClient& http, const RelayEndpoint& endpoint) {
            return FolderRepository::createWith(http, endpoint, parent, name);
        },
        [](const QString&) {});
}

void MailController::renameFolder(const QString& folder, const QString& name)
{
    runFolderMutation(
        i18n("Could not rename folder"),
        [folder, name](HttpClient& http, const RelayEndpoint& endpoint) {
            return FolderRepository::renameWith(http, endpoint, folder, name);
        },
        [this, folder](const QString& resultingFolder) {
            // Selecting a folder that no longer exists under its old name
            // would fetch an empty mailbox, so fall back to Inbox when the
            // current selection was the one renamed.
            if (m_currentFolder == folder) {
                selectFolderInternal(resultingFolder.isEmpty()
                                          ? standardFolderWireName(StandardFolder::Inbox)
                                          : resultingFolder);
            }
        });
}

void MailController::deleteFolder(const QString& folder)
{
    runFolderMutation(
        i18n("Could not delete folder"),
        [folder](HttpClient& http, const RelayEndpoint& endpoint) {
            return FolderRepository::removeWith(http, endpoint, folder);
        },
        [this, folder](const QString&) {
            if (m_currentFolder == folder)
                selectFolderInternal(standardFolderWireName(StandardFolder::Inbox));
        });
}

quint64 MailController::saveDraft(const QString& to, const QString& cc, const QString& bcc, const QString& subject,
                                    const QString& body, const QStringList& attachmentFilePaths)
{
    return saveDraftInternal(to, cc, bcc, subject, body, attachmentFilePaths, /*thenOpenWebmail=*/QUrl());
}

// `thenOpenWebmail`, when non-empty, is opened in the user's browser once the
// draft has actually reached the server. openWebmailDrafts() needs that
// ordering and cannot get it any other way now: the save no longer finishes
// before the call returns, and opening a browser onto a draft that is not
// there yet loses the user's message.
quint64 MailController::saveDraftInternal(const QString& to, const QString& cc, const QString& bcc,
                                            const QString& subject, const QString& body,
                                            const QStringList& attachmentFilePaths, const QUrl& thenOpenWebmail)
{
    QUrl serverBaseUrl;
    RelayAuth auth;
    if (!requirePairing(serverBaseUrl, auth))
        return 0;

    QVector<MailAttachmentUpload> attachments;
    if (!readAttachments(attachmentFilePaths, attachments))
        return 0;

    const quint64 token = m_nextPendingSendToken++;

    pushBusy();
    m_executor.run(
        this,
        [serverBaseUrl, auth, to, cc, bcc, subject, body, attachments](HttpClient& http) {
            RelayMailSource source(http);
            return source.saveDraft(serverBaseUrl, auth, to, cc, bcc, subject, body, QStringLiteral("html"),
                                     attachments);
        },
        [this, token, thenOpenWebmail](const SaveDraftResult& result) {
            popBusy();
            if (result.error.has_value() || !result.ok) {
                setLastError(result.detail.isEmpty() ? i18n("Could not save draft") : result.detail);
                emit draftSaveCompleted(token, false);
                return;
            }
            setLastError(QString());
            if (!thenOpenWebmail.isEmpty() && !QDesktopServices::openUrl(thenOpenWebmail)) {
                setLastError(i18n("Saved to Drafts, but KyPost could not open your browser."));
                emit draftSaveCompleted(token, false);
                return;
            }
            emit draftSaveCompleted(token, true);
        });
    return token;
}

quint64 MailController::sendMail(const QString& to, const QString& cc, const QString& bcc, const QString& subject,
                                   const QString& body, const QStringList& attachmentFilePaths, bool sign,
                                   bool encrypt)
{
    // FIRST statement, before any early return below: PendingSend's own doc
    // comment promises the cached plaintext dies when a fresh send starts,
    // and the guards below (not paired, unreadable/oversized attachment) used
    // to return without honoring that, leaving a previous refusal's payload
    // alive past the composition that made it.
    m_pendingSend = {};

    QUrl serverBaseUrl;
    RelayAuth auth;
    if (!requirePairing(serverBaseUrl, auth))
        return 0;

    QVector<MailAttachmentUpload> attachments;
    if (!readAttachments(attachmentFilePaths, attachments))
        return 0;

    const QString sendMode = QStringLiteral("html");
    const quint64 sendToken = m_nextPendingSendToken++;

    // Field order matters -- PendingSend is aggregate-initialized. Eleven
    // fields: valid, to, cc, bcc, subject, body, mode, attachments, sign,
    // encrypt, token.
    m_pendingSend = PendingSend{ true, to, cc, bcc, subject, body, sendMode, attachments, sign, encrypt, sendToken };

    pushBusy();
    m_executor.run(
        this,
        [serverBaseUrl, auth, to, cc, bcc, subject, body, sendMode, attachments, sign,
         encrypt](HttpClient& http) {
            RelayMailSource source(http);
            return source.sendMail(serverBaseUrl, auth, to, cc, bcc, subject, body, sendMode, attachments, sign,
                                    encrypt, /*allowPickupFallback=*/false);
        },
        [this, sendToken](const SendMailResult& result) { finishSend(sendToken, result); });
    return sendToken;
}

void MailController::finishSend(quint64 sendToken, const SendMailResult& result)
{
    popBusy();

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
        emit pickupFallbackRequired(sendToken, result.keylessRecipients);
        emit sendCompleted(sendToken, false);
        return;
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
        emit sendCompleted(sendToken, false);
        return;
    }

    // Only clear the pending send if it is still THIS send's. A second
    // composer that started its own send while this reply was in flight has
    // already replaced m_pendingSend, and clearing it here would throw away
    // that composition's payload -- so its user's confirmation would find
    // nothing to re-send.
    if (m_pendingSend.token == sendToken)
        m_pendingSend = {};

    if (!result.ok) {
        // Same shape as every other failure in this class: prefer the
        // server's own detail, fall back to localized wording.
        setLastError(result.detail.isEmpty() ? i18n("Could not send message") : result.detail);
        emit sendCompleted(sendToken, false);
        return;
    }
    if (!result.warning.isEmpty())
        emit sendWarning(result.warning);
    setLastError(QString());
    emit sendCompleted(sendToken, true);
}

// Re-sends the exact payload the server refused, with allowPickupFallback
// set. Byte-identical by construction: nothing is rebuilt, no file is
// re-read, and the preflight is not re-run. Returns false without sending
// when there is no pending send, or when `token` names a different one, so
// neither a stray confirm nor a confirmation collected in some other
// composer's dialog can mail anything.
quint64 MailController::confirmPickupFallbackSend(quint64 token)
{
    if (!m_pendingSend.valid || m_pendingSend.token != token)
        return 0;

    QUrl serverBaseUrl;
    RelayAuth auth;
    if (!requirePairing(serverBaseUrl, auth))
        return 0;

    const PendingSend pending = m_pendingSend;
    // Cleared before the call, not after: the opt-in is per-message, and a
    // failure here must not leave a payload a second confirm could re-send.
    m_pendingSend = {};

    // Re-uses the SAME token rather than minting a fresh one. The confirmed
    // re-send is the same logical send the composer started, so the composer
    // that is waiting on `token` is the one that must hear about it.
    const quint64 sendToken = token;

    // Bracketed the same way sendMail()/saveDraft() are. Without it the
    // "Sending…" indicator never appears and Send stays enabled for the whole
    // 30s HttpClient timeout, so a user who sees nothing happen presses Send
    // again -- which takes another 409, another confirmation, and delivers
    // the message twice.
    pushBusy();
    m_executor.run(
        this,
        [serverBaseUrl, auth, pending](HttpClient& http) {
            RelayMailSource source(http);
            return source.sendMail(serverBaseUrl, auth, pending.to, pending.cc, pending.bcc, pending.subject,
                                    pending.body, pending.mode, pending.attachments, pending.sign,
                                    pending.encrypt, /*allowPickupFallback=*/true);
        },
        [this, sendToken](const SendMailResult& result) {
            popBusy();
            if (!result.ok) {
                // Same detail-or-localized-fallback shape as every other
                // failure in this class: a 200 carrying {"ok":false} leaves
                // detail empty, and reporting that verbatim would fail
                // silently.
                setLastError(result.detail.isEmpty() ? i18n("Could not send message") : result.detail);
                emit sendCompleted(sendToken, false);
                return;
            }
            if (!result.warning.isEmpty())
                emit sendWarning(result.warning);
            // The one send that most needs to read as success: clear any
            // error left over from the refusal that opened the confirmation.
            setLastError(QString());
            emit sendCompleted(sendToken, true);
        });
    return sendToken;
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
    // Unconditional, ahead of the cache check below: pgpKeylessRecipients is
    // singleton state belonging to whatever was composed last, so a fresh
    // composer that ticks Encrypt would otherwise flash the PREVIOUS
    // message's keyless addresses until this session's debounce lands.
    if (!m_pgpKeylessRecipients.isEmpty()) {
        m_pgpKeylessRecipients.clear();
        emit pgpKeylessRecipientsChanged();
    }

    // At most one bootstrap fetch per session. Custody mode is fixed at key
    // creation and cannot change within a session, so re-asking on every
    // compose open buys nothing. The original reason for the cache was
    // sharper -- this was a synchronous call from Compose.qml's
    // Component.onCompleted, i.e. a nested event loop while the object tree
    // was still being built -- and that reason is gone; the cache is kept
    // for the remaining one.
    if (m_pgpComposeStateFetched && !force)
        return;
    // Coalescing: Compose can be opened twice before the first answer lands.
    if (m_pgpBootstrapInFlight)
        return;

    QUrl serverBaseUrl;
    RelayAuth auth;
    if (!requirePairing(serverBaseUrl, auth))
        return; // not cached: pairing later must still get a real answer

    const PairingIdentity requestedBy = m_pairingStore.currentIdentity();

    m_pgpBootstrapInFlight = true;
    m_executor.run(
        this,
        [serverBaseUrl, auth](HttpClient& http) {
            PgpBootstrapClient client(http);
            return client.fetch(serverBaseUrl, auth);
        },
        [this, requestedBy](const PgpBootstrapResult& bootstrap) {
            m_pgpBootstrapInFlight = false;
            // This answer describes the PREVIOUS account's key custody, and
            // it decides whether compose offers to sign and encrypt at all.
            // Applying it to the account paired now would either hide those
            // controls from someone entitled to them or offer them to someone
            // whose server has no identity to use. Left unfetched instead, so
            // the next compose open asks again.
            if (!m_pairingStore.stillCurrent(requestedBy))
                return;
            const PgpComposeState state = bootstrap.ok
                ? pgpComposeStateOf(bootstrap.hasIdentity, bootstrap.protection)
                : pgpComposeStateOf(std::nullopt, std::nullopt);

            // Only a real answer is cached. "Couldn't check" is not a custody
            // mode, so caching a failure would hide the PGP controls for the
            // rest of the session over one transient 503; the next compose
            // open retries instead.
            m_pgpComposeStateFetched = bootstrap.ok;
            m_pgpCanEncrypt = state.canEncrypt;
            m_pgpCanSign = state.canSign;
            m_pgpHandoffToWebmail = state.handoffToWebmail;
            emit pgpComposeStateChanged();
        });
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
    // Still dropped while one is in flight, but for a plainer reason than
    // before. This used to be a re-entrancy defence: check() ran a nested
    // QEventLoop that kept firing Compose.qml's 500ms debounce, so typing
    // landed back here one frame deeper per half-second, and the results
    // unwound LIFO -- the LAST write to m_pgpKeylessRecipients was the OLDEST
    // request's answer, an inline warning naming an address already removed.
    // Nothing re-enters now, so this is ordinary coalescing; dropping is
    // still right because this is a debounced advisory lower bound, not a
    // gate, and the next keystroke restarts the timer anyway.
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

    if (addresses.isEmpty()) {
        applyKeylessRecipients({});
        return;
    }

    const PairingIdentity requestedBy = m_pairingStore.currentIdentity();

    m_preflightInFlight = true;
    m_executor.run(
        this,
        [serverBaseUrl, auth, addresses](HttpClient& http) {
            PgpRecipientChecker checker(http);
            return checker.check(serverBaseUrl, auth, addresses);
        },
        [this, requestedBy](const RecipientKeyCheckResult& result) {
            m_preflightInFlight = false;
            // Which recipients the PREVIOUS account had keys for says nothing
            // about this one. Clear rather than keep: a stale all-clear is
            // the one outcome this warning must never show.
            if (!m_pairingStore.stillCurrent(requestedBy)) {
                applyKeylessRecipients(QStringList());
                return;
            }
            // A failed preflight shows nothing rather than a false all-clear
            // -- and clears whatever it showed before, rather than leaving a
            // stale list standing in for an answer this call did not get.
            applyKeylessRecipients(result.ok ? result.keylessRecipients : QStringList());
        });
}

void MailController::applyKeylessRecipients(const QStringList& keyless)
{
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
quint64 MailController::openWebmailDrafts(const QString& to, const QString& cc, const QString& bcc,
                                            const QString& subject, const QString& body,
                                            const QStringList& attachmentFilePaths)
{
    // Pairing is checked first so the two distinct failures read distinctly:
    // webmailMailboxUrl() returns an empty URL for an unpaired client just as
    // it does for a plain-http one, and reporting "paired over an insecure
    // connection" to someone who is not paired at all is simply wrong.
    QUrl serverBaseUrl;
    RelayAuth auth;
    if (!requirePairing(serverBaseUrl, auth)) // sets "Not paired"
        return 0;

    const QUrl url = webmailMailboxUrl(webmailBaseUrl(), QStringLiteral("Drafts"));
    // Checked BEFORE saving: a draft saved for a handoff that cannot open
    // leaves the user with a silently duplicated draft and no browser.
    if (url.isEmpty()) {
        setLastError(i18n("This device is paired over an insecure connection, so KyPost cannot open "
                           "webmail for you. Open your mail in a browser to send this message."));
        return 0;
    }
    // The browser is opened by the save's completion handler, not here: the
    // save no longer finishes before this returns, and opening a browser onto
    // a draft that has not landed yet loses the user's message.
    return saveDraftInternal(to, cc, bcc, subject, body, attachmentFilePaths, url);
}

void MailController::listAttachments(const QString& mailbox, const QString& messageId)
{
    QUrl serverBaseUrl;
    RelayAuth auth;
    if (!requirePairing(serverBaseUrl, auth)) {
        emit attachmentsListed(mailbox, messageId, {});
        return;
    }

    pushBusy();
    m_executor.run(
        this,
        [serverBaseUrl, auth, mailbox, messageId](HttpClient& http) {
            RelayMailSource source(http);
            return source.listAttachments(serverBaseUrl, auth, mailbox, messageId);
        },
        [this, mailbox, messageId](const ListAttachmentsResult& result) {
            popBusy();
            if (result.error.has_value()) {
                setLastError(result.detail.isEmpty() ? i18n("Could not list attachments") : result.detail);
                emit attachmentsListed(mailbox, messageId, {});
                return;
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
            emit attachmentsListed(mailbox, messageId, list);
        });
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

void MailController::downloadAttachment(const QString& mailbox, const QString& messageId, int index,
                                         const QString& suggestedName)
{
    QUrl serverBaseUrl;
    RelayAuth auth;
    if (!requirePairing(serverBaseUrl, auth)) {
        emit attachmentDownloaded(messageId, index, false);
        return;
    }

    // Captured at dispatch: which account this attachment belongs to.
    const PairingIdentity requestedBy = m_pairingStore.currentIdentity();

    pushBusy();
    m_executor.run(
        this,
        [serverBaseUrl, auth, mailbox, messageId, index](HttpClient& http) {
            RelayMailSource source(http);
            return source.downloadAttachment(serverBaseUrl, auth, mailbox, messageId, index);
        },
        [this, requestedBy, messageId, index, suggestedName](const DownloadAttachmentResult& result) {
            popBusy();
            // Nothing is written, and nothing is opened. The ephemeral branch
            // of storeDownloadedAttachment() hands the file straight to the
            // desktop, so proceeding here would not merely leave the previous
            // account's attachment on disk -- it would open it in front of
            // whoever is using the device now.
            if (!m_pairingStore.stillCurrent(requestedBy)) {
                emit attachmentDownloaded(messageId, index, false);
                return;
            }
            // Everything below -- the filename hardening, the exclusive
            // create, the ephemeral open -- stays on THIS thread. Only the
            // fetch moved. QStandardPaths, the 5-minute delete QTimer and
            // m_ephemeralAttachments all belong here, and the security
            // properties of that code are hard-won; running it somewhere
            // else would be a rewrite, not a move.
            emit attachmentDownloaded(messageId, index, storeDownloadedAttachment(suggestedName, result));
        });
}

// The half of downloadAttachment() that must not leave this thread: turns a
// fetched attachment into either a file in Downloads or an ephemeral open.
bool MailController::storeDownloadedAttachment(const QString& suggestedName,
                                                 const DownloadAttachmentResult& result)
{
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
    //
    // C0 controls are stripped FIRST, and NUL is the one that matters:
    // QFile::open() passes the encoded path to open(2), which truncates at a
    // NUL, while QFile::exists()/QFileInfo::exists()/QFile::remove() all
    // reject the same string. A sender-chosen "Invoice.desktop\0.pdf"
    // therefore satisfied the ephemeral path's MIME-driven extension forcing
    // -- which computes ".pdf" from what it believes is the whole name --
    // while creating "Invoice.desktop" on disk, handing the desktop's
    // handler choice back to the sender. The same split silently no-ops the
    // 5-minute delete timer, the exit cleanup and the write rollback, so the
    // file outlived the session in the one mode that promises it cannot.
    name.removeIf([](QChar c) { return c.unicode() < 0x20 || c.unicode() == 0x7f; });
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

    // Fail closed on a PREDICTABLE path. Another local user can create
    // /tmp/kypost-attachments first: mkpath() then succeeds because it
    // exists, the chmod fails because it is not ours, and both results used
    // to be dropped -- so the mail went in anyway.
    const QString dir = baseDir + QStringLiteral("/kypost-attachments");
    // Symlink first: a link sends the bytes wherever it points, and the
    // permission check below would only ever see the target's mode.
    if (QFileInfo(dir).isSymLink()) {
        setLastError(i18n("KyPost will not write this attachment: the temporary location (%1) is a link "
                           "to somewhere else.",
                           dir));
        return false;
    }
    if (PrivatePath::ensureDirectory(dir) != PrivatePath::Status::Ready) {
        setLastError(i18n("KyPost will not write this attachment: the temporary location (%1) cannot be "
                           "made private to you.",
                           dir));
        return false;
    }

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
    // briefly readable by other local users -- and proven on the file that is
    // now on the disk, because chmod reports success and changes nothing on a
    // filesystem with no permission bits.
    if (!outFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || !PrivatePath::isPrivate(path)) {
        outFile.close();
        QFile::remove(path);
        setLastError(i18n("KyPost will not write this attachment: it cannot be kept unreadable to other "
                           "users of this machine."));
        return false;
    }
    if (!writeAllOrRemove(outFile, data)) {
        setLastError(i18n("The attachment could not be written in full (is the disk full?)."));
        return false;
    }

    // Tracked only once there is something to hand over. A launch that never
    // happened leaves no file for a viewer to read, so it leaves none here.
    if (!m_launchAttachment(QUrl::fromLocalFile(path))) {
        QFile::remove(path);
        setLastError(i18n("KyPost could not open the attachment with any installed application."));
        return false;
    }
    m_ephemeralAttachments.append(path);

    // There is no reliable signal for "the external viewer closed" -- the
    // handler may be a long-running process that was already open. A timer
    // is the honest fallback: long enough for the viewer to read the file,
    // short enough that it does not linger. It is also deleted on exit (see
    // clearEphemeralAttachments) so a crash-free quit leaves nothing.
    QTimer::singleShot(kEphemeralAttachmentLifetimeMs, this, [this, path]() {
        // Forgetting a file we failed to delete is how "ephemeral" turns into
        // "on disk until the disk is wiped": it stays on the list so quit
        // retries it.
        if (QFile::remove(path) || !QFile::exists(path))
            m_ephemeralAttachments.removeAll(path);
        else
            qWarning("MailController: could not remove the temporary attachment; retrying on quit");
    });

    setLastError(QString());
    return true;
}

void MailController::setAttachmentLauncher(AttachmentLauncher launcher)
{
    if (launcher)
        m_launchAttachment = std::move(launcher);
}

void MailController::clearEphemeralAttachments()
{
    QStringList stillThere;
    for (const QString& path : m_ephemeralAttachments) {
        if (!QFile::remove(path) && QFile::exists(path)) {
            qCritical("MailController: a temporary attachment could not be deleted and is still on disk "
                       "at %s",
                       qUtf8Printable(path));
            stillThere.append(path);
        }
    }
    m_ephemeralAttachments = stillThere;
}

void MailController::openFromNotification(const QString& messageId)
{
    if (messageId.isEmpty())
        return;

    // Already here: answer without a round trip.
    if (const std::optional<Email> cached = m_mailRepository.findCachedEmail(messageId)) {
        emit notificationEmailResolved(messageId, cached->folder);
        return;
    }

    // Cached in several mailboxes. findCachedEmail refuses to pick one, and it
    // is right to -- opening the Archive copy of a message the notification
    // announced in INBOX is a wrong-message bug. But refusing is not an
    // answer on its own: this used to fall through to a refresh that cannot
    // disambiguate anything and then to a blank page.
    //
    // Asked BEFORE the refresh, and no request is made: a delta cannot remove
    // an ambiguity that is already in the cache, so spending a round trip to
    // arrive at the same question is just a slower way to ask it.
    if (const QStringList folders = m_mailRepository.foldersHolding(messageId); folders.size() > 1) {
        setLastError(QString());
        emit notificationEmailAmbiguous(messageId, folders);
        return;
    }

    m_pendingNotificationMessageId = messageId;

    // A delta refresh, not a forced full resync. The relay's cursor protocol
    // returns everything after the stored cursor, and a message that has just
    // arrived is by definition after it -- so a delta finds it, while a full
    // resync would re-download the entire window including every body to
    // learn the same thing.
    //
    // The Inbox specifically: the notification carries no mailbox (see
    // PushPayloadParser.h), and new mail arrives there. A message filed
    // somewhere else by a server-side rule will not be found, and that is
    // reported rather than papered over.
    const QString inbox = standardFolderWireName(StandardFolder::Inbox);
    if (m_currentFolder == inbox)
        refreshInternal(false);
    else
        selectFolderInternal(inbox);
}

QVariantMap MailController::findByMessageId(const QString& messageId) const
{
    // An empty map now means one of TWO things: the id isn't cached, or it is
    // cached under more than one folder and there is no way to tell which
    // copy the caller meant. The second case is not hypothetical -- the
    // UnifiedPush envelope carries no mailbox at all (see
    // PushPayloadParser.h's verified wire shape), so a message sitting in
    // both INBOX and Archive has no tie-breaker. Returning either copy would
    // silently open the wrong one, so MailRepository::findCachedEmail
    // refuses; see its header.
    //
    // Resolved 2026-08-23. An ambiguous id is no longer an empty result:
    // openFromNotification() asks MailRepository::foldersHolding() and emits
    // notificationEmailAmbiguous(), which both roots answer with
    // NotificationFolderDialog. This function still returns an empty map for
    // both cases, and that is fine -- its callers already know the folder.
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
    // Empty string when the server did not say. QML's emailBodyIsHtml() reads
    // "" as "fall back to the sniff", which is the same answer the absent
    // optional means -- see Format.emailBodyIsHtml.
    map[QStringLiteral("bodyMode")] = email->bodyMode.value_or(QString());
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
    // Whether to offer decryption on this row at all. Webmail stays offered
    // either way: a key kept on another machine is a perfectly ordinary
    // setup, and this button cannot help there.
    map[QStringLiteral("canDecryptHere")] =
        pgpState == PgpMessageState::ClientProtected && openPgpEngineAvailable();
    return map;
}

bool MailController::pgpCanSendFromThisDevice() const
{
    // Both halves matter. A client-custody account on a machine with no gpg
    // still needs the webmail hand-off, and offering a Send button that
    // cannot work would be worse than not offering one.
    return m_pgpHandoffToWebmail && openPgpEngineAvailable();
}

// Everything a client-encrypted send does, on the executor thread.
//
// Split out as a free function because it must touch nothing owned by the GUI
// thread: no DAO, no PairingStore, no member of this controller. The endpoint
// and the composed message are copied in; one value comes back.
namespace {

MailController::ClientEncryptedOutcome runClientEncryptedSend(
    HttpClient& http, const RelayEndpoint& endpoint, const QString& to, const QString& cc,
    const QString& bcc, const QString& subject, const QString& body, const QString& date,
    const QVector<MailAttachmentUpload>& attachments)
{
    using Failure = MailController::ClientEncryptedOutcome::Failure;
    MailController::ClientEncryptedOutcome outcome;

    const auto split = [](const QString& list) {
        QStringList out;
        for (const QString& piece : list.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString trimmed = piece.trimmed();
            if (!trimmed.isEmpty())
                out.append(trimmed);
        }
        return out;
    };
    const QStringList toList = split(to);
    const QStringList ccList = split(cc);
    const QStringList bccList = split(bcc);

    // The account's own address, which nothing else in this app knows: the
    // pairing carries a subscriber id, not a mailbox. Fetched per send rather
    // than cached, because the relay binds every delivery's From to it and a
    // stale value fails the whole send at the last step.
    const PgpBootstrapClient bootstrap(http);
    const PgpBootstrapResult identity = bootstrap.fetch(endpoint.serverBaseUrl, endpoint.auth);
    if (!identity.ok || identity.primaryAddress.isEmpty()) {
        outcome.failure = Failure::NotConfigured;
        outcome.detail = identity.detail;
        return outcome;
    }

    QStringList everyone = toList;
    everyone.append(ccList);
    everyone.append(bccList);
    if (everyone.isEmpty()) {
        outcome.failure = Failure::RecipientWithoutKey;
        return outcome;
    }

    const PgpResolveClient resolver(http);
    const PgpResolveResult resolved = resolver.resolve(endpoint.serverBaseUrl, endpoint.auth, everyone);
    if (resolved.status == PgpResolveStatus::ServerEncryptsInstead) {
        outcome.failure = Failure::ServerEncryptsInstead;
        return outcome;
    }
    if (resolved.status != PgpResolveStatus::Resolved) {
        outcome.failure = Failure::SendFailed;
        outcome.detail = resolved.detail;
        return outcome;
    }

    // Unusable is unusable. There is no downgrade here on purpose: the
    // relay's own plaintext fallback works by storing the message on the
    // relay, which is the thing this mode exists to prevent.
    QHash<QString, QString> fingerprints;
    for (const ResolvedRecipientKey& key : resolved.keys) {
        if (!key.usable || key.publicKey.isEmpty()) {
            outcome.namedRecipients.append(key.address);
            continue;
        }
        // Into the user's own keyring, so gpg owns the record -- AGENTS.md 4b.
        const PgpImportResult imported =
            importPublicKey(key.publicKey.toUtf8(), key.fingerprint, QString());
        if (imported.status != PgpImportStatus::Imported
            && imported.status != PgpImportStatus::Unchanged) {
            // A key whose fingerprint disagrees with what the relay claimed
            // about it is not one to encrypt to.
            //
            // WHEN IT CLAIMED ONE. `fingerprint` is omitempty on the wire and
            // a pinned contact key can reach us without it, so this comparison
            // does not always happen and requiring it would refuse sends that
            // are perfectly possible. It was never a defence against a hostile
            // relay in any case -- one that wants to substitute a key sends a
            // matching fingerprint with it, or none at all. What it catches is
            // a relay whose key and whose claim disagree, which is a bug or a
            // partial compromise rather than a determined one.
            outcome.namedRecipients.append(key.address);
            outcome.detail = imported.detail;
            continue;
        }
        fingerprints.insert(key.address, imported.fingerprint);
    }
    if (!outcome.namedRecipients.isEmpty()) {
        outcome.failure = Failure::RecipientWithoutKey;
        return outcome;
    }

    OutgoingMessage message;
    message.from = identity.primaryAddress;
    message.to = toList;
    message.cc = ccList;
    message.subject = subject;
    message.body = body;
    message.mode = QStringLiteral("plain");
    message.date = date;
    // Inside the encrypted part, so the filenames are encrypted too.
    message.attachments = attachments;

    const PgpSendPlan plan = buildPgpSendPlan(message, bccList, fingerprints,
                                               ownKeyFingerprint(identity.primaryAddress));
    switch (plan.status) {
    case PgpSendPlanStatus::Built:
        break;
    case PgpSendPlanStatus::RecipientWithoutKey:
        outcome.failure = Failure::RecipientWithoutKey;
        outcome.namedRecipients = plan.recipientsWithoutKeys;
        return outcome;
    case PgpSendPlanStatus::SigningUnavailable:
        outcome.failure = Failure::NoSigningKey;
        return outcome;
    case PgpSendPlanStatus::EngineUnavailable:
        outcome.failure = Failure::EngineUnavailable;
        return outcome;
    case PgpSendPlanStatus::EncryptionFailed:
        outcome.failure = Failure::EncryptionFailed;
        outcome.detail = plan.detail;
        return outcome;
    }

    const PgpSendClient sender(http);
    const PgpSendResult sent = sender.send(endpoint.serverBaseUrl, endpoint.auth,
                                            identity.primaryAddress, plan, toList, ccList, bccList,
                                            QStringLiteral("plain"));
    if (!sent.ok) {
        outcome.failure = sent.tooLarge ? Failure::TooLarge : Failure::SendFailed;
        outcome.detail = sent.detail;
        return outcome;
    }

    outcome.ok = true;
    // Either the planner never built a copy, or the relay did not file it.
    outcome.missingSentCopy = plan.sentCopyUnavailable || !sent.sentSaved;
    return outcome;
}

} // namespace

quint64 MailController::sendClientEncrypted(const QString& to, const QString& cc, const QString& bcc,
                                             const QString& subject, const QString& body,
                                             const QStringList& attachmentFilePaths)
{
    m_pendingSend = {};
    m_lastSendMissingSentCopy = false;

    QVector<MailAttachmentUpload> attachments;
    if (!readAttachments(attachmentFilePaths, attachments))
        return 0; // readAttachments has already said what went wrong

    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value()) {
        setLastError(i18n("Not paired"));
        return 0;
    }

    const RelayEndpoint endpoint{ QUrl(pairing->serverBaseUrl),
                                   RelayAuth{ pairing->deviceId, pairing->deviceSecret } };
    const PairingIdentity identity = identityOf(*pairing);
    const quint64 sendToken = m_nextPendingSendToken++;
    // Stamped here, on the thread that knows when the user pressed Send --
    // not inside the worker, where pinentry may have been open for minutes.
    const QString date = QDateTime::currentDateTime().toString(Qt::RFC2822Date);

    pushBusy();
    m_executor.run(
        this,
        [endpoint, to, cc, bcc, subject, body, date, attachments](HttpClient& http) {
            return runClientEncryptedSend(http, endpoint, to, cc, bcc, subject, body, date,
                                           attachments);
        },
        [this, sendToken, identity](const ClientEncryptedOutcome& outcome) {
            popBusy();
            // The account may have been replaced while pinentry was open.
            // Reporting this send's outcome into the new account's compose
            // screen would be the stale-reply defect again, on a screen where
            // it reads as "your message was sent".
            if (!m_pairingStore.stillCurrent(identity)) {
                setLastError(QString());
                emit sendCompleted(sendToken, false);
                return;
            }
            finishClientEncryptedSend(sendToken, outcome);
        });
    return sendToken;
}

void MailController::finishClientEncryptedSend(quint64 token, const ClientEncryptedOutcome& outcome)
{
    using Failure = ClientEncryptedOutcome::Failure;

    if (outcome.ok) {
        m_lastSendMissingSentCopy = outcome.missingSentCopy;
        setLastError(QString());
        emit pgpComposeStateChanged();
        emit sendCompleted(token, true);
        return;
    }

    switch (outcome.failure) {
    case Failure::None:
    case Failure::SendFailed:
        setLastError(i18n("The message could not be sent. Try again."));
        break;
    case Failure::NotConfigured:
        setLastError(i18n("This account has no mail configuration, so there is no address to send "
                           "from."));
        break;
    case Failure::RecipientWithoutKey:
        // Named, so the user can act. Never a downgrade offer: sending this
        // message unencrypted is not something to suggest on an account whose
        // key the server deliberately does not hold.
        setLastError(outcome.namedRecipients.isEmpty()
                          ? i18n("A recipient has no usable encryption key, so this message was not "
                                  "sent.")
                          : i18n("No usable encryption key for %1, so this message was not sent.",
                                  outcome.namedRecipients.join(QStringLiteral(", "))));
        break;
    case Failure::NoSigningKey:
        setLastError(i18n("Your own OpenPGP key is not available, so this message could not be "
                           "signed. It was not sent unsigned."));
        break;
    case Failure::Cancelled:
        setLastError(i18n("Unlocking your key was cancelled, so the message was not sent."));
        break;
    case Failure::KeyImportRefused:
        setLastError(i18n("A recipient's key did not match what the server said about it, so this "
                           "message was not sent."));
        break;
    case Failure::EncryptionFailed:
        setLastError(i18n("The message could not be encrypted, so it was not sent."));
        break;
    case Failure::EngineUnavailable:
        setLastError(i18n("GnuPG is not available on this computer, so this message cannot be "
                           "encrypted here."));
        break;
    case Failure::TooLarge:
        // Names the real constraint rather than "too big": this request
        // carries one copy of the message per recipient who gets their own
        // ciphertext, so the number of recipients matters as much as the
        // attachment does, and only saying "smaller attachment" would send
        // somebody trimming the wrong thing.
        setLastError(i18n("This message is too large to send encrypted. Each blind-copied "
                           "recipient receives their own encrypted copy, so removing an "
                           "attachment or some recipients will bring it under the limit."));
        break;
    case Failure::ServerEncryptsInstead:
        setLastError(i18n("This account's key is held by the server, which encrypts on its own. "
                           "Send it the usual way."));
        break;
    }
    emit sendCompleted(token, false);
}

bool MailController::decryptedStillOurs() const
{
    if (m_decryptedMessageId.isEmpty())
        return false;
    return m_pairingStore.stillCurrent(m_decryptedIdentity);
}

void MailController::forgetDecrypted()
{
    // Reads the MEMBERS, not the getters: the getters answer empty once the
    // account has been replaced, which is exactly when there is most to clear.
    if (m_decryptedMessageId.isEmpty() && m_decryptedHtml.isEmpty() && m_decryptedPlain.isEmpty()
        && m_decryptFailure.isEmpty() && m_decryptedSignature.isEmpty()) {
        return;
    }
    m_decryptedIdentity = {};
    m_decryptedMessageId.clear();
    m_decryptedHtml.clear();
    m_decryptedPlain.clear();
    m_decryptFailure.clear();
    m_decryptedSignature.clear();
    m_decryptedSignatureIsWarning = false;
    m_decryptRetryable = false;
    emit decryptedChanged();
}

void MailController::decryptMessage(const QString& messageId)
{
    if (messageId.isEmpty() || m_decryptInFlight)
        return;

    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value()) {
        setLastError(i18n("Not paired"));
        return;
    }

    // The mailbox comes from the CACHED row, not from the caller. The
    // endpoint takes a mailbox and a UID, and letting a view pass both would
    // make "which mailbox" a QML-side decision about someone else's mail.
    const std::optional<Email> email = m_mailRepository.findCachedEmail(messageId);
    if (!email.has_value())
        return;

    // Anything already held belongs to a different attempt. Dropped before
    // the new one starts, so a failure cannot leave the previous message's
    // plaintext on screen under the new one's headers.
    forgetDecrypted();

    const RelayEndpoint endpoint{ QUrl(pairing->serverBaseUrl),
                                   RelayAuth{ pairing->deviceId, pairing->deviceSecret } };
    const PairingIdentity identity = identityOf(*pairing);

    m_decryptInFlight = true;
    emit decryptedChanged();

    m_executor.run(
        this,
        [endpoint, mailbox = email->folder, messageId](HttpClient& http) {
            // Constructed here, on the executor thread, for the same reason
            // FolderRepository::listWith constructs its client there: the
            // HttpClient belongs to that thread and these are stateless
            // wrappers over it.
            const PgpPayloadClient payloads(http);
            const OpenPgpDecryptor decryptor;
            const EncryptedMessageReader reader(payloads, decryptor);
            return reader.read(endpoint.serverBaseUrl, endpoint.auth, mailbox, messageId);
        },
        [this, identity, messageId](const PgpReadResult& result) {
            applyDecryptResult(identity, messageId, result);
        });
}

void MailController::applyDecryptResult(const PairingIdentity& identity, const QString& messageId,
                                         const PgpReadResult& result)
{
    m_decryptInFlight = false;

    // The account may have been replaced while pinentry was open -- which is
    // an unbounded wait, so this window is wider here than anywhere else in
    // the app. Showing the previous account's decrypted mail in the new
    // account's reader is the stale-reply defect this repo has now fixed at
    // eleven sites; nothing is displayed and nothing is kept.
    if (!m_pairingStore.stillCurrent(identity)) {
        forgetDecrypted();
        emit decryptedChanged();
        return;
    }

    if (result.status != PgpReadStatus::Decrypted) {
        m_decryptFailure = pgpReadFailureMessage(result.status);
        m_decryptRetryable = pgpReadIsRetryable(result.status);
        emit decryptedChanged();
        return;
    }

    const MimeBody body = readMimeBody(result.plaintext);
    if (body.isEmpty()) {
        // It decrypted, and there is no text in it -- an entity whose only
        // parts are attachments, or one shaped in a way this parser will not
        // guess at. Its own sentence rather than a decryption failure,
        // because the decryption did work and saying otherwise would send
        // the user to check their key.
        m_decryptFailure = i18n("This message decrypted, but contains no readable text.");
        m_decryptRetryable = false;
        emit decryptedChanged();
        return;
    }

    m_decryptedIdentity = identity;
    m_decryptedMessageId = messageId;
    m_decryptedHtml = body.html;
    m_decryptedPlain = body.plain;
    // Against the RESOLVED sender, never the display form -- which this app
    // does not parse at all, precisely so it cannot end up here.
    m_decryptedSignature = pgpSignatureLabel(result.signature, result.signedBy);
    m_decryptedSignatureIsWarning = pgpSignatureIsWarning(result.signature);
    emit decryptedChanged();
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

void MailController::setKeywordOrder(const QStringList& keywords)
{
    m_keywordRepository.setOrder(keywords);
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
