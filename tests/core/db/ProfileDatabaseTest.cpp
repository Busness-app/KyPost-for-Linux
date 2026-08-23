#include "db/ProfileDatabase.h"

#include "db/Database.h"
#include "stores/SecureStore.h"
#include "stores/SecureStoreFile.h"

#include <QFile>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

namespace {

class UnreachableSecureStore : public SecureStore
{
public:
    ReadResult read(const QString&) const override { return ReadResult{ ReadStatus::Failed, QString() }; }
    bool set(const QString&, const QString&) override { return false; }
    std::optional<QString> get(const QString&) const override { return std::nullopt; }
    bool remove(const QString&) override { return false; }
    bool contains(const QString&) const override { return false; }
};

ProfileDatabaseInputs inputs(bool sqlcipher, DatabaseKeyStore::Status key, bool exists, bool encrypted)
{
    ProfileDatabaseInputs in;
    in.sqlCipherAvailable = sqlcipher;
    in.keyStatus = key;
    in.databaseFileExists = exists;
    in.databaseFileIsEncrypted = encrypted;
    return in;
}

} // namespace

class ProfileDatabaseTest : public QObject
{
    Q_OBJECT

private slots:
    void aNewProfileIsEncrypted();
    void anExistingPlaintextProfileIsOpenedAsItIs();
    void anUnreadableKeyStoreNeverMintsAKeyForANewProfile();
    void anUnreadableKeyStoreStillOpensAnExistingProfile();
    void aBuildWithoutSqlCipherStaysOnDisk();
    void anEncryptedProfileIsNeverReopenedInTheClear();
    void aNewProfileWithNowhereToKeepAKeyGoesToMemory();
};

// The decision table, exhaustively, with no disk and no keyring involved.

void ProfileDatabaseTest::aNewProfileIsEncrypted()
{
    QCOMPARE(chooseProfileDatabaseMode(inputs(true, DatabaseKeyStore::Status::Absent, false, false)),
             ProfileDatabaseMode::EncryptedOnDisk);
    // And an existing encrypted one stays that way.
    QCOMPARE(chooseProfileDatabaseMode(inputs(true, DatabaseKeyStore::Status::Found, true, true)),
             ProfileDatabaseMode::EncryptedOnDisk);
}

// Refusing here would not un-write the plaintext already on this disk. It
// would only take the user's mail away while leaving the exposure exactly
// where it was.
void ProfileDatabaseTest::anExistingPlaintextProfileIsOpenedAsItIs()
{
    QCOMPARE(chooseProfileDatabaseMode(inputs(true, DatabaseKeyStore::Status::Absent, true, false)),
             ProfileDatabaseMode::PlaintextOnDisk);
}

// The case that costs a user their mail if it is got wrong: minting a fresh
// key because the keyring happened to be locked overwrites the only copy of
// the real one, and the database becomes unopenable forever.
void ProfileDatabaseTest::anUnreadableKeyStoreNeverMintsAKeyForANewProfile()
{
    for (const auto status : { DatabaseKeyStore::Status::Unreadable, DatabaseKeyStore::Status::Corrupt }) {
        QCOMPARE(chooseProfileDatabaseMode(inputs(true, status, false, false)),
                 ProfileDatabaseMode::InMemoryNoKeyStorage);
    }
}

void ProfileDatabaseTest::anUnreadableKeyStoreStillOpensAnExistingProfile()
{
    QCOMPARE(chooseProfileDatabaseMode(inputs(true, DatabaseKeyStore::Status::Unreadable, true, false)),
             ProfileDatabaseMode::PlaintextOnDisk);
}

void ProfileDatabaseTest::aBuildWithoutSqlCipherStaysOnDisk()
{
    // A packaging state, not a user-facing security decision -- and forcing
    // every such build to run from memory would make it useless rather than
    // safe.
    QCOMPARE(chooseProfileDatabaseMode(inputs(false, DatabaseKeyStore::Status::Absent, false, false)),
             ProfileDatabaseMode::PlaintextOnDisk);
}

// Now against real files.

void ProfileDatabaseTest::anEncryptedProfileIsNeverReopenedInTheClear()
{
    if (!Database::encryptionAvailable())
        QSKIP("built without SQLCipher -- this behaviour is NOT covered");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("kypost.db"));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    DatabaseKeyStore keyStore(secureStore);

    {
        Database db;
        QCOMPARE(openProfileDatabase(db, keyStore, path), ProfileDatabaseMode::EncryptedOnDisk);
        QSqlQuery insert(db.handle());
        insert.prepare(QStringLiteral("INSERT INTO emails (message_id, folder, at_utc) VALUES (?, ?, ?)"));
        insert.addBindValue(QStringLiteral("m1"));
        insert.addBindValue(QStringLiteral("INBOX"));
        insert.addBindValue(QStringLiteral("2026-08-22T00:00:00Z"));
        QVERIFY(insert.exec());
    }

    QVERIFY2(databaseFileIsEncrypted(path), "the new profile was not actually encrypted on disk");

    // Reopening with the key present works.
    {
        Database db;
        QCOMPARE(openProfileDatabase(db, keyStore, path), ProfileDatabaseMode::EncryptedOnDisk);
        QSqlQuery read(db.handle());
        QVERIFY(read.exec(QStringLiteral("SELECT count(*) FROM emails")));
        QVERIFY(read.next());
        QCOMPARE(read.value(0).toInt(), 1);
    }

    // And with the key GONE, it does not quietly come up in the clear -- it
    // does not come up at all. Silently reopening an encrypted profile
    // unencrypted would be the same failure as PRAGMA key against ordinary
    // SQLite: everything looks fine and nothing is protected.
    QVERIFY(keyStore.clear());
    Database db;
    const ProfileDatabaseMode mode = openProfileDatabase(db, keyStore, path);
    QVERIFY2(mode == ProfileDatabaseMode::FailedToOpen,
             "an encrypted profile was reopened without its key");
}

void ProfileDatabaseTest::aNewProfileWithNowhereToKeepAKeyGoesToMemory()
{
    if (!Database::encryptionAvailable())
        QSKIP("built without SQLCipher -- this behaviour is NOT covered");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("kypost.db"));

    UnreachableSecureStore unreachable;
    DatabaseKeyStore keyStore(unreachable);

    Database db;
    QCOMPARE(openProfileDatabase(db, keyStore, path), ProfileDatabaseMode::InMemoryNoKeyStorage);

    // Usable -- migrations ran -- and nothing was written to the profile.
    QSqlQuery query(db.handle());
    QVERIFY(query.exec(QStringLiteral("SELECT count(*) FROM emails")));
    QVERIFY2(!QFile::exists(path), "a database file was created despite there being nowhere to keep its key");
}

QTEST_GUILESS_MAIN(ProfileDatabaseTest)
#include "ProfileDatabaseTest.moc"
