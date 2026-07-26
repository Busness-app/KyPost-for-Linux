#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

class SecureStore;

// PIN policy and lockout state for "Require Unlock to Open".
//
// Takes a SecureStore, NOT SettingsStore, and that choice is the feature.
// SettingsStore is a plain INI file: anyone with OS-level access to the
// account's files -- the exact access level this lock exists to survive --
// could flip `lockEnabled=false` in a text editor and relaunch. SecureStore
// (Secret Service in production) puts these fields behind the same
// access-control tier as the pairing credential itself.
//
// Note that `hostileLocationProtectionEnabled` deliberately does NOT live
// here: it is UI-gated behind an already-enabled, SecureStore-protected PIN,
// so it is safe as an ordinary SettingsStore preference.
class AppLockStore
{
public:
    explicit AppLockStore(SecureStore& secureStore);

    bool lockEnabled() const;

    // Enables the lock and stores a fresh salt + PBKDF2 hash of `pin`. The
    // raw PIN is never persisted. Returns false if the store write fails.
    bool setPin(const QString& pin);

    // Constant-time comparison against the stored hash. False when no PIN is
    // set, so a store that lost its keys fails closed rather than open.
    bool verifyPin(const QString& pin) const;

    // Clears the PIN, the lock flag, and all lockout state in one go. Used
    // when the user turns the lock off (having just proven the PIN) and by
    // the wipe path.
    bool clear();

    int failedAttemptCount() const;
    bool setFailedAttemptCount(int count);

    qint64 lockoutUntilEpochMs() const;
    bool setLockoutUntilEpochMs(qint64 epochMs);

    // "Require unlock to receive push/MFA": when on, the device secret is
    // stored sealed (see core/security/CredentialCipher.h) and is unusable
    // until the PIN has been entered this session.
    bool credentialPinGateEnabled() const;
    bool setCredentialPinGateEnabled(bool enabled);

private:
    SecureStore& m_secureStore;
};
