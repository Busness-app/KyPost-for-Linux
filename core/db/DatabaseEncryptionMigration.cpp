#include "db/DatabaseEncryptionMigration.h"

#include "db/Database.h"
#include "db/ProfileDatabase.h"
#include "db/SecurityWipe.h"

#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace {

// Row counts per user table, which is what "the copy matches" is checked
// against. Returns nullopt when the database could not be read at all --
// distinct from "an empty database", which is a legitimate answer.
std::optional<QMap<QString, int>> tableCounts(QSqlDatabase& db)
{
    QSqlQuery tables(db);
    if (!tables.exec(QStringLiteral(
            "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'"))) {
        return std::nullopt;
    }

    QStringList names;
    while (tables.next())
        names.append(tables.value(0).toString());
    tables.finish();

    QMap<QString, int> counts;
    for (const QString& name : names) {
        QSqlQuery count(db);
        // Table names cannot be bound. These come from sqlite_master, not
        // from user input, and are quoted defensively.
        if (!count.exec(QStringLiteral("SELECT count(*) FROM \"%1\"").arg(name)) || !count.next())
            return std::nullopt;
        counts.insert(name, count.value(0).toInt());
    }
    return counts;
}

int userVersion(QSqlDatabase& db)
{
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("PRAGMA user_version")) || !query.next())
        return -1;
    return query.value(0).toInt();
}

} // namespace

DatabaseEncryptionMigration::DatabaseEncryptionMigration(const QString& databasePath)
    : m_databasePath(databasePath)
    , m_markerPath(databasePath + QStringLiteral(".encrypting"))
    , m_workingCopyPath(databasePath + QStringLiteral(".encrypted-new"))
    , m_supersededPath(databasePath + QStringLiteral(".plaintext-old"))
{
}

bool DatabaseEncryptionMigration::interrupted() const
{
    return QFile::exists(m_markerPath);
}

bool DatabaseEncryptionMigration::arm()
{
    QFile marker(m_markerPath);
    if (!marker.open(QIODevice::WriteOnly))
        return false;
    marker.write("converting this profile's database to an encrypted one\n");
    marker.close();
    return true;
}

bool DatabaseEncryptionMigration::disarm()
{
    if (!QFile::exists(m_markerPath))
        return true;
    return QFile::remove(m_markerPath);
}

void DatabaseEncryptionMigration::reconcile()
{
    // The swap died between the two renames: the original was moved aside and
    // the copy never took its place. Put the original back -- it is the only
    // complete database on this disk.
    if (!QFileInfo::exists(m_databasePath) && QFileInfo::exists(m_supersededPath)) {
        QFile::rename(m_supersededPath, m_databasePath);
        SecurityWipe::removeDatabaseFiles(m_workingCopyPath);
        return;
    }

    // The swap completed but the secure delete did not. There is a verified
    // encrypted database in place, so the plaintext one is now purely an
    // exposure and goes.
    if (QFileInfo::exists(m_supersededPath) && databaseFileIsEncrypted(m_databasePath)) {
        SecurityWipe::removeDatabaseFiles(m_supersededPath);
        SecurityWipe::removeDatabaseFiles(m_workingCopyPath);
        return;
    }

    // Anything else: a half-written copy from a run that died during the
    // export. It is worthless -- there is no state in which it is preferred
    // to the original -- and leaving it would make the next attempt's ATTACH
    // land on a partial file.
    SecurityWipe::removeDatabaseFiles(m_workingCopyPath);
}

bool DatabaseEncryptionMigration::exportEncrypted(const QByteArray& key)
{
    Database source;
    if (!source.open(m_databasePath)) {
        qWarning("DatabaseEncryptionMigration: could not open the plaintext database");
        return false;
    }

    const int sourceVersion = userVersion(source.handle());
    if (sourceVersion < 0)
        return false;

    QSqlQuery attach(source.handle());
    if (!attach.exec(QStringLiteral("ATTACH DATABASE '%1' AS encrypted KEY \"x'%2'\"")
                          .arg(m_workingCopyPath, QString::fromLatin1(key.toHex())))) {
        qWarning("DatabaseEncryptionMigration: ATTACH failed: %s",
                  qUtf8Printable(attach.lastError().text()));
        return false;
    }
    attach.finish();

    QSqlQuery exportQuery(source.handle());
    const bool exported = exportQuery.exec(QStringLiteral("SELECT sqlcipher_export('encrypted')"));
    exportQuery.finish();
    if (!exported) {
        qWarning("DatabaseEncryptionMigration: sqlcipher_export failed: %s",
                  qUtf8Printable(exportQuery.lastError().text()));
        return false;
    }

    // sqlcipher_export copies the CONTENTS, not `PRAGMA user_version`. Left
    // at 0, the copy looks to Database::open() like a brand-new database and
    // every migration is replayed over rows that already have the changes --
    // which fails, on a database that is otherwise perfectly good.
    QSqlQuery version(source.handle());
    const bool versionCopied =
        version.exec(QStringLiteral("PRAGMA encrypted.user_version = %1").arg(sourceVersion));
    version.finish();
    if (!versionCopied)
        return false;

    QSqlQuery detach(source.handle());
    const bool detached = detach.exec(QStringLiteral("DETACH DATABASE encrypted"));
    detach.finish();
    return detached;
}

bool DatabaseEncryptionMigration::verifyCopyMatchesOriginal(const QByteArray& key) const
{
    Database original;
    if (!original.open(m_databasePath))
        return false;
    const std::optional<QMap<QString, int>> before = tableCounts(original.handle());
    const int beforeVersion = userVersion(original.handle());
    if (!before.has_value() || beforeVersion < 0)
        return false;

    Database copy;
    if (!copy.open(m_workingCopyPath, key)) {
        qWarning("DatabaseEncryptionMigration: the encrypted copy does not open with the key it was written with");
        return false;
    }
    const std::optional<QMap<QString, int>> after = tableCounts(copy.handle());
    const int afterVersion = userVersion(copy.handle());
    if (!after.has_value())
        return false;

    if (beforeVersion != afterVersion) {
        qWarning("DatabaseEncryptionMigration: schema version %d became %d in the copy", beforeVersion,
                  afterVersion);
        return false;
    }
    if (*before != *after) {
        qWarning("DatabaseEncryptionMigration: the encrypted copy does not hold the same rows");
        return false;
    }
    return true;
}

bool DatabaseEncryptionMigration::swapInEncrypted(const QByteArray& key)
{
    // Renamed aside rather than deleted. Between these two renames is the
    // only moment where the profile has no database at its usual path, and
    // reconcile() puts it back if we die inside it.
    if (!QFile::rename(m_databasePath, m_supersededPath)) {
        qWarning("DatabaseEncryptionMigration: could not move the plaintext database aside");
        return false;
    }
    if (!QFile::rename(m_workingCopyPath, m_databasePath)) {
        qWarning("DatabaseEncryptionMigration: could not put the encrypted database in place");
        QFile::rename(m_supersededPath, m_databasePath); // and back it goes
        return false;
    }

    // Proven at its final path, under its final name, before the only other
    // copy of this mail is destroyed.
    {
        Database finalCheck;
        if (!finalCheck.open(m_databasePath, key)) {
            qWarning("DatabaseEncryptionMigration: the encrypted database does not open in place; reverting");
            QFile::rename(m_databasePath, m_workingCopyPath);
            QFile::rename(m_supersededPath, m_databasePath);
            return false;
        }
    }

    // Last, and only now. secure_delete/VACUUM cannot help here -- this is a
    // whole file going -- so SecurityWipe removes it and its sidecars, which
    // under WAL can each still hold committed page data.
    if (!SecurityWipe::removeDatabaseFiles(m_supersededPath)) {
        // The encrypted database is in place and working; the plaintext copy
        // beside it is an exposure the next launch's reconcile() will retry.
        qCritical("DatabaseEncryptionMigration: the plaintext database could not be deleted after "
                   "conversion; a readable copy of this mail is still on disk at %s",
                   qUtf8Printable(m_supersededPath));
        return true;
    }
    return true;
}

DatabaseEncryptionMigration::Status DatabaseEncryptionMigration::run(const QByteArray& key)
{
    reconcile();

    if (key.size() != Database::kRawKeyBytes)
        return Status::Failed;
    if (!Database::encryptionAvailable())
        return Status::Failed;

    // Nothing to convert: no database yet, or one that is already encrypted.
    if (!QFileInfo::exists(m_databasePath) || QFileInfo(m_databasePath).size() == 0) {
        disarm();
        return Status::NotNeeded;
    }
    if (databaseFileIsEncrypted(m_databasePath)) {
        disarm();
        return Status::NotNeeded;
    }

    if (!arm()) {
        // Refused rather than attempted. Without the marker, a conversion cut
        // short leaves a half-swapped profile that the next launch has no way
        // to recognise -- and this is the one operation in the app that can
        // lose a user's whole mail store.
        qCritical("DatabaseEncryptionMigration: could not record that a conversion started; not converting");
        return Status::Failed;
    }

    if (!exportEncrypted(key) || !verifyCopyMatchesOriginal(key)) {
        SecurityWipe::removeDatabaseFiles(m_workingCopyPath);
        disarm();
        return Status::Failed; // the plaintext database is untouched
    }

    if (!swapInEncrypted(key)) {
        SecurityWipe::removeDatabaseFiles(m_workingCopyPath);
        disarm();
        return Status::Failed;
    }

    disarm();
    return Status::Migrated;
}
