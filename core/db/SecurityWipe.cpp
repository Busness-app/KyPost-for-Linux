#include "db/SecurityWipe.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace SecurityWipe {

bool removeDatabaseFiles(const QString& dbPath)
{
    if (dbPath.isEmpty() || dbPath == QStringLiteral(":memory:"))
        return true; // nothing on disk to remove

    // All four candidates, unconditionally. See the header for why the
    // sidecars are not optional.
    const QStringList candidates = {
        dbPath,
        dbPath + QStringLiteral("-journal"),
        dbPath + QStringLiteral("-wal"),
        dbPath + QStringLiteral("-shm"),
    };

    bool ok = true;
    for (const QString& path : candidates) {
        if (!QFile::exists(path))
            continue; // absent is fine, not a failure
        if (!QFile::remove(path))
            ok = false;
    }
    return ok;
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
