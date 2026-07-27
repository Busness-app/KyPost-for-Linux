#include "stores/SecureStoreFile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace {

// Keys are opaque identifiers for a file directly inside the store's
// directory (see core/stores/SecureStore.h for the fixed set this project
// actually uses: `sub`, `hash`, `deviceId`, pairing
// credential keys — none of which ever need path separators). QDir::filePath
// does not sanitize ".." components and returns an absolute second argument
// verbatim, so any key containing a separator, referring to the current/
// parent directory, or looking absolute is rejected outright rather than
// sanitized.
bool isValidKey(const QString& key)
{
    if (key.isEmpty())
        return false;
    if (key.contains(QLatin1Char('/')) || key.contains(QLatin1Char('\\')))
        return false;
    if (key == QLatin1String(".") || key == QLatin1String(".."))
        return false;
    if (QDir::isAbsolutePath(key))
        return false;
    return true;
}

}

SecureStoreFile::SecureStoreFile(const QString& directoryPath)
    : m_directoryPath(directoryPath)
{
}

QString SecureStoreFile::filePathFor(const QString& key) const
{
    return QDir(m_directoryPath).filePath(key);
}

bool SecureStoreFile::set(const QString& key, const QString& value)
{
    if (!isValidKey(key))
        return false;

    QFile file(filePathFor(key));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;

    // Tighten permissions immediately after the file is created (open() with
    // Truncate creates it) and before any content is written, so the secret
    // is never written under the process umask's looser permissions. Doing
    // this after write() (the original bug) leaves a real window where the
    // plaintext secret sits on disk at e.g. 0644.
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.close();
        file.remove();
        return false;
    }

    // Full write or nothing. `>= 0` used to be the test, which accepts a
    // PARTIAL write (QFile::write returns the byte count, and only -1 means
    // failure) -- a full disk then produced a truncated secret on disk and a
    // cheerful `true` to the caller. flush() is checked too, because close()
    // returns void and would swallow the error from the buffered tail.
    const QByteArray bytes = value.toUtf8();
    const qint64 written = file.write(bytes);
    const bool flushed = file.flush();
    file.close();
    if (written == bytes.size() && flushed)
        return true;

    // Leave nothing half-written: a truncated secret that still parses is
    // worse than an absent one, because callers treat "present" as usable.
    file.remove();
    return false;
}

std::optional<QString> SecureStoreFile::get(const QString& key) const
{
    if (!isValidKey(key))
        return std::nullopt;

    QFile file(filePathFor(key));
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;

    return QString::fromUtf8(file.readAll());
}

bool SecureStoreFile::remove(const QString& key)
{
    if (!isValidKey(key))
        return false;

    QFile file(filePathFor(key));
    if (!file.exists())
        return true;
    return file.remove();
}

bool SecureStoreFile::contains(const QString& key) const
{
    if (!isValidKey(key))
        return false;

    return QFileInfo::exists(filePathFor(key));
}
