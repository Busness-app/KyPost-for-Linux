#include "Database.h"

#include "MigrationSql.h"

#include <QAtomicInteger>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

namespace {

QAtomicInteger<quint64> g_connectionCounter{0};

} // namespace

QStringList splitSqlStatements(const QString& sql)
{
    QStringList statements;
    QString current;
    // Depth of unterminated BEGIN/CASE blocks -- both are closed by END in
    // SQLite. A CREATE TRIGGER body is BEGIN ... END; with internal
    // semicolons that are NOT statement separators, and the plain
    // sql.split(';') this replaced shattered any such migration into invalid
    // fragments the moment somebody wrote one.
    int blockDepth = 0;

    for (int i = 0; i < sql.size(); ++i) {
        const QChar c = sql.at(i);

        // -- line comment: skip to end of line.
        if (c == QLatin1Char('-') && i + 1 < sql.size() && sql.at(i + 1) == QLatin1Char('-')) {
            while (i < sql.size() && sql.at(i) != QLatin1Char('\n'))
                ++i;
            current += QLatin1Char('\n');
            continue;
        }

        // /* block comment */
        if (c == QLatin1Char('/') && i + 1 < sql.size() && sql.at(i + 1) == QLatin1Char('*')) {
            i += 2;
            while (i + 1 < sql.size()
                   && !(sql.at(i) == QLatin1Char('*') && sql.at(i + 1) == QLatin1Char('/')))
                ++i;
            ++i; // land on '/', loop's ++i steps past it
            // A comment is whitespace, not nothing: dropping it entirely
            // welded the tokens either side together, so `SELECT a/*x*/b`
            // became `SELECT ab`. The line-comment branch above already
            // emits its newline for the same reason.
            current += QLatin1Char(' ');
            continue;
        }

        // 'string literal', with '' as the embedded-quote escape, and
        // "quoted identifier" with "" likewise. Semicolons inside either are
        // ordinary characters.
        if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            const QChar quote = c;
            current += c;
            ++i;
            while (i < sql.size()) {
                if (sql.at(i) == quote) {
                    if (i + 1 < sql.size() && sql.at(i + 1) == quote) {
                        current += quote;
                        current += quote;
                        i += 2;
                        continue;
                    }
                    break;
                }
                current += sql.at(i);
                ++i;
            }
            if (i < sql.size())
                current += quote;
            continue;
        }

        if (c == QLatin1Char(';') && blockDepth == 0) {
            const QString trimmed = current.trimmed();
            if (!trimmed.isEmpty())
                statements.append(trimmed);
            current.clear();
            continue;
        }

        current += c;

        // Track the block keywords only on whole-word boundaries, so a
        // column named "beginner", "legend" or "staircase" can't move the
        // depth counter.
        //
        // CASE counts as an opener, and that is not decoration. SQLite
        // closes BOTH `BEGIN` and `CASE` with `END`, so counting only BEGIN
        // meant an ordinary `CASE WHEN ... END` inside a trigger body
        // decremented the depth to zero early; the next `;` -- still inside
        // the body -- was then treated as a statement separator and the
        // trigger was cut in half. The fragments fail to exec, open()
        // returns false, and main()'s qFatal turns that into an
        // unrecoverable crash on every launch, from writing perfectly
        // ordinary SQL in a migration.
        static const QRegularExpression kOpenerAtEnd(
            QStringLiteral("(?:^|[^A-Za-z0-9_])(?:BEGIN|CASE)$"), QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression kEndAtEnd(
            QStringLiteral("(?:^|[^A-Za-z0-9_])END$"), QRegularExpression::CaseInsensitiveOption);
        if (i + 1 >= sql.size() || !(sql.at(i + 1).isLetterOrNumber() || sql.at(i + 1) == QLatin1Char('_'))) {
            if (kOpenerAtEnd.match(current).hasMatch())
                ++blockDepth;
            else if (blockDepth > 0 && kEndAtEnd.match(current).hasMatch())
                --blockDepth;
        }
    }

    const QString trailing = current.trimmed();
    if (!trailing.isEmpty())
        statements.append(trailing);
    return statements;
}

Database::Database() = default;

Database::~Database()
{
    if (m_db.isOpen())
        m_db.close();
    m_db = QSqlDatabase();
    if (!m_connectionName.isEmpty())
        QSqlDatabase::removeDatabase(m_connectionName);
}

// Applies the key and then proves it took. Everything about this function is
// about not believing the happy path.
bool Database::applyEncryptionKey(const QByteArray& rawKey)
{
    // x'<hex>' is SQLCipher's raw-key form. The key never reaches the SQL
    // text in any form that needs escaping.
    QSqlQuery keyQuery(m_db);
    const bool keyExecuted =
        keyQuery.exec(QStringLiteral("PRAGMA key = \"x'%1'\"").arg(QString::fromLatin1(rawKey.toHex())));
    keyQuery.finish();
    if (!keyExecuted) {
        qWarning("Database: PRAGMA key failed: %s", qUtf8Printable(keyQuery.lastError().text()));
        return false;
    }

    // CHECK 1: is this actually SQLCipher?
    //
    // An ordinary SQLite answers `PRAGMA key` with success and does nothing,
    // so the exec() above proves nothing whatsoever. cipher_version is the
    // question stock SQLite cannot fake: it is not a pragma it knows, so it
    // returns no rows.
    QSqlQuery versionQuery(m_db);
    const bool haveVersion = versionQuery.exec(QStringLiteral("PRAGMA cipher_version"))
        && versionQuery.next() && !versionQuery.value(0).toString().trimmed().isEmpty();
    versionQuery.finish();
    if (!haveVersion) {
        qCritical("Database: refusing to open -- PRAGMA key was accepted but this is not SQLCipher, "
                   "so the database would be written in plaintext");
        return false;
    }

    // CHECK 2: is the key the RIGHT one?
    //
    // SQLCipher does not verify the key when it is set; it fails at the first
    // page it has to decrypt. Without this read, open() would succeed under a
    // wrong key and the failure would surface later as an unrelated-looking
    // query error, somewhere with no idea what to do about it.
    QSqlQuery probeQuery(m_db);
    const bool readable = probeQuery.exec(QStringLiteral("SELECT count(*) FROM sqlite_master")) && probeQuery.next();
    probeQuery.finish();
    if (!readable) {
        qWarning("Database: the encryption key does not open this database");
        return false;
    }

    return true;
}

#ifdef KYPOST_HAVE_SQLCIPHER
// Declared here rather than by including sqlite3.h: this file needs exactly
// one thing out of the library, and pulling the whole header in would put a
// SQLCipher include path in front of every consumer of Database.h.
extern "C" const char* sqlite3_libversion();
#endif

bool Database::encryptionAvailable()
{
#ifdef KYPOST_HAVE_SQLCIPHER
    // A real call, and it is load-bearing rather than decorative.
    //
    // Linking the library is not enough to make it LOAD. Debian and Ubuntu
    // pass -Wl,--as-needed by default, and nothing else in this codebase
    // references a sqlite3 symbol -- every SQL call goes through Qt's driver
    // plugin, not through us -- so the linker drops the DT_NEEDED entry as
    // unused. The RUNPATH survives, pointing at a library that is then never
    // mapped; Qt's plugin resolves its own libsqlite3.so.0 from the system,
    // and `PRAGMA key` becomes the silent no-op this class exists to catch.
    //
    // That is not hypothetical: it is what CI did on the first run of this
    // feature while the same build passed on a distribution that does not
    // default to --as-needed. Reproduced by adding the flag locally, then
    // fixed here. One honest reference to the library keeps it.
    return sqlite3_libversion() != nullptr;
#else
    return false;
#endif
}

bool Database::open(const QString& path, const QByteArray& rawKey)
{
    if (!rawKey.isEmpty()) {
        if (!encryptionAvailable()) {
            // Refused, not downgraded. Opening plaintext because this build
            // has no SQLCipher would produce exactly the database the caller
            // asked not to have, and nothing above would know.
            qWarning("Database: an encrypted database was requested, but this build has no SQLCipher");
            return false;
        }
        if (rawKey.size() != kRawKeyBytes) {
            // A wrong-length key is not passed on. SQLCipher treats anything
            // that is not exactly 32 raw bytes in x'' form as a PASSPHRASE
            // and runs PBKDF2 over it, so a truncated key would still open a
            // database -- a different one, silently, under a different key.
            qWarning("Database: refusing a %lld-byte key; exactly %d bytes are required",
                      static_cast<long long>(rawKey.size()), kRawKeyBytes);
            return false;
        }
    }

    m_connectionName = QStringLiteral("kypost_db_%1").arg(g_connectionCounter.fetchAndAddRelaxed(1));
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(path);
    if (!m_db.open())
        return false;

    if (!rawKey.isEmpty() && !applyEncryptionKey(rawKey)) {
        m_db.close();
        return false;
    }

    QSqlQuery foreignKeysQuery(m_db);
    foreignKeysQuery.exec(QStringLiteral("PRAGMA foreign_keys = ON"));

    QSqlQuery versionQuery(m_db);
    if (!versionQuery.exec(QStringLiteral("PRAGMA user_version")) || !versionQuery.next())
        return false;
    const int version = versionQuery.value(0).toInt();
    // Released before the migration loop, not at end of scope. A PRAGMA
    // leaves an open read cursor over the schema, and SQLite answers
    // SQLITE_LOCKED to any DROP/ALTER on the same connection while one is
    // live -- so a table-rebuild migration failed on its DROP with "database
    // table is locked" while this query sat unfinished for the whole
    // function.
    versionQuery.finish();
    foreignKeysQuery.finish();
    // Bounded on BOTH sides before it is used as an array index and a loop
    // bound. Negative values indexed kKyPostMigrationSql before its start and
    // CALLED whatever function pointer sat there -- a torn write or a
    // rolled-back database turned into an unrecoverable startup crash, on
    // every launch, with no in-app recovery. INT_MAX was worse in a quieter
    // way: `version + 1` is signed-overflow UB, which let the optimizer elide
    // the loop entirely and open the database with no migration applied.
    if (version < 0 || version > kKyPostMigrationCount) {
        qWarning("Database: refusing to open a database with unsupported schema version %d", version);
        return false;
    }

    // One transaction per migration, covering both its statements AND the
    // user_version bump. SQLite has transactional DDL, so this is real: a
    // migration either applies whole or not at all.
    //
    // Without it, a statement failing part-way (disk full, a column an
    // earlier aborted attempt already added) left the schema half-applied
    // with user_version still pointing at the PREVIOUS version. The next
    // launch replayed the same migration from its first statement, which
    // then failed on the objects the aborted run had already created --
    // permanently, so main()'s qFatal on a failed open() bricked the
    // profile with no recovery short of deleting the database by hand.
    for (int nextVersion = version + 1; nextVersion <= kKyPostMigrationCount; ++nextVersion) {
        const QString sql = kKyPostMigrationSql[nextVersion - 1]();
        const QStringList statements = splitSqlStatements(sql);

        if (!m_db.transaction())
            return false;

        for (const QString& statement : statements) {
            QSqlQuery schemaQuery(m_db);
            if (!schemaQuery.exec(statement)) {
                // Say WHICH statement and WHY. main() turns a failed open()
                // into qFatal, so this used to be a hard abort whose only
                // clue was the exit code -- and the whole point of the
                // rollback above is that the profile is still recoverable,
                // which nobody can act on without knowing what failed.
                qWarning("Database: migration %d failed: %s\n  statement: %s", nextVersion,
                          qUtf8Printable(schemaQuery.lastError().text()), qUtf8Printable(statement));
                m_db.rollback();
                return false;
            }
            // QSqlQuery holds the sqlite3_stmt open after exec(), and a
            // statement that read a table keeps a cursor on it: SQLite then
            // refuses to DROP or ALTER that table ("database table is
            // locked"). Relying on the QSqlQuery destructor is not enough --
            // it runs at the end of this iteration, which is already too late
            // for a migration whose next statement drops what the previous
            // one read. Migration 006 (INSERT ... SELECT FROM emails; DROP
            // TABLE emails) is the first to do that; finishing here means the
            // next one doesn't have to rediscover it.
            schemaQuery.finish();
        }

        // Inside the same transaction as the statements above: a bumped
        // version with unapplied statements is the same brick, mirrored.
        QSqlQuery setVersionQuery(m_db);
        if (!setVersionQuery.exec(QStringLiteral("PRAGMA user_version = %1").arg(nextVersion))) {
            m_db.rollback();
            return false;
        }

        if (!m_db.commit()) {
            m_db.rollback();
            return false;
        }
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
