#pragma once

#include <QObject>
#include <QString>

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
    // relaunchRequired() and the host wipes and restarts. Returns true when
    // the setting was accepted, meaning "a relaunch is now happening", not
    // "the mode is already active".
    Q_INVOKABLE bool setHostileLocationEnabled(bool enabled, const QString& currentPin);

    // Re-locks immediately. Called by the host's lock triggers; a no-op when
    // the lock is disabled.
    Q_INVOKABLE void lockNow();

signals:
    void lockedChanged();
    void lockStateChanged();
    void lockoutChanged();
    void credentialsUnavailableChanged();

    // Emitted when failed attempts reach the wipe threshold. The host owns
    // what "wipe" means (database, caches, pairing) -- this class knows only
    // that the threshold was crossed.
    void wipeRequested();

    // Emitted when a setting has been persisted that only takes effect at
    // startup. The host wipes on-disk data if `wipeDisk` is set, then
    // relaunches. Carried as a signal so this class stays free of process
    // and filesystem concerns.
    void relaunchRequired(bool wipeDisk);

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
    // In-process floor under the persisted counter, so even a store that
    // accepts writes and silently loses them still caps guessing per launch.
    int m_sessionFailedAttempts = 0;
};
