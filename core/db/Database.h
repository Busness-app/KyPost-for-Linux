#pragma once

#include <QSqlDatabase>
#include <QString>

// Opens a SQLite connection (":memory:" or a real file path) and applies
// core/db/migrations/*.sql in order, idempotently, guarded by
// `PRAGMA user_version`. Each Database owns a uniquely-named QSqlDatabase
// connection (Qt requires unique connection names) and removes it on
// destruction.
class Database
{
public:
    Database();
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool open(const QString& path);
    QSqlDatabase& handle();

    // Deletes every row from every user table, leaving the schema and
    // `PRAGMA user_version` intact so the app keeps working afterwards.
    //
    // Deliberately not "delete the file": the connection is open and every
    // DAO holds a bound QSqlDatabase& for its whole lifetime (see the DAO
    // constructors), so pulling the file out from under them would mean
    // rebuilding the entire composition graph. Emptying the tables achieves
    // the same thing an attacker would care about -- no cached mail,
    // contacts, or folders remain -- without that.
    //
    // sqlite_% tables are skipped: they are SQLite's own bookkeeping and
    // deleting from them errors.
    //
    // Sets `PRAGMA secure_delete = ON` and then VACUUMs, so deleted content
    // is overwritten rather than left recoverable in SQLite's freelist.
    // secure_delete is already the default in some SQLite builds but not
    // all, so it is set explicitly rather than assumed.
    bool wipeAllTables();

private:
    QSqlDatabase m_db;
    QString m_connectionName;
};
