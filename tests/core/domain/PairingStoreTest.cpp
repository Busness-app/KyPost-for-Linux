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

// A store that cannot be consulted at all -- no Secret Service provider
// running, a locked wallet, no D-Bus session. Distinct from
// UnremovableSecureStore above, which answers reads and only refuses
// removals: this one's reads FAIL, which is the case that must not be
// mistaken for "there is no pairing".
class UnreachableSecureStore : public SecureStore
{
public:
    ReadResult read(const QString&) const override { return ReadResult{ ReadStatus::Failed, QString() }; }
    bool set(const QString&, const QString&) override { return false; }
    std::optional<QString> get(const QString&) const override { return std::nullopt; }
    bool remove(const QString&) override { return false; }
    bool contains(const QString&) const override { return false; }
};


// Answers every key except one, whose read FAILS -- a Secret Service that
// re-locked, timed out or was restarted part-way through a load. The one
// case get() cannot express, because it turns a failure into std::nullopt.
class PartiallyReadableSecureStore : public SecureStore
{
public:
    explicit PartiallyReadableSecureStore(QString failingKey)
        : m_failingKey(std::move(failingKey))
    {
    }

    ReadResult read(const QString& key) const override
    {
        if (key == m_failingKey)
            return ReadResult{ ReadStatus::Failed, QString() };
        const auto it = m_values.constFind(key);
        if (it == m_values.constEnd())
            return ReadResult{ ReadStatus::Absent, QString() };
        return ReadResult{ ReadStatus::Found, *it };
    }
    bool set(const QString& key, const QString& value) override
    {
        m_values.insert(key, value);
        return true;
    }
    // get() keeps lying, exactly as the real keychain backend's does. That is
    // the point: the load path must not be reading through it.
    std::optional<QString> get(const QString& key) const override
    {
        const ReadResult result = read(key);
        return result.found() ? std::optional<QString>(result.value) : std::nullopt;
    }
    bool remove(const QString& key) override { return m_values.remove(key) > 0; }
    bool contains(const QString& key) const override { return m_values.contains(key); }

private:
    QString m_failingKey;
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
    void loadCheckedTellsUnreadableApartFromUnpaired();

    // Review-finding regression.
    void aFailedReadOfAnyFieldFailsTheWholeLoad();

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

// load() collapses "never paired" and "the store could not be consulted" into
// the same nullopt. That is the conflation SecureStore::read() exists to undo,
// and for account replacement it is not cosmetic: an unreadable store reading
// as "never paired" means a replacement is not detected, so the previous
// account's cached mail is never purged.
void PairingStoreTest::loadCheckedTellsUnreadableApartFromUnpaired()
{
    // 1. Unreadable.
    {
        UnreachableSecureStore unreachable;
        PairingStore pairingStore(unreachable);

        const PairingStore::LoadResult result = pairingStore.loadChecked();
        QCOMPARE(result.status, PairingStore::LoadStatus::Unreadable);
        QVERIFY(!result.pairing.has_value());
        // load() still reports nullopt, exactly as before -- the callers that
        // do not make a security decision are unaffected.
        QVERIFY(!pairingStore.load().has_value());
        // And an in-flight reply is discarded rather than written into a
        // profile whose pairing cannot be established.
        QVERIFY(!pairingStore.stillCurrent(PairingIdentity{}));
    }

    // 2. Readable, and genuinely not paired.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    PairingStore pairingStore(secureStore);

    QCOMPARE(pairingStore.loadChecked().status, PairingStore::LoadStatus::Absent);
    QVERIFY(!pairingStore.loadChecked().pairing.has_value());

    // 3. Readable and paired.
    const DevicePairing pairing = samplePairing();
    QVERIFY(pairingStore.save(pairing));

    const PairingStore::LoadResult loaded = pairingStore.loadChecked();
    QCOMPARE(loaded.status, PairingStore::LoadStatus::Loaded);
    QVERIFY(loaded.pairing.has_value());
    QCOMPARE(loaded.pairing->subscriberId, pairing.subscriberId);
}

// A keychain that answers "sub" and then fails on ANY later field must not
// produce a pairing.
//
// The certificate pin is the one that made this critical. Every field after
// "sub" was read through get(), which collapses "the store could not be
// consulted" into std::nullopt and then into an empty string -- so a partial
// outage produced a pairing with a valid subscriber id, a valid device
// secret and NO pin. That was cached as a successful load, HttpClient was
// configured with no TOFU enforcement, and because only mutations invalidate
// the cache the keychain was never asked again for the rest of the session:
// authenticated traffic kept flowing, unpinned, on a connection nothing was
// checking.
void PairingStoreTest::aFailedReadOfAnyFieldFailsTheWholeLoad()
{
    const QStringList fields = {
        QStringLiteral("pairing.serverBaseUrl"),  QStringLiteral("pairing.registrationUrl"),
        QStringLiteral("pairing.pairingToken"),   QStringLiteral("deviceId"),
        QStringLiteral("pairing.deviceName"),     QStringLiteral("pairing.deviceSecret"),
        QStringLiteral("pairing.certificateSpkiSha256"),
    };

    // Every field checked in one pass rather than stopping at the first, so
    // the message names ALL the positions that fail closed -- the
    // certificate pin above all, which is the one whose absence silently
    // dropped TLS pinning.
    QStringList accepted;
    for (const QString& failing : fields) {
        PartiallyReadableSecureStore store(failing);
        PairingStore pairingStore(store);
        QVERIFY(pairingStore.save(samplePairing()));
        // The pin is not part of save(); written directly, as HttpClient's
        // TOFU path does.
        QVERIFY(store.set(QStringLiteral("pairing.certificateSpkiSha256"),
                           QStringLiteral("abcd1234")));

        const PairingStore::LoadResult result = pairingStore.loadChecked();
        // Nothing may have been cached either: a cached half-pairing outlives
        // the outage that produced it, since only mutations drop the cache.
        if (result.status != PairingStore::LoadStatus::Unreadable || result.pairing.has_value()
            || pairingStore.loadChecked().status != PairingStore::LoadStatus::Unreadable
            || pairingStore.load().has_value()) {
            accepted.append(failing);
        }
    }
    QVERIFY2(accepted.isEmpty(),
             qPrintable(QStringLiteral("a failed read of these fields did not fail the load: %1")
                            .arg(accepted.join(QStringLiteral(", ")))));

    // The same store with nothing failing still loads, so the check above is
    // not passing for want of a working store.
    PartiallyReadableSecureStore healthy(QStringLiteral("nothing.fails"));
    PairingStore pairingStore(healthy);
    QVERIFY(pairingStore.save(samplePairing()));
    QCOMPARE(pairingStore.loadChecked().status, PairingStore::LoadStatus::Loaded);
}

QTEST_GUILESS_MAIN(PairingStoreTest)
#include "PairingStoreTest.moc"
