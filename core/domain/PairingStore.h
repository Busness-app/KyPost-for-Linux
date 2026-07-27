#pragma once

#include "domain/DevicePairing.h"
#include "security/CredentialCipher.h"

#include <QString>

#include <optional>

class SecureStore;

// The one shared contract for "are we paired, and with what credentials",
// so every Phase 4 repository that needs pairing state reads it here rather
// than re-deriving SecureStore key names independently.
class PairingStore
{
public:
    explicit PairingStore(SecureStore& secureStore);

    // nullopt when never paired -- specifically, when "sub" is absent.
    // Other fields default to empty QString if individually missing
    // (defensive; save() always writes all eight keys together so this
    // should not happen in practice).
    std::optional<DevicePairing> load() const;

    // Writes all eight keys. Returns false if any individual
    // SecureStore::set() call fails (matches SecureStore's own
    // bool-returning contract) -- on false, some keys may have been
    // written and others not; callers should treat this as "pairing
    // state is now indeterminate", not attempt partial rollback.
    //
    // Sealed-aware. When the credential gate is on, `pairing.deviceSecret`
    // is NOT written out in the clear: it is re-sealed under this session's
    // key and the plaintext key is left empty, exactly as sealDeviceSecret()
    // leaves it. Writing it plainly is what re-registration used to do --
    // the relay mints a fresh secret on every successful register, so the
    // rotated value landed on disk unencrypted while the stored gate flag
    // and the old sealed blob both still claimed it was protected. If the
    // secret is sealed and this session cannot re-seal (the app is locked,
    // so there is no session key), save() fails rather than downgrading it;
    // callers must check canResealDeviceSecret() BEFORE doing anything that
    // rotates the secret server-side.
    bool save(const DevicePairing& pairing);

    // Removes all eight keys, including any sealed device-secret blob.
    // Returns false if any individual removal failed -- the caller is the
    // wipe-on-repeated-PIN-failure path, for which "the credential is still
    // on disk" is the difference between a wipe and the appearance of one.
    bool clear();

    bool isPaired() const; // load().has_value()

    // --- credential PIN gate -------------------------------------------
    // Backs Settings > Security's "Require unlock to receive push/MFA".
    //
    // When sealed, `pairing.deviceSecret` is replaced by a PIN-derived
    // AES-GCM blob (core/security/CredentialCipher.h) under a separate key,
    // and load() returns an empty deviceSecret until unsealDeviceSecret()
    // has been called this session. Every authenticated request therefore
    // fails with 401 while the app is locked -- which is the point: push
    // and MFA cannot be serviced without the user present.

    // Seals the stored secret under `pin` and erases the plaintext copy.
    // No-op (returns true) if already sealed. Returns false if there is no
    // secret to seal or crypto fails.
    bool sealDeviceSecret(const QString& pin);

    // Opens the sealed blob into memory FOR THIS SESSION ONLY. The
    // plaintext is deliberately never written back to the store: doing so
    // would leave the secret unsealed on disk after the first unlock, which
    // is precisely what the gate exists to prevent. load() serves the
    // in-memory copy until lockDeviceSecret() drops it.
    //
    // Returns false on the wrong PIN, indistinguishable from a tampered
    // blob.
    bool unsealDeviceSecret(const QString& pin);

    // Drops the in-memory plaintext, so load() goes back to reporting an
    // empty deviceSecret. Called when the app re-locks. The sealed blob on
    // disk is untouched -- it is the durable copy.
    void lockDeviceSecret();

    // Permanently reverses sealDeviceSecret(): writes the plaintext back to
    // the store and deletes the sealed blob. This is what turning the gate
    // OFF must do -- the session-only unsealDeviceSecret() above would leave
    // the blob sealed under a PIN that is about to be destroyed, and the
    // pairing would be unrecoverable on next launch.
    bool unsealDeviceSecretPermanently(const QString& pin);

    // True when a sealed blob exists, whether or not it has been opened
    // this session.
    bool deviceSecretSealed() const;

    // True when a rotated secret could be re-sealed right now without the
    // user re-entering their PIN -- i.e. either nothing is sealed (the gate
    // is off, plaintext writes are the normal case) or this session has
    // unsealed the blob and still holds the derived key.
    //
    // Anything that causes the relay to mint a NEW device secret must check
    // this first and refuse before making the network call: registering
    // invalidates the old secret server-side, so failing afterwards would
    // leave the device with a credential the server has already retired.
    bool canResealDeviceSecret() const;

    // This session's re-seal key, captured so a caller can hold it across a
    // blocking network call. Pass it back to the save() overload below;
    // re-reading the live key after the call is a TOCTOU, because the nested
    // event loop can deliver a lock in between.
    CredentialCipher::SessionKey sealingKeySnapshot() const;
    bool save(const DevicePairing& pairing, const CredentialCipher::SessionKey& sealingKey);

private:
    // True per the authoritative applock.credentialPinGateEnabled flag, not
    // per the presence of a sealed blob.
    bool credentialGateEnabled() const;
    bool saveUnderCurrentKey(const DevicePairing& pairing);

    // Writes `secret` the way the current gate state requires: re-sealed
    // under the session key when a sealed blob exists, plaintext otherwise.
    bool storeDeviceSecret(const QString& secret);

    SecureStore& m_secureStore;
    // Session-only plaintext of a sealed device secret. Never persisted --
    // see unsealDeviceSecret().
    QString m_unsealedDeviceSecret;
    // The PBKDF2 output (and its salt) from this session's unseal, kept so a
    // rotated secret can be re-sealed without retaining the PIN -- the PIN
    // is the larger secret, since it also authorizes disabling the lock
    // outright. Cleared everywhere m_unsealedDeviceSecret is.
    CredentialCipher::SessionKey m_sessionKey;
};
