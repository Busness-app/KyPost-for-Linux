#include "stores/ContactPhotoCache.h"

#include "security/PrivatePath.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

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

    return path;
}
