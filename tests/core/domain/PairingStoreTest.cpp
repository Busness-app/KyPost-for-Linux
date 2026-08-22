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

    void stillCurrentTracksTheAccountAndRegistrationButNotTheSecret();

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

// The primitive every in-flight reply is judged against.
//
// What it must be sensitive to: a different account, and a re-registration of
// the same account. What it must NOT be sensitive to: the device secret,
// which the credential gate rewrites on every lock and unlock -- keying on it
// would throw away every legitimate reply that happened to span one.
void PairingStoreTest::stillCurrentTracksTheAccountAndRegistrationButNotTheSecret()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    PairingStore pairingStore(secureStore);

    const DevicePairing original = samplePairing();
    const PairingIdentity identity = identityOf(original);

    // Unpaired: nothing is current, and an empty identity is not a match for
    // an empty store either. "Both empty" must not read as "unchanged" --
    // there is no account to file a reply under.
    QVERIFY(!pairingStore.stillCurrent(identity));
    QVERIFY(!pairingStore.stillCurrent(PairingIdentity{}));
    QVERIFY(pairingStore.currentIdentity().isEmpty());

    QVERIFY(pairingStore.save(original));
    QVERIFY(pairingStore.stillCurrent(identity));
    QCOMPARE(pairingStore.currentIdentity(), identity);

    // Same account and registration, rotated secret -- as a lock/unlock
    // leaves it. Still current.
    DevicePairing rotatedSecret = original;
    rotatedSecret.deviceSecret = QStringLiteral("a-completely-different-secret");
    QVERIFY(pairingStore.save(rotatedSecret));
    QVERIFY(pairingStore.stillCurrent(identity));

    // Same account, re-registered. The previous registration's replies have
    // no claim on this one.
    DevicePairing reregistered = original;
    reregistered.deviceId = QStringLiteral("device-2");
    QVERIFY(pairingStore.save(reregistered));
    QVERIFY(!pairingStore.stillCurrent(identity));

    // A different account entirely.
    DevicePairing otherAccount = original;
    otherAccount.subscriberId = QStringLiteral("subscriber-999");
    QVERIFY(pairingStore.save(otherAccount));
    QVERIFY(!pairingStore.stillCurrent(identity));

    // And unpaired again.
    QVERIFY(pairingStore.clear());
    QVERIFY(!pairingStore.stillCurrent(identity));
}

QTEST_GUILESS_MAIN(PairingStoreTest)
#include "PairingStoreTest.moc"
