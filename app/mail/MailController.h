#pragma once

#include "domain/FolderRepository.h" // FolderMutationFetch -- carried across the thread hop below
#include "domain/MailRepository.h"   // MailRefreshPlan -- likewise
#include "mail/EmailListModel.h"
#include "models/Email.h"
#include "net/RelayMailSource.h" // MailAttachmentUpload -- held by value in PendingSend below
#include "pgp/EncryptedMessageReader.h" // PgpReadResult/PgpReadStatus -- carried across the hop

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <functional>
#include <optional>

class QFile;

class SettingsStore;
class RelayMailSource;
class KeywordRepository;
class NetworkExecutor;
class PairingStore;
class PgpBootstrapClient;
class PgpRecipientChecker;
class QUrl;

// QML-facing bridge (Task 32) over the core/domain mail stack: MailRepository
// (cache + delta-merge), RelayMailSource (direct relay calls for actions/
// send/attachments -- MailRepository itself only wraps fetchInbox), and
// KeywordRepository (per-folder keyword tab derivation). Registered as the
// "MailApp" QML singleton in main.cpp.
//
// THREADING: every network-reaching method here dispatches to the
// NetworkExecutor thread and returns immediately. None of them blocks the GUI
// thread, and none of them can be re-entered, so this class no longer needs
// ReentrancyGuard. The flags that remain (m_refreshInFlight,
// m_preflightInFlight, m_pgpBootstrapInFlight) are coalescing, not guards --
// they stop a second request piling up behind one already out.
//
// Methods that used to return a result now return either nothing or a token,
// and report through a signal. See docs/THREADING.md.
//
// Task 39: allKeywordSettings()/setKeywordVisible() (Settings > Keywords
// pane) are folded in here rather than a new KeywordSettingsController --
// this class already holds the keywordRepository reference and a cached
// email set to derive the keyword universe from, so a second controller
// would just be a thin pass-through with no state of its own.
class MailController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject* emailModel READ emailModel CONSTANT)
    Q_PROPERTY(QString currentFolder READ currentFolder NOTIFY currentFolderChanged)
    Q_PROPERTY(QString selectedKeyword READ selectedKeyword NOTIFY selectedKeywordChanged)
    Q_PROPERTY(QVariantList keywordTabs READ keywordTabs NOTIFY keywordTabsChanged)
    Q_PROPERTY(bool isBusy READ isBusy NOTIFY isBusyChanged)

    // --- Client-side OpenPGP decryption ---------------------------------
    //
    // The decrypted message lives in these members and NOWHERE ELSE. It is
    // never written to the cache and never reaches disk: the sender chose
    // end-to-end encryption, and the database key sits in the platform
    // secret store rather than behind their OpenPGP passphrase, so caching
    // the plaintext would quietly demote the message to the protection level
    // of every other mail in the app. Enforced by
    // MailControllerTest::aDecryptedMessageNeverReachesTheDatabase.
    //
    // Only one message is held at a time, and forgetDecrypted() drops it.
    Q_PROPERTY(bool decryptBusy READ decryptBusy NOTIFY decryptedChanged)
    // Which message the plaintext below belongs to; empty when none is held.
    // The view MUST check this against the message it is showing -- holding
    // the id with the body is what stops one message's plaintext appearing
    // under another's headers after the reader moves on.
    Q_PROPERTY(QString decryptedMessageId READ decryptedMessageId NOTIFY decryptedChanged)
    Q_PROPERTY(QString decryptedHtml READ decryptedHtml NOTIFY decryptedChanged)
    Q_PROPERTY(QString decryptedPlain READ decryptedPlain NOTIFY decryptedChanged)
    // Localized sentence for the last failed decryption, or empty. Paired
    // with decryptRetryable so the UI knows whether a Retry button can help.
    Q_PROPERTY(QString decryptFailure READ decryptFailure NOTIFY decryptedChanged)
    // Who signed the held message, in words, and whether that is a warning.
    // Empty when the message is unsigned -- saying "not signed" beside every
    // unsigned message trains people to ignore the line that matters.
    Q_PROPERTY(QString decryptedSignature READ decryptedSignature NOTIFY decryptedChanged)
    Q_PROPERTY(bool decryptedSignatureIsWarning READ decryptedSignatureIsWarning NOTIFY decryptedChanged)
    Q_PROPERTY(bool decryptRetryable READ decryptRetryable NOTIFY decryptedChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(bool pgpCanEncrypt READ pgpCanEncrypt NOTIFY pgpComposeStateChanged)
    Q_PROPERTY(bool pgpCanSign READ pgpCanSign NOTIFY pgpComposeStateChanged)
    Q_PROPERTY(bool pgpHandoffToWebmail READ pgpHandoffToWebmail NOTIFY pgpComposeStateChanged)
    // True when this device can encrypt and sign the message itself: the
    // account is client-custody AND there is a usable gpg here. Compose offers
    // a real Send in that case instead of the webmail hand-off, which stays
    // for everyone else.
    Q_PROPERTY(bool pgpCanSendFromThisDevice READ pgpCanSendFromThisDevice NOTIFY pgpComposeStateChanged)
    // Set when a send succeeded but no copy could be filed in Sent. Not an
    // error -- the mail went out -- but the user's outbox will not have it.
    Q_PROPERTY(bool lastSendMissingSentCopy READ lastSendMissingSentCopy NOTIFY pgpComposeStateChanged)
    Q_PROPERTY(QStringList pgpKeylessRecipients READ pgpKeylessRecipients NOTIFY pgpKeylessRecipientsChanged)

public:
    MailController(MailRepository& mailRepository, RelayMailSource& relayMailSource,
                    KeywordRepository& keywordRepository, PairingStore& pairingStore,
                    FolderRepository& folderRepository, SettingsStore& settingsStore,
                    PgpBootstrapClient& pgpBootstrapClient, PgpRecipientChecker& pgpRecipientChecker,
                    NetworkExecutor& networkExecutor, QObject* parent = nullptr);

    QObject* emailModel() const;
    QString currentFolder() const;
    QString selectedKeyword() const; // "" = All
    QVariantList keywordTabs() const; // [{name, count}, ...] -- "All" NOT included, see Task 38
    bool isBusy() const;
    QString lastError() const; // "" when none
    bool decryptBusy() const { return m_decryptInFlight; }
    // All three answer as though nothing is held once the account has been
    // replaced. See decryptedStillOurs().
    QString decryptedMessageId() const { return decryptedStillOurs() ? m_decryptedMessageId : QString(); }
    QString decryptedHtml() const { return decryptedStillOurs() ? m_decryptedHtml : QString(); }
    QString decryptedPlain() const { return decryptedStillOurs() ? m_decryptedPlain : QString(); }
    QString decryptFailure() const { return m_decryptFailure; }
    QString decryptedSignature() const { return m_decryptedSignature; }
    bool decryptedSignatureIsWarning() const { return m_decryptedSignatureIsWarning; }
    bool decryptRetryable() const { return m_decryptRetryable; }
    bool pgpCanEncrypt() const;
    bool pgpCanSign() const;
    bool pgpHandoffToWebmail() const;
    bool pgpCanSendFromThisDevice() const;
    bool lastSendMissingSentCopy() const { return m_lastSendMissingSentCopy; }
    QStringList pgpKeylessRecipients() const;

public slots:
    // Sets currentFolder, resets selectedKeyword to "", loads the on-disk
    // cache for the new folder so the model has something to show
    // immediately, then calls refresh() to hit the network.
    void selectFolder(const QString& wireFolder);
    // Re-filters the model from the already-cached folder emails. No
    // network call.
    void selectKeyword(const QString& keyword);
    void refresh(bool forceFullResync = false);
    // All four dispatch and return; the outcome arrives as actionCompleted().
    // They no longer return a bool because there is nothing to return yet --
    // see that signal for how a caller learns whether its own action landed.
    void archiveEmails(const QStringList& messageIds);
    void deleteEmails(const QStringList& messageIds);
    void markSpam(const QStringList& messageIds);
    void moveEmails(const QStringList& messageIds, const QString& targetFolder);
    // mode is hardcoded "html" -- Compose.qml's RichBodyEditor is the sole
    // caller and always produces sanitized HTML (see
    // docs/superpowers/specs/2026-07-18-html-compose-design.md). Reads each
    // local file path via QFile, derives mimeType via QMimeDatabase, and
    // enforces a 25 MB total-bytes cap across all attachments before calling
    // relayMailSource.sendMail -- returns false + sets lastError (without
    // truncating) if the cap is exceeded.
    //
    // sign/encrypt have no default -- Task 7 wires the real toggle states
    // through from Compose.qml; leaving them defaulted would let a call site
    // silently drift back to "never encrypt" instead of a compile error.
    //
    // RETURNS A SEND TOKEN, not a result -- 0 if the send was refused before
    // any request was built (not paired, unreadable or oversized
    // attachment), otherwise an identifier for this one send. Every later
    // signal about it carries the same token.
    //
    // That token is how a composer knows an answer is its own. Before, the
    // test was "is my sendMail() call on the stack", which worked only
    // because the request ran synchronously inside the invokable. Nothing has
    // a call on the stack now, so ownership has to be a value the caller
    // holds -- and it is the same token that already existed for the
    // pickup-fallback round trip, so the two mechanisms the old comment here
    // described separately are now one.
    quint64 sendMail(const QString& to, const QString& cc, const QString& bcc, const QString& subject,
                     const QString& body, const QStringList& attachmentFilePaths, bool sign, bool encrypt);
    // Dispatches; the list arrives as attachmentsListed().
    void listAttachments(const QString& mailbox, const QString& messageId);
    // Writes the downloaded bytes to QStandardPaths::DownloadLocation,
    // deduping the filename against anything already there -- or, under
    // Hostile Location Protection, opens them from a tmpfs file instead.
    // Dispatches; the outcome arrives as attachmentDownloaded(), with the
    // reason for a failure in lastError.
    void downloadAttachment(const QString& mailbox, const QString& messageId, int index,
                             const QString& suggestedName);
    QVariantList standardFolders() const; // [{wireName, displayName}, ...], enum order
    // Task 35: looks up a single cached Email by messageId (independent of
    // folder, see MailRepository::findCachedEmail) as a QVariantMap keyed
    // the same way as EmailListModel::roleNames() (messageId/folder/sender/
    // sentTo/cc/bcc/subject/preview/body/label/keywords/status/atUtc/
    // hasAttachments/sourceMode), so EmailDetail.qml can bind to
    // result.subject/result.sender/etc. the same way it already binds to
    // emailModel row properties. Returns an empty map if the messageId
    // isn't cached locally -- this is a pure local-cache read (no network
    // call), so a miss just means "not fetched/cached yet", not an error.
    Q_INVOKABLE QVariantMap findByMessageId(const QString& messageId) const;

    // Fetches this message's ciphertext and decrypts it with the user's own
    // gpg-agent. Dispatches and returns; the result arrives on the
    // decrypted* properties.
    //
    // Deliberately NOT automatic on opening a message. Decryption prompts
    // pinentry, which for a hardware token means the user has to physically
    // touch it -- so it happens when they ask for it, not when a list
    // selection changes.
    Q_INVOKABLE void decryptMessage(const QString& messageId);

    // Drops the held plaintext. Called when the reader moves on, and wired
    // to the app lock in main.cpp: a locked app must not still be holding a
    // decrypted message for whoever picks the machine up next.
    Q_INVOKABLE void forgetDecrypted();

public:
    // What sendClientEncrypted() does off-thread, as one value so the GUI
    // thread only ever sees the outcome. Public because the worker that fills
    // it is a free function -- deliberately, so it cannot touch anything this
    // controller owns on the other thread.
    struct ClientEncryptedOutcome
    {
        bool ok = false;
        bool missingSentCopy = false;
        // Already-localized? No -- core/ owns no wording. This is a key into
        // the switch in finishClientEncryptedSend().
        enum class Failure {
            None, NotConfigured, RecipientWithoutKey, NoSigningKey, Cancelled,
            KeyImportRefused, EncryptionFailed, SendFailed, EngineUnavailable,
            ServerEncryptsInstead, TooLarge
        } failure = Failure::None;
        QStringList namedRecipients;
        QString detail;
    };

public slots:
    // Sends a message this device encrypts and signs itself, for an account
    // whose PGP key the relay does not hold.
    //
    // Dispatches and returns a token, the same shape as sendMail(); the answer
    // arrives as sendCompleted(token, ok). Everything blocking happens on the
    // executor thread, and one of those steps is pinentry, which waits for the
    // user indefinitely.
    //
    // Attachments travel INSIDE the encrypted part, so their names and bytes
    // are encrypted with the message -- for mail somebody chose to encrypt,
    // the filenames are often the most telling thing about it.
    //
    // The size limit that bites here is not the attachment's. The request
    // carries one full copy of the message per delivery, and each blind-copied
    // recipient gets their own, so a file that is fine for one recipient can
    // put the request over the relay's cap with three.
    Q_INVOKABLE quint64 sendClientEncrypted(const QString& to, const QString& cc, const QString& bcc,
                                             const QString& subject, const QString& body,
                                             const QStringList& attachmentFilePaths);

    // The notification tap-through entry point.
    //
    // A push notification is generated server-side the instant mail arrives,
    // and this client only learns of the message on its next sync -- up to 90
    // seconds later on the polling tier. So the overwhelmingly common case
    // for "the user tapped View" is a message this device has never seen:
    // both roots used to hydrate it from the cache, miss, and push a blank
    // detail page. The mail was one refresh away and nothing said so.
    //
    // Answers immediately when the message is already cached; otherwise
    // selects the Inbox, refreshes, and answers when that lands. Either way
    // the answer arrives as notificationEmailResolved().
    Q_INVOKABLE void openFromNotification(const QString& messageId);
    // Task 39: Settings > Keywords pane. Wraps keywordRepository.allSettings()
    // over the Inbox's cached emails specifically (m_mailRepository.
    // cachedEmails("INBOX")), NOT m_currentFolderEmails/whatever folder is
    // currently selected -- this is the one deliberate exception to "read
    // from the already-filtered current folder" elsewhere in this class,
    // since a Settings screen should show a stable keyword universe
    // independent of whatever folder the mail list happens to be showing.
    // Known limitation (matches the task-39 brief's own note): a keyword
    // only ever seen on emails in folders OTHER than Inbox won't appear
    // here -- deriving the universe from every folder's cache would need a
    // repository method this task doesn't add (Global Constraint 7). Result
    // shape: [{keyword, visible}, ...], alphabetical (case-insensitive),
    // same ordering as keywordTabs()/allSettings() itself.
    // Sidebar folder list: the six standard mailboxes in their fixed order,
    // with any server-side subfolders nested under their parent. Each entry
    // is {wireName, displayName, depth, deletable, isStandard} -- a
    // superset of standardFolders()'s shape, so the QML delegate binds the
    // same way and simply indents by `depth`.
    //
    // Reads the local cache only; call refreshFolders() to go to the
    // network. Standard folders are always present even when unpaired or
    // the fetch failed, so the sidebar never empties out on a bad
    // connection.
    Q_INVOKABLE QVariantList mailFolders() const;

    // Fetches subfolders for the parents worth expanding and emits
    // foldersChanged(). Currently just Archive, matching Android's folder
    // picker -- the other five standard mailboxes have no subfolder UI.
    Q_INVOKABLE void refreshFolders();

    // Each dispatches and returns; the outcome arrives as
    // folderMutationCompleted(), and foldersChanged() is emitted first on
    // success. The backend refuses to rename or delete a built-in mailbox or
    // any top-level folder, so entries with deletable == false must not offer
    // the action.
    Q_INVOKABLE void createFolder(const QString& parent, const QString& name);
    Q_INVOKABLE void renameFolder(const QString& folder, const QString& name);
    Q_INVOKABLE void deleteFolder(const QString& folder);

    // Saves the current composition to the Drafts mailbox via
    // POST /api/mail/draft. Same arguments as sendMail() so Compose.qml can
    // call either with the same expression, and the same token contract: 0
    // if refused up front, otherwise an identifier that draftSaveCompleted()
    // carries back.
    Q_INVOKABLE quint64 saveDraft(const QString& to, const QString& cc, const QString& bcc,
                                   const QString& subject, const QString& body,
                                   const QStringList& attachmentFilePaths);

    // Deletes every ephemeral attachment still on disk. Called on shutdown
    // so a clean quit leaves nothing behind, rather than waiting for timers
    // that will never fire.
    Q_INVOKABLE void clearEphemeralAttachments();

    Q_INVOKABLE QVariantList allKeywordSettings() const;
    // Task 39: KeywordRepository::setVisible() -- also re-emits
    // keywordTabsChanged() since toggling a keyword's visibility can change
    // whether it appears in the currently-filtered folder's keyword tab row
    // (see keywordTabs()'s own doc comment).
    Q_INVOKABLE void setKeywordVisible(const QString& keyword, bool visible);

    // Called when Compose opens. Sets pgpCanEncrypt/pgpCanSign/
    // pgpHandoffToWebmail from GET /api/pgp/bootstrap. A failed or
    // unreachable bootstrap is never "no PGP" -- it leaves every control
    // hidden rather than guessing a custody mode.
    //
    // The bootstrap fetch happens at most ONCE per session (see
    // m_pgpComposeStateFetched): this runs synchronously on the GUI thread,
    // from Compose.qml's Component.onCompleted, so every compose open would
    // otherwise spin a nested event loop while the object tree is still being
    // built. Custody mode is fixed at key creation with no downgrade path, so
    // a cached answer cannot go stale within a session. Pass force = true to
    // go to the network anyway. Every call still clears
    // pgpKeylessRecipients, which IS per-composition state.
    Q_INVOKABLE void refreshPgpComposeState(bool force = false);
    // Inline, non-blocking recipient-key warning. Reads the user's contacts
    // only -- a lower bound, since the send path also runs WKD/keyserver
    // discovery -- so this never gates the send; the server's 409 is the
    // gate. Updates pgpKeylessRecipients.
    //
    // Re-entrant by construction (the HTTP call below runs a nested event
    // loop, which keeps firing Compose.qml's debounce timer), so an in-flight
    // call makes this a no-op rather than nesting a second one -- see
    // m_preflightInFlight.
    Q_INVOKABLE void preflightRecipients(const QString& to, const QString& cc, const QString& bcc);
    // Re-sends the exact payload the server refused with 409 +
    // keylessRecipients, with allowPickupFallback set. `token` is the value
    // pickupFallbackRequired() carried: this returns false without sending
    // unless it still matches the cached pending send, so a confirmation for
    // an older, already-resolved or already-replaced send cannot mail
    // anything. Also returns false when there is no pending send at all.
    // Returns the token it was given (so the caller keeps waiting on the same
    // one -- the confirmed re-send is the same logical send), or 0 when there
    // is no matching pending send and nothing was dispatched.
    Q_INVOKABLE quint64 confirmPickupFallbackSend(quint64 token);
    // Drops the cached pending send. Called when the user cancels the
    // pickup-fallback confirmation dialog rather than confirming it.
    Q_INVOKABLE void discardPendingSend();
    // Saves the composition to Drafts, then opens webmail there in the user's
    // system browser (never an embedded view) -- in that order, and only if
    // the save actually landed: opening a browser onto a draft that is not
    // there loses the user's message. Returns 0 without dispatching anything
    // when unpaired or when the pairing's base URL is not https; otherwise a
    // draftSaveCompleted() token, whose `ok` covers both the save and the
    // browser launch.
    Q_INVOKABLE quint64 openWebmailDrafts(const QString& to, const QString& cc, const QString& bcc,
                                           const QString& subject, const QString& body,
                                           const QStringList& attachmentFilePaths);

signals:
    void currentFolderChanged();
    void selectedKeywordChanged();
    void keywordTabsChanged();
    void isBusyChanged();
    void lastErrorChanged();
    void decryptedChanged();
    void foldersChanged();
    // Outcome of archiveEmails/deleteEmails/markSpam/moveEmails. `action` is
    // the wire verb ("archive"/"delete"/"spam"/"move") and `messageIds` is
    // the exact list that was requested.
    //
    // This class is a QML SINGLETON, so every live view receives this. The
    // messageIds are how a receiver decides whether it was theirs -- a
    // detail view checks its own messageId is in the list, a swipe row
    // checks its row's. That is deliberately a value carried by the signal
    // rather than an "is a call of mine on the stack" test, because with the
    // request off-thread nobody has one.
    //
    // `ok` is false for a refused pairing and for a rejected request. It is
    // TRUE when the server accepted the request but reported per-message
    // failures in `failed` -- those are in lastError, and the action itself
    // did happen for the rest.
    void actionCompleted(const QString& action, const QStringList& messageIds, bool ok);
    // Outcome of createFolder/renameFolder/deleteFolder. Unlike
    // actionCompleted there is nothing to disambiguate with: the folder
    // dialogs are modal and singular, so "which one was mine" cannot arise.
    void folderMutationCompleted(bool ok);
    // Result of listAttachments(). Carries the mailbox/messageId it was asked
    // for, for the same reason actionCompleted carries its ids: this is a
    // singleton, more than one EmailDetail can be open, and each has to be
    // able to tell whether the answer describes the message IT is showing.
    // Empty on failure, with the reason in lastError.
    void attachmentsListed(const QString& mailbox, const QString& messageId, const QVariantList& attachments);
    // Result of downloadAttachment(). `ok` false leaves the reason in
    // lastError. Note this reports that the bytes were SAVED (or opened
    // ephemerally), not merely fetched.
    void attachmentDownloaded(const QString& messageId, int index, bool ok);
    // Emitted when sendMail() is refused with 409 + keylessRecipients: the
    // server's own list of addresses with no usable PGP key, naming the
    // pending send confirmPickupFallbackSend() would re-send.
    //
    // `token` is the value sendMail() returned for the send being refused,
    // and must be handed back to confirmPickupFallbackSend(). This class is a
    // QML SINGLETON, so this signal reaches every live Compose instance --
    // two coexist easily (a pop-out compose window plus Ctrl+N in the main
    // window). The token answers both questions that keeps one message's
    // consent from being collected in another message's window:
    //
    //   * Ownership -- "is this signal mine?" The composer compares against
    //     the token sendMail() handed it. This used to be a sendInFlight flag
    //     instead, which worked only because the emit was SYNCHRONOUS, inside
    //     sendMail(), so the true owner was the one with that call on its
    //     stack. The request runs on the executor thread now and nobody has a
    //     call on the stack, so ownership had to become a carried value.
    //   * Staleness/replay -- "is this confirmation still valid?" The confirm
    //     path re-checks the token against the cached pending send, so a
    //     confirmation for a send already replaced or resolved is refused.
    void pickupFallbackRequired(quint64 token, const QStringList& recipients);
    // Terminal outcome of one sendMail()/confirmPickupFallbackSend(),
    // identified by the token that call returned. `ok` is false for an
    // outright failure AND for a pickup-fallback refusal -- the latter is
    // preceded by pickupFallbackRequired, which is what the composer acts on;
    // this one just says the attempt is over.
    void sendCompleted(quint64 token, bool ok);
    // Terminal outcome of one saveDraft()/openWebmailDrafts().
    void draftSaveCompleted(quint64 token, bool ok);
    // Emitted on a 200 response carrying a non-empty warning: the message
    // WAS sent, with partial trouble (e.g. the Sent copy failed, or a pickup
    // link did not deliver to everyone). Never a failure signal.
    //
    // Deliberately NOT displayed by Compose.qml: a successful send makes the
    // hosts destroy that component, so a notice parented to it would die in
    // the same turn it appeared. The sinks live in MobileRoot.qml /
    // DesktopRoot.qml (and DesktopRoot's pop-out compose Window), which
    // outlive the composer. This class is a singleton, so those receive it
    // regardless of which composer sent.
    void sendWarning(const QString& warning);
    void pgpComposeStateChanged();
    void pgpKeylessRecipientsChanged();
    // Task 42: forwarded straight from NotificationDispatcher::openRequested
    // (main.cpp connects the two directly -- signal-to-signal, no lambda,
    // since the shapes already match) when the user activates a
    // notification's "View" action. MailController itself has no window/
    // pageStack access (same constraint every other controller in this repo
    // respects -- see PairingController's deep-link routing for the closest
    // precedent), so this only carries the bare messageId to QML;
    // MobileRoot.qml/DesktopRoot.qml each have a
    // `Connections { target: MailApp }` block that hydrates the full email
    // via findByMessageId() and does the actual navigation + window
    // raise/focus.
    void openEmailRequested(const QString& messageId);

    // The answer to openFromNotification(): which mailbox `messageId` turned
    // out to be in, or an EMPTY folder when it could not be found even after
    // a refresh.
    //
    // A separate signal from openEmailRequested above rather than a return
    // value, because the answer may only arrive after a network round trip.
    // The roots open the detail page on a non-empty folder and stay put on an
    // empty one -- pushing a blank detail page for a message that is not
    // there is what this whole path exists to stop.
    void notificationEmailResolved(const QString& messageId, const QString& folder);

    // The id a notification carried is cached in more than one mailbox, and
    // nothing here can break the tie -- the UnifiedPush envelope carries no
    // mailbox at all (PushPayloadParser.h).
    //
    // Its own signal rather than an empty notificationEmailResolved, because
    // the two need opposite UI. "Not found" is terminal and the roots show the
    // error; this one the user can settle in one tap, and rendering it as a
    // blank page -- which is what happened until 2026-08-23 -- gave them
    // nothing to act on for a question only they can answer.
    void notificationEmailAmbiguous(const QString& messageId, const QStringList& folders);

private:
    void applyFilter(); // recomputes m_model from m_currentFolderEmails + m_selectedKeyword
    // Re-reads the current folder's cache into the model. Shared by
    // selectFolderInternal() and the refresh completion.
    void reloadCurrentFolder();
    // isBusy is a DEPTH, not a flag. With refresh() asynchronous and the rest
    // of this class still synchronous, the two overlap: a click on Archive
    // while a refresh is in flight used to run setBusy(true)/setBusy(false)
    // around itself and clear the indicator out from under the refresh, so
    // the UI reported idle with a request still outstanding. Every
    // network-reaching method brackets itself with these instead.
    void pushBusy();
    void popBusy();
    // Completion of the asynchronous refresh, on this object's own thread.
    void finishRefresh(const MailRefreshPlan& plan, const InboxFetchResult& result);
    void setLastError(const QString& error);
    // Runs on the GUI thread once the reader has answered. Takes the
    // identity the request was planned against so a reply for a replaced
    // account can be discarded rather than displayed.
    void applyDecryptResult(const PairingIdentity& identity, const QString& messageId,
                             const PgpReadResult& result);
    void finishClientEncryptedSend(quint64 token, const ClientEncryptedOutcome& outcome);
    // Loads pairing state via m_pairingStore.load() into serverBaseUrl/auth.
    // Returns false (and sets lastError to "Not paired") without touching
    // either out-param when there is no saved pairing -- every network-
    // calling method below short-circuits on this before making a request.
    bool requirePairing(QUrl& serverBaseUrl, RelayAuth& auth);
    // Shared by sendMail() and saveDraft() -- see the .cpp.
    bool readAttachments(const QStringList& paths, QVector<MailAttachmentUpload>& out);
    // The half of downloadAttachment() that must stay on this thread.
    bool storeDownloadedAttachment(const QString& suggestedName, const DownloadAttachmentResult& result);
    void applyKeylessRecipients(const QStringList& keyless);

    // These existed because the public entry points held a ReentrancyGuard
    // and several of them legitimately call each other (deleteFolder ->
    // selectFolder -> refresh, openWebmailDrafts -> saveDraft), so an inner
    // step would have been suppressed by the outer call's own guard.
    //
    // No guards remain, so the split is now only about the extra arguments
    // the internal callers need: saveDraftInternal takes the webmail URL to
    // open afterwards, which the QML-facing saveDraft() must not expose.
    void selectFolderInternal(const QString& wireFolder);
    void refreshInternal(bool forceFullResync);
    quint64 saveDraftInternal(const QString& to, const QString& cc, const QString& bcc, const QString& subject,
                               const QString& body, const QStringList& attachmentFilePaths,
                               const QUrl& thenOpenWebmail);
    void finishSend(quint64 sendToken, const SendMailResult& result);

    // Hostile Location Protection's replacement for save-to-Downloads: a
    // tmpfs-backed temporary file, opened with the OS handler and removed
    // shortly after. See the .cpp for the honest limits of the guarantee.
    //
    // `declaredMimeType` is the server's own type for the attachment and is
    // the gate: only an allowlisted type is opened at all, and the file's
    // extension is forced to match it. This path hands attacker-supplied
    // bytes to the desktop's handler for that extension, so trusting the
    // filename would make "Invoice.pdf.desktop" an execution primitive.
    bool openAttachmentEphemerally(const QString& name, const QString& declaredMimeType,
                                    const QByteArray& data);

    // Creates `path` with O_EXCL semantics (fails if it exists) -- closes the
    // check-then-use race between dedupedFilePath()'s exists() test and the
    // open that follows it. See the .cpp.
    static bool openForExclusiveWrite(QFile& file, const QString& path);
    // Writes `data` in full or removes the partial file and returns false, so
    // a full disk cannot produce a truncated attachment reported as saved.
    static bool writeAllOrRemove(QFile& file, const QByteArray& data);

    // How long an ephemeral attachment survives before it is deleted. Long
    // enough for a viewer to open and read it, short enough not to linger.
    static constexpr int kEphemeralAttachmentLifetimeMs = 5 * 60 * 1000;

    QStringList m_ephemeralAttachments;
    // Base URL for webmail deep links, or an empty QUrl when unpaired. See
    // the .cpp for why this is separate from requirePairing().
    QUrl webmailBaseUrl() const;
    // Shared body of archiveEmails/deleteEmails/markSpam/moveEmails.
    void performActionCommon(const QStringList& messageIds, const QString& action,
                              const std::optional<QString>& targetMailbox);
    void finishAction(const QString& action, const QStringList& messageIds, const ActionResult& result);
    // Shared body of createFolder/renameFolder/deleteFolder -- see the .cpp.
    void runFolderMutation(const QString& failureMessage,
                            std::function<FolderRepository::FolderMutationFetch(HttpClient&, const RelayEndpoint&)> work,
                            std::function<void(const QString& resultingFolder)> onApplied);
    static QString dedupedFilePath(const QString& directory, const QString& fileName);

    // The exact payload of a send the server refused with 409 +
    // keylessRecipients, held so confirmPickupFallbackSend() can re-send it
    // byte-identically with allowPickupFallback flipped. Rebuilding from the
    // QML fields would re-read and re-base64 every attachment off disk, and
    // any drift between the refused request and the confirmed one is a
    // message the user did not review.
    //
    // Holds the composed plaintext in memory until cleared, so it is cleared
    // on success, on cancel, and whenever a fresh send starts. Under Hostile
    // Location Protection this never reaches disk, matching the rest of that
    // mode.
    struct PendingSend
    {
        bool valid = false;
        QString to;
        QString cc;
        QString bcc;
        QString subject;
        QString body;
        QString mode;
        QVector<MailAttachmentUpload> attachments;
        bool sign = false;
        bool encrypt = false;
        // Identifies this one pending send across the pickupFallbackRequired
        // -> confirmPickupFallbackSend round trip. Appended after the ten
        // fields above so the existing aggregate initialization in the .cpp
        // keeps reading correctly. Never 0 for a valid pending send: the
        // counter starts at 1, so a default-constructed PendingSend can never
        // accidentally match a real token.
        quint64 token = 0;
    };
    PendingSend m_pendingSend;
    // Monotonic, per-session. quint64 crosses into QML as a JS number, which
    // is exact below 2^53 -- unreachable for a counter incremented once per
    // send attempt.
    quint64 m_nextPendingSendToken = 1;

    MailRepository& m_mailRepository;
    FolderRepository& m_folderRepository;
    SettingsStore& m_settingsStore;
    RelayMailSource& m_relayMailSource;
    KeywordRepository& m_keywordRepository;
    PairingStore& m_pairingStore;
    PgpBootstrapClient& m_pgpBootstrapClient;
    PgpRecipientChecker& m_pgpRecipientChecker;
    NetworkExecutor& m_executor;
    EmailListModel* m_model; // owned, parented to this
    QVector<Email> m_currentFolderEmails; // last cachedEmails(currentFolder) result, pre-keyword-filter
    QString m_currentFolder = QStringLiteral("INBOX");
    QString m_selectedKeyword;
    int m_busyDepth = 0;
    // A refresh is out on the executor thread. Coalescing state, NOT a
    // re-entrancy guard: the request cannot re-enter this object any more.
    // Pull-to-refresh, the toolbar button and selectFolder() can all fire
    // within one round trip, and N of them must cost one follow-up request,
    // not N.
    bool m_refreshInFlight = false;
    bool m_refreshPending = false;
    // forceFullResync is sticky across coalescing: folding a user-initiated
    // full resync into a background delta refresh would silently downgrade
    // the one request the user explicitly asked for.
    bool m_refreshPendingFullResync = false;
    // Set by openFromNotification() while it waits for a refresh to land.
    // Empty when no tap-through is outstanding.
    QString m_pendingNotificationMessageId;
    QString m_lastError;
    bool m_pgpCanEncrypt = false;
    bool m_pgpCanSign = false;
    bool m_pgpHandoffToWebmail = false;
    QStringList m_pgpKeylessRecipients;
    // True once a bootstrap fetch has actually reached the server this
    // session -- see refreshPgpComposeState(). A short-circuited pairing
    // lookup does NOT set it, so pairing later still gets a real answer.
    bool m_pgpComposeStateFetched = false;
    // Coalescing flags for the two PGP compose-state calls. Both used to be
    // re-entrancy defences against the nested event loop; the requests are
    // off-thread now, so they only stop a second call piling up behind one
    // already out.
    bool m_preflightInFlight = false;
    bool m_pgpBootstrapInFlight = false;

    // The held plaintext. See the decrypt* properties above for why it lives
    // here and not in the cache.
    // Whether what is held still belongs to the account this device is paired
    // to.
    //
    // Enforced on every READ rather than only by clearing on a signal,
    // because the identifier alone is not evidence of anything:
    // decryptedMessageId is an IMAP UID, UIDs are per-mailbox rather than
    // per-account, and "5" exists again in the next account and means
    // somebody else's message. Everything that decides whether to show the
    // held body compares that id, so a match after a replacement is a
    // coincidence and not a permission.
    //
    // Clearing on a signal is also done -- main.cpp wires it to
    // PairingController -- because that actually frees the memory. This is
    // what makes the property true regardless of whether anyone remembered
    // to wire it.
    bool decryptedStillOurs() const;

    PairingIdentity m_decryptedIdentity;
    QString m_decryptedMessageId;
    QString m_decryptedHtml;
    QString m_decryptedPlain;
    QString m_decryptFailure;
    QString m_decryptedSignature;
    bool m_decryptedSignatureIsWarning = false;
    bool m_decryptRetryable = false;
    bool m_decryptInFlight = false;
    bool m_lastSendMissingSentCopy = false;
};
