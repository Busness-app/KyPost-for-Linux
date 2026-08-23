#include "security/AppLockManager.h"

#include "security/AppLockStore.h"
#include "security/CredentialSealer.h"
#include "security/LockoutPolicy.h"
#include "security/PinPolicy.h"
#include "stores/SecureStore.h"
#include "stores/SecureStoreFile.h"
#include "stores/SettingsStore.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace {

// Records what AppLockManager asked for, and can refuse -- the failure
// direction is the whole point, since the bugs this class had were all
// "the seal silently didn't happen and nothing noticed".
class FakeSealer : public CredentialSealer
{
public:
    bool sealShouldFail = false;
    bool unsealShouldFail = false;

    bool resealShouldFail = false;

    int sealCalls = 0;
    int unsealPermanentlyCalls = 0;
    int unsealForSessionCalls = 0;
    int resealCalls = 0;
    int relockCalls = 0;
    QString lastPin;
    bool sealed = false;
    // The PIN the secret is currently wrapped under, and whether the
    // plaintext was ever written out. A PIN change must move the first
    // without ever setting the second.
    QString sealedUnderPin;
    bool plaintextEverExposed = false;

    bool seal(const QString& pin) override
    {
        ++sealCalls;
        lastPin = pin;
        if (sealShouldFail)
            return false;
        sealed = true;
        sealedUnderPin = pin;
        return true;
    }

    bool unsealPermanently(const QString& pin) override
    {
        ++unsealPermanentlyCalls;
        lastPin = pin;
        if (unsealShouldFail)
            return false;
        sealed = false;
        sealedUnderPin.clear();
        // This is the operation that puts the plaintext on disk. Recorded,
        // because a PIN change must not reach it.
        plaintextEverExposed = true;
        return true;
    }

    bool reseal(const QString& oldPin, const QString& newPin) override
    {
        ++resealCalls;
        lastPin = newPin;
        if (resealShouldFail)
            return false;
        if (sealed && sealedUnderPin != oldPin)
            return false; // wrong old PIN cannot open the blob
        sealed = true;
        sealedUnderPin = newPin;
        return true;
    }

    bool unsealForSession(const QString& pin) override
    {
        ++unsealForSessionCalls;
        lastPin = pin;
        return !unsealShouldFail;
    }

    void relock() override { ++relockCalls; }
    bool isSealed() const override { return sealed; }
};

// A SecureStore that works normally until told to start refusing writes --
// exactly what a Secret Service provider going away mid-session looks like.
class FlakySecureStore : public SecureStore
{
public:
    bool writesFail = false;

    bool set(const QString& key, const QString& value) override
    {
        if (writesFail)
            return false;
        m_values.insert(key, value);
        return true;
    }
    std::optional<QString> get(const QString& key) const override
    {
        const auto it = m_values.constFind(key);
        return it == m_values.constEnd() ? std::nullopt : std::optional<QString>(*it);
    }
    bool remove(const QString& key) override
    {
        if (writesFail)
            return false;
        m_values.remove(key);
        return true;
    }
    bool contains(const QString& key) const override { return m_values.contains(key); }

private:
    QHash<QString, QString> m_values;
};

// A SecureStore that cannot be consulted at all -- no Secret Service
// provider running, a locked wallet, no D-Bus session. Distinct from
// FlakySecureStore above, which answers reads and only refuses writes.
class UnreachableSecureStore : public SecureStore
{
public:
    ReadResult read(const QString&) const override { return ReadResult{ ReadStatus::Failed, QString() }; }
    bool set(const QString&, const QString&) override { return false; }
    std::optional<QString> get(const QString&) const override { return std::nullopt; }
    bool remove(const QString&) override { return false; }
    bool contains(const QString&) const override { return false; }
};

// PINs used throughout. Both satisfy PinPolicy (>= 6 chars, not all the same
// character, not a consecutive run) -- the old fixtures used "111111" and
// "123456", which the policy now correctly refuses.
const QString kGoodPin = QStringLiteral("419273");
const QString kOtherPin = QStringLiteral("860514");
const QString kWrongPin = QStringLiteral("305182");

} // namespace

class AppLockManagerTest : public QObject
{
    Q_OBJECT

private slots:
    void startsUnlockedWhenLockDisabled();
    void startsLockedWhenLockEnabled();
    void correctPinUnlocksAndClearsAttempts();
    void wrongPinCountsUpAndEventuallyLocksOut();
    void refusesAttemptsWhileLockedOut();
    void wipeRequestedAtThreshold();
    void disableLockRequiresCurrentPin();
    void changingPinRequiresCurrentPin();
    void credentialGateRequiresPinAndSealsThroughSealer();
    void lockNowIsANoOpWhenLockDisabled();

    // Regressions for the review findings.
    void rejectsPinsThatFailPolicy();
    void gateFlagNotSetWhenSealFails();
    void disableLockRefusedWhenUnsealFails();
    void changingPinRewrapsSecretUnderNewPin();
    void unlockReportsCredentialsUnavailableWhenUnsealFails();
    void lockNowDropsSessionSecret();
    void failsClosedWhenAttemptCountCannotBePersisted();
    void sessionCounterCapsGuessingWhenClockIsMovedForward();
    void staleAttemptCounterCannotTriggerAWipeAfterASuccessfulUnlock();
    void settingsPromptsHonourTheSessionFloorToo();
    void anUnreachableSecretStoreLeavesTheAppLocked();
    void aFailedPinRecordWriteMovesTheSecretBack();

    // Configurable erase-after threshold (phase 5).
    void switchingTheEraseOffKeepsTheRateLimit();
    void aLoweredEraseThresholdWipesEarlier();
    void changingTheEraseThresholdRequiresTheCurrentPin();

    // Configurable background-lock grace period (phase 5).
    void theDefaultGraceLocksSynchronouslyWithNoTimer();
    void aGracePeriodDelaysTheLockAndComingBackCancelsIt();
    void lockNowIgnoresAndCancelsTheGracePeriod();
    void changingTheGraceRequiresThePinAndIsClamped();
};

void AppLockManagerTest::startsUnlockedWhenLockDisabled()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    NullCredentialSealer sealer;
    AppLockManager manager(store, settingsStore, sealer);

    QVERIFY(!manager.lockEnabled());
    QVERIFY(!manager.locked());
}

void AppLockManagerTest::startsLockedWhenLockEnabled()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    NullCredentialSealer sealer;
    QVERIFY(store.setPin(kGoodPin));

    // A freshly-started process has by definition not been unlocked yet.
    AppLockManager manager(store, settingsStore, sealer);
    QVERIFY(manager.lockEnabled());
    QVERIFY(manager.locked());
}

void AppLockManagerTest::correctPinUnlocksAndClearsAttempts()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    NullCredentialSealer sealer;
    QVERIFY(store.setPin(kGoodPin));
    QVERIFY(store.setFailedAttemptCount(2));

    AppLockManager manager(store, settingsStore, sealer);
    QSignalSpy lockedSpy(&manager, &AppLockManager::lockedChanged);

    QVERIFY(manager.tryUnlock(kGoodPin));
    QVERIFY(!manager.locked());
    QCOMPARE(lockedSpy.count(), 1);
    // A successful unlock forgives prior failures -- the owner is present.
    QCOMPARE(manager.failedAttempts(), 0);
    QCOMPARE(manager.remainingLockoutSeconds(), 0);
}

void AppLockManagerTest::wrongPinCountsUpAndEventuallyLocksOut()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    NullCredentialSealer sealer;
    QVERIFY(store.setPin(kGoodPin));

    AppLockManager manager(store, settingsStore, sealer);

    // The first few are free -- fat-fingering shouldn't cost a delay.
    for (int i = 1; i <= LockoutPolicy::kFreeAttempts; ++i) {
        QVERIFY(!manager.tryUnlock(kWrongPin));
        QCOMPARE(manager.failedAttempts(), i);
        QCOMPARE(manager.remainingLockoutSeconds(), 0);
    }
    QVERIFY(manager.locked());

    // The next one starts the backoff.
    QVERIFY(!manager.tryUnlock(kWrongPin));
    QCOMPARE(manager.failedAttempts(), LockoutPolicy::kFreeAttempts + 1);
    QVERIFY(manager.remainingLockoutSeconds() > 0);
}

void AppLockManagerTest::refusesAttemptsWhileLockedOut()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    NullCredentialSealer sealer;
    QVERIFY(store.setPin(kGoodPin));
    // Force an active backoff far in the future.
    QVERIFY(store.setFailedAttemptCount(4));
    QVERIFY(store.setLockoutUntilEpochMs(QDateTime::currentMSecsSinceEpoch() + 600000));

    AppLockManager manager(store, settingsStore, sealer);

    // Even the CORRECT PIN is refused during a backoff -- otherwise the
    // delay is decorative and guessing continues at full speed.
    QVERIFY(!manager.tryUnlock(kGoodPin));
    QVERIFY(manager.locked());
    // A refused attempt must not itself count as a failure, or the user
    // could be wiped purely by retrying too eagerly.
    QCOMPARE(manager.failedAttempts(), 4);
}

void AppLockManagerTest::wipeRequestedAtThreshold()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    NullCredentialSealer sealer;
    QVERIFY(store.setPin(kGoodPin));
    // One short of the threshold, with no backoff in the way.
    QVERIFY(store.setFailedAttemptCount(LockoutPolicy::kDefaultWipeThreshold - 1));
    QVERIFY(store.setLockoutUntilEpochMs(0));

    AppLockManager manager(store, settingsStore, sealer);
    QSignalSpy wipeSpy(&manager, &AppLockManager::wipeRequested);

    QVERIFY(!manager.tryUnlock(kWrongPin));
    QCOMPARE(wipeSpy.count(), 1);
}

void AppLockManagerTest::disableLockRequiresCurrentPin()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    NullCredentialSealer sealer;
    QVERIFY(store.setPin(kGoodPin));

    AppLockManager manager(store, settingsStore, sealer);

    // Anyone with the window already open must not be able to remove the
    // protection the lock exists to provide.
    QVERIFY(!manager.disableLock(kWrongPin));
    QVERIFY(manager.lockEnabled());

    QVERIFY(manager.disableLock(kGoodPin));
    QVERIFY(!manager.lockEnabled());
    QVERIFY(!manager.locked());
}

void AppLockManagerTest::changingPinRequiresCurrentPin()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    NullCredentialSealer sealer;
    AppLockManager manager(store, settingsStore, sealer);

    // Setting the FIRST pin needs no current pin -- there is nothing to
    // prove yet.
    QVERIFY(manager.setPin(QString(), kGoodPin));
    QVERIFY(manager.lockEnabled());

    // Changing it does.
    QVERIFY(!manager.setPin(kWrongPin, kOtherPin));
    QVERIFY(store.verifyPin(kGoodPin));

    QVERIFY(manager.setPin(kGoodPin, kOtherPin));
    QVERIFY(store.verifyPin(kOtherPin));

    // An empty new PIN is not a way to disable the lock.
    QVERIFY(!manager.setPin(kOtherPin, QString()));
    QVERIFY(manager.lockEnabled());
}

void AppLockManagerTest::credentialGateRequiresPinAndSealsThroughSealer()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    FakeSealer sealer;
    AppLockManager manager(store, settingsStore, sealer);

    // Meaningless without a PIN to gate on: refuse rather than produce a
    // "sealed under nothing" state.
    QVERIFY(!manager.setCredentialPinGateEnabled(true, QString()));
    QCOMPARE(sealer.sealCalls, 0);

    QVERIFY(manager.setPin(QString(), kGoodPin));

    QVERIFY(!manager.setCredentialPinGateEnabled(true, kWrongPin));
    QCOMPARE(sealer.sealCalls, 0);

    QVERIFY(manager.setCredentialPinGateEnabled(true, kGoodPin));
    QVERIFY(manager.credentialPinGateEnabled());
    QCOMPARE(sealer.sealCalls, 1);
    QCOMPARE(sealer.lastPin, kGoodPin);
    QVERIFY(sealer.isSealed());

    // Turning the lock off while the gate is on must unseal first, or the
    // pairing becomes unrecoverable.
    QVERIFY(manager.disableLock(kGoodPin));
    QCOMPARE(sealer.unsealPermanentlyCalls, 1);
    QVERIFY(!sealer.isSealed());
}

void AppLockManagerTest::lockNowIsANoOpWhenLockDisabled()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    NullCredentialSealer sealer;
    AppLockManager manager(store, settingsStore, sealer);

    QSignalSpy lockedSpy(&manager, &AppLockManager::lockedChanged);
    manager.lockNow();
    QVERIFY(!manager.locked());
    QCOMPARE(lockedSpy.count(), 0);

    QVERIFY(manager.setPin(QString(), kGoodPin));
    QVERIFY(manager.tryUnlock(kGoodPin));
    QVERIFY(!manager.locked());

    manager.lockNow();
    QVERIFY(manager.locked());
}

// --- review-finding regressions ---------------------------------------

// A one-character PIN used to be accepted: the only check was isEmpty(), and
// QML enforced nothing beyond length > 0. That PIN is the key-encryption key
// for the relay device secret whenever the credential gate is on.
void AppLockManagerTest::rejectsPinsThatFailPolicy()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    NullCredentialSealer sealer;
    AppLockManager manager(store, settingsStore, sealer);

    QVERIFY(!manager.setPin(QString(), QStringLiteral("1")));
    QVERIFY(!manager.setPin(QString(), QStringLiteral("12345")));       // too short
    QVERIFY(!manager.setPin(QString(), QStringLiteral("111111")));      // all one digit
    QVERIFY(!manager.setPin(QString(), QStringLiteral("123456")));      // ascending run
    QVERIFY(!manager.setPin(QString(), QStringLiteral("654321")));      // descending run
    QVERIFY(!manager.lockEnabled());

    QVERIFY(manager.setPin(QString(), kGoodPin));
    QVERIFY(manager.lockEnabled());

    // The rejection reason is available to the UI before committing.
    QVERIFY(!manager.pinRejectionReason(QStringLiteral("12")).isEmpty());
    QVERIFY(manager.pinRejectionReason(kGoodPin).isEmpty());
    QCOMPARE(manager.minimumPinLength(), PinPolicy::kMinimumLength);
}

// The gate flag must never claim a seal that did not happen. Previously the
// seal was announced via a void-returning signal, so this branch could not
// exist and the flag was set unconditionally.
void AppLockManagerTest::gateFlagNotSetWhenSealFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    FakeSealer sealer;
    AppLockManager manager(store, settingsStore, sealer);
    QVERIFY(manager.setPin(QString(), kGoodPin));

    sealer.sealShouldFail = true;
    QVERIFY(!manager.setCredentialPinGateEnabled(true, kGoodPin));
    QCOMPARE(sealer.sealCalls, 1);
    // The flag must still be off: reporting "your credentials are encrypted"
    // while the plaintext is on disk is worse than not offering the feature.
    QVERIFY(!manager.credentialPinGateEnabled());
    QVERIFY(!sealer.isSealed());
}

// Turning the lock off used to destroy the PIN even when the unseal failed,
// leaving an AES-GCM blob under a key that existed nowhere -- a pairing that
// could never be recovered.
void AppLockManagerTest::disableLockRefusedWhenUnsealFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    FakeSealer sealer;
    AppLockManager manager(store, settingsStore, sealer);

    QVERIFY(manager.setPin(QString(), kGoodPin));
    QVERIFY(manager.setCredentialPinGateEnabled(true, kGoodPin));
    QVERIFY(sealer.isSealed());

    sealer.unsealShouldFail = true;
    QVERIFY(!manager.disableLock(kGoodPin));

    // The PIN survives, so the blob is still openable once the store
    // recovers. A lock that cannot be turned off is recoverable; an
    // unrecoverable pairing is not.
    QVERIFY(manager.lockEnabled());
    QVERIFY(store.verifyPin(kGoodPin));
    QVERIFY(sealer.isSealed());
}

// Same stranding hazard, different door: changing the PIN while the secret
// is sealed under the old one.
void AppLockManagerTest::changingPinRewrapsSecretUnderNewPin()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    FakeSealer sealer;
    AppLockManager manager(store, settingsStore, sealer);

    QVERIFY(manager.setPin(QString(), kGoodPin));
    QVERIFY(manager.setCredentialPinGateEnabled(true, kGoodPin));
    sealer.sealCalls = 0;

    QVERIFY(manager.setPin(kGoodPin, kOtherPin));
    // Moved from the old PIN to the new one as ONE operation.
    QCOMPARE(sealer.resealCalls, 1);
    QCOMPARE(sealer.lastPin, kOtherPin);
    QVERIFY(sealer.isSealed());
    QCOMPARE(sealer.sealedUnderPin, kOtherPin);
    QVERIFY(store.verifyPin(kOtherPin));

    // The point of the whole change: the plaintext never went to disk.
    // unsealPermanently() is the call that writes it there, and a PIN
    // change has no business touching it. Doing so left the secret in the
    // clear in the keychain for the length of two 150k-iteration PBKDF2
    // derivations, with applock.credentialPinGateEnabled still reading "1"
    // -- so an interruption in that window made the exposure permanent
    // while the UI went on reporting the protection as On.
    QCOMPARE(sealer.unsealPermanentlyCalls, 0);
    QVERIFY(!sealer.plaintextEverExposed);

    // And if the re-wrap fails, the PIN change is refused rather than
    // silently leaving the gate claiming a seal that isn't there.
    sealer.resealShouldFail = true;
    QVERIFY(!manager.setPin(kOtherPin, kGoodPin));
    QVERIFY(store.verifyPin(kOtherPin));
    QCOMPARE(sealer.sealedUnderPin, kOtherPin);
    QVERIFY(!sealer.plaintextEverExposed);
}

// The rollback half: the re-wrap succeeded but the PIN record could not be
// written. The secret is now under the NEW pin while the stored hash still
// says the old one -- so it must be moved back, or the next unlock opens
// nothing.
void AppLockManagerTest::aFailedPinRecordWriteMovesTheSecretBack()
{
    FlakySecureStore secureStore;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    FakeSealer sealer;
    AppLockManager manager(store, settingsStore, sealer);

    QVERIFY(manager.setPin(QString(), kGoodPin));
    QVERIFY(manager.setCredentialPinGateEnabled(true, kGoodPin));
    QCOMPARE(sealer.sealedUnderPin, kGoodPin);

    secureStore.writesFail = true;
    QVERIFY(!manager.setPin(kGoodPin, kOtherPin));
    secureStore.writesFail = false;

    // Back where it started, and still never written out in the clear.
    QCOMPARE(sealer.sealedUnderPin, kGoodPin);
    QVERIFY(!sealer.plaintextEverExposed);
    QVERIFY(store.verifyPin(kGoodPin));
}

// A correct PIN that cannot open the sealed secret still unlocks the UI, but
// every authenticated request will 401 -- the user must be told, or they
// diagnose it as a server outage.
void AppLockManagerTest::unlockReportsCredentialsUnavailableWhenUnsealFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    FakeSealer sealer;
    AppLockManager manager(store, settingsStore, sealer);

    QVERIFY(manager.setPin(QString(), kGoodPin));
    QVERIFY(manager.setCredentialPinGateEnabled(true, kGoodPin));
    manager.lockNow();
    QVERIFY(manager.locked());

    sealer.unsealShouldFail = true;
    QSignalSpy unavailableSpy(&manager, &AppLockManager::credentialsUnavailableChanged);

    QVERIFY(manager.tryUnlock(kGoodPin)); // the PIN was right
    QVERIFY(!manager.locked());
    QCOMPARE(sealer.unsealForSessionCalls, 1);
    QVERIFY(manager.credentialsUnavailable());
    QCOMPARE(unavailableSpy.count(), 1);
}

void AppLockManagerTest::lockNowDropsSessionSecret()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    FakeSealer sealer;
    AppLockManager manager(store, settingsStore, sealer);

    QVERIFY(manager.setPin(QString(), kGoodPin));
    QVERIFY(manager.tryUnlock(kGoodPin));

    manager.lockNow();
    QVERIFY(manager.locked());
    // Re-locking must drop the in-memory plaintext, or a locked app can
    // still service push and MFA.
    QCOMPARE(sealer.relockCalls, 1);
}

// If the attempt counter cannot be persisted there is no backoff and no wipe
// threshold -- failedAttemptCount() keeps reading its "0" default. That is an
// unlimited guessing oracle behind a UI that still claims to rate-limit.
void AppLockManagerTest::failsClosedWhenAttemptCountCannotBePersisted()
{
    FlakySecureStore secureStore;
    AppLockStore store(secureStore);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    NullCredentialSealer sealer;

    QVERIFY(store.setPin(kGoodPin));
    AppLockManager manager(store, settingsStore, sealer);

    secureStore.writesFail = true;
    QVERIFY(!manager.tryUnlock(kWrongPin));

    // Every later attempt is refused for the rest of the process -- even the
    // correct PIN. Relaunching is the recovery; serving an unmetered oracle
    // is not an option.
    QVERIFY(!manager.tryUnlock(kGoodPin));
    QVERIFY(manager.locked());

    secureStore.writesFail = false;
    QVERIFY(!manager.tryUnlock(kGoodPin));
}

// The persisted lockout deadline is compared against the wall clock, which
// someone holding the machine can move forward. The session counter cannot
// be moved, so the wipe threshold still bites.
void AppLockManagerTest::sessionCounterCapsGuessingWhenClockIsMovedForward()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    NullCredentialSealer sealer;
    QVERIFY(store.setPin(kGoodPin));

    AppLockManager manager(store, settingsStore, sealer);
    QSignalSpy wipeSpy(&manager, &AppLockManager::wipeRequested);

    for (int i = 0; i < LockoutPolicy::kDefaultWipeThreshold; ++i) {
        manager.tryUnlock(kWrongPin);
        // Simulate the attacker clearing the backoff between guesses, which
        // moving the system clock forward achieves.
        store.setLockoutUntilEpochMs(0);
    }

    QVERIFY(wipeSpy.count() >= 1);
}

// A successful unlock clears the failure counter. That write can fail, and
// its result used to be discarded -- leaving the persisted counter
// stale-high while failedAttempts() takes the MAX of persisted and session.
// The user who mistyped nine times, unlocked correctly, then mistyped once
// more would cross the wipe threshold and lose their local mail on their
// SECOND mistake.
void AppLockManagerTest::staleAttemptCounterCannotTriggerAWipeAfterASuccessfulUnlock()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FlakySecureStore secureStore;
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    NullCredentialSealer sealer;
    QVERIFY(store.setPin(kGoodPin));

    AppLockManager manager(store, settingsStore, sealer);
    QSignalSpy wipeSpy(&manager, &AppLockManager::wipeRequested);

    // Walk the persisted counter up to one short of the threshold.
    for (int i = 0; i < LockoutPolicy::kDefaultWipeThreshold - 1; ++i) {
        manager.tryUnlock(kWrongPin);
        store.setLockoutUntilEpochMs(0); // skip the backoff, as above
    }
    QCOMPARE(wipeSpy.count(), 0);

    // The right PIN, but the store cannot record the reset.
    secureStore.writesFail = true;
    QVERIFY(manager.tryUnlock(kGoodPin));
    QVERIFY(!manager.locked());

    // The counter is now known-stale, so the manager refuses further
    // attempts rather than acting on it. Crucially, no wipe: destroying the
    // user's mail on the strength of a counter we know we failed to reset is
    // the one outcome that cannot be undone.
    secureStore.writesFail = false;
    QVERIFY(!manager.tryUnlock(kWrongPin));
    QCOMPARE(wipeSpy.count(), 0);
}

// The session floor must apply to the Settings prompts, not just the unlock
// screen.
//
// verifyPinRateLimited() -- which guards change-PIN, disable-lock, the
// credential-gate toggle and Hostile Location Protection -- carried two of
// tryUnlock()'s three guards and not the third. The missing one is the only
// guard an attacker holding the machine cannot defeat: the persisted backoff
// is wall-clock based and evaporates when the system clock moves forward,
// and the persisted counter lives in a store that can silently lose writes.
// The session counter survives both.
//
// So with the clock rolled and the counter cleared, the change-PIN dialog
// went right on verifying guesses at full speed while the unlock screen
// beside it was correctly refusing -- the same "unlimited guessing oracle
// behind a UI that claims to be rate-limited" this class already refuses to
// serve one method up.
void AppLockManagerTest::settingsPromptsHonourTheSessionFloorToo()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    NullCredentialSealer sealer;
    QVERIFY(store.setPin(kGoodPin));

    AppLockManager manager(store, settingsStore, sealer);

    // Burn the session floor on the unlock screen, clearing the backoff
    // between guesses the way a forward clock jump does.
    for (int i = 0; i < LockoutPolicy::kDefaultWipeThreshold; ++i) {
        manager.tryUnlock(kWrongPin);
        store.setLockoutUntilEpochMs(0);
    }

    // Now erase every trace the Settings path used to rely on: the deadline
    // (clock jump) and the persisted counter (a store that accepts writes
    // and loses them). Only the in-process floor is left.
    QVERIFY(store.setLockoutUntilEpochMs(0));
    QVERIFY(store.setFailedAttemptCount(0));

    QSignalSpy wipeSpy(&manager, &AppLockManager::wipeRequested);

    // The CORRECT PIN, deliberately: once the floor is reached a wipe is
    // pending, and the point of the floor is that this process stops
    // answering PIN questions at all. tryUnlock() already refuses here --
    // its floor check runs before verifyPin(). The Settings prompts did not,
    // so `disableLock(correctPin)` succeeded, turned the lock off, cleared
    // the counters and cancelled the pending wipe. Anyone who had learned
    // the PIN -- or an owner being made to hand it over -- could walk the
    // ten-failure protection back to nothing through a different dialog.
    //
    // The deadline is re-cleared before each call so the wall-clock backoff
    // cannot be what does the refusing: this asserts the floor specifically.
    QVERIFY(store.setLockoutUntilEpochMs(0));
    QVERIFY(!manager.disableLock(kGoodPin));
    QVERIFY(store.lockEnabled());

    QVERIFY(store.setLockoutUntilEpochMs(0));
    QVERIFY(!manager.setPin(kGoodPin, kOtherPin));
    QVERIFY(store.verifyPin(kGoodPin)); // unchanged

    QVERIFY(store.setLockoutUntilEpochMs(0));
    QVERIFY(!manager.setCredentialPinGateEnabled(true, kGoodPin));
    QVERIFY(!store.credentialPinGateEnabled());

    QVERIFY(store.setLockoutUntilEpochMs(0));
    QVERIFY(!manager.setHostileLocationEnabled(true, kGoodPin));
    QVERIFY(!settingsStore.hostileLocationProtectionEnabled());

    // Each refusal re-signals the pending wipe rather than swallowing it.
    QCOMPARE(wipeSpy.count(), 4);
}

// An unreachable secret store must not disable the app lock.
//
// lockEnabled() read its flag through SecureStore::get(), whose
// std::optional cannot distinguish "the store says there is no such key"
// from "the store could not be consulted". So an unreachable keyring read
// as "no PIN configured": AppLockManager's constructor seeded m_locked from
// it and the process started UNLOCKED, with no PIN screen, no lock overlay
// and no credential gate. Stopping gnome-keyring, or renaming
// ~/.local/share/keyrings, was the entire bypass -- both are things an
// ordinary user account can do to itself, which is exactly the access level
// this lock exists to survive.
//
// main.cpp knew and logged a qCritical about it. A log line is not a
// control.
void AppLockManagerTest::anUnreachableSecretStoreLeavesTheAppLocked()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    UnreachableSecureStore secureStore;
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    NullCredentialSealer sealer;

    // Fails closed at the store...
    QVERIFY(store.lockEnabled());
    QVERIFY(!store.storeReadable());
    // ...including the credential gate, whose "off" answer is what sends
    // PairingStore::storeDeviceSecret() down the plaintext branch.
    QVERIFY(store.credentialPinGateEnabled());

    // ...and therefore at the manager, which seeds m_locked from it.
    AppLockManager manager(store, settingsStore, sealer);
    QVERIFY(manager.lockEnabled());
    QVERIFY(manager.locked());
    // Surfaced, so the overlay can explain why no PIN will work rather than
    // implying the user has forgotten theirs.
    QVERIFY(manager.storeUnavailable());

    // And no PIN opens it, because the stored hash is in the same
    // unreachable store. Refusing is correct; pretending there is no lock
    // is not.
    QVERIFY(!manager.tryUnlock(kGoodPin));
    QVERIFY(manager.locked());
}

// THE TRADE THAT WAS NEVER ON OFFER.
//
// The wipe threshold and the per-process refusal floor used to be the same
// constant. Making the threshold configurable without splitting them would
// have meant a user who switched off "erase this device" also switched off
// the only limit an attacker cannot defeat by moving the system clock
// forward -- turning the unlock screen back into the unmetered guessing
// oracle this class refuses to serve.
void AppLockManagerTest::switchingTheEraseOffKeepsTheRateLimit()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FlakySecureStore secureStore;
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    NullCredentialSealer sealer;
    QVERIFY(store.setPin(kGoodPin));
    QVERIFY(store.setWipeAfterAttempts(LockoutPolicy::kWipeNever));

    AppLockManager manager(store, settingsStore, sealer);
    QSignalSpy wipeSpy(&manager, &AppLockManager::wipeRequested);

    // Well past the old threshold, with the backoff cleared between guesses
    // exactly as a forward clock change would achieve.
    for (int i = 0; i < LockoutPolicy::kSessionRefuseFloor + 5; ++i) {
        manager.tryUnlock(kWrongPin);
        store.setLockoutUntilEpochMs(0);
    }

    // The user asked not to be erased, and was not.
    QCOMPARE(wipeSpy.count(), 0);

    // But guesses are refused all the same -- including the CORRECT PIN,
    // which is the honest consequence of the floor and the reason relaunching
    // is the documented recovery.
    QCOMPARE(manager.tryUnlock(kGoodPin), false);
}

void AppLockManagerTest::aLoweredEraseThresholdWipesEarlier()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FlakySecureStore secureStore;
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    NullCredentialSealer sealer;
    QVERIFY(store.setPin(kGoodPin));
    QVERIFY(store.setWipeAfterAttempts(LockoutPolicy::kMinWipeThreshold));

    AppLockManager manager(store, settingsStore, sealer);
    QCOMPARE(manager.wipeAfterAttempts(), LockoutPolicy::kMinWipeThreshold);
    QSignalSpy wipeSpy(&manager, &AppLockManager::wipeRequested);

    for (int i = 0; i < LockoutPolicy::kMinWipeThreshold - 1; ++i) {
        manager.tryUnlock(kWrongPin);
        store.setLockoutUntilEpochMs(0);
    }
    QCOMPARE(wipeSpy.count(), 0);

    manager.tryUnlock(kWrongPin);
    QVERIFY(wipeSpy.count() >= 1);
}

// Someone who walks up to an unlocked session must not be able to switch off
// the erase, any more than they can switch off the lock itself.
void AppLockManagerTest::changingTheEraseThresholdRequiresTheCurrentPin()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FlakySecureStore secureStore;
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    NullCredentialSealer sealer;
    QVERIFY(store.setPin(kGoodPin));

    AppLockManager manager(store, settingsStore, sealer);
    QCOMPARE(manager.wipeAfterAttempts(), LockoutPolicy::kDefaultWipeThreshold);

    QCOMPARE(manager.setWipeAfterAttempts(LockoutPolicy::kWipeNever, kWrongPin), false);
    QCOMPARE(manager.wipeAfterAttempts(), LockoutPolicy::kDefaultWipeThreshold);

    // The refused attempt counts as a failure, like every other PIN prompt --
    // otherwise this setter is an unmetered oracle of its own. Cleared here
    // so the backoff does not refuse the legitimate change below.
    QVERIFY(store.setFailedAttemptCount(0));
    QVERIFY(store.setLockoutUntilEpochMs(0));

    QCOMPARE(manager.setWipeAfterAttempts(LockoutPolicy::kWipeNever, kGoodPin), true);
    QCOMPARE(manager.wipeAfterAttempts(), LockoutPolicy::kWipeNever);

    // Out-of-range values are clamped rather than honoured, so nothing can
    // arrange to erase the device on the second mistyped digit.
    QCOMPARE(manager.setWipeAfterAttempts(1, kGoodPin), true);
    QCOMPARE(manager.wipeAfterAttempts(), LockoutPolicy::kMinWipeThreshold);
}

// The default path, and the one almost every user is on.
//
// Asserted to be SYNCHRONOUS, not "locks eventually". A zero-delay
// QTimer::singleShot would satisfy a looser test while leaving the app
// unlocked for the rest of the current event-loop pass -- which is exactly
// the window "lock immediately" exists to close.
void AppLockManagerTest::theDefaultGraceLocksSynchronouslyWithNoTimer()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FlakySecureStore secureStore;
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    NullCredentialSealer sealer;
    QVERIFY(store.setPin(kGoodPin));

    AppLockManager manager(store, settingsStore, sealer);
    QCOMPARE(manager.backgroundGraceSeconds(), LockoutPolicy::kDefaultBackgroundGraceSeconds);
    QVERIFY(manager.tryUnlock(kGoodPin));
    QCOMPARE(manager.locked(), false);

    manager.lockAfterGrace();

    // No event loop was spun between those two lines.
    QCOMPARE(manager.locked(), true);
    QCOMPARE(manager.lockPending(), false);
}

void AppLockManagerTest::aGracePeriodDelaysTheLockAndComingBackCancelsIt()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FlakySecureStore secureStore;
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    NullCredentialSealer sealer;
    QVERIFY(store.setPin(kGoodPin));
    QVERIFY(store.setBackgroundGraceSeconds(1));

    AppLockManager manager(store, settingsStore, sealer);
    QVERIFY(manager.tryUnlock(kGoodPin));

    // 1. Backgrounded, then back before the grace expires: still unlocked,
    // and nothing left counting down.
    manager.lockAfterGrace();
    QCOMPARE(manager.locked(), false);
    QCOMPARE(manager.lockPending(), true);

    manager.cancelPendingLock();
    QCOMPARE(manager.lockPending(), false);
    QTest::qWait(1200);
    QVERIFY2(!manager.locked(), "a cancelled grace period locked the app anyway");

    // 2. Backgrounded and left alone: it locks when the grace expires.
    manager.lockAfterGrace();
    QCOMPARE(manager.locked(), false);
    QTRY_VERIFY_WITH_TIMEOUT(manager.locked(), 5000);
    QCOMPARE(manager.lockPending(), false);
}

// A user who asks to lock is not asking to lock in five minutes.
void AppLockManagerTest::lockNowIgnoresAndCancelsTheGracePeriod()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FlakySecureStore secureStore;
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    NullCredentialSealer sealer;
    QVERIFY(store.setPin(kGoodPin));
    QVERIFY(store.setBackgroundGraceSeconds(LockoutPolicy::kMaxBackgroundGraceSeconds));

    AppLockManager manager(store, settingsStore, sealer);
    QVERIFY(manager.tryUnlock(kGoodPin));

    manager.lockAfterGrace();
    QCOMPARE(manager.lockPending(), true);

    manager.lockNow();
    QCOMPARE(manager.locked(), true);
    QVERIFY2(!manager.lockPending(),
             "an explicit lock left a grace timer running that could fire after the lock was turned off");
}

void AppLockManagerTest::changingTheGraceRequiresThePinAndIsClamped()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FlakySecureStore secureStore;
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    NullCredentialSealer sealer;
    QVERIFY(store.setPin(kGoodPin));

    AppLockManager manager(store, settingsStore, sealer);

    QCOMPARE(manager.setBackgroundGraceSeconds(60, kWrongPin), false);
    QCOMPARE(manager.backgroundGraceSeconds(), 0);

    QVERIFY(store.setFailedAttemptCount(0));
    QVERIFY(store.setLockoutUntilEpochMs(0));

    QCOMPARE(manager.setBackgroundGraceSeconds(60, kGoodPin), true);
    QCOMPARE(manager.backgroundGraceSeconds(), 60);

    // Above the ceiling, and below zero. Neither may read as "never lock".
    QCOMPARE(manager.setBackgroundGraceSeconds(99999, kGoodPin), true);
    QCOMPARE(manager.backgroundGraceSeconds(), LockoutPolicy::kMaxBackgroundGraceSeconds);
    QCOMPARE(manager.setBackgroundGraceSeconds(-5, kGoodPin), true);
    QCOMPARE(manager.backgroundGraceSeconds(), 0);
}

QTEST_GUILESS_MAIN(AppLockManagerTest)
#include "AppLockManagerTest.moc"
