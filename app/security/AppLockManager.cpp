#include "security/AppLockManager.h"

#include "security/AppLockStore.h"
#include "security/CredentialSealer.h"
#include "security/LockoutPolicy.h"
#include "security/PinPolicy.h"
#include "stores/SettingsStore.h"

#include <KLocalizedString>

#include <QDateTime>
#include <algorithm>

AppLockManager::AppLockManager(AppLockStore& store, SettingsStore& settingsStore, CredentialSealer& sealer,
                                QObject* parent)
    : QObject(parent)
    , m_store(store)
    , m_settingsStore(settingsStore)
    , m_sealer(sealer)
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
    // The larger of the two counters. The persisted one survives a relaunch;
    // the session one survives a store that accepts writes and loses them.
    return std::max(m_store.failedAttemptCount(), m_sessionFailedAttempts);
}

bool AppLockManager::credentialPinGateEnabled() const
{
    return m_store.credentialPinGateEnabled();
}

bool AppLockManager::credentialsUnavailable() const
{
    return m_credentialsUnavailable;
}

int AppLockManager::minimumPinLength() const
{
    return PinPolicy::kMinimumLength;
}

void AppLockManager::setCredentialsUnavailable(bool unavailable)
{
    if (m_credentialsUnavailable == unavailable)
        return;
    m_credentialsUnavailable = unavailable;
    emit credentialsUnavailableChanged();
}

QString AppLockManager::pinRejectionReason(const QString& pin) const
{
    switch (PinPolicy::validate(pin)) {
    case PinPolicy::Rejection::Ok:
        return QString();
    case PinPolicy::Rejection::TooShort:
        return i18np("Use at least %1 digit.", "Use at least %1 digits.", PinPolicy::kMinimumLength);
    case PinPolicy::Rejection::TooLong:
        return i18n("That PIN is too long (limit %1 characters).", PinPolicy::kMaximumLength);
    case PinPolicy::Rejection::AllSameCharacter:
        return i18n("Do not use the same digit repeated.");
    case PinPolicy::Rejection::Sequential:
        return i18n("Avoid a run of consecutive digits.");
    }
    return QString();
}

bool AppLockManager::recordFailedAttempt(qint64 nowEpochMs)
{
    ++m_sessionFailedAttempts;

    const int attempts = std::max(m_store.failedAttemptCount() + 1, m_sessionFailedAttempts);
    const qint64 backoffMs = LockoutPolicy::lockoutDurationMs(attempts);

    // Both writes checked. If either fails, the backoff and the wipe
    // threshold silently stop existing: failedAttemptCount() would keep
    // reading its value_or("0") default, lockoutDurationMs(0) is 0, and
    // shouldWipe(0) is false -- an unlimited guessing oracle behind a UI
    // that still claims to be rate-limited.
    const bool countStored = m_store.setFailedAttemptCount(attempts);
    const bool deadlineStored = m_store.setLockoutUntilEpochMs(backoffMs > 0 ? nowEpochMs + backoffMs : 0);
    emit lockoutChanged();

    if (!countStored || !deadlineStored) {
        m_attemptRecordingBroken = true;
        return false;
    }
    return true;
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

    // A store that could not record the last failure cannot rate-limit this
    // one either. Refuse for the rest of the process rather than serve an
    // unmetered oracle; relaunching is the recovery.
    if (m_attemptRecordingBroken)
        return false;

    // Refuse outright while a backoff is in force -- otherwise the delay is
    // decorative and an attacker can keep guessing at full speed.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (LockoutPolicy::isLockedOut(m_store.lockoutUntilEpochMs(), now))
        return false;

    // Session floor, independent of the wall clock the persisted deadline
    // uses: an attacker holding the machine can move the system clock
    // forward to skip every backoff, but cannot move this counter.
    if (LockoutPolicy::shouldWipe(m_sessionFailedAttempts)) {
        emit wipeRequested();
        return false;
    }

    if (m_store.verifyPin(pin)) {
        // Both checked, for the same reason recordFailedAttempt() checks
        // them: a reset that silently fails leaves the persisted counter
        // stale-high, and failedAttempts() takes the MAX of persisted and
        // session -- so a user who mistyped nine times, unlocked correctly,
        // and then mistyped once more would cross the wipe threshold and
        // lose their local mail on their second mistake. Marking the store
        // broken makes every later attempt this process refuse at the guard
        // above, which reaches neither the backoff nor the wipe; relaunching
        // is the recovery, and the stale counter is corrected by the next
        // successful reset.
        const bool countCleared = m_store.setFailedAttemptCount(0);
        const bool deadlineCleared = m_store.setLockoutUntilEpochMs(0);
        if (!countCleared || !deadlineCleared)
            m_attemptRecordingBroken = true;

        m_sessionFailedAttempts = 0;
        m_locked = false;

        // Open the sealed device secret for this session. A failure here is
        // NOT an unlock failure -- the PIN was right -- but it does mean
        // every authenticated request will 401, so it is surfaced rather
        // than left for the user to misdiagnose as a server outage.
        if (m_store.credentialPinGateEnabled())
            setCredentialsUnavailable(!m_sealer.unsealForSession(pin));
        else
            setCredentialsUnavailable(false);

        emit lockedChanged();
        emit lockoutChanged();
        return true;
    }

    if (!recordFailedAttempt(now))
        return false;

    if (LockoutPolicy::shouldWipe(failedAttempts()))
        emit wipeRequested();

    return false;
}

bool AppLockManager::setPin(const QString& currentPin, const QString& newPin)
{
    // Changing an existing PIN requires the old one; setting the first PIN
    // does not (there is nothing to prove yet).
    if (m_store.lockEnabled() && !m_store.verifyPin(currentPin))
        return false;

    // Enforced here, in C++, not only in Settings.qml: QML is a presentation
    // layer, not a security boundary, and this PIN is the key-encryption key
    // for the relay device secret whenever the credential gate is on.
    if (!PinPolicy::isAcceptable(newPin))
        return false;

    // Changing the PIN while the secret is sealed under the OLD one would
    // strand it, exactly like disableLock() below. Re-wrap first, and only
    // install the new PIN if that worked.
    const bool gateEnabled = m_store.lockEnabled() && m_store.credentialPinGateEnabled();
    if (gateEnabled && !m_sealer.unsealPermanently(currentPin))
        return false;

    if (!m_store.setPin(newPin)) {
        // Put the secret back the way it was, so a failed PIN change does
        // not silently leave the credential gate off in fact while the
        // stored flag still claims it is on.
        if (gateEnabled)
            m_sealer.seal(currentPin);
        return false;
    }

    if (gateEnabled && !m_sealer.seal(newPin)) {
        // The PIN changed but the secret could not be re-wrapped. Report the
        // gate as off rather than lying about it; the user can re-enable it.
        m_store.setCredentialPinGateEnabled(false);
        emit lockStateChanged();
        emit lockoutChanged();
        return false;
    }

    m_attemptRecordingBroken = false;
    m_sessionFailedAttempts = 0;
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
    // under this PIN. Unseal it BEFORE the PIN is destroyed -- and refuse
    // outright if that fails, because clear() below erases the salt and hash
    // and the blob would then be locked behind a key that exists nowhere.
    // Leaving the lock on is recoverable; an unrecoverable pairing is not.
    if (m_store.credentialPinGateEnabled() && !m_sealer.unsealPermanently(currentPin))
        return false;

    if (!m_store.clear())
        return false;

    m_locked = false;
    m_attemptRecordingBroken = false;
    m_sessionFailedAttempts = 0;
    setCredentialsUnavailable(false);
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

    // Seal/unseal first, and CHECK it: if the secret could not be re-wrapped
    // the stored flag must not claim it was. This is the whole reason
    // CredentialSealer is an interface rather than the signal it replaced --
    // a signal returns void, so this branch could not previously exist.
    const bool sealOk = enabled ? m_sealer.seal(currentPin) : m_sealer.unsealPermanently(currentPin);
    if (!sealOk)
        return false;

    if (!m_store.setCredentialPinGateEnabled(enabled)) {
        // Flag write failed after the crypto succeeded. Undo the crypto so
        // the two agree again, rather than leaving a sealed secret the
        // stored flag says is plaintext (or the reverse).
        if (enabled)
            m_sealer.unsealPermanently(currentPin);
        else
            m_sealer.seal(currentPin);
        return false;
    }

    if (!enabled)
        setCredentialsUnavailable(false);
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
    // Drop the session plaintext of the device secret, so a locked app
    // cannot service push/MFA. Cannot fail -- the durable blob is untouched.
    m_sealer.relock();
    setCredentialsUnavailable(false);
    emit lockedChanged();
}
