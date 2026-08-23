#include "stores/ContactPhotoCache.h"

#include "security/PrivatePath.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QFile>
#include <QFileInfo>

// Contact photos are pictures of the people the user knows, so this cache
// gets the same owner-only treatment as everything else under the data
// directory -- and, unlike before, says so when it does not get it. The
// unchecked mkpath() this replaces could not tell a usable cache from a
// directory that was never created: store() failed on every call and looked
// exactly like a cache miss.
ContactPhotoCache::ContactPhotoCache(const QString& cacheDir)
    : m_dir(cacheDir)
{
    switch (PrivatePath::ensureDirectory(cacheDir)) {
    case PrivatePath::Status::Ready:
        break;
    case PrivatePath::Status::NotCreated:
        qWarning("ContactPhotoCache: could not create the photo cache directory; photos will not be cached");
        break;
    case PrivatePath::Status::NotPrivate:
        qWarning("ContactPhotoCache: the photo cache directory is readable by other users on this machine");
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
    if (photoRef.isEmpty())
        return QString();

    const QString path = m_dir.filePath(fileNameFor(photoRef));
    return QFileInfo::exists(path) ? path : QString();
}

QString ContactPhotoCache::store(const QString& photoRef, const QByteArray& bytes) const
{
    if (photoRef.isEmpty() || bytes.isEmpty())
        return QString();

    const QString path = m_dir.filePath(fileNameFor(photoRef));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return QString();

    const qint64 written = file.write(bytes);
    file.close();
    if (written != bytes.size()) {
        QFile::remove(path); // don't leave a truncated/corrupt cache entry behind
        return QString();
    }

    return path;
}
