#include "net/PgpBootstrapClient.h"

#include "net/HttpClient.h"
#include "net/RelayAuth.h"

#include <QJsonArray>
#include <QJsonObject>

PgpBootstrapClient::PgpBootstrapClient(HttpClient& httpClient)
    : m_httpClient(httpClient)
{
}

PgpBootstrapResult PgpBootstrapClient::fetch(const QUrl& serverBaseUrl, const RelayAuth& auth) const
{
    const HttpClient::HttpResult result
        = m_httpClient.get(joinUrlPath(serverBaseUrl, QStringLiteral("api/pgp/bootstrap")), {}, auth.headerItems());

    PgpBootstrapResult out;

    if (result.error.has_value()) {
        out.error = result.error;
        out.detail = !result.detail.isEmpty()
            ? result.detail
            : QStringLiteral("PGP bootstrap fetch failed with status %1").arg(result.statusCode);
        return out;
    }

    QString errorString;
    const std::optional<QJsonObject> decoded = decodeJsonObject(result.body, &errorString);
    if (!decoded.has_value()) {
        out.error = NetworkError::Decoding;
        out.detail = QStringLiteral("Failed to decode PGP bootstrap response: %1").arg(errorString);
        return out;
    }

    const QJsonObject obj = *decoded;
    out.ok = true;
    out.hasIdentity = obj.value(QStringLiteral("hasIdentity")).toBool();
    out.protection = obj.value(QStringLiteral("protection")).toString();
    out.fingerprint = obj.value(QStringLiteral("fingerprint")).toString().trimmed();
    // Primary first, then any send-as aliases. Only the primary is taken:
    // this app has no send-as UI, and picking an alias the user did not choose
    // would put an address on their mail that they never selected.
    const QJsonArray userIds = obj.value(QStringLiteral("suggestedUserIDs")).toArray();
    if (!userIds.isEmpty())
        out.primaryAddress = userIds.first().toString().trimmed();
    return out;
}
