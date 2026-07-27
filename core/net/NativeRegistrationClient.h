#pragma once

#include "net/NetworkError.h"

#include <QString>
#include <QUrl>

class HttpClient;

// Response shape from `POST /api/notifications/native/register`, verified
// against the Go backend's handleNotificationNativeRegister (see Task 14
// brief for line references). pullEndpoint is populated on the client side
// with a derived value (scheme+host+port of the registration endpoint plus
// the well-known pull path) when the server omits it, mirroring
// kypost-for-Mac's resolvedPullEndpoint(srv:) — callers never see an
// empty pullEndpoint on Success.
struct NativeRegistrationResponse
{
    bool ok = false;
    bool synced = false;
    QString deviceId;
    // The raw per-device pairing secret, minted fresh on every successful
    // registration and returned only in this response -- never retrievable
    // again afterward. Callers must persist it unconditionally, overwriting
    // any prior value (see DeviceRegistrationService::pair()).
    QString deviceSecret;
    int devices = 0;
    QString deliveryMode; // "push" or "pull"
    QString pullEndpoint;
    QString transport; // server-normalized value, not necessarily what was sent

    bool operator==(const NativeRegistrationResponse&) const = default;
};

// Mirrors kypost-for-Mac's RegistrationOutcome. Unauthorized <-
// NetworkError::Unauthorized (401/403); BackendMisconfigured <-
// NetworkError::ServiceUnavailable (503, meaning PAIRING_SECRET isn't set
// server-side — persistent, don't retry); Failure covers everything else
// (Conflict/RateLimited/Server/Transport/InvalidUrl and local JSON decode
// failures).
// CredentialsLocked is the one value this client itself never produces --
// DeviceRegistrationService::pair() returns it WITHOUT making any request,
// when the credential PIN gate is on and the app is locked, so the rotated
// secret a successful register would mint could not be re-sealed. It is a
// deferral, not an error: nothing is wrong with the pairing, and retrying
// after the user unlocks succeeds.
enum class RegistrationOutcome
{
    Success,
    Unauthorized,
    BackendMisconfigured,
    Failure,
    CredentialsLocked,
};

struct NativeRegistrationResult
{
    RegistrationOutcome outcome = RegistrationOutcome::Failure;
    NativeRegistrationResponse response; // meaningful only when outcome == Success
    QString detail;                      // meaningful only when outcome == Failure
};

// Registers this device for native push with the Relay backend. Verified
// against internal/api/server.go's handleNotificationNativeRegister (see
// Task 14 brief) — this endpoint takes no query-param auth, unlike every
// other Relay endpoint in this batch; all fields ride in the JSON body.
//
// platform ("linux") and transport ("unifiedpush") are hardcoded inside
// register() rather than taken as parameters: this client only ever builds
// for one platform/transport pair, so there is nothing for a caller to vary.
class NativeRegistrationClient
{
public:
    explicit NativeRegistrationClient(HttpClient& httpClient);

    NativeRegistrationResult registerDevice(const QUrl& registrationEndpoint, const QString& subscriberId,
                                             const QString& pairingToken, const QString& deviceToken,
                                             const QString& deviceId, const QString& deviceName) const;

private:
    HttpClient& m_httpClient;
};
