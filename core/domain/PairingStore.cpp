#include "domain/PairingStore.h"

#include "security/CredentialCipher.h"

#include "stores/SecureStore.h"

namespace {

constexpr auto kSubscriberIdKey = "sub";
constexpr auto kServerBaseUrlKey = "pairing.serverBaseUrl";
constexpr auto kRegistrationUrlKey = "pairing.registrationUrl";
constexpr auto kPairingTokenKey = "pairing.pairingToken";
constexpr auto kDeviceIdKey = "deviceId";
constexpr auto kDeviceNameKey = "pairing.deviceName";
constexpr auto kDeviceSecretKey = "pairing.deviceSecret";
// Separate key so a sealed blob can never be mistaken for a usable secret
// by code that reads kDeviceSecretKey directly.
constexpr auto kSealedDeviceSecretKey = "pairing.deviceSecretSealed";
constexpr auto kCertificatePinKey = "pairing.certificateSpkiSha256";

QString valueOrEmpty(const std::optional<QString>& value)
{
    return value.value_or(QString());
}

}

PairingStore::PairingStore(SecureStore& secureStore)
    : m_secureStore(secureStore)
{
}

std::optional<DevicePairing> PairingStore::load() const
{
    const std::optional<QString> subscriberId = m_secureStore.get(QLatin1String(kSubscriberIdKey));
    if (!subscriberId.has_value())
        return std::nullopt;

    DevicePairing pairing;
    pairing.subscriberId = *subscriberId;
    pairing.serverBaseUrl = valueOrEmpty(m_secureStore.get(QLatin1String(kServerBaseUrlKey)));
    pairing.registrationUrl = valueOrEmpty(m_secureStore.get(QLatin1String(kRegistrationUrlKey)));
    pairing.pairingToken = valueOrEmpty(m_secureStore.get(QLatin1String(kPairingTokenKey)));
    pairing.deviceId = valueOrEmpty(m_secureStore.get(QLatin1String(kDeviceIdKey)));
    pairing.deviceName = valueOrEmpty(m_secureStore.get(QLatin1String(kDeviceNameKey)));
    pairing.deviceSecret = valueOrEmpty(m_secureStore.get(QLatin1String(kDeviceSecretKey)));
    // When the credential gate is on, the stored value is empty and the real
    // secret only exists in memory after a successful unlock. Everything
    // downstream (RelayAuth, every client) then simply sends an empty secret
    // and gets 401s while locked, which is the intended behaviour.
    if (pairing.deviceSecret.isEmpty() && !m_unsealedDeviceSecret.isEmpty())
        pairing.deviceSecret = m_unsealedDeviceSecret;
    pairing.certificateSpkiSha256 = valueOrEmpty(m_secureStore.get(QLatin1String(kCertificatePinKey)));
    return pairing;
}

bool PairingStore::storeDeviceSecret(const QString& secret)
{
    if (!deviceSecretSealed()) {
        // Gate off: plaintext in the system keychain is the normal, intended
        // resting state, same as it has always been.
        return m_secureStore.set(QLatin1String(kDeviceSecretKey), secret);
    }

    // Gate on. Re-wrap under this session's key, never write the plaintext.
    const std::optional<QString> resealed = CredentialCipher::sealWithKey(m_sessionKey, secret.toUtf8());
    if (!resealed.has_value())
        return false;
    if (!m_secureStore.set(QLatin1String(kSealedDeviceSecretKey), *resealed))
        return false;
    // Keep the session copy in step, so the rest of this session
    // authenticates with the new secret rather than the retired one.
    m_unsealedDeviceSecret = secret;
    // Belt and braces: the plaintext key must be empty while sealed, and a
    // previous build may have left a real value there.
    return m_secureStore.set(QLatin1String(kDeviceSecretKey), QString());
}

bool PairingStore::save(const DevicePairing& pairing)
{
    // Checked before ANY key is written: a caller that reaches here with a
    // sealed secret and no session key has already rotated the credential
    // server-side, and there is nothing useful left to do but report it.
    if (!canResealDeviceSecret())
        return false;

    bool ok = true;
    ok = m_secureStore.set(QLatin1String(kSubscriberIdKey), pairing.subscriberId) && ok;
    ok = m_secureStore.set(QLatin1String(kServerBaseUrlKey), pairing.serverBaseUrl) && ok;
    ok = m_secureStore.set(QLatin1String(kRegistrationUrlKey), pairing.registrationUrl) && ok;
    ok = m_secureStore.set(QLatin1String(kPairingTokenKey), pairing.pairingToken) && ok;
    ok = m_secureStore.set(QLatin1String(kDeviceIdKey), pairing.deviceId) && ok;
    ok = m_secureStore.set(QLatin1String(kDeviceNameKey), pairing.deviceName) && ok;
    ok = storeDeviceSecret(pairing.deviceSecret) && ok;
    ok = m_secureStore.set(QLatin1String(kCertificatePinKey), pairing.certificateSpkiSha256) && ok;
    return ok;
}

bool PairingStore::clear()
{
    // Every result checked and aggregated. The caller that matters is the
    // wipe-after-repeated-PIN-failure path, where a silently-failed removal
    // (a locked wallet, no Secret Service running) means the device secret
    // survives a "wipe" the UI already reported as done.
    bool ok = true;
    ok = m_secureStore.remove(QLatin1String(kSubscriberIdKey)) && ok;
    ok = m_secureStore.remove(QLatin1String(kServerBaseUrlKey)) && ok;
    ok = m_secureStore.remove(QLatin1String(kRegistrationUrlKey)) && ok;
    ok = m_secureStore.remove(QLatin1String(kPairingTokenKey)) && ok;
    ok = m_secureStore.remove(QLatin1String(kDeviceIdKey)) && ok;
    ok = m_secureStore.remove(QLatin1String(kDeviceNameKey)) && ok;
    ok = m_secureStore.remove(QLatin1String(kDeviceSecretKey)) && ok;
    // Also drop any sealed copy, or unpairing would leave an orphaned
    // credential blob behind that a later pairing could never open.
    ok = m_secureStore.remove(QLatin1String(kSealedDeviceSecretKey)) && ok;
    ok = m_secureStore.remove(QLatin1String(kCertificatePinKey)) && ok;
    m_unsealedDeviceSecret.clear();
    m_sessionKey = {};
    return ok;
}

bool PairingStore::isPaired() const
{
    return load().has_value();
}

bool PairingStore::sealDeviceSecret(const QString& pin)
{
    // Already sealed: nothing to encrypt, but still adopt the blob as this
    // session's key material. Otherwise turning the gate on would leave the
    // session unable to re-seal a rotated secret (canResealDeviceSecret()
    // false) until the next unlock, which would block re-registration for
    // the rest of the session for no reason.
    if (deviceSecretSealed())
        return unsealDeviceSecret(pin);

    const std::optional<QString> secret = m_secureStore.get(QLatin1String(kDeviceSecretKey));
    if (!secret.has_value() || secret->isEmpty())
        return false;

    const std::optional<QString> sealed = CredentialCipher::seal(pin, secret->toUtf8());
    if (!sealed.has_value())
        return false;

    // Write the sealed blob before removing the plaintext, so a failure
    // between the two loses nothing.
    if (!m_secureStore.set(QLatin1String(kSealedDeviceSecretKey), *sealed))
        return false;
    if (!m_secureStore.set(QLatin1String(kDeviceSecretKey), QString()))
        return false;

    // Adopt what was just written as the session key/plaintext, for the same
    // reason as the already-sealed branch above.
    return unsealDeviceSecret(pin);
}

bool PairingStore::unsealDeviceSecret(const QString& pin)
{
    const std::optional<QString> sealed = m_secureStore.get(QLatin1String(kSealedDeviceSecretKey));
    if (!sealed.has_value() || sealed->isEmpty())
        return false;

    const std::optional<std::pair<QByteArray, CredentialCipher::SessionKey>> opened =
        CredentialCipher::openWithKey(pin, *sealed);
    if (!opened.has_value())
        return false;

    // In memory only. Writing this back to the store would leave the secret
    // unsealed on disk from the first unlock onward, defeating the gate.
    m_unsealedDeviceSecret = QString::fromUtf8(opened->first);
    // Retained so a secret the relay rotates later in this session can be
    // re-sealed without asking for the PIN again -- see storeDeviceSecret().
    m_sessionKey = opened->second;
    return true;
}

void PairingStore::lockDeviceSecret()
{
    m_unsealedDeviceSecret.clear();
    // Dropped with the plaintext, not kept: a locked app that still held the
    // key could re-seal, which means it could also decrypt, which is exactly
    // what re-locking is supposed to end.
    m_sessionKey = {};
}

bool PairingStore::canResealDeviceSecret() const
{
    return !deviceSecretSealed() || m_sessionKey.isValid();
}

bool PairingStore::unsealDeviceSecretPermanently(const QString& pin)
{
    const std::optional<QString> sealed = m_secureStore.get(QLatin1String(kSealedDeviceSecretKey));
    if (!sealed.has_value() || sealed->isEmpty()) {
        // Nothing sealed: already in the desired state.
        return true;
    }

    const std::optional<QByteArray> plaintext = CredentialCipher::open(pin, *sealed);
    if (!plaintext.has_value())
        return false;

    // Restore the plaintext before dropping the blob, so a failure between
    // the two never loses the credential.
    if (!m_secureStore.set(QLatin1String(kDeviceSecretKey), QString::fromUtf8(*plaintext)))
        return false;
    m_unsealedDeviceSecret.clear();
    m_sessionKey = {};
    return m_secureStore.remove(QLatin1String(kSealedDeviceSecretKey));
}

bool PairingStore::deviceSecretSealed() const
{
    const std::optional<QString> sealed = m_secureStore.get(QLatin1String(kSealedDeviceSecretKey));
    return sealed.has_value() && !sealed->isEmpty();
}
