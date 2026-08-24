#include "net/PgpPayloadClient.h"

#include "net/RelayAuth.h"

#include <QJsonArray>
#include <QJsonObject>

namespace {

// HTTP statuses this endpoint gives specific meanings to. NetworkError maps
// 409 to Conflict but folds 404 and 413 into Server, so the code itself is
// what distinguishes them -- HttpResult::statusCode is populated on every
// path that got a response.
constexpr int kNotFound = 404;
constexpr int kConflict = 409;
constexpr int kPayloadTooLarge = 413;

} // namespace

PgpPayloadClient::PgpPayloadClient(HttpClient& httpClient)
    : m_httpClient(httpClient)
{
}

PgpPayloadResult PgpPayloadClient::fetch(const QUrl& serverBaseUrl, const RelayAuth& auth,
                                          const QString& mailbox, const QString& messageId) const
{
    QList<QPair<QString, QString>> query;
    query.append({ QStringLiteral("mailbox"), mailbox });
    query.append({ QStringLiteral("messageId"), messageId });

    // One message's ciphertext is the same class of object as one attachment,
    // and the relay bounds both the same way -- so this borrows the
    // attachment ceiling rather than inventing a second number that would
    // then have to be kept in step with it. The 8 MB default is not enough:
    // an armored message carrying a photo clears it easily, and the failure
    // would look to the user like a corrupt message rather than a large one.
    const HttpClient::HttpResult result
        = m_httpClient.get(joinUrlPath(serverBaseUrl, QStringLiteral("api/mail/pgp-payload")), query,
                            auth.headerItems(), {}, HttpClient::kMaxAttachmentResponseBytes);

    PgpPayloadResult out;

    if (result.error.has_value()) {
        out.error = result.error;
        out.detail = !result.detail.isEmpty()
            ? result.detail
            : QStringLiteral("PGP payload fetch failed with status %1").arg(result.statusCode);

        // Our own ceiling, checked before the status code for the same reason
        // HttpClient ranks it first: the response was aborted part-way, so
        // whatever status had arrived describes what the server was doing,
        // not what happened.
        if (result.error == NetworkError::ResponseTooLarge)
            out.status = PgpPayloadStatus::TooLarge;
        else if (result.statusCode == kPayloadTooLarge)
            out.status = PgpPayloadStatus::TooLarge;
        else if (result.statusCode == kNotFound)
            out.status = PgpPayloadStatus::NoCiphertext;
        else if (result.statusCode == kConflict)
            out.status = PgpPayloadStatus::ServerCustody;
        else
            out.status = PgpPayloadStatus::Failed;
        return out;
    }

    QString errorString;
    const std::optional<QJsonObject> decoded = decodeJsonObject(result.body, &errorString);
    if (!decoded.has_value()) {
        out.error = NetworkError::Decoding;
        out.detail = QStringLiteral("Failed to decode PGP payload response: %1").arg(errorString);
        return out;
    }

    out.encryptedPayload = decoded->value(QStringLiteral("encryptedPayload")).toString();
    out.resolvedSender = decoded->value(QStringLiteral("resolvedSender")).toString().trimmed();
    for (const QJsonValue& value : decoded->value(QStringLiteral("signerKeys")).toArray()) {
        const QJsonObject object = value.toObject();
        PgpSignerKey key;
        key.publicKey = object.value(QStringLiteral("publicKey")).toString();
        // Absent leaves this empty, which EncryptedMessageReader reads as
        // "do not import, do not credit" -- see PgpSignerKey::fingerprint.
        // Deliberately not derived from the key material: a fingerprint the
        // client computed from the same bytes it is checking cannot disagree
        // with them, so deriving it would turn the check into a tautology.
        key.fingerprint = object.value(QStringLiteral("fingerprint")).toString().trimmed();
        // Absent means false on the wire (omitempty), and false is the
        // permissive reading -- which is right here only because the relay
        // sets it when it knows of a conflict. Nothing is inferred.
        key.conflict = object.value(QStringLiteral("conflict")).toBool();
        if (!key.publicKey.trimmed().isEmpty())
            out.signerKeys.append(key);
    }
    // Blank ciphertext on a 200 is the signed-but-not-encrypted message. Not
    // a decode failure and not a fetch failure -- there is simply nothing on
    // this path to decrypt. Trimmed before the test because armor is
    // whitespace-delimited and a payload of nothing but newlines is not one.
    if (out.encryptedPayload.trimmed().isEmpty()) {
        out.encryptedPayload.clear();
        out.status = PgpPayloadStatus::NoCiphertext;
        return out;
    }

    out.status = PgpPayloadStatus::Fetched;
    return out;
}
