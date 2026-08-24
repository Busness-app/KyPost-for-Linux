#pragma once

#include "db/ProfileDatabase.h"

#include <QObject>
#include <QTimer>
#include <QString>

#include <functional>

class AppLockStore;
class CredentialSealer;
class SettingsStore;

// QML-facing app lock ("AppLock" singleton), backing Settings > Security's
// "Require Unlock to Open".
//
// `locked` is in-memory only and deliberately not persisted: it means "since
// this process started, has the correct PIN been presented", exactly as on
// Android. Persisting it would add nothing -- a process that has not started
// cannot be unlocked -- and would create a state that could disagree with
// reality after a crash.
//
// Lock triggers are wired by the host (main.cpp / the QML roots) via
// lockNow(), rather than subscribed to here: the signals that mean "the user
// walked away" differ between Desktop (hide-to-tray, suspend, session lock)
// and Mobile (application state leaving Active), and this repo already
// branches those at the GeneralController::isDesktopMode boundary.
//
// Sealing the device secret goes through an injected CredentialSealer whose
// methods return bool, NOT through a signal the host handles in a lambda.
// See core/security/CredentialSealer.h for the two data-loss bugs the signal
// version caused; the short version is that a void-returning signal cannot
// tell this class whether the operation it ordered succeeded, so the stored
// gate flag and the real state of the secret could disagree.
class AppLockManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool lockEnabled READ lockEnabled NOTIFY lockStateChanged)
    Q_PROPERTY(bool locked READ locked NOTIFY lockedChanged)
    // Seconds still to wait before another attempt is accepted, or 0. Read
    // on demand by the Unlock screen's countdown.
    Q_PROPERTY(int remainingLockoutSeconds READ remainingLockoutSeconds NOTIFY lockoutChanged)
    Q_PROPERTY(int failedAttempts READ failedAttempts NOTIFY lockoutChanged)
    Q_PROPERTY(bool credentialPinGateEnabled READ credentialPinGateEnabled NOTIFY lockStateChanged)
    Q_PROPERTY(bool hostileLocationEnabled READ hostileLocationEnabled NOTIFY lockStateChanged)
    // True when the correct PIN was accepted but the sealed device secret
    // could not be opened (secret store unavailable/corrupt). The app is
    // unlocked, but every authenticated request will 401 until this clears,
    // so the roots surface it as a banner rather than letting the user
    // conclude the server is down. See tryUnlock().
    Q_PROPERTY(bool credentialsUnavailable READ credentialsUnavailable NOTIFY credentialsUnavailableChanged)
    // Minimum PIN length, exposed so Settings.qml can enforce and explain
    // the same rule the C++ side enforces, instead of hardcoding a second
    // copy that could drift.
    Q_PROPERTY(int minimumPinLength READ minimumPinLength CONSTANT)
    // True when the secret store cannot be consulted at all -- no Secret
    // Service provider running, a locked wallet, no D-Bus session.
    //
    // The lock now fails CLOSED in that state (AppLockStore::lockEnabled),
    // which is the safe answer but an inexplicable one on its own: the user
    // gets an unlock screen that refuses every PIN, including the right one,
    // because verifyPin() cannot read the stored hash either. This property
    // is what lets the overlay say "your keyring is not running" instead of
    // silently implying they have forgotten their own PIN.
    Q_PROPERTY(bool storeUnavailable READ storeUnavailable NOTIFY lockStateChanged)

    // A wipe was started on this device and has never reported completing.
    //
    // The wipe-after-ten-failed-attempts path relaunches whether or not the
    // wipe worked, and a wipe interrupted part-way writes nothing at all --
    // so without this the next launch looks entirely ordinary while sitting
    // on top of whatever survived. The host runs the recovery attempt at
    // startup (core/domain/TrackedWipe) and sets this when it did not
    // succeed; the UI's job is to stop the user believing data is gone when
    // it is not.
    Q_PROPERTY(bool wipeIncomplete READ wipeIncomplete NOTIFY wipeIncompleteChanged)

    // How this profile's database ended up open. Two separate facts because
    // they need two separate things said about them: "your mail is on this
    // disk unencrypted" and "your mail is not being saved at all" are
    // different situations with different fixes.
    //
    // On AppLockManager rather than a singleton of their own: this is the
    // object the UI already asks about the security state of the device, and
    // one more registered singleton for two booleans is not worth the wiring.
    Q_PROPERTY(bool databaseUnencrypted READ databaseUnencrypted NOTIFY databaseModeChanged)
    Q_PROPERTY(bool databaseMemoryOnly READ databaseMemoryOnly NOTIFY databaseModeChanged)
    Q_PROPERTY(bool dataDirectoryUnprotected READ dataDirectoryUnprotected NOTIFY databaseModeChanged)

    // How many consecutive failed attempts erase this device, or
    // LockoutPolicy::kWipeNever for "do not erase". Settings binds this to
    // its erase-after control; the range and the meaning of 0 come from
    // core/security/LockoutPolicy.h.
    Q_PROPERTY(int wipeAfterAttempts READ wipeAfterAttempts NOTIFY lockStateChanged)

    // How long the app stays unlocked after leaving the foreground, in
    // seconds. 0 -- the default -- locks immediately, which is what this app
    // did before the setting existed.
    Q_PROPERTY(int backgroundGraceSeconds READ backgroundGraceSeconds NOTIFY lockStateChanged)

public:
    AppLockManager(AppLockStore& store, SettingsStore& settingsStore, CredentialSealer& sealer,
                   QObject* parent = nullptr);

    bool lockEnabled() const;
    bool locked() const;
    int remainingLockoutSeconds() const;
    int failedAttempts() const;
    bool credentialPinGateEnabled() const;
    bool hostileLocationEnabled() const;
    bool credentialsUnavailable() const;
    bool storeUnavailable() const;
    int minimumPinLength() const;

    // Returns true and clears `locked` on the right PIN. On a wrong PIN,
    // increments the attempt count, applies the backoff, and -- at the wipe
    // threshold -- emits wipeRequested() and returns false.
    //
    // Fails closed if the attempt count cannot be persisted: without a
    // durable counter there is no backoff and no wipe threshold, i.e. an
    // unlimited guessing oracle, so this refuses further attempts for the
    // rest of the process instead.
    Q_INVOKABLE bool tryUnlock(const QString& pin);

    // Turns the lock on (or changes the PIN). Changing an existing PIN
    // requires the current one; `currentPin` is ignored when no lock is set
    // yet. Returns false if the current PIN is wrong, the new PIN fails
    // PinPolicy, or the store write fails.
    Q_INVOKABLE bool setPin(const QString& currentPin, const QString& newPin);

    // PinPolicy::validate() as a localized reason string, or "" when the PIN
    // is acceptable. Lets the Settings dialog explain a rejection before the
    // user commits to it rather than only reporting a bare failure.
    Q_INVOKABLE QString pinRejectionReason(const QString& pin) const;

    // Turns the lock off. Requires the current PIN -- otherwise anyone with
    // the app open could remove the protection that the lock exists to
    // provide.
    //
    // Refuses (returns false, lock stays on) if the credential gate is
    // enabled and the device secret cannot be unsealed: destroying the PIN
    // in that state would strand the pairing behind a key that no longer
    // exists anywhere.
    Q_INVOKABLE bool disableLock(const QString& currentPin);

    // Requires the current PIN in BOTH directions: enabling seals the device
    // secret under the PIN, disabling unseals it, and both need the key. The
    // stored flag is only written once the seal/unseal has actually
    // succeeded.
    Q_INVOKABLE bool setCredentialPinGateEnabled(bool enabled, const QString& currentPin);

    // Hostile Location Protection. Requires the current PIN in both
    // directions, like the credential gate.
    //
    // Cannot take effect in the running process -- the database is chosen
    // before the composition root exists -- so on success this emits
    // relaunchRequired() and the host restarts. Returns true when the mode
    // was accepted, meaning "a relaunch is now happening", not "the mode is
    // already active".
    //
    // Turning it ON erases the on-disk profile FIRST and refuses the mode if
    // that erase could not be completed. It used to relaunch regardless,
    // logging a qCritical nobody reads: the replacement process came up in
    // memory-only mode, showed Hostile Location Protection as on, and the
    // mail it was supposed to have destroyed was still on the disk. A mode
    // whose entire promise is "nothing is on this disk" must not report
    // itself active on unverified ground.
    Q_INVOKABLE bool setHostileLocationEnabled(bool enabled, const QString& currentPin);

    // Re-locks immediately. Called by the explicit "Lock now" action; a
    // no-op when the lock is disabled.
    //
    // Always immediate, and always cancels a grace period already running.
    // A user who asks to lock is not asking to lock in five minutes, and if
    // this honoured the grace there would be no way to lock at once at all.
    Q_INVOKABLE void lockNow();

    // The host's "the window went away" trigger: minimised, hidden to tray,
    // or the application state left Active.
    //
    // Locks immediately when the grace period is 0, WITHOUT going through a
    // zero-delay timer -- a queued timer would leave the app unlocked for the
    // rest of the current event-loop pass, which is exactly the window this
    // is supposed to close. Otherwise it starts the grace timer.
    Q_INVOKABLE void lockAfterGrace();

    // The host's "the window came back" trigger. Cancels a running grace
    // period. Safe to call when none is running.
    Q_INVOKABLE void cancelPendingLock();

    // True while a grace period is running -- the app is unlocked but on its
    // way to locking. Exposed so a host can show it, and so tests can assert
    // on it rather than on a timer they cannot see.
    Q_PROPERTY(bool lockPending READ lockPending NOTIFY lockPendingChanged)
    bool lockPending() const;

    int backgroundGraceSeconds() const;

    // Requires the current PIN, for the same reason the erase threshold does:
    // this weakens the lock, and someone at an unlocked session must not be
    // able to grant themselves five minutes of access after the owner walks
    // away. Clamped to LockoutPolicy's range.
    Q_INVOKABLE bool setBackgroundGraceSeconds(int seconds, const QString& currentPin);

    int wipeAfterAttempts() const;

    // Requires the current PIN whenever a lock is set, checked through the
    // same rate-limited path every other PIN prompt uses. Returns false if
    // the PIN is wrong, if a backoff is in force, or if the store refused the
    // write -- in which case the policy is unchanged and the caller must not
    // report success. `attempts` is clamped to LockoutPolicy's range.
    Q_INVOKABLE bool setWipeAfterAttempts(int attempts, const QString& currentPin);

    bool wipeIncomplete() const;

    bool databaseUnencrypted() const;
    bool databaseMemoryOnly() const;
    // Also memory-only, but for a reason with a different fix: the data
    // directory is readable by other accounts on this machine and could not
    // be tightened, so this session refuses to write mail or contacts into
    // it. Distinct from databaseMemoryOnly, which is about the keyring.
    bool dataDirectoryUnprotected() const;
    // Host-set at startup from openProfileDatabase()'s answer. Not
    // Q_INVOKABLE: QML must not be able to claim the database is encrypted.
    void setDatabaseMode(ProfileDatabaseMode mode);
    // Host-set, at startup, from the interrupted-wipe recovery attempt. Not
    // Q_INVOKABLE: QML must never be able to clear this.
    void setWipeIncomplete(bool incomplete);

    // Host-set at startup: erases the on-disk profile, returning false if
    // anything survived. Called by setHostileLocationEnabled(true) before it
    // commits to the mode.
    //
    // A callable returning bool rather than a signal, for the same reason
    // CredentialSealer is not a signal (see core/security/CredentialSealer.h):
    // a void return cannot tell this class whether the erase it ordered
    // actually happened. Kept out of the constructor because it needs the
    // composition root's LocalDataWipe, which is built later.
    //
    // Defaults to "could not erase", so a host that never wires it refuses
    // the mode rather than enabling it on ground nothing has checked.
    void setOnDiskDataWiper(std::function<bool()> wiper);

signals:
    void lockedChanged();
    void lockStateChanged();
    void lockoutChanged();
    void credentialsUnavailableChanged();
    void wipeIncompleteChanged();
    void databaseModeChanged();
    void lockPendingChanged();

    // Emitted when failed attempts reach the wipe threshold. The host owns
    // what "wipe" means (database, caches, pairing) -- this class knows only
    // that the threshold was crossed.
    void wipeRequested();

    // Emitted when a setting has been persisted that only takes effect at
    // startup, and any erase it required has already succeeded. The host
    // relaunches. Carried as a signal so this class stays free of process
    // concerns.
    void relaunchRequired();

private:
    void setCredentialsUnavailable(bool unavailable);
    // Records a failed attempt durably. False means the store refused, which
    // this class treats as a hard stop -- see tryUnlock().
    bool recordFailedAttempt(qint64 nowEpochMs);
    // Every PIN verification outside tryUnlock() goes through here, so the
    // lockout, the session floor and the wipe threshold apply to the Settings
    // prompts too. A PIN is only as strong as the slowest way to guess it.
    bool verifyPinRateLimited(const QString& pin);

    // The three conditions under which a PIN must NOT be checked at all,
    // in the order that matters. True means "refuse, do not verify".
    //
    // One function rather than one copy per entry point, because the copies
    // drifted: tryUnlock() had all three and verifyPinRateLimited() -- which
    // guards change-PIN, disable-lock, the credential gate toggle and
    // Hostile Location Protection -- was missing the session floor. That
    // floor is the only one of the three an attacker holding the machine
    // cannot defeat by moving the system clock forward, so its absence made
    // the Settings prompts the cheap way to grind a six-digit PIN while the
    // unlock screen next to them was properly rate-limited.
    bool mustRefuseGuess();

    AppLockStore& m_store;
    SettingsStore& m_settingsStore;
    CredentialSealer& m_sealer;
    bool m_locked = false;
    bool m_credentialsUnavailable = false;
    // Latched once the attempt counter could not be persisted. Never
    // cleared: a store that failed to record a guess cannot be trusted to
    // rate-limit the next one, and a relaunch is a cheap recovery.
    bool m_attemptRecordingBroken = false;
    bool m_wipeIncomplete = false;
    ProfileDatabaseMode m_databaseMode = ProfileDatabaseMode::PlaintextOnDisk;
    std::function<bool()> m_wipeOnDiskData = []() { return false; };
    QTimer m_graceTimer;
    // In-process floor under the persisted counter, so even a store that
    // accepts writes and silently loses them still caps guessing per launch.
    int m_sessionFailedAttempts = 0;
};
