#include "security/AppLockStore.h"

#include "stores/SecureStore.h"
#include "stores/SecureStoreFile.h"

#include <QMap>
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
    void aPinSetByAnOlderBuildStillVerifies();
    void aFailedRecordWriteLeavesThePreviousPinWorking();
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
    // Simulate a store that lost the credential material but kept the enabled
    // flag: the app must refuse every PIN rather than let anything through.
    QVERIFY(secureStore.remove(QStringLiteral("applock.pinRecord")));
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

    const auto saltOf = [&secureStore]() {
        const QString record = secureStore.get(QStringLiteral("applock.pinRecord")).value_or(QString());
        return record.left(record.indexOf(QLatin1Char(':')));
    };
    const auto hashOf = [&secureStore]() {
        const QString record = secureStore.get(QStringLiteral("applock.pinRecord")).value_or(QString());
        return record.mid(record.indexOf(QLatin1Char(':')) + 1);
    };

    QVERIFY(store.setPin(QStringLiteral("123456")));
    const QString firstHash = hashOf();
    const QString firstSalt = saltOf();

    QVERIFY(store.setPin(QStringLiteral("123456"))); // same PIN again
    const QString secondHash = hashOf();
    const QString secondSalt = saltOf();

    QVERIFY(!firstSalt.isEmpty());
    QVERIFY(firstSalt != secondSalt);
    // Same PIN, different salt => different hash. Without this, two devices
    // with the same PIN would store identical hashes.
    QVERIFY(firstHash != secondHash);
    QVERIFY(store.verifyPin(QStringLiteral("123456")));
}

// The pre-2026-07-27 layout wrote applock.pinSalt/applock.pinHash. An
// install carrying those must keep working, or the atomicity fix below would
// itself lock out every existing user on first launch.
void AppLockStoreTest::aPinSetByAnOlderBuildStillVerifies()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    AppLockStore store(secureStore);

    QVERIFY(store.setPin(QStringLiteral("123456")));
    // Rewrite the record in the old split form, exactly as an older build left it.
    const QString record = secureStore.get(QStringLiteral("applock.pinRecord")).value_or(QString());
    const qsizetype sep = record.indexOf(QLatin1Char(':'));
    QVERIFY(sep > 0);
    QVERIFY(secureStore.set(QStringLiteral("applock.pinSalt"), record.left(sep)));
    QVERIFY(secureStore.set(QStringLiteral("applock.pinHash"), record.mid(sep + 1)));
    QVERIFY(secureStore.remove(QStringLiteral("applock.pinRecord")));

    QVERIFY(store.lockEnabled());
    QVERIFY(store.verifyPin(QStringLiteral("123456")));
    QVERIFY(!store.verifyPin(QStringLiteral("654321")));
}

// Salt and hash used to be two separate writes. On a PIN *change* the enabled
// flag is already set, so a salt that landed followed by a hash that did not
// left verifyPin() comparing PBKDF2(pin, newSalt) against the OLD hash --
// neither PIN verified, and guessing reached the ten-failure wipe.
void AppLockStoreTest::aFailedRecordWriteLeavesThePreviousPinWorking()
{
    class RefusingStore : public SecureStore
    {
    public:
        bool refuseRecordWrites = false;
        bool set(const QString& key, const QString& value) override
        {
            if (refuseRecordWrites && key == QStringLiteral("applock.pinRecord"))
                return false;
            m_values[key] = value;
            return true;
        }
        std::optional<QString> get(const QString& key) const override
        {
            const auto it = m_values.constFind(key);
            return it == m_values.constEnd() ? std::nullopt : std::optional<QString>(*it);
        }
        bool remove(const QString& key) override { m_values.remove(key); return true; }
        bool contains(const QString& key) const override { return m_values.contains(key); }

    private:
        QMap<QString, QString> m_values;
    };

    RefusingStore secureStore;
    AppLockStore store(secureStore);

    QVERIFY(store.setPin(QStringLiteral("123456")));
    QVERIFY(store.verifyPin(QStringLiteral("123456")));

    secureStore.refuseRecordWrites = true;
    QVERIFY(!store.setPin(QStringLiteral("654321")));

    // The old PIN still works and the app is still usable -- the failed
    // change changed nothing.
    QVERIFY(store.lockEnabled());
    QVERIFY(store.verifyPin(QStringLiteral("123456")));
    QVERIFY(!store.verifyPin(QStringLiteral("654321")));
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
