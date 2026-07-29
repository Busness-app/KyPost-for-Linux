#include "net/NativeRegistrationClient.h"

#include "net/HttpClient.h"

#include <QJsonObject>

namespace {

// Reconstructs {scheme}://{host}[:port]/api/notifications/native/pull from
// the registration endpoint's authority, for when the server response omits
// pullEndpoint. Mirrors kypost-for-Mac's resolvedPullEndpoint(srv:).
QString derivePullEndpoint(const QUrl& registrationEndpoint)
{
    QUrl pull;
    pull.setScheme(registrationEndpoint.scheme());
    pull.setHost(registrationEndpoint.host());
    if (registrationEndpoint.port() != -1)
        pull.setPort(registrationEndpoint.port());
    pull.setPath(QStringLiteral("/api/notifications/native/pull"));
    return pull.toString();
}

} // namespace

NativeRegistrationClient::NativeRegistrationClient(HttpClient& httpClient)
    : m_httpClient(httpClient)
{
}

NativeRegistrationResult NativeRegistrationClient::registerDevice(
    const QUrl& registrationEndpoint, const QString& subscriberId, const QString& pairingToken,
    const QString& deviceToken, const QString& deviceId, const QString& deviceName) const
{
    QJsonObject body;
    body[QStringLiteral("subscriberId")] = subscriberId;
    body[QStringLiteral("pairingToken")] = pairingToken;
    body[QStringLiteral("deviceToken")] = deviceToken;
    if (!deviceId.isEmpty())
        body[QStringLiteral("deviceId")] = deviceId;
    if (!deviceName.isEmpty())
        body[QStringLiteral("deviceName")] = deviceName;
    body[QStringLiteral("platform")] = QStringLiteral("linux");
    body[QStringLiteral("transport")] = QStringLiteral("unifiedpush");

    // No query-param auth on this endpoint, unlike every other Relay
    // endpoint in this batch — every credential rides in the JSON body.
    const HttpClient::HttpResult result = m_httpClient.post(registrationEndpoint, {}, body);

    NativeRegistrationResult out;
    out.peerSpkiSha256 = result.peerSpkiSha256;

    if (result.error.has_value()) {
        switch (*result.error) {
        case NetworkError::Unauthorized:
            out.outcome = RegistrationOutcome::Unauthorized;
            return out;
        case NetworkError::ServiceUnavailable:
            out.outcome = RegistrationOutcome::BackendMisconfigured;
            return out;
        default:
            out.outcome = RegistrationOutcome::Failure;
            out.detail = !result.detail.isEmpty() ? result.detail
                                                    : QStringLiteral("Registration request failed with status %1")
                                                          .arg(result.statusCode);
            return out;
        }
    }

    QString errorString;
    const std::optional<QJsonObject> decoded = decodeJsonObject(result.body, &errorString);
    if (!decoded.has_value()) {
        out.outcome = RegistrationOutcome::Failure;
        out.detail = QStringLiteral("Failed to decode registration response: %1").arg(errorString);
        return out;
    }

    const QJsonObject json = *decoded;
    NativeRegistrationResponse response;
    response.ok = json.value(QStringLiteral("ok")).toBool();
    response.synced = json.value(QStringLiteral("synced")).toBool();
    response.deviceId = json.value(QStringLiteral("deviceId")).toString();
    response.deviceSecret = json.value(QStringLiteral("deviceSecret")).toString();
    response.devices = json.value(QStringLiteral("devices")).toInt();
    response.deliveryMode = json.value(QStringLiteral("deliveryMode")).toString();
    response.pullEndpoint = json.value(QStringLiteral("pullEndpoint")).toString();
    if (response.pullEndpoint.isEmpty())
        response.pullEndpoint = derivePullEndpoint(registrationEndpoint);
    response.transport = json.value(QStringLiteral("transport")).toString();

    // A 200 carrying *any* JSON object used to count as Success: `ok` was
    // parsed and never read, and an empty deviceId/deviceSecret was persisted
    // as-is. Combined with the registration URL's path being unconstrained,
    // that let one deep link -- naming the user's own real mail server, so
    // the confirm dialog showed nothing amiss -- POST to an unrelated
    // same-origin endpoint (the relay's unauthenticated /api/health answers
    // POST with a JSON object) and overwrite the working device credential
    // with empty strings. Every later relay call then 401s while the UI
    // still reports "Paired", and the pairing token has been consumed.
    // Refuse before the caller can persist anything.
    if (!response.ok || response.deviceId.isEmpty() || response.deviceSecret.isEmpty()) {
        out.outcome = RegistrationOutcome::Failure;
        out.detail = QStringLiteral("The server did not return a usable device credential.");
        return out;
    }

    out.outcome = RegistrationOutcome::Success;
    out.response = response;
    return out;
}
