#include "net/ClientVersionClient.h"

// joinUrlPath is declared in HttpClient.h (:380), not in a header of its own.
#include "net/HttpClient.h"
#include "net/RelayAuth.h"

#include <QJsonDocument>
#include <QJsonObject>

ClientVersionClient::ClientVersionClient(HttpClient& httpClient)
    : m_httpClient(httpClient)
{
}

ClientVersionResult ClientVersionClient::fetch(const QUrl& serverBaseUrl, const RelayAuth& auth) const
{
    const HttpClient::HttpResult result = m_httpClient.get(
        joinUrlPath(serverBaseUrl, QStringLiteral("api/client/version")), {}, auth.headerItems());

    ClientVersionResult out;

    // Checked before the error branch. HttpClient::waitForReply() maps ANY
    // received HTTP status (statusCode != 0) to result.error via
    // networkErrorFromStatusCode() -- 404 included -- but it sets
    // result.statusCode unconditionally first, from the same reply
    // attribute, regardless of that mapping. So a 404 still carries
    // statusCode == 404 alongside a populated error, and this check on the
    // status code (verified against HttpClient.cpp) reclassifies it as "no
    // such endpoint" before the generic error branch below would otherwise
    // treat an older server as a network failure.
    if (result.statusCode == 404) {
        out.supported = false;
        return out;
    }
    if (result.error.has_value()) {
        out.error = result.error;
        return out;
    }

    const QJsonObject json = QJsonDocument::fromJson(result.body).object();
    out.latestVersion = json.value(QStringLiteral("latestVersion")).toString();
    out.checkedAt = json.value(QStringLiteral("checkedAt")).toString();
    return out;
}
