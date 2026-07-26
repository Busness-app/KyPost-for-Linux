#include "db/Database.h"

#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

class DatabaseTest : public QObject
{
    Q_OBJECT

private slots:
    void opensInMemoryAndAppliesSchema();
    void openIsIdempotentOnRealFile();
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

QTEST_GUILESS_MAIN(DatabaseTest)
#include "DatabaseTest.moc"
