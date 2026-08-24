#pragma once

#include <QString>

#include "net/NetworkError.h"

#include <QByteArray>
#include <QUrl>
#include <optional>

class HttpClient;
struct RelayAuth;

enum class DeviceEnrollmentCallStatus { Ok, NotFound, Unauthorized, RateLimited, Failed };

struct DeviceEnrollmentCallResult
{
    DeviceEnrollmentCallStatus status = DeviceEnrollmentCallStatus::Failed;
    QByteArray envelope;
    QString detail;
};

class PgpDeviceEnrollmentClient
{
public:
    explicit PgpDeviceEnrollmentClient(HttpClient& httpClient);
    DeviceEnrollmentCallResult publishKey(const QUrl& serverBaseUrl, const RelayAuth& auth,
                                           const QByteArray& publicKeyBase64) const;
    DeviceEnrollmentCallResult fetchEnvelope(const QUrl& serverBaseUrl, const RelayAuth& auth) const;
    DeviceEnrollmentCallResult reportState(const QUrl& serverBaseUrl, const RelayAuth& auth,
                                            bool enrolled) const;
private:
    HttpClient& m_httpClient;
};
