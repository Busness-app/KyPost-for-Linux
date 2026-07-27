#include "domain/DeviceRegistrationService.h"

#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "net/HttpClient.h"
#include "security/CredentialCipher.h"
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

    // Captured BEFORE the network call. The check above is only true at the
    // moment it runs: HttpClient blocks on a nested QEventLoop, so QML keeps
    // delivering events and a window minimise reaches AppLock.lockNow(),
    // which drops the session key mid-flight. save() then failed its own
    // guard and this function used to respond by clearing the whole pairing.
    const CredentialCipher::SessionKey sealingKey = m_pairingStore.sealingKeySnapshot();

    // The registration is what establishes a new trust anchor, so the old
    // pin must not be allowed to abort it -- that is what made the
    // certificate-mismatch banner's own "unpair and pair again" advice
    // impossible to follow without restarting the process.
    m_httpClient.clearCertificatePin();

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
    // Trust on first use: pin the key that served THIS registration, read
    // from the reply itself. Reading a shared "last handshake seen" value on
    // HttpClient instead meant a pooled keep-alive connection (the steady
    // state, given the 90 s poll) left it holding whatever host handshook
    // most recently -- so a scanned QR code aimed at an attacker's server
    // could decide what the next unattended re-registration pinned as the
    // relay's key, permanently and across restarts.
    //
    // Empty over plain http (no handshake, so nothing to pin), which is the
    // testing case -- enforcement then stays off rather than failing every
    // later request.
    pairing.certificateSpkiSha256 = QString::fromLatin1(result.peerSpkiSha256.toBase64());

    // Checked, not fire-and-forget. SecureStoreKeychain::set() returns false
    // whenever no Secret Service provider is running -- a bare WM session, a
    // locked wallet, a Flatpak on a host without gnome-keyring/kwalletd --
    // which is not exotic on Linux. Ignoring it meant the server minted and
    // burned a one-shot deviceSecret, nothing reached disk, and the UI still
    // reported "paired". The next launch was unpaired, and re-pairing then
    // failed too because the pairing token had already been consumed.
    if (!m_pairingStore.save(pairing, sealingKey)) {
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
    // session are checked rather than waiting for the next launch. Scoped to
    // the paired server's origin: the pin describes that relay, and enforcing
    // it on the deliberately cross-server PGP QR fetch only ever produced a
    // false "your mail server is being impersonated" alarm.
    if (!result.peerSpkiSha256.isEmpty())
        m_httpClient.setCertificatePin(result.peerSpkiSha256, QUrl(params.serverBaseUrl));

    const QUrl serverOrigin(params.serverBaseUrl);
    const QUrl advertisedPullEndpoint(result.response.pullEndpoint);
    const QString pullEndpoint = (!result.response.pullEndpoint.isEmpty()
                                   && sameUrlOrigin(advertisedPullEndpoint, serverOrigin))
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
