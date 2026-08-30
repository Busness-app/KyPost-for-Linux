#include "domain/DeviceRegistrationService.h"

#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "net/CertificatePinSink.h"
#include "net/HttpClient.h"
#include "security/CredentialCipher.h"
#include "stores/SettingsStore.h"

#include <QLoggingCategory>

#include <utility>
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
                                                       SettingsStore& settingsStore, CertificatePinSink& pinSink)
    : m_client(client)
    , m_pairingStore(pairingStore)
    , m_settingsStore(settingsStore)
    , m_pinSink(pinSink)
{
}

// ---- PairAttempt -------------------------------------------------------
//
// Carries the pin suspension across the async gap. Destroying it without
// finishing restores the pin, which is the property ScopedPinSuspension used
// to give within one stack frame: an abandoned attempt -- executor shut
// down, controller gave up -- must not leave pinning off for the rest of the
// process.

DeviceRegistrationService::PairAttempt::~PairAttempt()
{
    if (m_restorePin && m_pinSink)
        m_pinSink->restorePin(m_savedPin);
}

DeviceRegistrationService::PairAttempt::PairAttempt(PairAttempt&& other) noexcept
{
    *this = std::move(other);
}

DeviceRegistrationService::PairAttempt&
DeviceRegistrationService::PairAttempt::operator=(PairAttempt&& other) noexcept
{
    if (this == &other)
        return *this;
    if (m_restorePin && m_pinSink)
        m_pinSink->restorePin(m_savedPin);
    m_pinSink = other.m_pinSink;
    m_savedPin = other.m_savedPin;
    m_sealingKey = other.m_sealingKey;
    m_refusedOutcome = other.m_refusedOutcome;
    m_restorePin = other.m_restorePin;
    // The moved-from attempt must not also restore.
    other.m_restorePin = false;
    other.m_pinSink = nullptr;
    return *this;
}

DeviceRegistrationService::PairAttempt DeviceRegistrationService::beginPair(const PairingParams& params)
{
    PairAttempt attempt;

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
    // control that is supposed to make a locked device useless.
    if (!m_pairingStore.canResealDeviceSecret()) {
        attempt.m_refusedOutcome = RegistrationOutcome::CredentialsLocked;
        return attempt;
    }

    // Captured BEFORE the request. Re-reading it afterwards is a TOCTOU: the
    // app can lock while the request is out, dropping the session key.
    attempt.m_sealingKey = m_pairingStore.sealingKeySnapshot();

    // The registration is what establishes a new trust anchor, so the old
    // pin must not be allowed to abort it -- that is what made the
    // certificate-mismatch banner's own "unpair and pair again" advice
    // impossible to follow without restarting the process.
    //
    // Either way the previous pin is saved first and ~PairAttempt puts it
    // back, so an attempt that is abandoned or fails cannot leave this device
    // less protected than it was.
    attempt.m_pinSink = &m_pinSink;
    attempt.m_savedPin = m_pinSink.pinState();
    attempt.m_restorePin = true;
    if (params.spkiPin.isEmpty()) {
        m_pinSink.clearPin();
    } else {
        // The link named the key. Arm it now, so the handshake carrying the
        // pairing token and the push credentials is checked before those
        // bytes are written -- HttpClient tests the pin on ::encrypted, which
        // fires after the handshake and before the request body goes out.
        //
        // Scoped to serverBaseUrl, the origin the confirm dialog showed and
        // the origin the parser already forced registrationUrl to share.
        m_pinSink.setPin(params.spkiPin, QUrl(params.serverBaseUrl));
    }
    return attempt;
}

NativeRegistrationResult DeviceRegistrationService::sendRegistration(HttpClient& httpClient,
                                                                       const PairingParams& params,
                                                                       const QString& deviceToken)
{
    // Constructed here rather than held as a member: NativeRegistrationClient
    // is a stateless wrapper over an HttpClient reference, and on the async
    // path that HttpClient belongs to the executor thread.
    NativeRegistrationClient client(httpClient);
    return client.registerDevice(QUrl(params.registrationUrl), params.subscriberId, params.pairingToken,
                                  deviceToken, QString(), params.deviceName);
}

NativeRegistrationResult DeviceRegistrationService::finishPair(PairAttempt attempt, const PairingParams& params,
                                                                const NativeRegistrationResult& result)
{
    if (result.outcome != RegistrationOutcome::Success)
        return result; // ~PairAttempt restores the pin

    // Do not reinterpret Qt's exported chain here. HttpClient already checked
    // the configured pin against the reply's verified chain; carry that fact
    // across the async boundary. A supplied HTTPS pin that was not enforced
    // is still a fail-closed condition.
    if (!params.spkiPin.isEmpty() && QUrl(params.serverBaseUrl).scheme() == QLatin1String("https")
        && !result.certificatePinVerified) {
        NativeRegistrationResult mismatch = result;
        mismatch.outcome = RegistrationOutcome::Failure;
        qWarning("DeviceRegistrationService: the registration did not verify the pairing link's "
                 "certificate pin; refusing to save this pairing");
        return mismatch; // ~PairAttempt restores the pin
    }

    // The key this pairing is anchored to. The link's pin wins: it was
    // enforced on the handshake that carried the registration, so a success
    // means the peer matched it, and taking it from here keeps a pooled
    // connection that produced no fresh handshake evidence
    // (peerSpkiSha256 empty) from downgrading a pinned pairing to none.
    const QByteArray anchorPin = !result.peerSpkiSha256.isEmpty() ? result.peerSpkiSha256 : params.spkiPin;

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
    // Without a link pin this is trust on first use: pin the key that served
    // THIS registration, read from the reply itself. Reading a shared "last
    // handshake seen" value on HttpClient instead meant a pooled keep-alive
    // connection left it holding whatever host handshook most recently -- so
    // a scanned QR code aimed at an attacker's server could decide what the
    // next unattended re-registration pinned as the relay's key.
    //
    // Empty over plain http (no handshake, so nothing to pin), which is the
    // testing case -- enforcement then stays off rather than failing every
    // later request.
    pairing.certificateSpkiSha256 = QString::fromLatin1(anchorPin.toBase64());

    // Checked, not fire-and-forget. SecureStoreKeychain::set() returns false
    // whenever no Secret Service provider is running, which is not exotic on
    // Linux. Ignoring it meant the server minted and burned a one-shot
    // deviceSecret, nothing reached disk, and the UI still reported "paired".
    if (!m_pairingStore.save(pairing, attempt.m_sealingKey)) {
        NativeRegistrationResult persistFailed = result;
        // The outcome carries the fact; app/ carries the sentence. This used
        // to set an English `detail`, which PairingController rendered
        // verbatim -- untranslatable, since core/ cannot call i18n()
        // (AGENTS.md section 5), and exactly the defect HttpClient's
        // certificate-mismatch branch records.
        persistFailed.outcome = RegistrationOutcome::CredentialsNotSaved;
        // Leave nothing half-written: a partial record whose `sub` key
        // landed would make isPaired() true with an unusable secret.
        // A failure to remove it is a different fact for the user -- the
        // device still looks paired -- so it is a different outcome.
        if (!m_pairingStore.clear())
            persistFailed.outcome = RegistrationOutcome::CredentialsNotSavedAndNotCleared;
        return persistFailed; // ~PairAttempt restores the pin
    }

    // Enforce immediately, so even the requests made later in this same
    // session are checked rather than waiting for the next launch. Scoped to
    // the paired server's origin: the pin describes that relay, and enforcing
    // it on the deliberately cross-server PGP QR fetch only ever produced a
    // false "your mail server is being impersonated" alarm.
    if (!anchorPin.isEmpty()) {
        m_pinSink.setPin(anchorPin, QUrl(params.serverBaseUrl));
        attempt.m_restorePin = false; // the new pin stands; the saved one is stale
    } else if (attempt.m_savedPin.isEnforcing()) {
        // Registered successfully but there is nothing to pin. Over plain
        // http that is expected (no handshake) -- but it also happens when
        // QSslCertificate::publicKey() yields a key the backend cannot
        // represent, and in that case silently dropping a pin this device
        // was ALREADY enforcing is a downgrade, not a no-op. ~PairAttempt
        // restores the previous pin; say so, because the pinned key and the
        // newly-registered server may now disagree.
        qWarning("DeviceRegistrationService: registration succeeded but the peer key could not be "
                 "read; keeping the previously pinned certificate rather than disabling pinning");
    }

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

// The synchronous form, kept as the composition of the three phases above.
//
// Not a duplicate implementation: every caller that can afford to block --
// reregisterIfPaired(), and the tests that cover this class's policy -- goes
// through here, so the async path cannot drift away from the behaviour those
// tests pin.
NativeRegistrationResult DeviceRegistrationService::pair(const PairingParams& params, const QString& deviceToken)
{
    PairAttempt attempt = beginPair(params);
    if (const std::optional<RegistrationOutcome> refused = attempt.refusedOutcome()) {
        NativeRegistrationResult out;
        out.outcome = *refused;
        return out;
    }

    const NativeRegistrationResult result = m_client.registerDevice(
        QUrl(params.registrationUrl), params.subscriberId, params.pairingToken, deviceToken, QString(),
        params.deviceName);

    return finishPair(std::move(attempt), params, result);
}

// Note what this deliberately does NOT copy: the stored certificate pin.
//
// Leaving spkiPin empty means beginPair() suspends the pin for a
// re-registration and finishPair() re-anchors to whatever key answered. In the
// threat model pinning exists for -- an attacker holding a locally trusted CA
// -- that is a window: wait for a push-endpoint rotation, read the pairing
// token, have your own key persisted as the pin.
//
// It stays open because closing it costs more than it buys here. The pin is
// over the SubjectPublicKeyInfo, so it only breaks when the KEY rotates -- and
// the deployment this project documents (kypost-server's
// docs/Reverse_Proxy_Networking.md) puts a reverse proxy or cloudflared in
// front, where the device validates an edge certificate whose key the operator
// does not hold and cannot choose to reuse. That key rotates on someone else's
// schedule. Enforcing the pin here would abort every relay request until the
// user ran "Reconnect to server", on a cycle nobody can predict. This
// re-anchor is what absorbs that today.
//
// What bounds the damage: the pin is checked on TOP of Qt's chain validation
// (HttpClient never relaxes peer verification), so the unpinned window still
// requires a certificate the CA chain accepts for this host. Not "trust
// anything".
//
// The principled fix is server-side, and is recorded rather than done: the
// relay already knows whether it publishes a pin it controls (leafSPKIPin --
// own key, reusable across renewal) or one probed off whatever fronts it
// (probedSPKIPin). Only the first kind can be hard-enforced across a rotation.
// Revisit if that distinction ever reaches the client.
std::optional<PairingParams> DeviceRegistrationService::reregistrationParams() const
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
    return params;
}

std::optional<NativeRegistrationResult> DeviceRegistrationService::reregisterIfPaired(const QString& deviceToken)
{
    const std::optional<PairingParams> params = reregistrationParams();
    if (!params.has_value())
        return std::nullopt;

    return pair(*params, deviceToken);
}
