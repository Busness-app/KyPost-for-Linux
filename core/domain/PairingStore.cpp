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

// Drops load()'s cache on entry AND on exit of a mutating method.
//
// Both ends, not just one. In production every SecureStore write is a
// QKeychain job on a nested QEventLoop, so QML input keeps being delivered
// while a mutator is mid-flight -- which means some other code path can call
// load() from inside it and repopulate the cache from half-written state.
// Invalidating only on entry would leave that stale value in place for the
// rest of the session; only on exit would serve it during the call.
struct PairingStore::CacheInvalidation
{
    explicit CacheInvalidation(const PairingStore& store)
        : m_store(store)
    {
        m_store.m_cache.reset();
    }
    ~CacheInvalidation() { m_store.m_cache.reset(); }

    CacheInvalidation(const CacheInvalidation&) = delete;
    CacheInvalidation& operator=(const CacheInvalidation&) = delete;

private:
    const PairingStore& m_store;
};

PairingStore::PairingStore(SecureStore& secureStore)
    : m_secureStore(secureStore)
{
}

std::optional<DevicePairing> PairingStore::load() const
{
    return loadChecked().pairing;
}

PairingStore::LoadResult PairingStore::loadChecked() const
{
    // Served from the cache when one is current.
    //
    // Every field below is a separate SecureStore::get(), and in production
    // that is a QKeychain job on a nested QEventLoop and a D-Bus round trip
    // to the Secret Service -- eight of them per call. isPaired() is
    // load().has_value(), so answering a yes/no cost eight; MailController
    // calls requirePairing() (hence load()) on every mail operation; and
    // MfaController::respond() made nine blocking IPC calls before a single
    // byte went on the wire. On a session where kwalletd is slow or
    // prompting, opening one email visibly froze the UI.
    //
    // Each of those nested loops was also a re-entrancy window, so this cut
    // the interleaving surface as well as the latency. The loops are gone now
    // (the requests run on NetworkExecutor's thread); the latency argument
    // stands on its own. Invalidated by every mutation below -- save(),
    // clear(), and the seal/unseal/lock transitions that change what
    // deviceSecret resolves to.
    if (m_cache.has_value())
        return { LoadStatus::Loaded, m_cache };

    // read(), not get(): "sub" is the key that decides paired-vs-not, so it
    // is the one place the absent/unreadable distinction has to survive. The
    // seven fields below stay on get() deliberately -- they are only reached
    // once "sub" has answered Found, and an individually missing one has
    // always been treated as empty rather than as a failure.
    const SecureStore::ReadResult subscriberId = m_secureStore.read(QLatin1String(kSubscriberIdKey));
    if (subscriberId.failed())
        return { LoadStatus::Unreadable, std::nullopt };
    if (subscriberId.status != SecureStore::ReadStatus::Found)
        return { LoadStatus::Absent, std::nullopt };

    DevicePairing pairing;
    pairing.subscriberId = subscriberId.value;
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
    m_cache = pairing;
    return { LoadStatus::Loaded, pairing };
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
    const SecureStore::ReadResult gate = m_secureStore.read(QLatin1String(AppLockStore::kCredentialGateKey));
    // A store that could not be consulted must not report "gate off" --
    // that is the answer that sends storeDeviceSecret() down the plaintext
    // branch, which is precisely the outcome this function's own comment
    // above says both failure directions must avoid.
    if (gate.failed())
        return true;
    return gate.value == QStringLiteral("1");
}

bool PairingStore::storeDeviceSecret(const QString& secret)
{
    const CacheInvalidation invalidate(*this);
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
    const CacheInvalidation invalidate(*this);
    // The key is passed in, not read live, so that a lock arriving DURING a
    // blocking registration cannot invalidate a check that already passed.
    // HttpClient runs a nested QEventLoop, so minimising the window mid-call
    // reached lockDeviceSecret() and dropped m_sessionKey; save() then failed
    // its own guard and pair() responded by clearing the entire pairing --
    // subscriber id, device id, sealed blob and TOFU pin -- after the server
    // had already minted and retired the secret.
    const quint64 epochBefore = m_lockEpoch;
    const CredentialCipher::SessionKey previousKey = m_sessionKey;
    m_sessionKey = sealingKey;
    const bool ok = saveUnderCurrentKey(pairing);

    // What happens next depends on whether the app locked while the writes
    // above were blocked, and that is NOT answerable from the key alone.
    //
    // This used to unconditionally restore `previousKey`, with a comment
    // saying "if the app locked during the call it must stay locked" -- and
    // it did the exact opposite. saveUnderCurrentKey() performs nine
    // keychain writes, each on a nested QEventLoop, so a window minimise
    // during it reaches lockDeviceSecret(), which sets m_sessionKey = {}.
    // Restoring the pre-call key then put a live PBKDF2 key back after the
    // lock had deliberately destroyed it, leaving a locked app holding the
    // means to decrypt its own sealed blob -- precisely what
    // lockDeviceSecret()'s own comment says re-locking exists to end.
    //
    // The epoch answers the real question. Unchanged: nothing locked, the
    // caller's snapshot was a licence for this one write and the previous
    // key resumes. Changed: a lock landed mid-write and wins outright.
    if (m_lockEpoch == epochBefore)
        m_sessionKey = previousKey;
    else
        m_sessionKey = {};
    return ok;
}

bool PairingStore::saveUnderCurrentKey(const DevicePairing& pairing)
{
    const CacheInvalidation invalidate(*this);
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
    const CacheInvalidation invalidate(*this);
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
    // Same reason as lockDeviceSecret(): an unpair racing a blocked save()
    // must not have its key resurrected on that save()'s way out.
    ++m_lockEpoch;
    return ok;
}

bool PairingStore::isPaired() const
{
    return load().has_value();
}

PairingIdentity PairingStore::currentIdentity() const
{
    const std::optional<DevicePairing> pairing = load();
    if (!pairing.has_value())
        return {};
    return identityOf(*pairing);
}

bool PairingStore::stillCurrent(const PairingIdentity& identity) const
{
    const PairingIdentity current = currentIdentity();
    // Both empty is NOT a match. An unpaired store reached that state by an
    // unpair, by a failed replacement that cleared a half-written record, or
    // by not being readable at all -- and for every one of those the right
    // answer for an in-flight reply is "throw it away", not "no pairing
    // changed, go ahead". The unreadable case is why this cannot be written
    // as `current == identity`: that would compare two empty identities and
    // let the write through exactly when the store cannot be checked.
    if (current.isEmpty())
        return false;
    return current == identity;
}

bool PairingStore::sealDeviceSecret(const QString& pin)
{
    const CacheInvalidation invalidate(*this);
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
    const CacheInvalidation invalidate(*this);
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

bool PairingStore::resealDeviceSecret(const QString& oldPin, const QString& newPin)
{
    const CacheInvalidation invalidate(*this);
    const std::optional<QString> sealed = m_secureStore.get(QLatin1String(kSealedDeviceSecretKey));
    if (!sealed.has_value() || sealed->isEmpty())
        return false;

    // Open under the old PIN, in memory.
    const std::optional<std::pair<QByteArray, CredentialCipher::SessionKey>> opened =
        CredentialCipher::openWithKey(oldPin, *sealed);
    if (!opened.has_value())
        return false;

    // Re-wrap under the new one, still in memory. seal() (not sealWithKey())
    // because a new PIN means a new key, which means a fresh salt.
    const std::optional<QString> resealed = CredentialCipher::seal(newPin, opened->first);
    if (!resealed.has_value())
        return false;

    // The single write. Until this line lands, the old blob is intact and
    // the old PIN still opens it; after it, the new one does. There is no
    // instant at which the secret exists on disk unprotected, and no instant
    // at which it exists on disk under no key at all.
    if (!m_secureStore.set(QLatin1String(kSealedDeviceSecretKey), *resealed))
        return false;

    // Adopt the new key/plaintext for the rest of this session, so a secret
    // the relay rotates later can still be re-sealed without another PIN
    // prompt -- same reason sealDeviceSecret() ends with an unseal.
    return unsealDeviceSecret(newPin);
}

void PairingStore::lockDeviceSecret()
{
    const CacheInvalidation invalidate(*this);
    m_unsealedDeviceSecret.clear();
    // Dropped with the plaintext, not kept: a locked app that still held the
    // key could re-seal, which means it could also decrypt, which is exactly
    // what re-locking is supposed to end.
    m_sessionKey = {};
    // Recorded, so a save() that is currently blocked inside a nested event
    // loop can see on the way out that this happened and must not put the
    // key back. See save().
    ++m_lockEpoch;
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
    const CacheInvalidation invalidate(*this);
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
    // Every deliberate destruction of the session key bumps the epoch, so a
    // save() blocked in a nested event loop can never resurrect one. See
    // save().
    ++m_lockEpoch;
    return m_secureStore.remove(QLatin1String(kSealedDeviceSecretKey));
}

bool PairingStore::deviceSecretSealed() const
{
    const std::optional<QString> sealed = m_secureStore.get(QLatin1String(kSealedDeviceSecretKey));
    return sealed.has_value() && !sealed->isEmpty();
}
