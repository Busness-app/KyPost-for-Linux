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
//   3. save() writing a ROTATED secret out in plaintext next to a sealed
//      blob and a gate flag that both still claim it is protected. This is
//      the one that shipped: the relay mints a new deviceSecret on every
//      successful re-registration, and re-registration runs unattended on
//      every launch that has a UnifiedPush distributor -- so the gate
//      silently downgraded itself to nothing, while locked.
// All three are covered below.
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
    void unpairingClearsTheGateFlagSoARePairDoesNotWritePlaintext();
    void aGateFlagWithNoBlobStillRefusesAPlaintextWrite();

    // Regressions for the rotated-secret finding (3 above).
    void savingARotatedSecretResealsItRatherThanWritingPlaintext();
    void savingARotatedSecretIsRefusedWhileLocked();
    void canResealTracksTheSessionRatherThanTheGateFlag();

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

    // The DURABLE state is what the gate protects: nothing readable on disk
    // without the PIN. This assertion used to also require load() to report
    // an empty secret immediately after sealing, on the grounds that "every
    // authenticated request 401s while locked". But sealing happens from
    // Settings, with the app UNLOCKED and the user standing right there --
    // conflating "sealed" with "locked" meant enabling the gate silently
    // broke mail sync for the rest of the session, with no lock screen shown
    // and no error to explain it. The session copy is now retained until
    // lockDeviceSecret(), which relockHidesItAgain() below pins down, and
    // the next-process case is pinned by
    // sessionUnsealDoesNotWriteThePlaintextBack()'s `reopened` store.
    const std::optional<QString> raw = secureStore.get(QStringLiteral("pairing.deviceSecret"));
    QVERIFY(!raw.has_value() || raw->isEmpty());
    const std::optional<QString> blob = secureStore.get(QStringLiteral("pairing.deviceSecretSealed"));
    QVERIFY(blob.has_value());
    QVERIFY(!blob->contains(QStringLiteral("super-secret-value")));

    // A fresh store over the same backing files -- a new process -- has no
    // session key and must be locked out.
    PairingStore reopened(secureStore);
    QVERIFY(reopened.load()->deviceSecret.isEmpty());
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

    // Relock first, so this exercises the case that matters: a process that
    // has NOT already opened the blob this session. (sealDeviceSecret()
    // leaves the sealing session holding the plaintext, by design -- see
    // sealHidesTheSecretFromLoadAndFromDisk.)
    store.lockDeviceSecret();

    QVERIFY(!store.unsealDeviceSecret(QStringLiteral("000000")));
    QVERIFY(store.load()->deviceSecret.isEmpty());
    QVERIFY(!store.unsealDeviceSecretPermanently(QStringLiteral("000000")));
    QVERIFY(store.deviceSecretSealed());
    // A failed unseal must not leave a usable session key behind either --
    // otherwise a wrong PIN would still enable re-sealing, which means it
    // would still enable rotating the secret.
    QVERIFY(!store.canResealDeviceSecret());
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

    QVERIFY(store.clear());
    QVERIFY(!store.deviceSecretSealed());
    // An orphaned blob would outlive the pairing it belonged to.
    const std::optional<QString> blob = secureStore.get(QStringLiteral("pairing.deviceSecretSealed"));
    QVERIFY(!blob.has_value() || blob->isEmpty());
}

void PairingStoreSealTest::savingARotatedSecretResealsItRatherThanWritingPlaintext()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    PairingStore store(secureStore);
    QVERIFY(store.save(samplePairing()));
    QVERIFY(store.sealDeviceSecret(QStringLiteral("123456")));

    // What re-registration does: the relay minted a brand-new secret and
    // retired the old one, and DeviceRegistrationService::pair() saves the
    // whole DevicePairing back with it.
    DevicePairing rotated = samplePairing();
    rotated.deviceSecret = QStringLiteral("rotated-secret-value");
    QVERIFY(store.save(rotated));

    // The rotated secret must NOT be readable on disk. Before the fix this
    // is exactly where "rotated-secret-value" appeared, in the clear.
    const std::optional<QString> raw = secureStore.get(QStringLiteral("pairing.deviceSecret"));
    QVERIFY(!raw.has_value() || raw->isEmpty());
    const std::optional<QString> blob = secureStore.get(QStringLiteral("pairing.deviceSecretSealed"));
    QVERIFY(blob.has_value());
    QVERIFY(!blob->contains(QStringLiteral("rotated-secret-value")));
    QVERIFY(store.deviceSecretSealed());

    // The gate is still real after a restart, and the blob now holds the NEW
    // secret -- re-sealing the old one would leave the device authenticating
    // with a credential the relay has already retired.
    PairingStore reopened(secureStore);
    QVERIFY(reopened.load()->deviceSecret.isEmpty());
    QVERIFY(reopened.unsealDeviceSecret(QStringLiteral("123456")));
    QCOMPARE(reopened.load()->deviceSecret, QStringLiteral("rotated-secret-value"));
}

void PairingStoreSealTest::savingARotatedSecretIsRefusedWhileLocked()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    PairingStore store(secureStore);
    QVERIFY(store.save(samplePairing()));
    QVERIFY(store.sealDeviceSecret(QStringLiteral("123456")));
    store.lockDeviceSecret(); // the app locks; the session key is gone

    DevicePairing rotated = samplePairing();
    rotated.deviceSecret = QStringLiteral("rotated-secret-value");
    // Refused outright rather than downgrading the gate. This is why
    // DeviceRegistrationService::pair() checks canResealDeviceSecret()
    // BEFORE its network call: by the time save() fails, the relay would
    // already have retired the old secret.
    QVERIFY(!store.save(rotated));

    const std::optional<QString> raw = secureStore.get(QStringLiteral("pairing.deviceSecret"));
    QVERIFY(!raw.has_value() || raw->isEmpty());
    // ...and nothing else was written either -- the check runs before the
    // first key is touched, so a refused save leaves the record coherent.
    QCOMPARE(secureStore.get(QStringLiteral("sub")).value_or(QString()), QStringLiteral("sub-1"));
}

void PairingStoreSealTest::canResealTracksTheSessionRatherThanTheGateFlag()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    PairingStore store(secureStore);
    QVERIFY(store.save(samplePairing()));

    // Gate off: plaintext writes are the normal resting state.
    QVERIFY(store.canResealDeviceSecret());

    // Sealing from Settings happens while unlocked, so the session that just
    // enabled the gate keeps working -- including re-registration.
    QVERIFY(store.sealDeviceSecret(QStringLiteral("123456")));
    QVERIFY(store.canResealDeviceSecret());

    store.lockDeviceSecret();
    QVERIFY(!store.canResealDeviceSecret());

    QVERIFY(store.unsealDeviceSecret(QStringLiteral("123456")));
    QVERIFY(store.canResealDeviceSecret());
}


// PairingStore used to infer the gate's state from its own sealed blob.
// clear() removes the blob and could not reach the flag, so unpairing and
// pairing again wrote the new device secret in PLAINTEXT while Settings still
// displayed "Require unlock to receive push and MFA: On".
void PairingStoreSealTest::unpairingClearsTheGateFlagSoARePairDoesNotWritePlaintext()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    PairingStore store(secureStore);

    DevicePairing pairing;
    pairing.subscriberId = QStringLiteral("sub-1");
    pairing.deviceSecret = QStringLiteral("original-secret");
    QVERIFY(store.save(pairing));
    QVERIFY(store.sealDeviceSecret(QStringLiteral("123456")));
    QVERIFY(secureStore.set(QStringLiteral("applock.credentialPinGateEnabled"), QStringLiteral("1")));

    QVERIFY(store.clear());

    // The flag went with the blob, so the gate no longer claims to protect
    // something that is not there.
    QCOMPARE(secureStore.get(QStringLiteral("applock.credentialPinGateEnabled")).value_or(QString()),
             QStringLiteral("0"));
}

// The other direction: a flag left set with no blob (or a blob the keychain
// could not read) must NOT be treated as "gate off, plaintext is fine".
void PairingStoreSealTest::aGateFlagWithNoBlobStillRefusesAPlaintextWrite()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    PairingStore store(secureStore);

    // Gate on per the authoritative flag, but no sealed blob and no session
    // key -- the state a failed keychain read produces.
    QVERIFY(secureStore.set(QStringLiteral("applock.credentialPinGateEnabled"), QStringLiteral("1")));

    DevicePairing rotated;
    rotated.subscriberId = QStringLiteral("sub-1");
    rotated.deviceSecret = QStringLiteral("rotated-secret");

    QVERIFY(!store.canResealDeviceSecret());
    QVERIFY(!store.save(rotated));

    // Nothing in the clear.
    const std::optional<QString> raw = secureStore.get(QStringLiteral("pairing.deviceSecret"));
    QVERIFY(!raw.has_value() || raw->isEmpty());
    QVERIFY(!raw.has_value() || *raw != QStringLiteral("rotated-secret"));
}

QTEST_APPLESS_MAIN(PairingStoreSealTest)
#include "PairingStoreSealTest.moc"

