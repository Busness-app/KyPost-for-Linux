#pragma once

#include <QObject>
#include <QString>

class AppLockStore;
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

public:
    AppLockManager(AppLockStore& store, SettingsStore& settingsStore, QObject* parent = nullptr);

    bool lockEnabled() const;
    bool locked() const;
    int remainingLockoutSeconds() const;
    int failedAttempts() const;
    bool credentialPinGateEnabled() const;
    bool hostileLocationEnabled() const;

    // Returns true and clears `locked` on the right PIN. On a wrong PIN,
    // increments the attempt count, applies the backoff, and -- at the wipe
    // threshold -- emits wipeRequested() and returns false.
    Q_INVOKABLE bool tryUnlock(const QString& pin);

    // Turns the lock on (or changes the PIN). Changing an existing PIN
    // requires the current one; `currentPin` is ignored when no lock is set
    // yet. Returns false if the current PIN is wrong or the store write
    // fails.
    Q_INVOKABLE bool setPin(const QString& currentPin, const QString& newPin);

    // Turns the lock off. Requires the current PIN -- otherwise anyone with
    // the app open could remove the protection that the lock exists to
    // provide.
    Q_INVOKABLE bool disableLock(const QString& currentPin);

    // Requires the current PIN in BOTH directions: enabling seals the device
    // secret under the PIN, disabling unseals it, and both need the key.
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

    // Emitted when failed attempts reach the wipe threshold. The host owns
    // what "wipe" means (database, caches, pairing) -- this class knows only
    // that the threshold was crossed.
    void wipeRequested();

    // Emitted when the credential gate is toggled, so the host can seal or
    // unseal the stored device secret. Carries the PIN because the host
    // needs it to derive the key, and holding it here would keep it in
    // memory longer than the operation needs.
    void credentialGateChanged(bool enabled, const QString& pin);

    // Emitted on a successful unlock, carrying the PIN so the host can open
    // the sealed device secret for this session. Carried rather than stored
    // so the PIN lives no longer than the operation needs.
    void unlockedWithPin(const QString& pin);

    // Emitted when the app re-locks, so the host can drop the in-memory
    // plaintext secret. No PIN: re-locking needs no key, because the sealed
    // blob on disk was never destroyed.
    void relockRequested();

    // Emitted when a setting has been persisted that only takes effect at
    // startup. The host wipes on-disk data if `wipeDisk` is set, then
    // relaunches. Carried as a signal so this class stays free of process
    // and filesystem concerns.
    void relaunchRequired(bool wipeDisk);

private:
    AppLockStore& m_store;
    SettingsStore& m_settingsStore;
    bool m_locked = false;
};
