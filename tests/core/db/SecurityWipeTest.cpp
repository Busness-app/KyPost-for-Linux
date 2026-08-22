#include "db/SecurityWipe.h"

#include "db/Database.h"
#include "db/EmailDao.h"
#include "models/Email.h"

#include <QDir>
#include <QFile>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

class SecurityWipeTest : public QObject
{
    Q_OBJECT

private slots:
    void removesDatabaseAndEverySidecar();
    void missingFilesAreNotAFailure();
    void neverTouchesTheInMemoryPseudoPath();
    void clearsCacheDirectoryContentsButKeepsTheDirectory();
    void clearCacheHandlesNestedDirectories();
    void wipeAllTablesEmptiesRowsAndKeepsSchema();
    void wipeAllTablesLeavesNoRecoverableContent();
};

void SecurityWipeTest::removesDatabaseAndEverySidecar()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("kypost.db"));

    // This repo sets no journal_mode pragma, so a wipe must not assume
    // rollback-journal-only: -wal and -shm can each still hold committed
    // page data, i.e. readable mail.
    const QStringList all = { dbPath, dbPath + QStringLiteral("-journal"),
                               dbPath + QStringLiteral("-wal"), dbPath + QStringLiteral("-shm") };
    for (const QString& path : all) {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("secret");
        f.close();
    }

    QVERIFY(SecurityWipe::removeDatabaseFiles(dbPath));
    for (const QString& path : all)
        QVERIFY2(!QFile::exists(path), qPrintable(path));
}

void SecurityWipeTest::missingFilesAreNotAFailure()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // Nothing exists at all: still a success, not an error to surface.
    QVERIFY(SecurityWipe::removeDatabaseFiles(dir.filePath(QStringLiteral("absent.db"))));

    // Main file present but no sidecars -- the common case.
    const QString dbPath = dir.filePath(QStringLiteral("only-main.db"));
    QFile f(dbPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.close();
    QVERIFY(SecurityWipe::removeDatabaseFiles(dbPath));
    QVERIFY(!QFile::exists(dbPath));
}

void SecurityWipeTest::neverTouchesTheInMemoryPseudoPath()
{
    // ":memory:" is not a filesystem path; treating it as one could create
    // or delete a literal file of that name in the working directory.
    QVERIFY(SecurityWipe::removeDatabaseFiles(QStringLiteral(":memory:")));
    QVERIFY(!QFile::exists(QStringLiteral(":memory:")));
    QVERIFY(SecurityWipe::removeDatabaseFiles(QString()));
}

void SecurityWipeTest::clearsCacheDirectoryContentsButKeepsTheDirectory()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("contact-photos"));
    QVERIFY(QDir().mkpath(cacheDir));

    for (const QString& name : { QStringLiteral("a.png"), QStringLiteral("b.png") }) {
        QFile f(cacheDir + QLatin1Char('/') + name);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("image");
        f.close();
    }

    QVERIFY(SecurityWipe::clearCacheDirectory(cacheDir));
    // Emptied, but still present -- the app keeps writing here.
    QVERIFY(QDir(cacheDir).exists());
    QCOMPARE(QDir(cacheDir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 0);
}

void SecurityWipeTest::clearCacheHandlesNestedDirectories()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("cache"));
    QVERIFY(QDir().mkpath(cacheDir + QStringLiteral("/nested/deeper")));
    QFile f(cacheDir + QStringLiteral("/nested/deeper/x.bin"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("data");
    f.close();

    QVERIFY(SecurityWipe::clearCacheDirectory(cacheDir));
    QVERIFY(QDir(cacheDir).exists());
    QCOMPARE(QDir(cacheDir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 0);

    // A directory that was never created is not an error.
    QVERIFY(SecurityWipe::clearCacheDirectory(dir.filePath(QStringLiteral("never-existed"))));
}

void SecurityWipeTest::wipeAllTablesEmptiesRowsAndKeepsSchema()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    EmailDao dao(db.handle());

    Email email;
    email.messageId = QStringLiteral("m1");
    email.folder = QStringLiteral("INBOX");
    email.subject = QStringLiteral("Confidential");
    email.body = QStringLiteral("secret body text");
    QVERIFY(dao.insertOrReplace(email));
    QVERIFY(dao.findById(QStringLiteral("INBOX"), QStringLiteral("m1")).has_value());

    QVERIFY(db.wipeAllTables());

    QVERIFY(!dao.findById(QStringLiteral("INBOX"), QStringLiteral("m1")).has_value());
    // The schema must survive -- the app keeps running after a wipe, and a
    // dropped table would break every later query.
    QVERIFY(dao.insertOrReplace(email));
    QVERIFY(dao.findById(QStringLiteral("INBOX"), QStringLiteral("m1")).has_value());
}

void SecurityWipeTest::wipeAllTablesLeavesNoRecoverableContent()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("wipe.db"));

    static const QByteArray marker = "UNIQUE-SECRET-MARKER-9F3A";
    {
        Database db;
        QVERIFY(db.open(dbPath));
        EmailDao dao(db.handle());
        for (int i = 0; i < 50; ++i) {
            Email email;
            email.messageId = QStringLiteral("m%1").arg(i);
            email.folder = QStringLiteral("INBOX");
            email.body = QString::fromLatin1(marker);
            QVERIFY(dao.insertOrReplace(email));
        }
        QVERIFY(QFile(dbPath).size() > 0);
        QVERIFY(db.wipeAllTables());
    }

    // Asserts the end state -- no message content readable straight out of
    // the file -- not any particular mechanism. Measured on this build:
    // secure_delete is ON by default, so the DELETE alone already achieves
    // this and the test passes even with VACUUM removed. wipeAllTables()
    // still sets secure_delete explicitly and VACUUMs, because that default
    // is not universal across SQLite builds; this test is what would catch a
    // build where neither happened.
    QFile f(dbPath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray raw = f.readAll();
    f.close();
    QVERIFY2(!raw.contains(marker), "wiped database still contains message content on disk");
}

QTEST_GUILESS_MAIN(SecurityWipeTest)
#include "SecurityWipeTest.moc"
