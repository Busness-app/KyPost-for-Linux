#include "domain/DeviceRegistrationService.h"

#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "net/HttpClient.h"
#include "stores/SettingsStore.h"

#include <QUrl>

namespace {

// Fallback used when the registration response's pullEndpoint is missing or
// fails the sameOrigin() check below -- mirrors NativeRegistrationClient's
// own derivePullEndpoint(), but anchored to the server the user actually
// paired with rather than the (possibly different-origin) registration URL.
QString derivePullEndpoint(const QUrl& serverBaseUrl)
{
    QUrl pull;
    pull.setScheme(serverBaseUrl.scheme());
    pull.setHost(serverBaseUrl.host());
    if (serverBaseUrl.port() != -1)
        pull.setPort(serverBaseUrl.port());
    pull.setPath(QStringLiteral("/api/notifications/native/pull"));
    return pull.toString();
}

// VibeSec finding: pullEndpoint used to be trusted verbatim from the
// registration response and persisted, then hit on every future poll with
// the device's real deviceId/deviceSecret attached
// (PushNotificationClient::pull). A single malicious or compromised
// response from an otherwise-trusted relay could silently redirect all
// future credentialed polling to an arbitrary host, persistently, until
// re-pairing. Only accept a pullEndpoint that shares scheme+host+port with
// the server the user actually paired with.
bool sameOrigin(const QUrl& a, const QUrl& b)
{
    return a.scheme() == b.scheme() && a.host() == b.host() && a.port() == b.port();
}

} // namespace

DeviceRegistrationService::DeviceRegistrationService(NativeRegistrationClient& client, PairingStore& pairingStore,
                                                       SettingsStore& settingsStore, HttpClient& httpClient)
    : m_client(client)
    , m_pairingStore(pairingStore)
    , m_settingsStore(settingsStore)
    , m_httpClient(httpClient)
{
}

NativeRegistrationResult DeviceRegistrationService::pair(const PairingParams& params, const QString& deviceToken)
{
    const NativeRegistrationResult result = m_client.registerDevice(
        QUrl(params.registrationUrl), params.subscriberId, params.pairingToken, deviceToken, QString(), params.deviceName);

    if (result.outcome != RegistrationOutcome::Success)
        return result;

    DevicePairing pairing;
    pairing.subscriberId = params.subscriberId;
    pairing.serverBaseUrl = params.serverBaseUrl;
    pairing.registrationUrl = params.registrationUrl;
    pairing.pairingToken = params.pairingToken;
    pairing.deviceId = result.response.deviceId;
    pairing.deviceName = params.deviceName;
    // Every successful register mints a brand-new secret server-side,
    // invalidating whatever was stored before -- persist unconditionally,
    // never fall back to the previous value.
    pairing.deviceSecret = result.response.deviceSecret;
    // Trust on first use: pin whatever key just served the registration.
    // Empty over plain http (no handshake, so nothing to pin), which is the
    // testing case -- enforcement then stays off rather than failing every
    // later request.
    pairing.certificateSpkiSha256 = QString::fromLatin1(m_httpClient.lastPeerSpkiSha256().toBase64());
    m_pairingStore.save(pairing);

    // Enforce immediately, so even the requests made later in this same
    // session are checked rather than waiting for the next launch.
    if (!pairing.certificateSpkiSha256.isEmpty())
        m_httpClient.setCertificatePin(m_httpClient.lastPeerSpkiSha256());

    const QUrl serverOrigin(params.serverBaseUrl);
    const QUrl advertisedPullEndpoint(result.response.pullEndpoint);
    const QString pullEndpoint = (!result.response.pullEndpoint.isEmpty()
                                   && sameOrigin(advertisedPullEndpoint, serverOrigin))
        ? result.response.pullEndpoint
        : derivePullEndpoint(serverOrigin);

    m_settingsStore.setDeliveryMode(result.response.deliveryMode);
    m_settingsStore.setTransport(result.response.transport);
    m_settingsStore.setPullEndpoint(pullEndpoint);

    return result;
}

std::optional<NativeRegistrationResult> DeviceRegistrationService::reregisterIfPaired(const QString& deviceToken)
{
    const std::optional<DevicePairing> pairing = m_pairingStore.load();
    if (!pairing.has_value())
        return std::nullopt;

    PairingParams params;
    params.subscriberId = pairing->subscriberId;
    params.serverBaseUrl = pairing->serverBaseUrl;
    params.registrationUrl = pairing->registrationUrl;
    params.pairingToken = pairing->pairingToken;
    params.deviceName = pairing->deviceName;

    return pair(params, deviceToken);
}
