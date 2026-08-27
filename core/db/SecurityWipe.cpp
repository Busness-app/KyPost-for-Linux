#include "db/SecurityWipe.h"

#include "db/DatabaseEncryptionMigration.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace {

// Every path this one database stands for: the live file, and the three the
// encryption migration puts beside it under names only it knows -- its two
// copies, and the marker saying the profile is mid-conversion. Taken from
// that class rather than restated here, so a suffix cannot be renamed out
// from under the wipe.
QStringList migrationCopies(const QString& dbPath)
{
    QStringList databases;
    for (const QLatin1StringView suffix : { DatabaseEncryptionMigration::kMarkerSuffix,
                                             DatabaseEncryptionMigration::kWorkingCopySuffix,
                                             DatabaseEncryptionMigration::kSupersededSuffix })
        databases.append(dbPath + suffix);
    return databases;
}

// All four candidates, unconditionally: the migration's copies are databases
// in their own right and carry their own sidecars. See the header for why
// neither set is optional.
bool removeWithSidecars(const QString& database)
{
    bool ok = true;
    for (const QString& path : { database, database + QStringLiteral("-journal"),
                                  database + QStringLiteral("-wal"),
                                  database + QStringLiteral("-shm") }) {
        if (!QFile::exists(path))
            continue; // absent is fine, not a failure
        if (!QFile::remove(path))
            ok = false;
    }
    return ok;
}

} // namespace

namespace SecurityWipe {

bool removeMigrationCopies(const QString& dbPath)
{
    if (dbPath.isEmpty() || dbPath == QStringLiteral(":memory:"))
        return true; // nothing on disk to remove

    bool ok = true;
    for (const QString& copy : migrationCopies(dbPath))
        ok = removeWithSidecars(copy) && ok;
    return ok;
}

bool removeDatabaseFiles(const QString& dbPath)
{
    if (dbPath.isEmpty() || dbPath == QStringLiteral(":memory:"))
        return true; // nothing on disk to remove

    // Both calls made, then aggregated: a live file that will not unlink must
    // not leave the plaintext copy behind as well.
    const bool liveRemoved = removeWithSidecars(dbPath);
    return removeMigrationCopies(dbPath) && liveRemoved;
}

bool clearCacheDirectory(const QString& cacheDir)
{
    if (cacheDir.isEmpty())
        return true;

    QDir dir(cacheDir);
    if (!dir.exists())
        return true;

    bool ok = true;
    const QFileInfoList entries =
        dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo& entry : entries) {
        if (entry.isDir()) {
            QDir sub(entry.absoluteFilePath());
            if (!sub.removeRecursively())
                ok = false;
        } else if (!QFile::remove(entry.absoluteFilePath())) {
            ok = false;
        }
    }
    return ok;
}

bool eraseOnDiskProfile(const QString& databasePath, const QStringList& legacyDatabasePaths,
                        const QString& cacheDir)
{
    // Every call made, then aggregated -- deliberately not short-circuited.
    // Stopping at the first failure would leave the rest of the profile on
    // disk because one file could not be removed.
    bool erased = removeDatabaseFiles(databasePath);
    for (const QString& legacyPath : legacyDatabasePaths)
        erased = removeDatabaseFiles(legacyPath) && erased;
    return clearCacheDirectory(cacheDir) && erased;
}

} // namespace SecurityWipe
