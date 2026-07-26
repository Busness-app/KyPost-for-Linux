#include "security/AppLockManager.h"

#include "security/AppLockStore.h"
#include "security/LockoutPolicy.h"
#include "stores/SecureStoreFile.h"
#include "stores/SettingsStore.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

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
    void credentialGateRequiresPinAndEmitsForHost();
    void lockNowIsANoOpWhenLockDisabled();
};

void AppLockManagerTest::startsUnlockedWhenLockDisabled()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    AppLockManager manager(store, settingsStore);

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
    QVERIFY(store.setPin(QStringLiteral("123456")));

    // A freshly-started process has by definition not been unlocked yet.
    AppLockManager manager(store, settingsStore);
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
    QVERIFY(store.setPin(QStringLiteral("123456")));
    QVERIFY(store.setFailedAttemptCount(2));

    AppLockManager manager(store, settingsStore);
    QSignalSpy lockedSpy(&manager, &AppLockManager::lockedChanged);

    QVERIFY(manager.tryUnlock(QStringLiteral("123456")));
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
    QVERIFY(store.setPin(QStringLiteral("123456")));

    AppLockManager manager(store, settingsStore);

    // The first few are free -- fat-fingering shouldn't cost a delay.
    for (int i = 1; i <= LockoutPolicy::kFreeAttempts; ++i) {
        QVERIFY(!manager.tryUnlock(QStringLiteral("000000")));
        QCOMPARE(manager.failedAttempts(), i);
        QCOMPARE(manager.remainingLockoutSeconds(), 0);
    }
    QVERIFY(manager.locked());

    // The next one starts the backoff.
    QVERIFY(!manager.tryUnlock(QStringLiteral("000000")));
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
    QVERIFY(store.setPin(QStringLiteral("123456")));
    // Force an active backoff far in the future.
    QVERIFY(store.setFailedAttemptCount(4));
    QVERIFY(store.setLockoutUntilEpochMs(QDateTime::currentMSecsSinceEpoch() + 600000));

    AppLockManager manager(store, settingsStore);

    // Even the CORRECT PIN is refused during a backoff -- otherwise the
    // delay is decorative and guessing continues at full speed.
    QVERIFY(!manager.tryUnlock(QStringLiteral("123456")));
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
    QVERIFY(store.setPin(QStringLiteral("123456")));
    // One short of the threshold, with no backoff in the way.
    QVERIFY(store.setFailedAttemptCount(LockoutPolicy::kWipeThreshold - 1));
    QVERIFY(store.setLockoutUntilEpochMs(0));

    AppLockManager manager(store, settingsStore);
    QSignalSpy wipeSpy(&manager, &AppLockManager::wipeRequested);

    QVERIFY(!manager.tryUnlock(QStringLiteral("000000")));
    QCOMPARE(wipeSpy.count(), 1);
}

void AppLockManagerTest::disableLockRequiresCurrentPin()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    QVERIFY(store.setPin(QStringLiteral("123456")));

    AppLockManager manager(store, settingsStore);

    // Anyone with the window already open must not be able to remove the
    // protection the lock exists to provide.
    QVERIFY(!manager.disableLock(QStringLiteral("000000")));
    QVERIFY(manager.lockEnabled());

    QVERIFY(manager.disableLock(QStringLiteral("123456")));
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
    AppLockManager manager(store, settingsStore);

    // Setting the FIRST pin needs no current pin -- there is nothing to
    // prove yet.
    QVERIFY(manager.setPin(QString(), QStringLiteral("111111")));
    QVERIFY(manager.lockEnabled());

    // Changing it does.
    QVERIFY(!manager.setPin(QStringLiteral("000000"), QStringLiteral("222222")));
    QVERIFY(store.verifyPin(QStringLiteral("111111")));

    QVERIFY(manager.setPin(QStringLiteral("111111"), QStringLiteral("222222")));
    QVERIFY(store.verifyPin(QStringLiteral("222222")));

    // An empty new PIN is not a way to disable the lock.
    QVERIFY(!manager.setPin(QStringLiteral("222222"), QString()));
    QVERIFY(manager.lockEnabled());
}

void AppLockManagerTest::credentialGateRequiresPinAndEmitsForHost()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    AppLockManager manager(store, settingsStore);

    // Meaningless without a PIN to gate on: refuse rather than produce a
    // "sealed under nothing" state.
    QVERIFY(!manager.setCredentialPinGateEnabled(true, QString()));

    QVERIFY(manager.setPin(QString(), QStringLiteral("123456")));
    QSignalSpy gateSpy(&manager, &AppLockManager::credentialGateChanged);

    QVERIFY(!manager.setCredentialPinGateEnabled(true, QStringLiteral("000000")));
    QCOMPARE(gateSpy.count(), 0);

    QVERIFY(manager.setCredentialPinGateEnabled(true, QStringLiteral("123456")));
    QVERIFY(manager.credentialPinGateEnabled());
    QCOMPARE(gateSpy.count(), 1);
    QCOMPARE(gateSpy.at(0).at(0).toBool(), true);
    QCOMPARE(gateSpy.at(0).at(1).toString(), QStringLiteral("123456"));

    // Turning the lock off while the gate is on must unseal first, or the
    // pairing becomes unrecoverable.
    QVERIFY(manager.disableLock(QStringLiteral("123456")));
    QCOMPARE(gateSpy.count(), 2);
    QCOMPARE(gateSpy.at(1).at(0).toBool(), false);
}

void AppLockManagerTest::lockNowIsANoOpWhenLockDisabled()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    AppLockManager manager(store, settingsStore);

    QSignalSpy lockedSpy(&manager, &AppLockManager::lockedChanged);
    manager.lockNow();
    QVERIFY(!manager.locked());
    QCOMPARE(lockedSpy.count(), 0);

    QVERIFY(manager.setPin(QString(), QStringLiteral("123456")));
    QVERIFY(manager.tryUnlock(QStringLiteral("123456")));
    QVERIFY(!manager.locked());

    manager.lockNow();
    QVERIFY(manager.locked());
}

QTEST_GUILESS_MAIN(AppLockManagerTest)
#include "AppLockManagerTest.moc"
