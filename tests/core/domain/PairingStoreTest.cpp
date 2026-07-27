#include "domain/PairingStore.h"
#include "stores/SecureStore.h"
#include "stores/SecureStoreFile.h"

#include <QHash>
#include <QTemporaryDir>
#include <QTest>

namespace {

// Accepts writes but refuses every removal -- what SecureStoreKeychain does
// when the Secret Service is unreachable or the wallet has re-locked
// mid-session.
class UnremovableSecureStore : public SecureStore
{
public:
    bool set(const QString& key, const QString& value) override
    {
        m_values.insert(key, value);
        return true;
    }
    std::optional<QString> get(const QString& key) const override
    {
        const auto it = m_values.constFind(key);
        return it == m_values.constEnd() ? std::nullopt : std::optional<QString>(*it);
    }
    bool remove(const QString&) override { return false; }
    bool contains(const QString& key) const override { return m_values.contains(key); }

private:
    QHash<QString, QString> m_values;
};

} // namespace

class PairingStoreTest : public QObject
{
    Q_OBJECT

private slots:
    void isPairedFalseBeforeAnySave();
    void saveThenLoadRoundTripsEveryField();
    void loadReturnsNulloptWhenSubMissingEvenIfOtherKeysExist();
    void clearThenLoadReturnsNullopt();
    void deviceSecretEmptyStringRoundTripsAsEmpty();

    // Review-finding regression.
    void clearReportsFailureWhenTheStoreCannotRemove();

private:
    static DevicePairing samplePairing();
};

DevicePairing PairingStoreTest::samplePairing()
{
    DevicePairing pairing;
    pairing.subscriberId = QStringLiteral("subscriber-123");
    pairing.serverBaseUrl = QStringLiteral("https://relay.example.com");
    pairing.registrationUrl = QStringLiteral("https://relay.example.com/api/notifications/native/register");
    pairing.pairingToken = QStringLiteral("pairing-token-abc");
    pairing.deviceId = QStringLiteral("device-1");
    pairing.deviceName = QStringLiteral("My Linux Desktop");
    pairing.deviceSecret = QStringLiteral("deadbeef");
    return pairing;
}

void PairingStoreTest::isPairedFalseBeforeAnySave()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    PairingStore pairingStore(secureStore);

    QVERIFY(!pairingStore.isPaired());
    QVERIFY(!pairingStore.load().has_value());
}

void PairingStoreTest::saveThenLoadRoundTripsEveryField()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    PairingStore pairingStore(secureStore);

    const DevicePairing pairing = samplePairing();
    QVERIFY(pairingStore.save(pairing));

    const std::optional<DevicePairing> loaded = pairingStore.load();
    QVERIFY(loaded.has_value());
    QCOMPARE(*loaded, pairing);
    QVERIFY(pairingStore.isPaired());
}

void PairingStoreTest::loadReturnsNulloptWhenSubMissingEvenIfOtherKeysExist()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    PairingStore pairingStore(secureStore);

    // Write the other six keys directly via the underlying SecureStoreFile,
    // skipping "sub", to confirm load() still treats this as unpaired.
    QVERIFY(secureStore.set(QStringLiteral("pairing.deviceSecret"), QStringLiteral("deadbeef")));
    QVERIFY(secureStore.set(QStringLiteral("pairing.serverBaseUrl"), QStringLiteral("https://relay.example.com")));
    QVERIFY(secureStore.set(QStringLiteral("pairing.registrationUrl"),
        QStringLiteral("https://relay.example.com/api/notifications/native/register")));
    QVERIFY(secureStore.set(QStringLiteral("pairing.pairingToken"), QStringLiteral("pairing-token-abc")));
    QVERIFY(secureStore.set(QStringLiteral("deviceId"), QStringLiteral("device-1")));
    QVERIFY(secureStore.set(QStringLiteral("pairing.deviceName"), QStringLiteral("My Linux Desktop")));

    QVERIFY(!pairingStore.load().has_value());
    QVERIFY(!pairingStore.isPaired());
}

void PairingStoreTest::clearThenLoadReturnsNullopt()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    PairingStore pairingStore(secureStore);

    QVERIFY(pairingStore.save(samplePairing()));
    QVERIFY(pairingStore.isPaired());

    pairingStore.clear();

    QVERIFY(!pairingStore.load().has_value());
    QVERIFY(!pairingStore.isPaired());
}

void PairingStoreTest::deviceSecretEmptyStringRoundTripsAsEmpty()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    PairingStore pairingStore(secureStore);

    DevicePairing pairing = samplePairing();
    pairing.deviceSecret = QString();
    QVERIFY(pairingStore.save(pairing));

    const std::optional<DevicePairing> loaded = pairingStore.load();
    QVERIFY(loaded.has_value());
    QVERIFY(loaded->deviceSecret.isEmpty());
    QCOMPARE(*loaded, pairing);
}

void PairingStoreTest::clearReportsFailureWhenTheStoreCannotRemove()
{
    UnremovableSecureStore secureStore;
    PairingStore pairingStore(secureStore);
    QVERIFY(pairingStore.save(samplePairing()));

    // clear() used to return void and discard all nine remove() results, so
    // the wipe-after-repeated-PIN-failure path in main.cpp relaunched into a
    // state that merely LOOKED wiped -- the device secret still in the
    // keychain, the app still able to reach the relay.
    QVERIFY(!pairingStore.clear());

    // And the caller's suspicion is correct: the credential really is still
    // there. This is what main.cpp now shouts about.
    QVERIFY(pairingStore.isPaired());
}

QTEST_GUILESS_MAIN(PairingStoreTest)
#include "PairingStoreTest.moc"
