#include "db/Database.h"

#include <QFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QVariant>

// SQLCipher, checked against the file on disk rather than against the API's
// own opinion of itself.
//
// The reason every test here reads the raw bytes at some point: `PRAGMA key`
// is not an error on ordinary SQLite. It is an unrecognised pragma, silently
// ignored, and QSqlQuery::exec() returns true. A build linked against stock
// libsqlite3 will run the whole encryption path, report success at every
// step, and leave a database whose contents are legible in a text editor. So
// "the call succeeded" is worth nothing here and the assertions do not use
// it.
class DatabaseEncryptionTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void anEncryptedDatabaseIsNotReadableAsPlaintext();
    void theWrongKeyDoesNotOpenIt();
    void aWrongLengthKeyIsRefusedRatherThanStretched();
    void anEmptyKeyStillOpensAnOrdinaryDatabase();

private:
    static QByteArray keyA();
    static QByteArray keyB();
};

QByteArray DatabaseEncryptionTest::keyA()
{
    return QByteArray(Database::kRawKeyBytes, '\x41');
}

QByteArray DatabaseEncryptionTest::keyB()
{
    return QByteArray(Database::kRawKeyBytes, '\x42');
}

void DatabaseEncryptionTest::initTestCase()
{
    if (!Database::encryptionAvailable()) {
        // Skipped LOUDLY, never passed quietly. A build with no SQLCipher
        // cannot say anything about encryption, and a green tick here would
        // claim otherwise.
        QSKIP("built without SQLCipher (KYPOST_SQLCIPHER_ROOT unset) -- encryption is NOT covered");
    }
}

void DatabaseEncryptionTest::anEncryptedDatabaseIsNotReadableAsPlaintext()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("encrypted.db"));

    {
        Database db;
        QVERIFY(db.open(path, keyA()));

        QSqlQuery insert(db.handle());
        insert.prepare(QStringLiteral(
            "INSERT INTO emails (message_id, folder, subject, at_utc) VALUES (?, ?, ?, ?)"));
        insert.addBindValue(QStringLiteral("m1"));
        insert.addBindValue(QStringLiteral("INBOX"));
        insert.addBindValue(QStringLiteral("TOPSECRETSUBJECTLINE"));
        insert.addBindValue(QStringLiteral("2026-08-22T00:00:00Z"));
        QVERIFY2(insert.exec(), qUtf8Printable(insert.lastError().text()));
    }

    // The actual claim: the bytes on disk.
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray onDisk = file.readAll();
    QVERIFY(!onDisk.isEmpty());
    QVERIFY2(!onDisk.contains("TOPSECRETSUBJECTLINE"),
             "the subject line is sitting in the database file in plaintext");
    // SQLite's format header is the other half of it: an encrypted file does
    // not begin with the magic string, so nothing can open it as a database
    // by accident either.
    QVERIFY2(!onDisk.startsWith("SQLite format 3"),
             "the file still has a plaintext SQLite header, so it was never encrypted");

    // And it opens with the right key.
    Database reopened;
    QVERIFY(reopened.open(path, keyA()));
    QSqlQuery read(reopened.handle());
    QVERIFY(read.exec(QStringLiteral("SELECT subject FROM emails WHERE message_id = 'm1'")));
    QVERIFY(read.next());
    QCOMPARE(read.value(0).toString(), QStringLiteral("TOPSECRETSUBJECTLINE"));
}

// SQLCipher does not verify the key when it is set -- it fails at the first
// page it has to decrypt. Without the probe read in Database::applyEncryptionKey,
// open() would return true under a wrong key and the failure would surface
// later, somewhere with no idea what to do about it.
void DatabaseEncryptionTest::theWrongKeyDoesNotOpenIt()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("encrypted.db"));

    {
        Database db;
        QVERIFY(db.open(path, keyA()));
    }

    Database wrong;
    QVERIFY2(!wrong.open(path, keyB()), "a database opened under a key that cannot decrypt it");

    // Not a one-way door: the right key still works afterwards.
    Database right;
    QVERIFY(right.open(path, keyA()));
}

// Anything that is not exactly 32 raw bytes is treated by SQLCipher as a
// PASSPHRASE and run through PBKDF2 -- so a truncated key would still open a
// database, just a different one, silently, under a different key. It is
// refused before it can reach the pragma.
void DatabaseEncryptionTest::aWrongLengthKeyIsRefusedRatherThanStretched()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Database tooShort;
    QVERIFY(!tooShort.open(dir.filePath(QStringLiteral("a.db")), QByteArray(31, '\x41')));

    Database tooLong;
    QVERIFY(!tooLong.open(dir.filePath(QStringLiteral("b.db")), QByteArray(33, '\x41')));

    // Neither attempt may leave a usable database behind.
    QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("a.db")))
            || QFile(dir.filePath(QStringLiteral("a.db"))).size() == 0);
}

// The unkeyed path is what the app still uses today, and it must be
// completely unchanged -- migrations included.
void DatabaseEncryptionTest::anEmptyKeyStillOpensAnOrdinaryDatabase()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("plain.db"));

    Database db;
    QVERIFY(db.open(path));

    QSqlQuery query(db.handle());
    QVERIFY(query.exec(QStringLiteral("PRAGMA user_version")));
    QVERIFY(query.next());
    QVERIFY2(query.value(0).toInt() > 0, "migrations did not run on the unencrypted path");
}

QTEST_GUILESS_MAIN(DatabaseEncryptionTest)
#include "DatabaseEncryptionTest.moc"
