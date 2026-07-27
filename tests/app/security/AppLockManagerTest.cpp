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

    int sealCalls = 0;
    int unsealPermanentlyCalls = 0;
    int unsealForSessionCalls = 0;
    int relockCalls = 0;
    QString lastPin;
    bool sealed = false;

    bool seal(const QString& pin) override
    {
        ++sealCalls;
        lastPin = pin;
        if (sealShouldFail)
            return false;
        sealed = true;
        return true;
    }

    bool unsealPermanently(const QString& pin) override
    {
        ++unsealPermanentlyCalls;
        lastPin = pin;
        if (unsealShouldFail)
            return false;
        sealed = false;
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
    QVERIFY(store.setFailedAttemptCount(LockoutPolicy::kWipeThreshold - 1));
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
    // Unwrapped from the old PIN, re-wrapped under the new one.
    QCOMPARE(sealer.unsealPermanentlyCalls, 1);
    QCOMPARE(sealer.sealCalls, 1);
    QCOMPARE(sealer.lastPin, kOtherPin);
    QVERIFY(sealer.isSealed());
    QVERIFY(store.verifyPin(kOtherPin));

    // And if the re-wrap fails, the PIN change is refused rather than
    // silently leaving the gate claiming a seal that isn't there.
    sealer.unsealShouldFail = true;
    QVERIFY(!manager.setPin(kOtherPin, kGoodPin));
    QVERIFY(store.verifyPin(kOtherPin));
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

    for (int i = 0; i < LockoutPolicy::kWipeThreshold; ++i) {
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
    for (int i = 0; i < LockoutPolicy::kWipeThreshold - 1; ++i) {
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

QTEST_GUILESS_MAIN(AppLockManagerTest)
#include "AppLockManagerTest.moc"
