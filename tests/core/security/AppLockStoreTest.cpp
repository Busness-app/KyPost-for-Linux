#include "security/AppLockStore.h"

#include "stores/SecureStoreFile.h"

#include <QTemporaryDir>
#include <QTest>

class AppLockStoreTest : public QObject
{
    Q_OBJECT

private slots:
    void startsDisabledWithNoPin();
    void setPinEnablesAndVerifies();
    void neverStoresTheRawPin();
    void verifyFailsClosedWhenMaterialIsMissing();
    void eachPinGetsAFreshSalt();
    void clearDisablesEverything();
    void tracksAttemptsAndLockoutDeadline();
    void credentialGateRoundTrips();
};

void AppLockStoreTest::startsDisabledWithNoPin()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);

    QVERIFY(!store.lockEnabled());
    QVERIFY(!store.verifyPin(QStringLiteral("123456")));
    QCOMPARE(store.failedAttemptCount(), 0);
    QCOMPARE(store.lockoutUntilEpochMs(), 0LL);
    QVERIFY(!store.credentialPinGateEnabled());
}

void AppLockStoreTest::setPinEnablesAndVerifies()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);

    QVERIFY(store.setPin(QStringLiteral("123456")));
    QVERIFY(store.lockEnabled());
    QVERIFY(store.verifyPin(QStringLiteral("123456")));
    QVERIFY(!store.verifyPin(QStringLiteral("123457")));
    QVERIFY(!store.verifyPin(QString()));
}

void AppLockStoreTest::neverStoresTheRawPin()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);

    const QString pin = QStringLiteral("864209");
    QVERIFY(store.setPin(pin));

    // Whatever the backing store persisted, the PIN itself must not be in it.
    for (const QString& key : { QStringLiteral("applock.pinSalt"), QStringLiteral("applock.pinHash"),
                                 QStringLiteral("applock.enabled") }) {
        const std::optional<QString> raw = secureStore.get(key);
        if (raw.has_value())
            QVERIFY(!raw->contains(pin));
    }
}

void AppLockStoreTest::verifyFailsClosedWhenMaterialIsMissing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);

    QVERIFY(store.setPin(QStringLiteral("123456")));
    // Simulate a store that lost the hash but kept the enabled flag: the app
    // must refuse every PIN rather than let anything through.
    QVERIFY(secureStore.remove(QStringLiteral("applock.pinHash")));
    QVERIFY(store.lockEnabled());
    QVERIFY(!store.verifyPin(QStringLiteral("123456")));
    QVERIFY(!store.verifyPin(QString()));
}

void AppLockStoreTest::eachPinGetsAFreshSalt()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);

    QVERIFY(store.setPin(QStringLiteral("123456")));
    const QString firstHash = secureStore.get(QStringLiteral("applock.pinHash")).value_or(QString());
    const QString firstSalt = secureStore.get(QStringLiteral("applock.pinSalt")).value_or(QString());

    QVERIFY(store.setPin(QStringLiteral("123456"))); // same PIN again
    const QString secondHash = secureStore.get(QStringLiteral("applock.pinHash")).value_or(QString());
    const QString secondSalt = secureStore.get(QStringLiteral("applock.pinSalt")).value_or(QString());

    QVERIFY(!firstSalt.isEmpty());
    QVERIFY(firstSalt != secondSalt);
    // Same PIN, different salt => different hash. Without this, two devices
    // with the same PIN would store identical hashes.
    QVERIFY(firstHash != secondHash);
    QVERIFY(store.verifyPin(QStringLiteral("123456")));
}

void AppLockStoreTest::clearDisablesEverything()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);

    QVERIFY(store.setPin(QStringLiteral("123456")));
    QVERIFY(store.setCredentialPinGateEnabled(true));
    QVERIFY(store.setFailedAttemptCount(4));
    QVERIFY(store.setLockoutUntilEpochMs(99999));

    QVERIFY(store.clear());
    QVERIFY(!store.lockEnabled());
    QVERIFY(!store.verifyPin(QStringLiteral("123456")));
    QVERIFY(!store.credentialPinGateEnabled());
    QCOMPARE(store.failedAttemptCount(), 0);
    QCOMPARE(store.lockoutUntilEpochMs(), 0LL);
}

void AppLockStoreTest::tracksAttemptsAndLockoutDeadline()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);

    QVERIFY(store.setFailedAttemptCount(3));
    QCOMPARE(store.failedAttemptCount(), 3);
    QVERIFY(store.setLockoutUntilEpochMs(1234567890123LL));
    QCOMPARE(store.lockoutUntilEpochMs(), 1234567890123LL);

    // setPin resets both -- succeeding at setting a new PIN means the owner
    // is present, so prior failures shouldn't keep them locked out.
    QVERIFY(store.setPin(QStringLiteral("111111")));
    QCOMPARE(store.failedAttemptCount(), 0);
    QCOMPARE(store.lockoutUntilEpochMs(), 0LL);
}

void AppLockStoreTest::credentialGateRoundTrips()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);

    QVERIFY(!store.credentialPinGateEnabled());
    QVERIFY(store.setCredentialPinGateEnabled(true));
    QVERIFY(store.credentialPinGateEnabled());
    QVERIFY(store.setCredentialPinGateEnabled(false));
    QVERIFY(!store.credentialPinGateEnabled());
}

QTEST_APPLESS_MAIN(AppLockStoreTest)
#include "AppLockStoreTest.moc"
