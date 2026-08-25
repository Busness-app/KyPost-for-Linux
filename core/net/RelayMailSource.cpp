#include "net/RelayMailSource.h"

#include "net/HttpClient.h"
#include "net/RelayAuth.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

namespace {

// Maps one wire inbox item -- {messageId, sender, sentTo, cc, bcc, subject,
// body, bodyMode, status, atUtc, hasAttachments, label, keywords, detail?,
// changeType?}
// -- onto
// InboxEmailItem. atUtc is a direct pass-through (core/models/Email::atUtc
// already matches the wire key exactly, no casing translation). There is no
// distinct "preview" key on the wire, so Email::preview is left empty here
// (deferred, not guessed). folder is not itself a wire field on the item --
// it's set by the caller from the enclosing byTab map key.
InboxEmailItem inboxItemFromJson(const QJsonObject& obj)
{
    InboxEmailItem item;
    item.email.messageId = obj.value(QStringLiteral("messageId")).toString();
    item.email.sender = obj.value(QStringLiteral("sender")).toString();
    item.email.sentTo = obj.value(QStringLiteral("sentTo")).toString();
    item.email.cc = obj.value(QStringLiteral("cc")).toString();
    item.email.bcc = obj.value(QStringLiteral("bcc")).toString();
    item.email.subject = obj.value(QStringLiteral("subject")).toString();
    if (obj.contains(QStringLiteral("body")) && !obj.value(QStringLiteral("body")).isNull())
        item.email.body = obj.value(QStringLiteral("body")).toString();
    // Travels with the body, and only with it. `omitempty` on the wire, so an
    // absent key stays absent here rather than becoming "plain" -- see
    // Email::bodyMode for what re-deriving it costs.
    if (obj.contains(QStringLiteral("bodyMode")) && !obj.value(QStringLiteral("bodyMode")).isNull())
        item.email.bodyMode = obj.value(QStringLiteral("bodyMode")).toString();
    item.email.status = obj.value(QStringLiteral("status")).toString();
    item.email.atUtc = obj.value(QStringLiteral("atUtc")).toString();
    item.email.hasAttachments = obj.value(QStringLiteral("hasAttachments")).toBool();
    item.email.label = obj.value(QStringLiteral("label")).toString();

    // The message's real IMAP keywords. Never read before, which meant
    // Email::keywords was always empty from the relay -- so MailController's
    // keyword filter and the keyword pill row could never match anything a
    // server actually set. The anti-phishing banner reads $Phishing from here.
    // omitempty on the wire, so an absent key yields an empty list.
    const QJsonArray keywords = obj.value(QStringLiteral("keywords")).toArray();
    item.email.keywords.reserve(keywords.size());
    for (const QJsonValue& keyword : keywords)
        item.email.keywords.append(keyword.toString());

    // Both are `omitempty` on the wire, so absent means false/empty -- which
    // is exactly what toBool()/toString() yield for a missing key. See
    // core/domain/PgpMessageState.h for what the pair means once combined
    // with the body.
    item.email.pgpEncrypted = obj.value(QStringLiteral("pgpEncrypted")).toBool();
    item.email.pgpDecryptError = obj.value(QStringLiteral("pgpDecryptError")).toString();

    if (obj.contains(QStringLiteral("detail")))
        item.detail = obj.value(QStringLiteral("detail")).toString();
    if (obj.contains(QStringLiteral("changeType")))
        item.changeType = obj.value(QStringLiteral("changeType")).toString();

    return item;
}

ActionFailure actionFailureFromJson(const QJsonObject& obj)
{
    ActionFailure failure;
    failure.messageId = obj.value(QStringLiteral("messageId")).toString();
    failure.error = obj.value(QStringLiteral("error")).toString();
    return failure;
}

MailAttachmentInfo mailAttachmentInfoFromJson(const QJsonObject& obj)
{
    MailAttachmentInfo item;
    item.index = obj.value(QStringLiteral("index")).toInt();
    item.name = obj.value(QStringLiteral("name")).toString();
    item.mimeType = obj.value(QStringLiteral("mimeType")).toString();
    item.size = obj.value(QStringLiteral("size")).toInt();
    return item;
}

// Parses the filename out of a Content-Disposition header value shaped like
// `attachment; filename="<value>"`, with backslash-escaped quotes/
// backslashes inside the quoted string, per Go's mime.FormatMediaType (RFC
// 2045 quoted-string) -- confirmed as the only shape the backend emits, so
// this is deliberately not a general Content-Disposition/RFC 5987 parser.
QString filenameFromContentDisposition(const QString& headerValue)
{
    const QString marker = QStringLiteral("filename=\"");
    const int start = headerValue.indexOf(marker);
    if (start < 0)
        return {};

    QString filename;
    int i = start + marker.size();
    while (i < headerValue.size()) {
        const QChar ch = headerValue.at(i);
        if (ch == QLatin1Char('\\') && i + 1 < headerValue.size()) {
            filename += headerValue.at(i + 1);
            i += 2;
            continue;
        }
        if (ch == QLatin1Char('"'))
            break;
        filename += ch;
        ++i;
    }
    return filename;
}

// /api/mail/send and /api/mail/draft are decoded by the same backend
// function (decodeMailRequest), so they take a byte-identical body. Shared
// here rather than duplicated so a future field can't be added to one and
// forgotten on the other.
QJsonObject mailRequestBody(const QString& to, const QString& cc, const QString& bcc, const QString& subject,
                             const QString& body, const QString& mode,
                             const QVector<MailAttachmentUpload>& attachments)
{
    QJsonArray attachmentsJson;
    for (const MailAttachmentUpload& attachment : attachments) {
        QJsonObject attachmentJson;
        attachmentJson[QStringLiteral("name")] = attachment.name;
        attachmentJson[QStringLiteral("mimeType")] = attachment.mimeType;
        attachmentJson[QStringLiteral("dataBase64")] = QString::fromLatin1(attachment.data.toBase64());
        attachmentsJson.append(attachmentJson);
    }

    QJsonObject requestBody;
    requestBody[QStringLiteral("to")] = to;
    requestBody[QStringLiteral("cc")] = cc;
    requestBody[QStringLiteral("bcc")] = bcc;
    requestBody[QStringLiteral("subject")] = subject;
    requestBody[QStringLiteral("body")] = body;
    requestBody[QStringLiteral("mode")] = mode;
    requestBody[QStringLiteral("attachments")] = attachmentsJson;
    return requestBody;
}

} // namespace

RelayMailSource::RelayMailSource(HttpClient& httpClient)
    : m_httpClient(httpClient)
{
}

InboxFetchResult RelayMailSource::fetchInbox(const QUrl& serverBaseUrl, const RelayAuth& auth,
                                              std::optional<int> limit, const QString& mailbox,
                                              std::optional<qint64> since) const
{
    QList<QPair<QString, QString>> query;
    if (limit.has_value())
        query.append({ QStringLiteral("limit"), QString::number(*limit) });
    query.append({ QStringLiteral("mailbox"), mailbox });
    if (since.has_value())
        query.append({ QStringLiteral("since"), QString::number(*since) });

    // The largest response this client ever reads, and the one route that
    // needs more than HttpClient's default ceiling: a full window is up to
    // `maxInboxLimit` (500) messages carrying complete HTML bodies, which the
    // relay does not truncate. Named here rather than raising the global
    // default, so every other route keeps the tight one.
    const HttpClient::HttpResult result = m_httpClient.get(
        joinUrlPath(serverBaseUrl, QStringLiteral("api/inbox")), query, auth.headerItems(), {},
        HttpClient::kMaxInboxResponseBytes);

    InboxFetchResult out;
    if (result.error.has_value()) {
        out.error = result.error;
        out.detail = !result.detail.isEmpty() ? result.detail
                                               : QStringLiteral("Inbox fetch failed with status %1").arg(result.statusCode);
        return out;
    }

    QString errorString;
    const std::optional<QJsonObject> decoded = decodeJsonObject(result.body, &errorString);
    if (!decoded.has_value()) {
        out.error = NetworkError::Decoding;
        out.detail = QStringLiteral("Failed to decode inbox response: %1").arg(errorString);
        return out;
    }

    const QJsonObject json = *decoded;
    for (const QJsonValue& tab : json.value(QStringLiteral("tabs")).toArray())
        out.tabs.append(tab.toString());

    const QJsonObject byTab = json.value(QStringLiteral("byTab")).toObject();
    for (auto it = byTab.constBegin(); it != byTab.constEnd(); ++it) {
        QVector<InboxEmailItem> items;
        const QJsonArray array = it.value().toArray();
        items.reserve(array.size());
        for (const QJsonValue& value : array) {
            InboxEmailItem item = inboxItemFromJson(value.toObject());
            item.email.folder = it.key();
            items.append(item);
        }
        out.byTab.insert(it.key(), items);
    }

    out.isDelta = json.value(QStringLiteral("delta")).toBool();
    out.cursor = static_cast<qint64>(json.value(QStringLiteral("cursor")).toDouble());
    for (const QJsonValue& value : json.value(QStringLiteral("removed")).toArray())
        out.removed.append(value.toString());

    return out;
}

ActionResult RelayMailSource::performAction(const QUrl& serverBaseUrl, const RelayAuth& auth, const QString& action,
                                             const QStringList& messageIds, const QString& mailbox,
                                             const std::optional<QString>& targetMailbox) const
{
    QJsonObject body;
    body[QStringLiteral("action")] = action;
    body[QStringLiteral("messageIds")] = QJsonArray::fromStringList(messageIds);
    body[QStringLiteral("mailbox")] = mailbox;
    if (targetMailbox.has_value())
        body[QStringLiteral("targetMailbox")] = *targetMailbox;

    const HttpClient::HttpResult result = m_httpClient.post(
        joinUrlPath(serverBaseUrl, QStringLiteral("api/inbox/actions")), {}, body, auth.headerItems());

    ActionResult out;
    if (result.error.has_value()) {
        out.error = result.error;
        out.detail = !result.detail.isEmpty()
            ? result.detail
            : QStringLiteral("Inbox action failed with status %1").arg(result.statusCode);
        return out;
    }

    QString errorString;
    const std::optional<QJsonObject> decoded = decodeJsonObject(result.body, &errorString);
    if (!decoded.has_value()) {
        out.error = NetworkError::Decoding;
        out.detail = QStringLiteral("Failed to decode inbox action response: %1").arg(errorString);
        return out;
    }

    const QJsonObject json = *decoded;
    out.ok = json.value(QStringLiteral("ok")).toBool();
    out.action = json.value(QStringLiteral("action")).toString();
    out.processed = json.value(QStringLiteral("processed")).toInt();
    out.targetMailbox = json.value(QStringLiteral("targetMailbox")).toString();
    for (const QJsonValue& value : json.value(QStringLiteral("failed")).toArray())
        out.failed.append(actionFailureFromJson(value.toObject()));

    return out;
}

SendMailResult RelayMailSource::sendMail(const QUrl& serverBaseUrl, const RelayAuth& auth, const QString& to,
                                          const QString& cc, const QString& bcc, const QString& subject,
                                          const QString& body, const QString& mode,
                                          const QVector<MailAttachmentUpload>& attachments, bool sign, bool encrypt,
                                          bool allowPickupFallback) const
{
    QJsonObject requestBody = mailRequestBody(to, cc, bcc, subject, body, mode, attachments);
    // Inserted here rather than inside mailRequestBody() because saveDraft()
    // shares that helper and the draft handler ignores these fields.
    requestBody.insert(QStringLiteral("sign"), sign);
    requestBody.insert(QStringLiteral("encrypt"), encrypt);
    requestBody.insert(QStringLiteral("allowPickupFallback"), allowPickupFallback);

    const HttpClient::HttpResult result = m_httpClient.post(
        joinUrlPath(serverBaseUrl, QStringLiteral("api/mail/send")), {}, requestBody, auth.headerItems());

    SendMailResult out;
    if (result.error.has_value()) {
        out.error = result.error;

        // The backend's failures carry a human-readable `error` string in a
        // JSON body; HttpClient only fills `detail` for transport-level
        // failures, so without this every rejection surfaced as the useless
        // "Mail send failed with status 409". Decoding is best-effort: a
        // non-JSON body (proxy error page, empty 502) just falls through to
        // the status-code wording.
        QString bodyError;
        QString decodeError;
        const std::optional<QJsonObject> errorJson = decodeJsonObject(result.body, &decodeError);
        if (errorJson.has_value()) {
            bodyError = errorJson->value(QStringLiteral("error")).toString();
            // Order matches the server's own: a client-custody account is
            // refused at server.go:1207, before the keyless gate at :1272, so
            // the two never arrive together. Checking clientSideNeeded first
            // means a future server that did send both still reports the
            // unrecoverable one.
            out.clientSideNeeded = errorJson->value(QStringLiteral("clientSideNeeded")).toBool();
            if (!out.clientSideNeeded) {
                const QJsonArray keyless = errorJson->value(QStringLiteral("keylessRecipients")).toArray();
                for (const QJsonValue& value : keyless) {
                    const QString address = value.toString();
                    if (!address.isEmpty())
                        out.keylessRecipients.append(address);
                }
                // Driven by the field's presence, not by pickupFallbackAvailable:
                // the server sets that to a constant true, so treating it as the
                // trigger would add a dependency on a value that carries no
                // information.
                out.pickupFallbackNeeded = !out.keylessRecipients.isEmpty();
            }
        }

        if (!result.detail.isEmpty())
            out.detail = result.detail;
        else if (!bodyError.isEmpty())
            out.detail = bodyError;
        else
            out.detail = QStringLiteral("Mail send failed with status %1").arg(result.statusCode);
        return out;
    }

    QString errorString;
    const std::optional<QJsonObject> decoded = decodeJsonObject(result.body, &errorString);
    if (!decoded.has_value()) {
        out.error = NetworkError::Decoding;
        out.detail = QStringLiteral("Failed to decode mail send response: %1").arg(errorString);
        return out;
    }

    const QJsonObject json = *decoded;
    out.ok = json.value(QStringLiteral("ok")).toBool();
    out.sentSaved = json.value(QStringLiteral("sentSaved")).toBool();
    out.warning = json.value(QStringLiteral("warning")).toString();
    return out;
}

SaveDraftResult RelayMailSource::saveDraft(const QUrl& serverBaseUrl, const RelayAuth& auth, const QString& to,
                                            const QString& cc, const QString& bcc, const QString& subject,
                                            const QString& body, const QString& mode,
                                            const QVector<MailAttachmentUpload>& attachments) const
{
    const HttpClient::HttpResult result = m_httpClient.post(
        joinUrlPath(serverBaseUrl, QStringLiteral("api/mail/draft")), {},
        mailRequestBody(to, cc, bcc, subject, body, mode, attachments), auth.headerItems());

    SaveDraftResult out;
    if (result.error.has_value()) {
        out.error = result.error;
        // Unlike /api/mail/send, every failure here is http.Error -- a
        // plain-text body, not JSON. The body is already the human-readable
        // message ("imap configuration is required before saving drafts"),
        // so prefer it, capped so a proxy's HTML error page can't become the
        // toast.
        const QString bodyText = QString::fromUtf8(result.body).trimmed();
        if (!result.detail.isEmpty())
            out.detail = result.detail;
        else if (!bodyText.isEmpty() && !bodyText.startsWith(QLatin1Char('<')) && bodyText.size() <= 200)
            out.detail = bodyText;
        else
            out.detail = QStringLiteral("Draft save failed with status %1").arg(result.statusCode);
        return out;
    }

    QString errorString;
    const std::optional<QJsonObject> decoded = decodeJsonObject(result.body, &errorString);
    if (!decoded.has_value()) {
        out.error = NetworkError::Decoding;
        out.detail = QStringLiteral("Failed to decode draft response: %1").arg(errorString);
        return out;
    }

    out.ok = decoded->value(QStringLiteral("ok")).toBool();
    return out;
}

ListAttachmentsResult RelayMailSource::listAttachments(const QUrl& serverBaseUrl, const RelayAuth& auth,
                                                         const QString& mailbox, const QString& messageId) const
{
    QList<QPair<QString, QString>> query;
    query.append({ QStringLiteral("mailbox"), mailbox });
    query.append({ QStringLiteral("messageId"), messageId });

    const HttpClient::HttpResult result = m_httpClient.get(
        joinUrlPath(serverBaseUrl, QStringLiteral("api/mail/attachments")), query, auth.headerItems());

    ListAttachmentsResult out;
    if (result.error.has_value()) {
        out.error = result.error;
        out.detail = !result.detail.isEmpty()
            ? result.detail
            : QStringLiteral("Attachment list failed with status %1").arg(result.statusCode);
        return out;
    }

    QString errorString;
    const std::optional<QJsonObject> decoded = decodeJsonObject(result.body, &errorString);
    if (!decoded.has_value()) {
        out.error = NetworkError::Decoding;
        out.detail = QStringLiteral("Failed to decode attachment list response: %1").arg(errorString);
        return out;
    }

    const QJsonObject json = *decoded;
    for (const QJsonValue& value : json.value(QStringLiteral("attachments")).toArray())
        out.attachments.append(mailAttachmentInfoFromJson(value.toObject()));

    return out;
}

DownloadAttachmentResult RelayMailSource::downloadAttachment(const QUrl& serverBaseUrl, const RelayAuth& auth,
                                                               const QString& mailbox, const QString& messageId,
                                                               int index) const
{
    QList<QPair<QString, QString>> query;
    query.append({ QStringLiteral("mailbox"), mailbox });
    query.append({ QStringLiteral("messageId"), messageId });
    query.append({ QStringLiteral("index"), QString::number(index) });

    // The other exception: one attachment, bounded by the relay's own 25 MB
    // cap. The attachment LIST above deliberately keeps the default -- it
    // returns names and sizes, not content.
    const HttpClient::HttpResult result = m_httpClient.get(
        joinUrlPath(serverBaseUrl, QStringLiteral("api/mail/attachment")), query, auth.headerItems(), {},
        HttpClient::kMaxAttachmentResponseBytes);

    DownloadAttachmentResult out;
    if (result.error.has_value()) {
        out.error = result.error;
        out.detail = !result.detail.isEmpty()
            ? result.detail
            : QStringLiteral("Attachment download failed with status %1").arg(result.statusCode);
        return out;
    }

    // Not JSON -- the body is the attachment's raw bytes verbatim, filename/
    // mime type come from the response headers instead (see
    // filenameFromContentDisposition above).
    out.data = result.body;
    for (const auto& header : result.headers) {
        if (header.first.compare(QStringLiteral("Content-Type"), Qt::CaseInsensitive) == 0)
            out.mimeType = header.second;
        else if (header.first.compare(QStringLiteral("Content-Disposition"), Qt::CaseInsensitive) == 0)
            out.filename = filenameFromContentDisposition(header.second);
    }
    return out;
}
