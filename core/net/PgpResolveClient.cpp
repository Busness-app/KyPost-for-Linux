#include "net/PgpResolveClient.h"

#include "net/RelayAuth.h"

#include <QJsonArray>
#include <QJsonObject>

namespace {

constexpr int kConflict = 409;
constexpr int kPayloadTooLarge = 413;

ResolvedRecipientKey keyFromJson(const QJsonObject& object)
{
    ResolvedRecipientKey key;
    key.address = object.value(QStringLiteral("address")).toString();
    key.publicKey = object.value(QStringLiteral("publicKey")).toString();
    key.fingerprint = object.value(QStringLiteral("fingerprint")).toString();
    key.tier = object.value(QStringLiteral("tier")).toString();
    // Absent means false, which is the safe reading: `usable` is omitempty on
    // the wire, and a missing field must never be taken as "yes, encrypt to
    // this".
    key.usable = object.value(QStringLiteral("usable")).toBool();
    return key;
}

} // namespace

PgpResolveClient::PgpResolveClient(HttpClient& httpClient)
    : m_httpClient(httpClient)
{
}

PgpResolveResult PgpResolveClient::resolve(const QUrl& serverBaseUrl, const RelayAuth& auth,
                                            const QStringList& addresses) const
{
    PgpResolveResult out;

    QJsonArray wireAddresses;
    for (const QString& address : addresses)
        wireAddresses.append(address);

    QJsonObject body;
    body[QStringLiteral("addresses")] = wireAddresses;

    const HttpClient::HttpResult result = m_httpClient.post(
        joinUrlPath(serverBaseUrl, QStringLiteral("api/pgp/recipients/resolve")), {}, body,
        auth.headerItems());

    if (result.error.has_value()) {
        out.error = result.error;
        out.detail = !result.detail.isEmpty()
            ? result.detail
            : QStringLiteral("Recipient key lookup failed with status %1").arg(result.statusCode);
        if (result.statusCode == kConflict)
            out.status = PgpResolveStatus::ServerEncryptsInstead;
        else if (result.statusCode == kPayloadTooLarge)
            out.status = PgpResolveStatus::TooManyRecipients;
        else
            out.status = PgpResolveStatus::Failed;
        return out;
    }

    QString errorString;
    const std::optional<QJsonObject> decoded = decodeJsonObject(result.body, &errorString);
    if (!decoded.has_value()) {
        out.error = NetworkError::Decoding;
        out.detail = QStringLiteral("Failed to decode recipient key response: %1").arg(errorString);
        return out;
    }

    const QJsonArray results = decoded->value(QStringLiteral("results")).toArray();
    out.keys.reserve(results.size());
    for (const QJsonValue& value : results)
        out.keys.append(keyFromJson(value.toObject()));

    out.status = PgpResolveStatus::Resolved;
    return out;
}
