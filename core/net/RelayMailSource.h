#pragma once

#include "models/Email.h"
#include "net/NetworkError.h"

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>
#include <optional>

class HttpClient;
struct RelayAuth;

// One entry of GET /api/inbox's byTab[<tab>] array. Wraps core/models/Email
// (unmodified, per the Task 16/17 rule that plain model headers stay
// wire-format-agnostic) with the two extra wire fields the inbox endpoint
// carries that Email has no field for: `detail` (optional error/status
// detail) and `changeType` (only present in delta/`since=`-mode responses,
// "new" or "updated"). No delta-merge logic is implemented here -- deciding
// what "updated" means for a locally-cached row is a Phase 4 domain-layer
// concern; this type only carries the field through.
struct InboxEmailItem
{
    Email email;
    QString detail;
    std::optional<QString> changeType;

    bool operator==(const InboxEmailItem&) const = default;
};

struct InboxFetchResult
{
    std::optional<NetworkError> error;
    QString detail; // human-readable detail on error; empty otherwise
    QStringList tabs;
    // QMap sorts by key alphabetically -- iterate via tabs (order-preserving) if wire/tab order matters, not this map directly.
    QMap<QString, QVector<InboxEmailItem>> byTab;
    // Delta (`since=`) response fields -- confirmed against
    // internal/api/server.go's delta branch of handleInbox (~line 2097-2105):
    // a delta response is {tabs, byTab, delta:true, cursor, removed:[messageId,...]},
    // a full-snapshot response (no `since` sent) is just {tabs, byTab} with
    // none of these three keys present, hence the false/0/empty defaults.
    bool isDelta = false;   // json "delta", defaults false when absent (full-snapshot response)
    qint64 cursor = 0;      // json "cursor", meaningless when !isDelta
    QStringList removed;    // json "removed", always empty when !isDelta
};

// One entry of POST /api/inbox/actions's "failed" array.
struct ActionFailure
{
    QString messageId;
    QString error;

    bool operator==(const ActionFailure&) const = default;
};

// POST /api/inbox/actions response: {ok, action, processed, failed,
// targetMailbox}. targetMailbox is always present on the wire (echoed back,
// even as "" when action != "move") -- this is distinct from the request
// body, where targetMailbox is omitted entirely for non-move actions.
struct ActionResult
{
    std::optional<NetworkError> error;
    QString detail;
    bool ok = false;
    QString action;
    int processed = 0;
    QVector<ActionFailure> failed;
    QString targetMailbox;
};

// One entry of POST /api/mail/send's "attachments" request array. Holds raw
// bytes -- sendMail base64-encodes into `dataBase64` on the wire itself, so
// callers never have to think about the wire encoding, mirroring how
// performAction's plain QString action keeps wire-format details inside
// this class rather than pushed onto callers.
struct MailAttachmentUpload
{
    QString name;
    QString mimeType;
    QByteArray data;

    bool operator==(const MailAttachmentUpload&) const = default;
};

// POST /api/mail/send response: {ok, sentSaved, warning}. warning is always
// present on the wire, even as "" when there's nothing to warn about --
// same always-present-possibly-empty pattern as ActionResult::targetMailbox
// above, confirmed against handleMailSend in the Go backend.
struct SendMailResult
{
    std::optional<NetworkError> error;
    QString detail;
    bool ok = false;
    bool sentSaved = false;
    QString warning;

    // Set when the backend refused with 409 + `clientSideNeeded: true`: the
    // account's PGP key is end-to-end protected, so the server cannot sign
    // or encrypt on the user's behalf and deliberately refuses rather than
    // silently sending in the clear (backend: handleMailSend's
    // `req.Sign || req.Encrypt` branch).
    //
    // As of this task, reachable: sendMail's sign/encrypt flags below can
    // trigger it -- carried so this 409 is never mistaken for a generic
    // conflict.
    bool clientSideNeeded = false;

    // Set when the backend refused with 409 + `keylessRecipients`: at least
    // one recipient has no usable PGP key, and the server refused rather than
    // quietly falling back to a one-time link that stores this message's
    // plaintext server-side for seven days.
    //
    // Nothing was delivered -- the refusal happens before any SMTP
    // (server.go:1272), so re-sending the identical request with
    // allowPickupFallback set is safe and cannot duplicate. Distinct from
    // clientSideNeeded above, which is the same status for a different and
    // unrecoverable reason.
    bool pickupFallbackNeeded = false;
    QStringList keylessRecipients;
};

// POST /api/mail/draft response: {ok}. No sentSaved/warning -- the backend
// answers with a bare ok on success and http.Error otherwise.
struct SaveDraftResult
{
    std::optional<NetworkError> error;
    QString detail;
    bool ok = false;
};

// One entry of GET /api/mail/attachments's "attachments" array (backend's
// AttachmentInfo struct: index/name/mimeType/size, metadata only, no
// content).
struct MailAttachmentInfo
{
    int index = 0;
    QString name;
    QString mimeType;
    int size = 0;

    bool operator==(const MailAttachmentInfo&) const = default;
};

struct ListAttachmentsResult
{
    std::optional<NetworkError> error;
    QString detail;
    QVector<MailAttachmentInfo> attachments;
};

// GET /api/mail/attachment response: raw binary body, not JSON -- filename
// and mime type travel in the Content-Disposition/Content-Type response
// headers instead (Content-Disposition per Go's mime.FormatMediaType, i.e.
// `attachment; filename="<name>"` with RFC 2045 quoted-string escaping).
struct DownloadAttachmentResult
{
    std::optional<NetworkError> error;
    QString detail;
    QByteArray data;
    QString mimeType;
    QString filename;
};

// Inbox fetch, inbox action, mail send, and attachment list/download calls
// against the Relay backend's /api/inbox, /api/inbox/actions, /api/mail/
// send, /api/mail/attachments, and /api/mail/attachment endpoints. sub/hash
// (RelayAuth) apply uniformly to every method here via query params,
// confirmed against resolveMailAuthContext / withMailAuth in the Go backend.
class RelayMailSource
{
public:
    explicit RelayMailSource(HttpClient& httpClient);

    // limit/since are passed through only when the caller provides them --
    // the server enforces its own default (500) / max (5000) bound, this
    // client never clamps client-side. since present (even 0) puts the
    // server into delta mode (only new/updated items, with changeType set);
    // this method doesn't need to know or care about that distinction, it
    // just parses whatever comes back via InboxEmailItem.
    InboxFetchResult fetchInbox(const QUrl& serverBaseUrl, const RelayAuth& auth, std::optional<int> limit,
                                 const QString& mailbox, std::optional<qint64> since) const;

    // Plain QString action (not an enum) -- mirrors how Email::status/
    // Email::label are already plain QString wire values, keeping this
    // client a thin pass-through of whatever action strings the server
    // defines rather than a second source of truth for the valid action
    // set. targetMailbox is included in the request body only when it has
    // a value (omitted entirely otherwise, never sent as "").
    ActionResult performAction(const QUrl& serverBaseUrl, const RelayAuth& auth, const QString& action,
                                const QStringList& messageIds, const QString& mailbox,
                                const std::optional<QString>& targetMailbox) const;

    // to/cc/bcc are comma-joined address-list strings on the wire (the
    // server splits them via parseRecipientList) -- NOT JSON arrays, unlike
    // performAction's messageIds. Callers are responsible for joining.
    // mode is a plain QString ("plain"/"html", mailmsg.Message.Mode's known
    // values) mirroring the plain-QString-action convention used by
    // performAction, rather than a client-side enum. There is no mailbox
    // parameter -- confirmed against decodeMailRequest/handleMailSend, send
    // is not scoped to a mailbox the way fetch/actions are (it sends via
    // SMTP using the account's configured credentials, then best-effort
    // saves to Sent).
    SendMailResult sendMail(const QUrl& serverBaseUrl, const RelayAuth& auth, const QString& to, const QString& cc,
                             const QString& bcc, const QString& subject, const QString& body, const QString& mode,
                             const QVector<MailAttachmentUpload>& attachments, bool sign = false, bool encrypt = false,
                             bool allowPickupFallback = false) const;

    // POST /api/mail/draft. Same request body as sendMail (the backend
    // decodes both with decodeMailRequest), but a much simpler response:
    // {ok: true}, with every failure arriving as plain text via http.Error
    // rather than JSON.
    SaveDraftResult saveDraft(const QUrl& serverBaseUrl, const RelayAuth& auth, const QString& to,
                               const QString& cc, const QString& bcc, const QString& subject,
                               const QString& body, const QString& mode,
                               const QVector<MailAttachmentUpload>& attachments) const;

    // messageId is an IMAP UID parsed server-side as an integer, but travels
    // as an ordinary query-string value like everywhere else in this class
    // -- no client-side validation/conversion needed.
    ListAttachmentsResult listAttachments(const QUrl& serverBaseUrl, const RelayAuth& auth, const QString& mailbox,
                                           const QString& messageId) const;

    DownloadAttachmentResult downloadAttachment(const QUrl& serverBaseUrl, const RelayAuth& auth,
                                                 const QString& mailbox, const QString& messageId, int index) const;

private:
    HttpClient& m_httpClient;
};
