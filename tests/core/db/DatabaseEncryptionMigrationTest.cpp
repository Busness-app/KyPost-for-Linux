#include "db/DatabaseEncryptionMigration.h"

#include "db/Database.h"
#include "db/ProfileDatabase.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QVariant>

namespace {

QByteArray testKey()
{
    return QByteArray(Database::kRawKeyBytes, '\x4b');
}

// A plaintext profile with recognisable content in it.
void seedPlaintextProfile(const QString& path, int messageCount)
{
    Database db;
    QVERIFY(db.open(path));
    for (int i = 0; i < messageCount; ++i) {
        QSqlQuery insert(db.handle());
        insert.prepare(QStringLiteral(
            "INSERT INTO emails (message_id, folder, subject, at_utc) VALUES (?, ?, ?, ?)"));
        insert.addBindValue(QStringLiteral("m%1").arg(i));
        insert.addBindValue(QStringLiteral("INBOX"));
        insert.addBindValue(QStringLiteral("SECRETSUBJECT%1").arg(i));
        insert.addBindValue(QStringLiteral("2026-08-22T00:00:00Z"));
        QVERIFY(insert.exec());
    }
}

int countEmails(Database& db)
{
    QSqlQuery query(db.handle());
    if (!query.exec(QStringLiteral("SELECT count(*) FROM emails")) || !query.next())
        return -1;
    return query.value(0).toInt();
}

bool anyFileUnder(const QString& dir, const QByteArray& needle)
{
    const QDir d(dir);
    for (const QFileInfo& info : d.entryInfoList(QDir::Files)) {
        QFile f(info.absoluteFilePath());
        if (f.open(QIODevice::ReadOnly) && f.readAll().contains(needle))
            return true;
    }
    return false;
}

} // namespace

class DatabaseEncryptionMigrationTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void aPlaintextProfileIsConvertedAndTheOriginalErased();
    void anAlreadyEncryptedProfileIsLeftAlone();
    void aMissingProfileIsNotAnError();
    void aRefusedKeyLeavesThePlaintextDatabaseUsable();
    void anInterruptedSwapIsRepairedOnTheNextLaunch();
    void aHalfWrittenCopyFromADeadRunIsDiscarded();
    void runningTwiceIsHarmless();
};

void DatabaseEncryptionMigrationTest::initTestCase()
{
    if (!Database::encryptionAvailable())
        QSKIP("built without SQLCipher -- migration is NOT covered");
}

// The whole point, end to end: the mail survives, and the plaintext copy
// does not remain anywhere in the profile directory.
void DatabaseEncryptionMigrationTest::aPlaintextProfileIsConvertedAndTheOriginalErased()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("kypost.db"));
    seedPlaintextProfile(path, 5);
    QVERIFY(!databaseFileIsEncrypted(path));
    QVERIFY(anyFileUnder(dir.path(), "SECRETSUBJECT0"));

    DatabaseEncryptionMigration migration(path);
    QCOMPARE(migration.run(testKey()), DatabaseEncryptionMigration::Status::Migrated);

    QVERIFY2(databaseFileIsEncrypted(path), "the profile database is still plaintext");
    // Not just the database: nothing anywhere in the profile directory still
    // holds the plaintext, which is what the .plaintext-old secure delete and
    // the sidecar removal are for.
    QVERIFY2(!anyFileUnder(dir.path(), "SECRETSUBJECT0"),
             "a readable copy of the mail is still on disk somewhere in the profile");

    // And the mail is all there.
    Database reopened;
    QVERIFY(reopened.open(path, testKey()));
    QCOMPARE(countEmails(reopened), 5);
}

void DatabaseEncryptionMigrationTest::anAlreadyEncryptedProfileIsLeftAlone()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("kypost.db"));

    {
        Database db;
        QVERIFY(db.open(path, testKey()));
    }

    DatabaseEncryptionMigration migration(path);
    QCOMPARE(migration.run(testKey()), DatabaseEncryptionMigration::Status::NotNeeded);
    QVERIFY(databaseFileIsEncrypted(path));
}

void DatabaseEncryptionMigrationTest::aMissingProfileIsNotAnError()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DatabaseEncryptionMigration migration(dir.filePath(QStringLiteral("kypost.db")));
    QCOMPARE(migration.run(testKey()), DatabaseEncryptionMigration::Status::NotNeeded);
}

// Losing the user's mail is a worse outcome than leaving it unencrypted for
// another launch. Every failure path is written that way round, so this
// asserts the database is still THERE and still READABLE afterwards.
void DatabaseEncryptionMigrationTest::aRefusedKeyLeavesThePlaintextDatabaseUsable()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("kypost.db"));
    seedPlaintextProfile(path, 3);

    DatabaseEncryptionMigration migration(path);
    QCOMPARE(migration.run(QByteArray(8, '\x01')), DatabaseEncryptionMigration::Status::Failed);

    QVERIFY(!databaseFileIsEncrypted(path));
    Database still;
    QVERIFY(still.open(path));
    QCOMPARE(countEmails(still), 3);
}

// The process died between the two renames -- the original moved aside, the
// encrypted copy never put in its place. That is the only moment where the
// profile has no database at its usual path.
void DatabaseEncryptionMigrationTest::anInterruptedSwapIsRepairedOnTheNextLaunch()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("kypost.db"));
    seedPlaintextProfile(path, 4);

    // Stage exactly that state by hand.
    QVERIFY(QFile::rename(path, path + QStringLiteral(".plaintext-old")));
    QFile marker(path + QStringLiteral(".encrypting"));
    QVERIFY(marker.open(QIODevice::WriteOnly));
    marker.close();
    QVERIFY(!QFile::exists(path));

    DatabaseEncryptionMigration migration(path);
    QVERIFY(migration.interrupted());
    const auto status = migration.run(testKey());

    // Whatever it decides to do about encryption, the mail must be back.
    QVERIFY2(QFile::exists(path), "the profile database was not restored after an interrupted swap");
    QVERIFY(status == DatabaseEncryptionMigration::Status::Migrated);
    Database reopened;
    QVERIFY(reopened.open(path, testKey()));
    QCOMPARE(countEmails(reopened), 4);
    QVERIFY(!migration.interrupted());
}

// A copy left behind by a run that died during the export is worthless --
// there is no state in which it is preferred to the original -- and leaving
// it would make the next attempt's ATTACH land on a partial file.
void DatabaseEncryptionMigrationTest::aHalfWrittenCopyFromADeadRunIsDiscarded()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("kypost.db"));
    seedPlaintextProfile(path, 2);

    QFile stale(path + QStringLiteral(".encrypted-new"));
    QVERIFY(stale.open(QIODevice::WriteOnly));
    stale.write("not a database, just the first few bytes of one");
    stale.close();

    DatabaseEncryptionMigration migration(path);
    QCOMPARE(migration.run(testKey()), DatabaseEncryptionMigration::Status::Migrated);

    QVERIFY(!QFile::exists(path + QStringLiteral(".encrypted-new")));
    Database reopened;
    QVERIFY(reopened.open(path, testKey()));
    QCOMPARE(countEmails(reopened), 2);
}

// run() is called on every launch, so it has to be safe to call when there
// is nothing to do.
void DatabaseEncryptionMigrationTest::runningTwiceIsHarmless()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("kypost.db"));
    seedPlaintextProfile(path, 6);

    DatabaseEncryptionMigration migration(path);
    QCOMPARE(migration.run(testKey()), DatabaseEncryptionMigration::Status::Migrated);
    QCOMPARE(migration.run(testKey()), DatabaseEncryptionMigration::Status::NotNeeded);

    Database reopened;
    QVERIFY(reopened.open(path, testKey()));
    QCOMPARE(countEmails(reopened), 6);
}

QTEST_GUILESS_MAIN(DatabaseEncryptionMigrationTest)
#include "DatabaseEncryptionMigrationTest.moc"
