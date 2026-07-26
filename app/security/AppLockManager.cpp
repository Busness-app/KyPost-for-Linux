#include "security/AppLockManager.h"

#include "security/AppLockStore.h"
#include "security/LockoutPolicy.h"
#include "stores/SettingsStore.h"

#include <QDateTime>

AppLockManager::AppLockManager(AppLockStore& store, SettingsStore& settingsStore, QObject* parent)
    : QObject(parent)
    , m_store(store)
    , m_settingsStore(settingsStore)
    // Start locked whenever the lock is on: a freshly-started process has
    // by definition not been unlocked yet.
    , m_locked(store.lockEnabled())
{
}

bool AppLockManager::lockEnabled() const
{
    return m_store.lockEnabled();
}

bool AppLockManager::locked() const
{
    return m_locked;
}

int AppLockManager::remainingLockoutSeconds() const
{
    return LockoutPolicy::remainingLockoutSeconds(m_store.lockoutUntilEpochMs(),
                                                   QDateTime::currentMSecsSinceEpoch());
}

int AppLockManager::failedAttempts() const
{
    return m_store.failedAttemptCount();
}

bool AppLockManager::credentialPinGateEnabled() const
{
    return m_store.credentialPinGateEnabled();
}

bool AppLockManager::tryUnlock(const QString& pin)
{
    if (!m_store.lockEnabled()) {
        // Nothing to unlock. Treat as success so a host that calls this
        // defensively doesn't wedge itself.
        if (m_locked) {
            m_locked = false;
            emit lockedChanged();
        }
        return true;
    }

    // Refuse outright while a backoff is in force -- otherwise the delay is
    // decorative and an attacker can keep guessing at full speed.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (LockoutPolicy::isLockedOut(m_store.lockoutUntilEpochMs(), now))
        return false;

    if (m_store.verifyPin(pin)) {
        m_store.setFailedAttemptCount(0);
        m_store.setLockoutUntilEpochMs(0);
        m_locked = false;
        emit unlockedWithPin(pin);
        emit lockedChanged();
        emit lockoutChanged();
        return true;
    }

    const int attempts = m_store.failedAttemptCount() + 1;
    m_store.setFailedAttemptCount(attempts);

    const qint64 backoffMs = LockoutPolicy::lockoutDurationMs(attempts);
    m_store.setLockoutUntilEpochMs(backoffMs > 0 ? now + backoffMs : 0);
    emit lockoutChanged();

    if (LockoutPolicy::shouldWipe(attempts))
        emit wipeRequested();

    return false;
}

bool AppLockManager::setPin(const QString& currentPin, const QString& newPin)
{
    // Changing an existing PIN requires the old one; setting the first PIN
    // does not (there is nothing to prove yet).
    if (m_store.lockEnabled() && !m_store.verifyPin(currentPin))
        return false;
    if (newPin.isEmpty())
        return false;

    if (!m_store.setPin(newPin))
        return false;

    emit lockStateChanged();
    emit lockoutChanged();
    return true;
}

bool AppLockManager::disableLock(const QString& currentPin)
{
    if (!m_store.lockEnabled())
        return true;
    if (!m_store.verifyPin(currentPin))
        return false;

    // If the credential gate is on, the device secret is currently sealed
    // under this PIN. Unseal it before the PIN is destroyed, or the pairing
    // becomes unrecoverable and the user has to re-pair.
    const bool gateWasEnabled = m_store.credentialPinGateEnabled();
    if (gateWasEnabled)
        emit credentialGateChanged(false, currentPin);

    if (!m_store.clear())
        return false;

    m_locked = false;
    emit lockedChanged();
    emit lockStateChanged();
    emit lockoutChanged();
    return true;
}

bool AppLockManager::setCredentialPinGateEnabled(bool enabled, const QString& currentPin)
{
    // The gate is meaningless without a PIN to gate on, and the UI enforces
    // the dependency too -- but enforce it here as well so a QML mistake
    // can't produce a "sealed under nothing" state.
    if (!m_store.lockEnabled())
        return false;
    if (!m_store.verifyPin(currentPin))
        return false;
    if (m_store.credentialPinGateEnabled() == enabled)
        return true;

    // Seal/unseal first: if the host cannot re-wrap the secret, the stored
    // flag must not claim it did.
    emit credentialGateChanged(enabled, currentPin);

    if (!m_store.setCredentialPinGateEnabled(enabled))
        return false;

    emit lockStateChanged();
    return true;
}

bool AppLockManager::hostileLocationEnabled() const
{
    return m_settingsStore.hostileLocationProtectionEnabled();
}

bool AppLockManager::setHostileLocationEnabled(bool enabled, const QString& currentPin)
{
    // Same dependency as the credential gate: this mode is only meaningful
    // alongside a lock, and the UI gates it that way too.
    if (!m_store.lockEnabled())
        return false;
    if (!m_store.verifyPin(currentPin))
        return false;
    if (m_settingsStore.hostileLocationProtectionEnabled() == enabled)
        return true;

    m_settingsStore.setHostileLocationProtectionEnabled(enabled);
    emit lockStateChanged();

    // Turning it ON must erase whatever is already on disk -- otherwise the
    // mode starts "protecting" a machine that still holds every cached
    // message. Turning it OFF has nothing to erase: the on-disk database was
    // already deleted when the mode was switched on, and the next launch
    // rebuilds it from the server via the roots' existing startup refresh.
    emit relaunchRequired(/*wipeDisk=*/enabled);
    return true;
}

void AppLockManager::lockNow()
{
    if (!m_store.lockEnabled() || m_locked)
        return;
    m_locked = true;
    emit relockRequested();
    emit lockedChanged();
}
