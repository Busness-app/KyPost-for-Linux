#include "domain/PairingStore.h"

#include "domain/DevicePairing.h"
#include "stores/SecureStoreFile.h"

#include <QTemporaryDir>
#include <QTest>

// Focused on the credential PIN gate's seal/unseal semantics, which are easy
// to get subtly and dangerously wrong in two specific ways:
//   1. unsealing back to disk, leaving the secret plaintext forever after the
//      first unlock (the gate then protects nothing);
//   2. turning the gate off without restoring the plaintext, stranding the
//      pairing behind a PIN that is about to be deleted.
// Both are covered below.
class PairingStoreSealTest : public QObject
{
    Q_OBJECT

private slots:
    void sealHidesTheSecretFromLoadAndFromDisk();
    void sessionUnsealDoesNotWriteThePlaintextBack();
    void relockHidesItAgain();
    void wrongPinNeverReveals();
    void permanentUnsealRestoresPlaintextAndDropsBlob();
    void clearDropsTheSealedBlobToo();

private:
    static DevicePairing samplePairing();
};

DevicePairing PairingStoreSealTest::samplePairing()
{
    DevicePairing p;
    p.subscriberId = QStringLiteral("sub-1");
    p.serverBaseUrl = QStringLiteral("https://mail.example.com");
    p.deviceId = QStringLiteral("dev-1");
    p.deviceSecret = QStringLiteral("super-secret-value");
    return p;
}

void PairingStoreSealTest::sealHidesTheSecretFromLoadAndFromDisk()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    PairingStore store(secureStore);
    QVERIFY(store.save(samplePairing()));

    QVERIFY(store.sealDeviceSecret(QStringLiteral("123456")));
    QVERIFY(store.deviceSecretSealed());

    // load() must report an empty secret, so every authenticated request
    // 401s while locked -- that is the feature.
    const std::optional<DevicePairing> loaded = store.load();
    QVERIFY(loaded.has_value());
    QVERIFY(loaded->deviceSecret.isEmpty());

    // And the plaintext must not be sitting in the backing store either.
    const std::optional<QString> raw = secureStore.get(QStringLiteral("pairing.deviceSecret"));
    QVERIFY(!raw.has_value() || raw->isEmpty());
    const std::optional<QString> blob = secureStore.get(QStringLiteral("pairing.deviceSecretSealed"));
    QVERIFY(blob.has_value());
    QVERIFY(!blob->contains(QStringLiteral("super-secret-value")));
}

void PairingStoreSealTest::sessionUnsealDoesNotWriteThePlaintextBack()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    PairingStore store(secureStore);
    QVERIFY(store.save(samplePairing()));
    QVERIFY(store.sealDeviceSecret(QStringLiteral("123456")));

    QVERIFY(store.unsealDeviceSecret(QStringLiteral("123456")));

    // Usable in this session...
    QCOMPARE(store.load()->deviceSecret, QStringLiteral("super-secret-value"));

    // ...but NOT written back to disk. If it were, the gate would protect
    // nothing from the first unlock onward.
    const std::optional<QString> raw = secureStore.get(QStringLiteral("pairing.deviceSecret"));
    QVERIFY(!raw.has_value() || raw->isEmpty());
    QVERIFY(store.deviceSecretSealed());

    // A fresh PairingStore over the same backing store -- i.e. a new process
    // -- must be back to locked.
    PairingStore reopened(secureStore);
    QVERIFY(reopened.load()->deviceSecret.isEmpty());
}

void PairingStoreSealTest::relockHidesItAgain()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    PairingStore store(secureStore);
    QVERIFY(store.save(samplePairing()));
    QVERIFY(store.sealDeviceSecret(QStringLiteral("123456")));
    QVERIFY(store.unsealDeviceSecret(QStringLiteral("123456")));
    QCOMPARE(store.load()->deviceSecret, QStringLiteral("super-secret-value"));

    store.lockDeviceSecret();
    QVERIFY(store.load()->deviceSecret.isEmpty());
    // The durable copy survives, so unlocking again works.
    QVERIFY(store.deviceSecretSealed());
    QVERIFY(store.unsealDeviceSecret(QStringLiteral("123456")));
    QCOMPARE(store.load()->deviceSecret, QStringLiteral("super-secret-value"));
}

void PairingStoreSealTest::wrongPinNeverReveals()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    PairingStore store(secureStore);
    QVERIFY(store.save(samplePairing()));
    QVERIFY(store.sealDeviceSecret(QStringLiteral("123456")));

    QVERIFY(!store.unsealDeviceSecret(QStringLiteral("000000")));
    QVERIFY(store.load()->deviceSecret.isEmpty());
    QVERIFY(!store.unsealDeviceSecretPermanently(QStringLiteral("000000")));
    QVERIFY(store.deviceSecretSealed());
}

void PairingStoreSealTest::permanentUnsealRestoresPlaintextAndDropsBlob()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    PairingStore store(secureStore);
    QVERIFY(store.save(samplePairing()));
    QVERIFY(store.sealDeviceSecret(QStringLiteral("123456")));

    QVERIFY(store.unsealDeviceSecretPermanently(QStringLiteral("123456")));
    QVERIFY(!store.deviceSecretSealed());

    // Survives a restart: this is what turning the gate off must guarantee,
    // otherwise the pairing is stranded behind a PIN that gets deleted.
    PairingStore reopened(secureStore);
    QCOMPARE(reopened.load()->deviceSecret, QStringLiteral("super-secret-value"));
}

void PairingStoreSealTest::clearDropsTheSealedBlobToo()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    PairingStore store(secureStore);
    QVERIFY(store.save(samplePairing()));
    QVERIFY(store.sealDeviceSecret(QStringLiteral("123456")));

    store.clear();
    QVERIFY(!store.deviceSecretSealed());
    // An orphaned blob would outlive the pairing it belonged to.
    const std::optional<QString> blob = secureStore.get(QStringLiteral("pairing.deviceSecretSealed"));
    QVERIFY(!blob.has_value() || blob->isEmpty());
}

QTEST_APPLESS_MAIN(PairingStoreSealTest)
#include "PairingStoreSealTest.moc"
