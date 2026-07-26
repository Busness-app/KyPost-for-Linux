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

} // namespace SecurityWipe
