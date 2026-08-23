#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

class SecureStore;

// PIN policy and lockout state for "Require Unlock to Open".
//
// Takes a SecureStore, NOT SettingsStore, and that choice is the feature.
// SettingsStore is a plain INI file: anyone with write access to the
// account's files could flip `lockEnabled=false` in a text editor and
// relaunch. SecureStore (Secret Service in production) puts these fields
// behind the same access-control tier as the pairing credential itself.
//
// WHAT THIS DOES AND DOES NOT PROTECT. The earlier wording here claimed
// SettingsStore was unsuitable because file access is "the exact access
// level this lock exists to survive". It is not, and the difference matters
// enough to state: kypost.db is an UNENCRYPTED SQLite file holding cached
// mail bodies and full contact records. Anyone who can read the account's
// files can read all of it with sqlite3(1) without going near this class.
//
// So this lock guards the running application -- the window, the
// notifications, the pairing credential, and (with the credential gate on)
// the device secret, which really is encrypted at rest. It is not at-rest
// protection for mail content. The mode that provides that is Hostile
// Location Protection, which keeps the database in memory and writes none of
// it; Settings says so in those words rather than letting the PIN prompt
// imply more than it delivers.
//
// The full fix is to key SQLCipher from the PIN-derived session key this
// class's sibling already computes (core/security/CredentialCipher.h's
// SessionKey). That is a schema-and-migration project, not a patch, and
// until it lands the honest statement above is what ships.
//
// Note that `hostileLocationProtectionEnabled` deliberately does NOT live
// here: it is UI-gated behind an already-enabled, SecureStore-protected PIN,
// so it is safe as an ordinary SettingsStore preference.
class AppLockStore
{
public:
    explicit AppLockStore(SecureStore& secureStore);

    // Fails CLOSED: an unreadable secret store reports the lock as ON.
    // See the .cpp, and SecureStore::ReadStatus for why get() could not
    // express this.
    bool lockEnabled() const;

    // False when the secret store cannot be consulted at all -- no Secret
    // Service provider running, a locked wallet, no D-Bus session. Exposed
    // so the UI can explain why an unlock screen is refusing every PIN,
    // rather than leaving the user to conclude they have forgotten it.
    bool storeReadable() const;

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

    // How many consecutive failed attempts erase this device, or
    // LockoutPolicy::kWipeNever for "do not erase".
    //
    // Here rather than in SettingsStore for the same reason lockEnabled() is:
    // settings.ini is a plain text file, so anyone with file access could
    // switch the erase off by editing it -- which is exactly the access level
    // this policy exists to survive. It is stored next to the PIN, behind the
    // same secret store.
    //
    // An absent, unparseable or out-of-range value reads as the DEFAULT, not
    // as "never": the failure direction has to be the protective one, and an
    // unreadable keyring is not the user asking for the erase to stop.
    int wipeAfterAttempts() const;

    // Clamped to LockoutPolicy's range before it is written, so nothing
    // downstream has to defend against a value that would erase the device on
    // the second mistyped digit.
    bool setWipeAfterAttempts(int attempts);

    // How long the app may stay unlocked after leaving the foreground, in
    // seconds. 0 means "lock immediately", which is the default and what this
    // app did before the setting existed.
    //
    // Behind the secret store for the same reason the erase threshold is: a
    // grace period in settings.ini could be stretched to five minutes by
    // anyone with file access, who would then only have to wait for the owner
    // to walk away from an unlocked session. Absent, unparseable and
    // out-of-range all read as 0 -- the protective direction.
    int backgroundGraceSeconds() const;
    bool setBackgroundGraceSeconds(int seconds);

    // "Require unlock to receive push/MFA": when on, the device secret is
    // stored sealed (see core/security/CredentialCipher.h) and is unusable
    // until the PIN has been entered this session.
    bool credentialPinGateEnabled() const;

    // The authoritative "is the credential PIN gate on" flag, exposed so
    // PairingStore can consult it directly. PairingStore used to infer the
    // gate's state from the presence and readability of its own sealed blob,
    // which is not the same question: unpairing removed the blob while the
    // flag stayed set, and a failed keychain read is indistinguishable from
    // "no blob" -- both made the next write take the plaintext branch while
    // the UI still reported the gate as On.
    static constexpr auto kCredentialGateKey = "applock.credentialPinGateEnabled";
    bool setCredentialPinGateEnabled(bool enabled);

private:
    SecureStore& m_secureStore;
};
