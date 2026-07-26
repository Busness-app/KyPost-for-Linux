#pragma once

#include "mail/EmailListModel.h"
#include "models/Email.h"
#include "net/RelayMailSource.h" // MailAttachmentUpload -- held by value in PendingSend below

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <optional>

class MailRepository;
class FolderRepository;
class SettingsStore;
class RelayMailSource;
class KeywordRepository;
class PairingStore;
class PgpBootstrapClient;
class PgpRecipientChecker;
struct RelayAuth;
class QUrl;

// QML-facing bridge (Task 32) over the core/domain mail stack: MailRepository
// (cache + delta-merge), RelayMailSource (direct relay calls for actions/
// send/attachments -- MailRepository itself only wraps fetchInbox), and
// KeywordRepository (per-folder keyword tab derivation). Registered as the
// "MailApp" QML singleton in main.cpp. Every method here that reaches the
// network runs synchronously on the calling (GUI) thread -- see Phase 6
// global constraint 2, this is a known, accepted freeze-the-UI tradeoff for
// this phase, not a bug.
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
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(bool pgpCanEncrypt READ pgpCanEncrypt NOTIFY pgpComposeStateChanged)
    Q_PROPERTY(bool pgpCanSign READ pgpCanSign NOTIFY pgpComposeStateChanged)
    Q_PROPERTY(bool pgpHandoffToWebmail READ pgpHandoffToWebmail NOTIFY pgpComposeStateChanged)
    Q_PROPERTY(QStringList pgpKeylessRecipients READ pgpKeylessRecipients NOTIFY pgpKeylessRecipientsChanged)

public:
    MailController(MailRepository& mailRepository, RelayMailSource& relayMailSource,
                    KeywordRepository& keywordRepository, PairingStore& pairingStore,
                    FolderRepository& folderRepository, SettingsStore& settingsStore,
                    PgpBootstrapClient& pgpBootstrapClient, PgpRecipientChecker& pgpRecipientChecker,
                    QObject* parent = nullptr);

    QObject* emailModel() const;
    QString currentFolder() const;
    QString selectedKeyword() const; // "" = All
    QVariantList keywordTabs() const; // [{name, count}, ...] -- "All" NOT included, see Task 38
    bool isBusy() const;
    QString lastError() const; // "" when none
    bool pgpCanEncrypt() const;
    bool pgpCanSign() const;
    bool pgpHandoffToWebmail() const;
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
    bool archiveEmails(const QStringList& messageIds);
    bool deleteEmails(const QStringList& messageIds);
    bool markSpam(const QStringList& messageIds);
    bool moveEmails(const QStringList& messageIds, const QString& targetFolder);
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
    bool sendMail(const QString& to, const QString& cc, const QString& bcc, const QString& subject,
                  const QString& body, const QStringList& attachmentFilePaths, bool sign, bool encrypt);
    QVariantList listAttachments(const QString& mailbox, const QString& messageId); // [{index, name, mimeType, size}, ...]
    // Writes the downloaded bytes to QStandardPaths::DownloadLocation,
    // deduping the filename against anything already there. Returns false +
    // sets lastError on failure.
    bool downloadAttachment(const QString& mailbox, const QString& messageId, int index,
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

    // Each returns true on success and emits foldersChanged(); on failure
    // sets lastError and returns false. The backend refuses to rename or
    // delete a built-in mailbox or any top-level folder, so entries with
    // deletable == false must not offer the action.
    Q_INVOKABLE bool createFolder(const QString& parent, const QString& name);
    Q_INVOKABLE bool renameFolder(const QString& folder, const QString& name);
    Q_INVOKABLE bool deleteFolder(const QString& folder);

    // Saves the current composition to the Drafts mailbox via
    // POST /api/mail/draft. Same arguments as sendMail() so Compose.qml can
    // call either with the same expression.
    Q_INVOKABLE bool saveDraft(const QString& to, const QString& cc, const QString& bcc,
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
    Q_INVOKABLE bool confirmPickupFallbackSend(quint64 token);
    // Drops the cached pending send. Called when the user cancels the
    // pickup-fallback confirmation dialog rather than confirming it.
    Q_INVOKABLE void discardPendingSend();
    // Saves the composition to Drafts, then opens webmail there in the
    // user's system browser (never an embedded view). Returns false without
    // opening anything if the draft did not save, or if the pairing's base
    // URL is not https.
    Q_INVOKABLE bool openWebmailDrafts(const QString& to, const QString& cc, const QString& bcc,
                                        const QString& subject, const QString& body,
                                        const QStringList& attachmentFilePaths);

signals:
    void currentFolderChanged();
    void selectedKeywordChanged();
    void keywordTabsChanged();
    void isBusyChanged();
    void lastErrorChanged();
    void foldersChanged();
    // Emitted when sendMail() is refused with 409 + keylessRecipients: the
    // server's own list of addresses with no usable PGP key, naming the
    // pending send confirmPickupFallbackSend() would re-send.
    //
    // `token` identifies that one pending send and must be handed back to
    // confirmPickupFallbackSend(). This class is a QML SINGLETON, so this
    // signal reaches every live Compose instance -- two coexist easily (a
    // pop-out compose window plus Ctrl+N in the main window). Two separate
    // mechanisms keep that from collecting one message's consent in another
    // message's window, because they answer different questions:
    //
    //   * Ownership -- "is this signal mine?" Answered on the QML side by a
    //     sendInFlight flag, which works because this signal is emitted
    //     SYNCHRONOUSLY inside sendMail(), so the only instance with a
    //     sendMail() call on its stack is the true owner. Do not make the
    //     emit asynchronous (queued/deferred) without replacing that.
    //   * Staleness/replay -- "is this confirmation still valid?" Answered by
    //     the token, which the confirm path re-checks against the cached
    //     pending send. Needed independently: the flag says nothing about a
    //     confirmation that arrives after the pending send was replaced or
    //     already resolved.
    void pickupFallbackRequired(quint64 token, const QStringList& recipients);
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

private:
    void applyFilter(); // recomputes m_model from m_currentFolderEmails + m_selectedKeyword
    void setBusy(bool busy);
    void setLastError(const QString& error);
    // Loads pairing state via m_pairingStore.load() into serverBaseUrl/auth.
    // Returns false (and sets lastError to "Not paired") without touching
    // either out-param when there is no saved pairing -- every network-
    // calling method below short-circuits on this before making a request.
    bool requirePairing(QUrl& serverBaseUrl, RelayAuth& auth);
    // Shared by sendMail() and saveDraft() -- see the .cpp.
    bool readAttachments(const QStringList& paths, QVector<MailAttachmentUpload>& out);

    // Hostile Location Protection's replacement for save-to-Downloads: a
    // tmpfs-backed temporary file, opened with the OS handler and removed
    // shortly after. See the .cpp for the honest limits of the guarantee.
    bool openAttachmentEphemerally(const QString& name, const QByteArray& data);

    // How long an ephemeral attachment survives before it is deleted. Long
    // enough for a viewer to open and read it, short enough not to linger.
    static constexpr int kEphemeralAttachmentLifetimeMs = 5 * 60 * 1000;

    QStringList m_ephemeralAttachments;
    // Base URL for webmail deep links, or an empty QUrl when unpaired. See
    // the .cpp for why this is separate from requirePairing().
    QUrl webmailBaseUrl() const;
    // Shared body of archiveEmails/deleteEmails/markSpam/moveEmails.
    bool performActionCommon(const QStringList& messageIds, const QString& action,
                              const std::optional<QString>& targetMailbox);
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
    EmailListModel* m_model; // owned, parented to this
    QVector<Email> m_currentFolderEmails; // last cachedEmails(currentFolder) result, pre-keyword-filter
    QString m_currentFolder = QStringLiteral("INBOX");
    QString m_selectedKeyword;
    bool m_isBusy = false;
    QString m_lastError;
    bool m_pgpCanEncrypt = false;
    bool m_pgpCanSign = false;
    bool m_pgpHandoffToWebmail = false;
    QStringList m_pgpKeylessRecipients;
    // True once a bootstrap fetch has actually reached the server this
    // session -- see refreshPgpComposeState(). A short-circuited pairing
    // lookup does NOT set it, so pairing later still gets a real answer.
    bool m_pgpComposeStateFetched = false;
    // Guards preflightRecipients() against re-entering through the nested
    // event loop its own HTTP call runs.
    bool m_preflightInFlight = false;
};
