#include "Database.h"

#include "MigrationSql.h"

#include <QAtomicInteger>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

namespace {

QAtomicInteger<quint64> g_connectionCounter{0};

} // namespace

Database::Database() = default;

Database::~Database()
{
    if (m_db.isOpen())
        m_db.close();
    m_db = QSqlDatabase();
    if (!m_connectionName.isEmpty())
        QSqlDatabase::removeDatabase(m_connectionName);
}

bool Database::open(const QString& path)
{
    m_connectionName = QStringLiteral("kypost_db_%1").arg(g_connectionCounter.fetchAndAddRelaxed(1));
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(path);
    if (!m_db.open())
        return false;

    QSqlQuery foreignKeysQuery(m_db);
    foreignKeysQuery.exec(QStringLiteral("PRAGMA foreign_keys = ON"));

    QSqlQuery versionQuery(m_db);
    if (!versionQuery.exec(QStringLiteral("PRAGMA user_version")) || !versionQuery.next())
        return false;
    const int version = versionQuery.value(0).toInt();

    for (int nextVersion = version + 1; nextVersion <= kKyPostMigrationCount; ++nextVersion) {
        const QString sql = kKyPostMigrationSql[nextVersion - 1]();
        const QStringList statements = sql.split(QLatin1Char(';'), Qt::SkipEmptyParts);
        for (const QString& rawStatement : statements) {
            const QString statement = rawStatement.trimmed();
            if (statement.isEmpty())
                continue;
            QSqlQuery schemaQuery(m_db);
            if (!schemaQuery.exec(statement))
                return false;
        }
        QSqlQuery setVersionQuery(m_db);
        if (!setVersionQuery.exec(QStringLiteral("PRAGMA user_version = %1").arg(nextVersion)))
            return false;
    }

    return true;
}

bool Database::wipeAllTables()
{
    if (!m_db.isOpen())
        return false;

    QSqlQuery tablesQuery(m_db);
    if (!tablesQuery.exec(QStringLiteral(
            "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'"))) {
        return false;
    }

    QStringList tables;
    while (tablesQuery.next())
        tables.append(tablesQuery.value(0).toString());

    // secure_delete makes SQLite overwrite deleted content with zeroes
    // rather than just unlinking it into the freelist. It is ON by default
    // in this build (verified), but that is a compile-time default
    // (SQLITE_SECURE_DELETE) that other builds -- notably a vanilla
    // amalgamation -- leave OFF. Set it explicitly so the guarantee does not
    // silently depend on whose SQLite we happen to link.
    {
        QSqlQuery pragmaQuery(m_db);
        pragmaQuery.exec(QStringLiteral("PRAGMA secure_delete = ON"));
    }

    if (!m_db.transaction())
        return false;

    for (const QString& table : tables) {
        QSqlQuery deleteQuery(m_db);
        // Table names cannot be bound as parameters. These come from
        // sqlite_master, not from user input, and are quoted defensively.
        if (!deleteQuery.exec(QStringLiteral("DELETE FROM \"%1\"").arg(table))) {
            m_db.rollback();
            return false;
        }
    }

    if (!m_db.commit())
        return false;

    // Belt and braces on top of secure_delete above: VACUUM rebuilds the
    // file from live content only, which also reclaims freelist pages that
    // predate this call (e.g. written before secure_delete was set). Must
    // run outside a transaction, hence after the commit.
    //
    // Measured, not assumed: with secure_delete ON, a plain DELETE already
    // removes the content from the file, so VACUUM is defence in depth here
    // rather than the mechanism doing the work.
    QSqlQuery vacuumQuery(m_db);
    return vacuumQuery.exec(QStringLiteral("VACUUM"));
}

QSqlDatabase& Database::handle()
{
    return m_db;
}
