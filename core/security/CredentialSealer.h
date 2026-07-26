#pragma once

#include <QString>

// How AppLockManager seals and opens the relay device secret under the PIN.
//
// This exists as an interface, and every method returns bool, because the
// previous design could not work: AppLockManager announced the seal/unseal
// through a Qt signal (credentialGateChanged) and the host performed it in a
// lambda. A signal returns void, so the class that owned the policy had no
// way to learn whether the operation it had just ordered actually happened,
// and the two bool returns that would have told it were discarded at the
// connection site. Two concrete failures followed from that:
//
//   * Enabling the gate while the secret store was unwritable set
//     `credentialPinGateEnabled = true` with the device secret still sitting
//     in plaintext. The UI reported the protection as on. It was not.
//
//   * Disabling the gate (or turning the lock off entirely) proceeded to
//     erase the PIN even when the unseal had failed, leaving an AES-GCM blob
//     on disk under a key that no longer existed anywhere -- an
//     unrecoverable pairing.
//
// Ordering is the caller's responsibility and is the whole point: seal/
// unseal FIRST, and only persist the flag (or destroy the PIN) once the
// crypto side reports success.
class CredentialSealer
{
public:
    virtual ~CredentialSealer() = default;

    // Wraps the stored device secret under `pin` and erases the plaintext.
    // False means nothing was sealed and the plaintext is still there.
    virtual bool seal(const QString& pin) = 0;

    // Permanently reverses seal(): restores the plaintext, drops the blob.
    // False means the secret is STILL sealed under `pin` -- callers must not
    // destroy that PIN.
    virtual bool unsealPermanently(const QString& pin) = 0;

    // Opens the sealed blob into memory for this session only. False means
    // authenticated requests will keep failing until a successful unlock.
    virtual bool unsealForSession(const QString& pin) = 0;

    // Drops the session plaintext. Cannot fail: the durable blob is
    // untouched.
    virtual void relock() = 0;

    // True when a sealed blob exists on disk, opened this session or not.
    virtual bool isSealed() const = 0;
};

// Used wherever the app lock is exercised without a pairing to protect
// (tests, and any future surface that has no PairingStore). Reports success
// for the no-op directions and "nothing is sealed", so AppLockManager's
// ordering logic is exercised unchanged.
class NullCredentialSealer : public CredentialSealer
{
public:
    bool seal(const QString&) override { return true; }
    bool unsealPermanently(const QString&) override { return true; }
    bool unsealForSession(const QString&) override { return true; }
    void relock() override { }
    bool isSealed() const override { return false; }
};
