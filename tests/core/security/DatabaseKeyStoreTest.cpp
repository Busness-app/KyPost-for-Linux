#include "security/DatabaseKeyStore.h"

#include "stores/SecureStore.h"
#include "stores/SecureStoreFile.h"

#include <QHash>
#include <QTemporaryDir>
#include <QTest>

namespace {

// Reads FAIL rather than answering "absent" -- a locked wallet, no Secret
// Service, a D-Bus timeout. The single most dangerous input this class has.
class UnreachableSecureStore : public SecureStore
{
public:
    ReadResult read(const QString&) const override { return ReadResult{ ReadStatus::Failed, QString() }; }
    bool set(const QString&, const QString&) override { return false; }
    std::optional<QString> get(const QString&) const override { return std::nullopt; }
    bool remove(const QString&) override { return false; }
    bool contains(const QString&) const override { return false; }
};

// Answers reads, refuses writes -- a store that is readable but cannot
// persist anything new.
class ReadOnlySecureStore : public SecureStore
{
public:
    bool set(const QString&, const QString&) override { return false; }
    std::optional<QString> get(const QString& key) const override
    {
        const auto it = m_values.constFind(key);
        return it == m_values.constEnd() ? std::nullopt : std::optional<QString>(*it);
    }
    bool remove(const QString&) override { return false; }
    bool contains(const QString& key) const override { return m_values.contains(key); }

    QHash<QString, QString> m_values;
};

} // namespace

class DatabaseKeyStoreTest : public QObject
{
    Q_OBJECT

private slots:
    void createPersistsAKeyThatComesBackIdentically();
    void createdKeysAreNotPredictable();
    void anUnreadableStoreIsNeverReportedAsAbsent();
    void aCorruptStoredKeyIsNotReportedAsAbsentEither();
    void aRefusedWriteYieldsNoKeyAtAll();
    void clearRemovesIt();
};

void DatabaseKeyStoreTest::createPersistsAKeyThatComesBackIdentically()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    DatabaseKeyStore keyStore(secureStore);

    QCOMPARE(keyStore.existing().status, DatabaseKeyStore::Status::Absent);

    const QByteArray created = keyStore.create();
    QCOMPARE(created.size(), DatabaseKeyStore::kKeyBytes);

    // A second store over the same backing, as the next launch sees it.
    DatabaseKeyStore afterRelaunch(secureStore);
    const DatabaseKeyStore::Result loaded = afterRelaunch.existing();
    QCOMPARE(loaded.status, DatabaseKeyStore::Status::Found);
    QCOMPARE(loaded.key, created);
}

// Not a proof of randomness -- nothing here can be -- but it does catch the
// two ways this has been got wrong in real code: a constant, and a key that
// is only re-derived per process.
void DatabaseKeyStoreTest::createdKeysAreNotPredictable()
{
    QTemporaryDir dirA;
    QTemporaryDir dirB;
    QVERIFY(dirA.isValid() && dirB.isValid());
    SecureStoreFile storeA(dirA.path());
    SecureStoreFile storeB(dirB.path());

    const QByteArray a = DatabaseKeyStore(storeA).create();
    const QByteArray b = DatabaseKeyStore(storeB).create();

    QCOMPARE(a.size(), DatabaseKeyStore::kKeyBytes);
    QCOMPARE(b.size(), DatabaseKeyStore::kKeyBytes);
    QVERIFY2(a != b, "two profiles were given the same database key");
    QVERIFY2(!a.startsWith(QByteArray(8, '\0')), "the key begins with eight zero bytes");
}

// THE ONE THAT MATTERS.
//
// If an unreadable store read as Absent, a caller doing the obvious
// load-or-create would mint a fresh key over the top of the real one -- and
// the database it was meant to open becomes unopenable by anyone, including
// its owner, permanently. There is no recovery: the key that decrypts it no
// longer exists.
void DatabaseKeyStoreTest::anUnreadableStoreIsNeverReportedAsAbsent()
{
    UnreachableSecureStore unreachable;
    DatabaseKeyStore keyStore(unreachable);

    const DatabaseKeyStore::Result result = keyStore.existing();
    QCOMPARE(result.status, DatabaseKeyStore::Status::Unreadable);
    QVERIFY(result.key.isEmpty());
    QVERIFY2(result.status != DatabaseKeyStore::Status::Absent,
             "an unreadable keyring reported as 'no key', which invites overwriting the real one");
}

// Same reasoning, different cause: a key that is present but damaged is not
// a licence to generate a replacement over it.
void DatabaseKeyStoreTest::aCorruptStoredKeyIsNotReportedAsAbsentEither()
{
    ReadOnlySecureStore store;
    store.m_values.insert(QStringLiteral("db.encryptionKey"), QStringLiteral("not-hex-and-far-too-short"));

    const DatabaseKeyStore::Result result = DatabaseKeyStore(store).existing();
    QCOMPARE(result.status, DatabaseKeyStore::Status::Corrupt);
    QVERIFY(result.key.isEmpty());
}

// A key that was never saved is worse than no key: the caller would encrypt
// a database with something that exists only in this process, readable until
// the app exits and never again.
void DatabaseKeyStoreTest::aRefusedWriteYieldsNoKeyAtAll()
{
    ReadOnlySecureStore store;
    DatabaseKeyStore keyStore(store);

    QVERIFY2(keyStore.create().isEmpty(), "a key was handed back that the store never accepted");
    QCOMPARE(keyStore.existing().status, DatabaseKeyStore::Status::Absent);
}

void DatabaseKeyStoreTest::clearRemovesIt()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SecureStoreFile secureStore(dir.path());
    DatabaseKeyStore keyStore(secureStore);

    QVERIFY(!keyStore.create().isEmpty());
    QCOMPARE(keyStore.existing().status, DatabaseKeyStore::Status::Found);

    QVERIFY(keyStore.clear());
    QCOMPARE(keyStore.existing().status, DatabaseKeyStore::Status::Absent);
}

QTEST_GUILESS_MAIN(DatabaseKeyStoreTest)
#include "DatabaseKeyStoreTest.moc"
