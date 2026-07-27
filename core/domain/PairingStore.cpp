#include "domain/PairingStore.h"

#include "security/AppLockStore.h"
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

bool PairingStore::credentialGateEnabled() const
{
    // The flag OR the blob -- either is sufficient, so both failure
    // directions land closed.
    //
    // The blob alone was not enough. clear() removes it and cannot reach the
    // flag, so an unpair followed by a re-pair wrote the new secret in
    // plaintext under a flag still claiming protection; and a failed keychain
    // read is indistinguishable from "absent", so a locked wallet did the
    // same. Both ended with the device secret in the clear while Settings
    // displayed "Require unlock to receive push and MFA: On".
    //
    // The flag alone is not enough either: sealDeviceSecret() is reachable
    // without the flag having been written yet, and treating that as "gate
    // off" would write plaintext next to a blob that does exist.
    if (deviceSecretSealed())
        return true;
    return m_secureStore.get(QLatin1String(AppLockStore::kCredentialGateKey)).value_or(QString())
        == QStringLiteral("1");
}

bool PairingStore::storeDeviceSecret(const QString& secret)
{
    if (!credentialGateEnabled()) {
        // Gate off: plaintext in the system keychain is the normal, intended
        // resting state, same as it has always been.
        return m_secureStore.set(QLatin1String(kDeviceSecretKey), secret);
    }

    // Gate on. Re-wrap under this session's key, never write the plaintext.
    // Failing closed here is deliberate: with the gate on there is no
    // acceptable plaintext fallback, and a caller that reaches this without a
    // session key has already been told to defer (canResealDeviceSecret).
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

CredentialCipher::SessionKey PairingStore::sealingKeySnapshot() const
{
    return m_sessionKey;
}

bool PairingStore::save(const DevicePairing& pairing)
{
    return save(pairing, m_sessionKey);
}

bool PairingStore::save(const DevicePairing& pairing, const CredentialCipher::SessionKey& sealingKey)
{
    // The key is passed in, not read live, so that a lock arriving DURING a
    // blocking registration cannot invalidate a check that already passed.
    // HttpClient runs a nested QEventLoop, so minimising the window mid-call
    // reached lockDeviceSecret() and dropped m_sessionKey; save() then failed
    // its own guard and pair() responded by clearing the entire pairing --
    // subscriber id, device id, sealed blob and TOFU pin -- after the server
    // had already minted and retired the secret.
    const CredentialCipher::SessionKey previousKey = m_sessionKey;
    m_sessionKey = sealingKey;
    const bool ok = saveUnderCurrentKey(pairing);
    // Restore rather than keep: if the app locked during the call it must
    // stay locked, and the caller's snapshot was only ever a licence to
    // finish this one write.
    m_sessionKey = previousKey;
    return ok;
}

bool PairingStore::saveUnderCurrentKey(const DevicePairing& pairing)
{
    // Checked before ANY key is written: a caller that reaches here with the
    // gate on and no session key has already rotated the credential
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
    // The gate flag lives in AppLockStore but is meaningless without a
    // pairing to protect, and leaving it set is what let the next pairing
    // write its secret in plaintext while the UI still reported the gate as
    // On. Reset it in the same operation that removes the blob.
    ok = m_secureStore.set(QLatin1String(AppLockStore::kCredentialGateKey), QStringLiteral("0")) && ok;
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
    // Keyed on the flag, matching storeDeviceSecret(). Using the blob here
    // instead let an unreadable keychain -- or an unpair that removed the
    // blob but not the flag -- report "nothing to re-seal, plaintext is
    // fine" while the gate was still nominally on.
    return !credentialGateEnabled() || m_sessionKey.isValid();
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
