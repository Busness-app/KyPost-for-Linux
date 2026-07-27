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
    // Checked BEFORE the network call, not after. Every successful register
    // mints a new deviceSecret and retires the old one server-side, so
    // discovering afterwards that the new one cannot be stored would leave
    // the device holding a credential the relay no longer accepts.
    //
    // The case this guards is the credential PIN gate with the app locked:
    // the sealed blob can only be re-wrapped by a session that has opened it
    // (PairingStore::canResealDeviceSecret). Registering anyway is what used
    // to write the rotated secret out in plaintext next to a gate flag still
    // claiming it was sealed -- a silent, persistent downgrade of the one
    // control that is supposed to make a locked device useless. Deferring
    // costs a push-endpoint update until the user unlocks; main.cpp retries
    // this call on unlock.
    if (!m_pairingStore.canResealDeviceSecret()) {
        NativeRegistrationResult deferred;
        deferred.outcome = RegistrationOutcome::CredentialsLocked;
        return deferred;
    }

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

    // Checked, not fire-and-forget. SecureStoreKeychain::set() returns false
    // whenever no Secret Service provider is running -- a bare WM session, a
    // locked wallet, a Flatpak on a host without gnome-keyring/kwalletd --
    // which is not exotic on Linux. Ignoring it meant the server minted and
    // burned a one-shot deviceSecret, nothing reached disk, and the UI still
    // reported "paired". The next launch was unpaired, and re-pairing then
    // failed too because the pairing token had already been consumed.
    if (!m_pairingStore.save(pairing)) {
        NativeRegistrationResult persistFailed = result;
        persistFailed.outcome = RegistrationOutcome::Failure;
        persistFailed.detail = QStringLiteral(
            "Registered with the server, but the credentials could not be saved to this "
            "system's secret store. Check that a keyring service (gnome-keyring or "
            "kwallet) is running, then pair again.");
        // Leave nothing half-written: a partial record whose `sub` key
        // landed would make isPaired() true with an unusable secret. If even
        // the cleanup cannot write, say so -- "pair again" is bad advice for
        // a store that is going to refuse that too.
        if (!m_pairingStore.clear()) {
            persistFailed.detail += QStringLiteral(
                " The partially-written pairing record could also not be removed.");
        }
        return persistFailed;
    }

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
