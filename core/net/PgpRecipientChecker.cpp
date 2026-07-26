#include "net/PgpRecipientChecker.h"

#include "net/HttpClient.h"
#include "net/RelayAuth.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

PgpRecipientChecker::PgpRecipientChecker(HttpClient& httpClient)
    : m_httpClient(httpClient)
{
}

RecipientKeyCheckResult PgpRecipientChecker::check(const QUrl& serverBaseUrl, const RelayAuth& auth,
                                                     const QStringList& addresses) const
{
    const QJsonObject body{ { QStringLiteral("addresses"), QJsonArray::fromStringList(addresses) } };
    const HttpClient::HttpResult result = m_httpClient.post(
        joinUrlPath(serverBaseUrl, QStringLiteral("api/pgp/recipients/check")), {}, body, auth.headerItems());

    RecipientKeyCheckResult out;

    if (result.error.has_value()) {
        out.error = result.error;
        out.detail = !result.detail.isEmpty()
            ? result.detail
            : QStringLiteral("PGP recipient check failed with status %1").arg(result.statusCode);
        return out;
    }

    QString errorString;
    const std::optional<QJsonObject> decoded = decodeJsonObject(result.body, &errorString);
    if (!decoded.has_value()) {
        out.error = NetworkError::Decoding;
        out.detail = QStringLiteral("Failed to decode PGP recipient check response: %1").arg(errorString);
        return out;
    }

    const QJsonObject obj = *decoded;
    out.ok = true;
    const QJsonArray results = obj.value(QStringLiteral("results")).toArray();
    for (const QJsonValue& value : results) {
        const QJsonObject entry = value.toObject();
        const QString address = entry.value(QStringLiteral("address")).toString();
        if (address.isEmpty())
            continue;
        if (!entry.value(QStringLiteral("hasKey")).toBool())
            out.keylessRecipients.append(address);
    }
    return out;
}
