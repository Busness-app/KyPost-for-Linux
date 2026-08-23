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
    m_graceTimer.setSingleShot(true);
    // The timer only ever fires the lock. Cancelling is cancelPendingLock()'s
    // job; there is no path where an expiring grace does anything other than
    // lock, which is what makes "the app is unlocked but on its way to
    // locking" a state with exactly one exit.
    connect(&m_graceTimer, &QTimer::timeout, this, [this]() {
        emit lockPendingChanged();
        lockNow();
    });
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

bool AppLockManager::storeUnavailable() const
{
    return !m_store.storeReadable();
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

bool AppLockManager::mustRefuseGuess()
{
    // A store that could not record the last failure cannot rate-limit this
    // one either. Refuse for the rest of the process rather than serve an
    // unmetered oracle; relaunching is the recovery.
    if (m_attemptRecordingBroken)
        return true;

    // Refuse outright while a backoff is in force -- otherwise the delay is
    // decorative and an attacker can keep guessing at full speed.
    if (LockoutPolicy::isLockedOut(m_store.lockoutUntilEpochMs(), QDateTime::currentMSecsSinceEpoch()))
        return true;

    // Session floor, independent of the wall clock the persisted deadline
    // uses: an attacker holding the machine can move the system clock
    // forward to skip every backoff, but cannot move this counter.
    //
    // NOT the wipe threshold, which the user can lower, raise, or switch off
    // entirely. These were the same number until the threshold became
    // configurable, and leaving them joined would have meant "never erase
    // this device" also silently removed the only limit a clock change
    // cannot defeat. The erase is the user's to decline; the rate limit is
    // not.
    if (LockoutPolicy::shouldRefuseForSession(m_sessionFailedAttempts)) {
        if (LockoutPolicy::shouldWipe(m_sessionFailedAttempts, m_store.wipeAfterAttempts()))
            emit wipeRequested();
        return true;
    }

    return false;
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

    if (mustRefuseGuess())
        return false;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
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

    if (LockoutPolicy::shouldWipe(failedAttempts(), m_store.wipeAfterAttempts()))
        emit wipeRequested();

    return false;
}

bool AppLockManager::verifyPinRateLimited(const QString& pin)
{
    // The Settings prompts (change PIN, disable lock, toggle the credential
    // gate, toggle Hostile Location Protection) used to call
    // AppLockStore::verifyPin() directly, bypassing every control tryUnlock()
    // applies: no backoff, no persisted attempt counter, and no progress
    // toward the ten-failure wipe. That is exactly the "unlimited guessing
    // oracle behind a UI that still claims to be rate-limited" this class
    // already refuses to serve on the unlock path.
    //
    // Shared with tryUnlock() rather than restated: when these were two
    // copies, this one was missing the session floor, so a clock moved
    // forward turned the Settings prompts back into that same oracle.
    if (mustRefuseGuess())
        return false;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_store.verifyPin(pin)) {
        if (!m_store.setFailedAttemptCount(0) || !m_store.setLockoutUntilEpochMs(0))
            m_attemptRecordingBroken = true;
        m_sessionFailedAttempts = 0;
        emit lockoutChanged();
        return true;
    }

    if (!recordFailedAttempt(now))
        return false;
    if (LockoutPolicy::shouldWipe(failedAttempts(), m_store.wipeAfterAttempts()))
        emit wipeRequested();
    return false;
}

bool AppLockManager::setPin(const QString& currentPin, const QString& newPin)
{
    // Changing an existing PIN requires the old one; setting the first PIN
    // does not (there is nothing to prove yet).
    if (m_store.lockEnabled() && !verifyPinRateLimited(currentPin))
        return false;

    // Enforced here, in C++, not only in Settings.qml: QML is a presentation
    // layer, not a security boundary, and this PIN is the key-encryption key
    // for the relay device secret whenever the credential gate is on.
    if (!PinPolicy::isAcceptable(newPin))
        return false;

    // Changing the PIN while the secret is sealed under the OLD one would
    // strand it, exactly like disableLock() below. Re-wrap first, and only
    // install the new PIN if that worked.
    //
    // reseal(), NOT unsealPermanently() + seal(). Those two compose
    // correctly on paper and were what this did, and between them the relay
    // device secret sat in the keychain in the clear -- for the length of
    // two 150k-iteration PBKDF2 derivations -- while
    // applock.credentialPinGateEnabled still read "1". A crash, an OOM kill
    // or a power loss in that window left it plaintext on disk permanently,
    // under a UI that went on reporting the protection as On. See
    // CredentialSealer::reseal.
    const bool gateEnabled = m_store.lockEnabled() && m_store.credentialPinGateEnabled();
    if (gateEnabled && !m_sealer.reseal(currentPin, newPin))
        return false;

    if (!m_store.setPin(newPin)) {
        // The secret is now under newPin but the PIN record still says
        // currentPin. Put the secret back so the two agree again; a failed
        // PIN change must change nothing.
        if (gateEnabled)
            m_sealer.reseal(newPin, currentPin);
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
    if (!verifyPinRateLimited(currentPin))
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
    if (!verifyPinRateLimited(currentPin))
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
    if (!verifyPinRateLimited(currentPin))
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

int AppLockManager::wipeAfterAttempts() const
{
    return m_store.wipeAfterAttempts();
}

bool AppLockManager::setWipeAfterAttempts(int attempts, const QString& currentPin)
{
    // The PIN is required, and checked through the rate-limited path, for the
    // same reason disabling the lock is: this decides whether a device that
    // is being guessed at erases itself. Someone who walks up to an unlocked
    // session must not be able to switch that off, and the check must not be
    // an unmetered oracle either.
    if (m_store.lockEnabled() && !verifyPinRateLimited(currentPin))
        return false;

    const int clamped = LockoutPolicy::clampWipeThreshold(attempts);
    if (!m_store.setWipeAfterAttempts(clamped))
        return false;

    emit lockStateChanged();
    return true;
}

bool AppLockManager::wipeIncomplete() const
{
    return m_wipeIncomplete;
}

void AppLockManager::setWipeIncomplete(bool incomplete)
{
    if (m_wipeIncomplete == incomplete)
        return;
    m_wipeIncomplete = incomplete;
    emit wipeIncompleteChanged();
}

bool AppLockManager::lockPending() const
{
    return m_graceTimer.isActive();
}

int AppLockManager::backgroundGraceSeconds() const
{
    return m_store.backgroundGraceSeconds();
}

bool AppLockManager::setBackgroundGraceSeconds(int seconds, const QString& currentPin)
{
    // PIN-gated for the same reason the erase threshold is: this weakens the
    // lock, and someone who walks up to an unlocked session must not be able
    // to grant themselves five minutes of access after the owner leaves.
    if (m_store.lockEnabled() && !verifyPinRateLimited(currentPin))
        return false;

    const int clamped = LockoutPolicy::clampBackgroundGraceSeconds(seconds);
    if (!m_store.setBackgroundGraceSeconds(clamped))
        return false;

    // A shortened grace must not leave a longer one already counting down --
    // the user just asked for less exposure, not for this one time to be
    // exempt.
    if (m_graceTimer.isActive()) {
        m_graceTimer.stop();
        emit lockPendingChanged();
        lockAfterGrace();
    }

    emit lockStateChanged();
    return true;
}

void AppLockManager::lockAfterGrace()
{
    if (!m_store.lockEnabled() || m_locked)
        return;

    const int graceSeconds = m_store.backgroundGraceSeconds();
    if (graceSeconds <= 0) {
        // Synchronously, NOT via a zero-delay timer. A queued single-shot
        // would leave the app unlocked for the remainder of this event-loop
        // pass, which is precisely the window "lock immediately" exists to
        // close -- and it is the default, so it is the path almost every
        // user is on.
        lockNow();
        return;
    }

    if (m_graceTimer.isActive())
        return; // already counting down; a second background event does not extend it

    m_graceTimer.start(graceSeconds * 1000);
    emit lockPendingChanged();
}

void AppLockManager::cancelPendingLock()
{
    if (!m_graceTimer.isActive())
        return;
    m_graceTimer.stop();
    emit lockPendingChanged();
}

bool AppLockManager::databaseUnencrypted() const
{
    return m_databaseMode == ProfileDatabaseMode::PlaintextOnDisk;
}

bool AppLockManager::databaseMemoryOnly() const
{
    return m_databaseMode == ProfileDatabaseMode::InMemoryNoKeyStorage;
}

void AppLockManager::setDatabaseMode(ProfileDatabaseMode mode)
{
    if (m_databaseMode == mode)
        return;
    m_databaseMode = mode;
    emit databaseModeChanged();
}

void AppLockManager::lockNow()
{
    // Stopped before the early return below, not after: an explicit lock
    // request must clear a pending one even when the lock is already engaged
    // or disabled, or a timer started while the lock was on could fire after
    // the user had turned it off.
    const bool wasPending = m_graceTimer.isActive();
    m_graceTimer.stop();
    if (wasPending)
        emit lockPendingChanged();

    if (!m_store.lockEnabled() || m_locked)
        return;
    m_locked = true;
    // Drop the session plaintext of the device secret, so a locked app
    // cannot service push/MFA. Cannot fail -- the durable blob is untouched.
    m_sealer.relock();
    setCredentialsUnavailable(false);
    emit lockedChanged();
}
