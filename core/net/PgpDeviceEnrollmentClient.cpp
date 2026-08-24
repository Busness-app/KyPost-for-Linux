#include "net/PgpDeviceEnrollmentClient.h"

#include "net/ContactSyncClient.h"
#include "net/HttpClient.h"
#include "net/RelayAuth.h"

#include <QJsonObject>

namespace {
DeviceEnrollmentCallResult statusOf(const HttpClient::HttpResult& result)
{
    DeviceEnrollmentCallResult out;
    if (!result.error.has_value()) {
        out.status = DeviceEnrollmentCallStatus::Ok;
        return out;
    }
    switch (result.statusCode) {
    case 401: out.status = DeviceEnrollmentCallStatus::Unauthorized; break;
    case 404: out.status = DeviceEnrollmentCallStatus::NotFound; break;
    case 429: out.status = DeviceEnrollmentCallStatus::RateLimited; break;
    default: out.status = DeviceEnrollmentCallStatus::Failed; break;
    }
    out.detail = result.detail;
    return out;
}
}

PgpDeviceEnrollmentClient::PgpDeviceEnrollmentClient(HttpClient& httpClient) : m_httpClient(httpClient) {}

DeviceEnrollmentCallResult PgpDeviceEnrollmentClient::publishKey(
    const QUrl& serverBaseUrl, const RelayAuth& auth, const QByteArray& publicKeyBase64) const
{
    return statusOf(m_httpClient.post(
        joinUrlPath(serverBaseUrl, QStringLiteral("api/pgp/device/enrollment-key")), {},
        QJsonObject{{QStringLiteral("publicKey"), QString::fromLatin1(publicKeyBase64)}}, auth.headerItems()));
}

DeviceEnrollmentCallResult PgpDeviceEnrollmentClient::fetchEnvelope(
    const QUrl& serverBaseUrl, const RelayAuth& auth) const
{
    const HttpClient::HttpResult result = m_httpClient.get(
        joinUrlPath(serverBaseUrl, QStringLiteral("api/pgp/device/envelope")), {}, auth.headerItems());
    DeviceEnrollmentCallResult out = statusOf(result);
    if (out.status != DeviceEnrollmentCallStatus::Ok)
        return out;
    QString decodeError;
    const std::optional<QJsonObject> decoded = decodeJsonObject(result.body, &decodeError);
    if (!decoded.has_value()) {
        out.status = DeviceEnrollmentCallStatus::Failed;
        out.detail = decodeError;
        return out;
    }
    out.envelope = decoded->value(QStringLiteral("envelope")).toString().toUtf8();
    if (out.envelope.isEmpty()) {
        out.status = DeviceEnrollmentCallStatus::Failed;
        out.detail = QStringLiteral("empty device envelope");
    }
    return out;
}

DeviceEnrollmentCallResult PgpDeviceEnrollmentClient::reportState(
    const QUrl& serverBaseUrl, const RelayAuth& auth, bool enrolled) const
{
    return statusOf(m_httpClient.post(
        joinUrlPath(serverBaseUrl, QStringLiteral("api/pgp/device/enrollment-state")), {},
        QJsonObject{{QStringLiteral("encryptionEnrolled"), enrolled}}, auth.headerItems()));
}
