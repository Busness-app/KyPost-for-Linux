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

bool PairingStore::save(const DevicePairing& pairing)
{
    bool ok = true;
    ok = m_secureStore.set(QLatin1String(kSubscriberIdKey), pairing.subscriberId) && ok;
    ok = m_secureStore.set(QLatin1String(kServerBaseUrlKey), pairing.serverBaseUrl) && ok;
    ok = m_secureStore.set(QLatin1String(kRegistrationUrlKey), pairing.registrationUrl) && ok;
    ok = m_secureStore.set(QLatin1String(kPairingTokenKey), pairing.pairingToken) && ok;
    ok = m_secureStore.set(QLatin1String(kDeviceIdKey), pairing.deviceId) && ok;
    ok = m_secureStore.set(QLatin1String(kDeviceNameKey), pairing.deviceName) && ok;
    ok = m_secureStore.set(QLatin1String(kDeviceSecretKey), pairing.deviceSecret) && ok;
    ok = m_secureStore.set(QLatin1String(kCertificatePinKey), pairing.certificateSpkiSha256) && ok;
    return ok;
}

void PairingStore::clear()
{
    m_secureStore.remove(QLatin1String(kSubscriberIdKey));
    m_secureStore.remove(QLatin1String(kServerBaseUrlKey));
    m_secureStore.remove(QLatin1String(kRegistrationUrlKey));
    m_secureStore.remove(QLatin1String(kPairingTokenKey));
    m_secureStore.remove(QLatin1String(kDeviceIdKey));
    m_secureStore.remove(QLatin1String(kDeviceNameKey));
    m_secureStore.remove(QLatin1String(kDeviceSecretKey));
    // Also drop any sealed copy, or unpairing would leave an orphaned
    // credential blob behind that a later pairing could never open.
    m_secureStore.remove(QLatin1String(kSealedDeviceSecretKey));
    m_secureStore.remove(QLatin1String(kCertificatePinKey));
    m_unsealedDeviceSecret.clear();
}

bool PairingStore::isPaired() const
{
    return load().has_value();
}

bool PairingStore::sealDeviceSecret(const QString& pin)
{
    if (deviceSecretSealed())
        return true;

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
    return m_secureStore.set(QLatin1String(kDeviceSecretKey), QString());
}

bool PairingStore::unsealDeviceSecret(const QString& pin)
{
    const std::optional<QString> sealed = m_secureStore.get(QLatin1String(kSealedDeviceSecretKey));
    if (!sealed.has_value() || sealed->isEmpty())
        return false;

    const std::optional<QByteArray> plaintext = CredentialCipher::open(pin, *sealed);
    if (!plaintext.has_value())
        return false;

    // In memory only. Writing this back to the store would leave the secret
    // unsealed on disk from the first unlock onward, defeating the gate.
    m_unsealedDeviceSecret = QString::fromUtf8(*plaintext);
    return true;
}

void PairingStore::lockDeviceSecret()
{
    m_unsealedDeviceSecret.clear();
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
    return m_secureStore.remove(QLatin1String(kSealedDeviceSecretKey));
}

bool PairingStore::deviceSecretSealed() const
{
    const std::optional<QString> sealed = m_secureStore.get(QLatin1String(kSealedDeviceSecretKey));
    return sealed.has_value() && !sealed->isEmpty();
}
