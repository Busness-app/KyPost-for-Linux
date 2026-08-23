#include "db/ProfileDatabase.h"

#include "db/Database.h"
#include "stores/SecureStore.h"
#include "stores/SecureStoreFile.h"

#include <QFile>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

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
    void anExistingPlaintextProfileIsConverted();
    void anUnreadableKeyStoreNeverMintsAKeyForANewProfile();
    void anUnreadableKeyStoreStillOpensAnExistingProfile();
    void aBuildWithoutSqlCipherStaysOnDisk();
    void anEncryptedProfileIsNeverReopenedInTheClear();
    void aNewProfileWithNowhereToKeepAKeyGoesToMemory();
    void openingAnExistingPlaintextProfileConvertsItInPlace();
    void aStrandedProfileIsRefusedRatherThanReplacedWithAnEmptyOne();
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

// Converted rather than left alone (user's call): most people never open
// Settings, so an opt-in would leave most mail in plaintext indefinitely.
// The conversion is written so that losing the mail is never the failure
// mode, and openProfileDatabase() falls back to the untouched plaintext
// database if it does fail.
void ProfileDatabaseTest::anExistingPlaintextProfileIsConverted()
{
    QCOMPARE(chooseProfileDatabaseMode(inputs(true, DatabaseKeyStore::Status::Absent, true, false)),
             ProfileDatabaseMode::EncryptedOnDisk);
    // Same for a key that exists while the file has not caught up, which is
    // what an interrupted conversion looks like.
    QCOMPARE(chooseProfileDatabaseMode(inputs(true, DatabaseKeyStore::Status::Found, true, false)),
             ProfileDatabaseMode::EncryptedOnDisk);
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

// End to end on the path every existing user takes on their next launch:
// they had a plaintext database, they open the app, and afterwards their
// mail is encrypted and still all there.
void ProfileDatabaseTest::openingAnExistingPlaintextProfileConvertsItInPlace()
{
    if (!Database::encryptionAvailable())
        QSKIP("built without SQLCipher -- this behaviour is NOT covered");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("kypost.db"));

    // The profile as it exists today: plaintext, with mail in it.
    {
        Database plain;
        QVERIFY(plain.open(path));
        QSqlQuery insert(plain.handle());
        insert.prepare(QStringLiteral("INSERT INTO emails (message_id, folder, subject, at_utc) "
                                       "VALUES (?, ?, ?, ?)"));
        insert.addBindValue(QStringLiteral("m1"));
        insert.addBindValue(QStringLiteral("INBOX"));
        insert.addBindValue(QStringLiteral("SUBJECTFROMBEFORE"));
        insert.addBindValue(QStringLiteral("2026-08-22T00:00:00Z"));
        QVERIFY(insert.exec());
    }
    QVERIFY(!databaseFileIsEncrypted(path));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    DatabaseKeyStore keyStore(secureStore);
    QCOMPARE(keyStore.existing().status, DatabaseKeyStore::Status::Absent);

    Database db;
    QCOMPARE(openProfileDatabase(db, keyStore, path), ProfileDatabaseMode::EncryptedOnDisk);

    QVERIFY2(databaseFileIsEncrypted(path), "the existing profile was not converted");
    QCOMPARE(keyStore.existing().status, DatabaseKeyStore::Status::Found);

    QSqlQuery read(db.handle());
    QVERIFY(read.exec(QStringLiteral("SELECT subject FROM emails WHERE message_id = 'm1'")));
    QVERIFY(read.next());
    QCOMPARE(read.value(0).toString(), QStringLiteral("SUBJECTFROMBEFORE"));
}

// An interrupted conversion left the only complete database under another
// name and it could not be moved back. Opening the profile's path now creates
// a NEW, empty encrypted database on top of it -- the app looks fine, the
// mailbox is empty, and the real one is deleted by the next successful
// conversion. Refusing is the only answer that keeps the mail.
void ProfileDatabaseTest::aStrandedProfileIsRefusedRatherThanReplacedWithAnEmptyOne()
{
    if (!Database::encryptionAvailable())
        QSKIP("built without SQLCipher -- this behaviour is NOT covered");
    if (::geteuid() == 0)
        QSKIP("running as root: an unwritable directory does not stop root from renaming in it");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("kypost.db"));
    const QString superseded = path + QStringLiteral(".plaintext-old");

    {
        Database plain;
        QVERIFY(plain.open(path));
        QSqlQuery insert(plain.handle());
        insert.prepare(QStringLiteral("INSERT INTO emails (message_id, folder, subject, at_utc) "
                                       "VALUES (?, ?, ?, ?)"));
        insert.addBindValue(QStringLiteral("m1"));
        insert.addBindValue(QStringLiteral("INBOX"));
        insert.addBindValue(QStringLiteral("SUBJECTFROMBEFORE"));
        insert.addBindValue(QStringLiteral("2026-08-22T00:00:00Z"));
        QVERIFY(insert.exec());
    }
    QVERIFY(QFile::rename(path, superseded));
    QVERIFY(QFile::setPermissions(dir.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));

    QTemporaryDir secureDir;
    QVERIFY(secureDir.isValid());
    SecureStoreFile secureStore(secureDir.path());
    DatabaseKeyStore keyStore(secureStore);

    Database db;
    QCOMPARE(openProfileDatabase(db, keyStore, path), ProfileDatabaseMode::FailedToOpen);
    QVERIFY2(!QFile::exists(path), "an empty database was created over a stranded profile");
    QVERIFY(QFile::exists(superseded));

    QVERIFY(QFile::setPermissions(dir.path(),
                                   QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                       | QFileDevice::ExeOwner));

    // And once the directory is writable again, the same call recovers it.
    Database recovered;
    QCOMPARE(openProfileDatabase(recovered, keyStore, path), ProfileDatabaseMode::EncryptedOnDisk);
    QSqlQuery read(recovered.handle());
    QVERIFY(read.exec(QStringLiteral("SELECT subject FROM emails WHERE message_id = 'm1'")));
    QVERIFY(read.next());
    QCOMPARE(read.value(0).toString(), QStringLiteral("SUBJECTFROMBEFORE"));
}

QTEST_GUILESS_MAIN(ProfileDatabaseTest)
#include "ProfileDatabaseTest.moc"
