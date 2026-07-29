#include "domain/PairingStore.h"

#include "domain/DevicePairing.h"
#include "stores/SecureStoreFile.h"

#include "stores/SecureStore.h"

#include <QHash>
#include <QTemporaryDir>
#include <QTest>

#include <functional>

namespace {

// A SecureStore that runs a caller-supplied hook in the middle of a write.
//
// This is the unit-test stand-in for the thing that actually happens in
// production: SecureStoreKeychain::set() spins a nested QEventLoop per
// write, so QML input keeps being delivered while PairingStore::save() is
// suspended inside it. A window minimise during those nine writes reaches
// AppLock.lockNow() -> PairingStore::lockDeviceSecret(). There is no other
// way to reproduce a mid-save lock without standing up a real keychain and
// a real event loop.
class ReentrantSecureStore : public SecureStore
{
public:
    // Runs once, before the `nth` set() call FROM NOW completes. Relative,
    // not absolute: the test has to build a sealed pairing first, which is
    // itself a dozen writes, so an absolute index would silently never fire
    // and the test would pass by doing nothing.
    void interruptAfterNextWrites(int nth, std::function<void()> hook)
    {
        m_interruptAt = m_writes + nth;
        m_hook = std::move(hook);
    }

    bool hookFired() const { return m_hookFired; }

    int reads() const { return m_reads; }
    void resetReadCount() { m_reads = 0; }

    bool set(const QString& key, const QString& value) override
    {
        ++m_writes;
        if (m_writes == m_interruptAt && m_hook) {
            const std::function<void()> hook = m_hook;
            m_hook = nullptr;
            m_hookFired = true;
            hook();
        }
        m_values.insert(key, value);
        return true;
    }
    std::optional<QString> get(const QString& key) const override
    {
        ++m_reads;
        const auto it = m_values.constFind(key);
        return it == m_values.constEnd() ? std::nullopt : std::optional<QString>(*it);
    }
    bool remove(const QString& key) override
    {
        m_values.remove(key);
        return true;
    }
    bool contains(const QString& key) const override { return m_values.contains(key); }

private:
    QHash<QString, QString> m_values;
    mutable int m_reads = 0;
    int m_writes = 0;
    int m_interruptAt = -1;
    bool m_hookFired = false;
    std::function<void()> m_hook;
};

} // namespace

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

    // Regression for the PIN-change plaintext-window finding.
    void resealMovesTheSecretBetweenPinsWithoutTouchingDisk();
    void resealUnderTheWrongOldPinChangesNothing();

    // load() caching, and the invalidation that keeps it honest.
    void repeatedLoadsDoNotReReadTheStore();
    void everyMutationInvalidatesTheCache();

    // Regression for the mid-save lock finding.
    void aLockArrivingDuringSaveIsNotUndoneBySave();
    void anUninterruptedSaveStillRestoresTheSessionKey();

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

// A PIN change must move the secret from one key to another without the
// plaintext ever existing on disk.
//
// AppLockManager::setPin() used to do this as unsealPermanently(old) +
// seal(new). That composes correctly and leaves the device secret sitting
// in the keychain in the clear in between -- for the length of two
// 150k-iteration PBKDF2 derivations, which on a slow machine is a real
// window. A crash, an OOM kill or a power loss there makes the exposure
// permanent, with applock.credentialPinGateEnabled still reading "1" so the
// UI goes on reporting "Require unlock to receive push and MFA: On".
void PairingStoreSealTest::resealMovesTheSecretBetweenPinsWithoutTouchingDisk()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    PairingStore store(secureStore);

    QVERIFY(store.save(samplePairing()));
    QVERIFY(store.sealDeviceSecret(QStringLiteral("428391")));

    QVERIFY(store.resealDeviceSecret(QStringLiteral("428391"), QStringLiteral("760142")));

    // The plaintext slot is empty at every observable point -- most
    // importantly right now, after the operation that used to fill it.
    QVERIFY(secureStore.get(QStringLiteral("pairing.deviceSecret")).value_or(QString()).isEmpty());
    // ...and nothing anywhere in the store contains the secret in the clear.
    for (const QString& key : { QStringLiteral("pairing.deviceSecret"),
                                QStringLiteral("pairing.deviceSecretSealed") }) {
        QVERIFY(!secureStore.get(key).value_or(QString()).contains(QStringLiteral("super-secret-value")));
    }

    // The new PIN opens it; the old one does not.
    store.lockDeviceSecret();
    QVERIFY(!store.unsealDeviceSecret(QStringLiteral("428391")));
    QVERIFY(store.unsealDeviceSecret(QStringLiteral("760142")));
    const std::optional<DevicePairing> loaded = store.load();
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->deviceSecret, QStringLiteral("super-secret-value"));
}

void PairingStoreSealTest::resealUnderTheWrongOldPinChangesNothing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    PairingStore store(secureStore);

    QVERIFY(store.save(samplePairing()));
    QVERIFY(store.sealDeviceSecret(QStringLiteral("428391")));
    const QString blobBefore = secureStore.get(QStringLiteral("pairing.deviceSecretSealed")).value_or(QString());

    QVERIFY(!store.resealDeviceSecret(QStringLiteral("999999"), QStringLiteral("760142")));

    // Untouched: same blob, still opens under the original PIN, and no
    // plaintext was written on the way to failing.
    QCOMPARE(secureStore.get(QStringLiteral("pairing.deviceSecretSealed")).value_or(QString()), blobBefore);
    QVERIFY(secureStore.get(QStringLiteral("pairing.deviceSecret")).value_or(QString()).isEmpty());
    store.lockDeviceSecret();
    QVERIFY(store.unsealDeviceSecret(QStringLiteral("428391")));
}

// load() reads eight keys. In production each is a QKeychain job on a nested
// QEventLoop and a D-Bus round trip to the Secret Service, and isPaired() is
// load().has_value() -- so answering "are we paired" cost eight of them, and
// MailController asks on every mail operation.
void PairingStoreSealTest::repeatedLoadsDoNotReReadTheStore()
{
    ReentrantSecureStore secureStore;
    PairingStore store(secureStore);
    QVERIFY(store.save(samplePairing()));

    secureStore.resetReadCount();
    QVERIFY(store.load().has_value());
    const int firstLoadReads = secureStore.reads();
    QVERIFY(firstLoadReads > 1); // it really does read per field

    secureStore.resetReadCount();
    for (int i = 0; i < 20; ++i)
        QVERIFY(store.isPaired());
    QCOMPARE(secureStore.reads(), 0);
}

// The cache is only safe if every mutation drops it. A stale pairing here
// would mean sending a retired device secret, or reporting a wiped device as
// still paired.
void PairingStoreSealTest::everyMutationInvalidatesTheCache()
{
    ReentrantSecureStore secureStore;
    PairingStore store(secureStore);
    QVERIFY(store.save(samplePairing()));
    QVERIFY(store.load().has_value());

    // save() with a rotated secret.
    DevicePairing rotated = samplePairing();
    rotated.deviceSecret = QStringLiteral("rotated");
    QVERIFY(store.save(rotated));
    QCOMPARE(store.load()->deviceSecret, QStringLiteral("rotated"));

    // seal() -- the plaintext must disappear from load().
    QVERIFY(store.sealDeviceSecret(QStringLiteral("428391")));
    QCOMPARE(store.load()->deviceSecret, QStringLiteral("rotated")); // session copy
    store.lockDeviceSecret();
    QVERIFY(store.load()->deviceSecret.isEmpty());

    // unseal() -- and back again.
    QVERIFY(store.unsealDeviceSecret(QStringLiteral("428391")));
    QCOMPARE(store.load()->deviceSecret, QStringLiteral("rotated"));

    // reseal() under a new PIN.
    QVERIFY(store.resealDeviceSecret(QStringLiteral("428391"), QStringLiteral("760142")));
    QCOMPARE(store.load()->deviceSecret, QStringLiteral("rotated"));

    // permanent unseal.
    QVERIFY(store.unsealDeviceSecretPermanently(QStringLiteral("760142")));
    QCOMPARE(store.load()->deviceSecret, QStringLiteral("rotated"));

    // clear() -- the one where a stale cache would report a wiped device as
    // still paired, which is what the wipe-after-ten-failures path depends
    // on being false.
    QVERIFY(store.clear());
    QVERIFY(!store.load().has_value());
    QVERIFY(!store.isPaired());
}

// A lock that lands while save() is blocked must win.
//
// save() takes the sealing key as a parameter precisely so a lock mid-call
// cannot fail a check that already passed -- correct, and it then undid the
// lock on the way out. It restored the key captured BEFORE the call, so
// after lockDeviceSecret() had deliberately destroyed m_sessionKey, save()
// handed it straight back. The app was locked, the plaintext was gone, and
// the PBKDF2 key that decrypts the sealed blob was live again -- which is
// what lockDeviceSecret()'s own comment says re-locking exists to end.
void PairingStoreSealTest::aLockArrivingDuringSaveIsNotUndoneBySave()
{
    ReentrantSecureStore secureStore;
    PairingStore store(secureStore);
    QVERIFY(store.save(samplePairing()));
    QVERIFY(store.sealDeviceSecret(QStringLiteral("428391")));
    QVERIFY(store.sealingKeySnapshot().isValid());

    const CredentialCipher::SessionKey sealingKey = store.sealingKeySnapshot();

    // Minimise the window on the third write of the nine save() makes --
    // i.e. from inside the nested event loop, exactly as production does.
    secureStore.interruptAfterNextWrites(3, [&store]() { store.lockDeviceSecret(); });

    DevicePairing rotated = samplePairing();
    rotated.deviceSecret = QStringLiteral("rotated-by-the-relay");
    store.save(rotated, sealingKey);

    // Guard the guard: if the hook silently never ran, everything below
    // would pass for the wrong reason.
    QVERIFY(secureStore.hookFired());

    // The lock stands: no session key, nothing to re-seal or decrypt with.
    QVERIFY(!store.sealingKeySnapshot().isValid());
    QVERIFY(!store.canResealDeviceSecret());
    // And the secret is not readable, which is the point of being locked.
    const std::optional<DevicePairing> loaded = store.load();
    QVERIFY(loaded.has_value());
    QVERIFY(loaded->deviceSecret.isEmpty());
}

// The other half of the same rule: with no lock, the key the caller was
// using before the call must come back, or an ordinary re-registration
// would leave the session unable to re-seal until the next unlock.
void PairingStoreSealTest::anUninterruptedSaveStillRestoresTheSessionKey()
{
    ReentrantSecureStore secureStore;
    PairingStore store(secureStore);
    QVERIFY(store.save(samplePairing()));
    QVERIFY(store.sealDeviceSecret(QStringLiteral("428391")));

    const CredentialCipher::SessionKey sealingKey = store.sealingKeySnapshot();
    QVERIFY(sealingKey.isValid());

    DevicePairing rotated = samplePairing();
    rotated.deviceSecret = QStringLiteral("rotated-by-the-relay");
    QVERIFY(store.save(rotated, sealingKey));

    QVERIFY(store.sealingKeySnapshot().isValid());
    QVERIFY(store.canResealDeviceSecret());
    const std::optional<DevicePairing> loaded = store.load();
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->deviceSecret, QStringLiteral("rotated-by-the-relay"));
}

QTEST_APPLESS_MAIN(PairingStoreSealTest)
#include "PairingStoreSealTest.moc"

