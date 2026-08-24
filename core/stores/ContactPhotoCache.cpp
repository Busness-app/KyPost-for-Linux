#include "stores/ContactPhotoCache.h"

#include "security/PrivatePath.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

QString ContactPhotoCache::directoryFor(const QString& dataDir, bool persistent)
{
    return persistent ? dataDir + QStringLiteral("/contact-photos") : QString();
}

// Contact photos are pictures of the people the user knows, so this cache
// gets the same owner-only treatment as everything else under the data
// directory -- and, unlike before, REFUSES TO RUN when it does not get it.
// Warning about a cache directory other local users can read, and then
// filling it with recognisable faces anyway, protects nobody: the log line
// is not read until after the exposure. A disabled cache costs a re-fetch
// per photo; that is the whole cost of failing closed here.
ContactPhotoCache::ContactPhotoCache(const QString& cacheDir)
    : m_dir(cacheDir)
{
    if (cacheDir.isEmpty())
        return; // deliberately disabled by the caller -- see the header

    switch (PrivatePath::ensureDirectory(cacheDir)) {
    case PrivatePath::Status::Ready:
        m_available = true;
        break;
    case PrivatePath::Status::NotCreated:
        qWarning("ContactPhotoCache: could not create the photo cache directory; photos will not be cached");
        break;
    case PrivatePath::Status::NotPrivate:
        qWarning("ContactPhotoCache: the photo cache directory is readable by other users on this "
                 "machine; photos will not be cached");
        break;
    }
}

QString ContactPhotoCache::fileNameFor(const QString& photoRef) const
{
    const QByteArray hash = QCryptographicHash::hash(photoRef.toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(hash.toHex());
}

QString ContactPhotoCache::cachedPathFor(const QString& photoRef) const
{
    if (!m_available || photoRef.isEmpty())
        return QString();

    const QString path = m_dir.filePath(fileNameFor(photoRef));
    return QFileInfo::exists(path) ? path : QString();
}

QString ContactPhotoCache::store(const QString& photoRef, const QByteArray& bytes) const
{
    if (!m_available || photoRef.isEmpty() || bytes.isEmpty())
        return QString();

    // QSaveFile writes to a temporary beside the target and renames on
    // commit(), which also flushes: a reader either sees the previous file or
    // the whole new one, never the half of it that reached the disk before a
    // crash. The old code wrote in place and deleted a short write afterwards
    // -- a window in which cachedPathFor() would hand out a truncated image.
    const QString path = m_dir.filePath(fileNameFor(photoRef));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return QString();
    if (file.write(bytes) != bytes.size() || !file.commit())
        return QString(); // commit() failing leaves the temporary, not a corrupt entry

    // Tightened and CHECKED after the rename, which is the only point at
    // which the answer is stable: commit() applies its own permissions to the
    // final name, and a chmod reports success while changing nothing on a
    // filesystem that has no permission bits.
    if (!PrivatePath::ensureFile(path)) {
        QFile::remove(path);
        qWarning("ContactPhotoCache: a cached photo could not be kept private and was discarded");
        return QString();
    }

    evictToBudget(path);
    return path;
}

void ContactPhotoCache::evictToBudget(const QString& keepPath) const
{
    // QDir::Time is newest-first, so Reversed is oldest-first.
    const QFileInfoList entries = m_dir.entryInfoList(QDir::Files, QDir::Time | QDir::Reversed);
    qint64 total = 0;
    for (const QFileInfo& entry : entries)
        total += entry.size();

    // keepPath is the file store() just wrote and is about to hand back, so
    // it is skipped by NAME. This used to skip the last list entry instead,
    // on the assumption that oldest-first sorting put the new file there --
    // which holds only while every mtime is distinct. Filesystem timestamp
    // granularity is coarse enough (and a cache fill fast enough) for several
    // entries to share one mtime, at which point their relative order is
    // whatever the sort happened to do, and a budget smaller than two photos
    // could delete the path the caller was still being handed.
    for (const QFileInfo& entry : entries) {
        if (total <= kMaxCacheBytes)
            break;
        const QString candidate = entry.absoluteFilePath();
        if (candidate == keepPath)
            continue;
        const qint64 size = entry.size();
        if (QFile::remove(candidate))
            total -= size;
    }
}
