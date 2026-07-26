#include "db/Database.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

class DatabaseTest : public QObject
{
    Q_OBJECT

private slots:
    void opensInMemoryAndAppliesSchema();
    void openIsIdempotentOnRealFile();

    // Review-finding regressions.
    void splitsStatementsOnTopLevelSemicolonsOnly();
    void migrationIsAtomicSoAFailureLeavesNoHalfAppliedSchema();
};

void DatabaseTest::opensInMemoryAndAppliesSchema()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));

    QSqlQuery versionQuery(db.handle());
    QVERIFY(versionQuery.exec(QStringLiteral("PRAGMA user_version")));
    QVERIFY(versionQuery.next());
    // 5 migrations on disk (001_initial, 002_native_contact_links,
    // 003_extended_contact_fields, 004_contact_self_and_merged,
    // 005_email_pgp_state) -- bumping this when a migration is added is how
    // this test proves the loop in Database::open() actually walks
    // version+1..N end-to-end.
    QCOMPARE(versionQuery.value(0).toInt(), 5);

    QSqlQuery tablesQuery(db.handle());
    QVERIFY(tablesQuery.exec(
        QStringLiteral("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")));
    QStringList tables;
    while (tablesQuery.next())
        tables.append(tablesQuery.value(0).toString());

    QVERIFY(tables.contains(QStringLiteral("emails")));
    QVERIFY(tables.contains(QStringLiteral("contacts")));
    QVERIFY(tables.contains(QStringLiteral("folders")));
    QVERIFY(tables.contains(QStringLiteral("pending_contact_changes")));
    QVERIFY(tables.contains(QStringLiteral("push_notifications")));
    QVERIFY(tables.contains(QStringLiteral("native_contact_links")));

    // 003_extended_contact_fields added 12 columns to `contacts` via ALTER
    // TABLE (no new table) -- verify a representative sample of them.
    QSqlQuery columnsQuery(db.handle());
    QVERIFY(columnsQuery.exec(QStringLiteral("PRAGMA table_info(contacts)")));
    QStringList columns;
    while (columnsQuery.next())
        columns.append(columnsQuery.value(QStringLiteral("name")).toString());
    QVERIFY(columns.contains(QStringLiteral("groups_json")));
    QVERIFY(columns.contains(QStringLiteral("photo_ref")));
    QVERIFY(columns.contains(QStringLiteral("pgp_key")));
    QVERIFY(columns.contains(QStringLiteral("ims_json")));
    QVERIFY(columns.contains(QStringLiteral("websites_json")));
    QVERIFY(columns.contains(QStringLiteral("relations_json")));
    QVERIFY(columns.contains(QStringLiteral("events_json")));
    QVERIFY(columns.contains(QStringLiteral("phonetic_given_name")));
    QVERIFY(columns.contains(QStringLiteral("phonetic_family_name")));
    QVERIFY(columns.contains(QStringLiteral("department")));
    QVERIFY(columns.contains(QStringLiteral("custom_fields_json")));
    QVERIFY(columns.contains(QStringLiteral("pronouns")));
}

void DatabaseTest::openIsIdempotentOnRealFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("kypost-test.sqlite"));

    {
        Database db1;
        QVERIFY(db1.open(path));
        QSqlQuery query(db1.handle());
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 5);
    }
    {
        Database db2;
        QVERIFY(db2.open(path));
        QSqlQuery query(db2.handle());
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 5);
    }
}


// sql.split(';') shattered any statement containing a semicolon inside a
// string literal, a quoted identifier, a comment, or a CREATE TRIGGER body.
// No migration had one yet, which meant the schema was one ordinary change
// away from silently producing invalid fragments.
void DatabaseTest::splitsStatementsOnTopLevelSemicolonsOnly()
{
    QCOMPARE(splitSqlStatements(QStringLiteral("SELECT 1; SELECT 2;")).size(), 2);

    // Semicolon inside a string literal.
    const QStringList literal =
        splitSqlStatements(QStringLiteral("INSERT INTO t VALUES ('a;b'); SELECT 1;"));
    QCOMPARE(literal.size(), 2);
    QVERIFY(literal.at(0).contains(QStringLiteral("'a;b'")));

    // A doubled quote is an escape, not a terminator.
    const QStringList escaped = splitSqlStatements(QStringLiteral("SELECT 'it''s; fine'; SELECT 2;"));
    QCOMPARE(escaped.size(), 2);
    QVERIFY(escaped.at(0).contains(QStringLiteral("it''s; fine")));

    // Quoted identifier.
    QCOMPARE(splitSqlStatements(QStringLiteral("CREATE TABLE \"a;b\" (x INT); SELECT 1;")).size(), 2);

    // Comments.
    QCOMPARE(splitSqlStatements(QStringLiteral("SELECT 1; -- a; comment\nSELECT 2;")).size(), 2);
    QCOMPARE(splitSqlStatements(QStringLiteral("SELECT 1; /* a; comment */ SELECT 2;")).size(), 2);

    // A trigger body's internal semicolons are not separators.
    const QStringList trigger = splitSqlStatements(QStringLiteral(
        "CREATE TRIGGER t AFTER INSERT ON x BEGIN UPDATE y SET a=1; UPDATE y SET b=2; END;"
        "SELECT 1;"));
    QCOMPARE(trigger.size(), 2);
    QVERIFY(trigger.at(0).startsWith(QStringLiteral("CREATE TRIGGER")));
    QVERIFY(trigger.at(0).contains(QStringLiteral("UPDATE y SET b=2")));

    // A column whose name merely contains "begin"/"end" must not move the
    // block depth.
    QCOMPARE(splitSqlStatements(QStringLiteral("CREATE TABLE t (legend INT, beginner INT); SELECT 1;")).size(),
             2);

    // A trailing statement with no terminator still counts.
    QCOMPARE(splitSqlStatements(QStringLiteral("SELECT 1")).size(), 1);
    QCOMPARE(splitSqlStatements(QStringLiteral("   \n  ")).size(), 0);
}

// Migrations used to run with no transaction: a statement failing part-way
// left the schema half-applied with user_version still on the PREVIOUS
// version, so the next launch replayed the migration from its first
// statement and failed on the objects the aborted run had already created.
// main() treats a failed open() as qFatal, so that bricked the profile
// permanently, with no recovery short of deleting the database by hand.
//
// Proven by making migration 2 fail: an object it creates is pre-created by
// hand so its CREATE collides. What must hold afterwards is that nothing
// from that migration survives, the version is unchanged, and removing the
// obstruction lets it apply -- i.e. the profile is recoverable.
void DatabaseTest::migrationIsAtomicSoAFailureLeavesNoHalfAppliedSchema()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("atomic.db"));

    // Plant a table that migration 001 also creates, but NOT its first one --
    // `folders` is the third CREATE, so the migration gets several statements
    // in before it collides. That is what makes this a rollback test rather
    // than a "nothing happened" test.
    {
        QSqlDatabase seed = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("seed"));
        seed.setDatabaseName(path);
        QVERIFY(seed.open());
        QSqlQuery q(seed);
        QVERIFY(q.exec(QStringLiteral("CREATE TABLE folders (bogus INT)")));
        seed.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("seed"));

    {
        Database db;
        QVERIFY2(!db.open(path), "migration 1 must fail against the planted conflict");
    }

    {
        QSqlDatabase check = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("check"));
        check.setDatabaseName(path);
        QVERIFY(check.open());

        QSqlQuery versionQuery(check);
        QVERIFY(versionQuery.exec(QStringLiteral("PRAGMA user_version")));
        QVERIFY(versionQuery.next());
        QCOMPARE(versionQuery.value(0).toInt(), 0);

        // The statements that DID succeed before the collision must have been
        // rolled back. Without the transaction, `emails` would be sitting
        // here -- and the next launch would then fail on IT instead,
        // permanently, because user_version never advanced.
        QSqlQuery tables(check);
        QVERIFY(tables.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table'")));
        QStringList names;
        while (tables.next())
            names.append(tables.value(0).toString());
        QVERIFY2(!names.contains(QStringLiteral("emails")),
                 "a half-applied migration is exactly what the transaction must prevent");
        QVERIFY(names.contains(QStringLiteral("folders"))); // the decoy, untouched

        QSqlQuery columns(check);
        QVERIFY(columns.exec(QStringLiteral("PRAGMA table_info(folders)")));
        int columnCount = 0;
        while (columns.next())
            ++columnCount;
        QCOMPARE(columnCount, 1);
        check.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("check"));

    // Remove the obstruction: the migration now applies cleanly all the way
    // to the current version. That recoverability is what the transaction
    // buys -- previously this profile could never be opened again.
    {
        QSqlDatabase fix = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("fix"));
        fix.setDatabaseName(path);
        QVERIFY(fix.open());
        QSqlQuery q(fix);
        QVERIFY(q.exec(QStringLiteral("DROP TABLE folders")));
        fix.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("fix"));

    Database db;
    QVERIFY2(db.open(path), "the profile must not be permanently wedged by a failed migration");
    QSqlQuery versionQuery(db.handle());
    QVERIFY(versionQuery.exec(QStringLiteral("PRAGMA user_version")));
    QVERIFY(versionQuery.next());
    QCOMPARE(versionQuery.value(0).toInt(), 5);
}

QTEST_GUILESS_MAIN(DatabaseTest)
#include "DatabaseTest.moc"
