#pragma once

#include <QByteArray>
#include <QDir>
#include <QString>

// Plain QDir-based disk cache for contact photo bytes, keyed by
// Contact::photoRef. photoRef is described by the source doc as an opaque,
// content-hashed reference generated server-side (see Contact.h's own
// comment on the field) -- so it's immutable once fetched, and this cache
// deliberately implements no invalidation/TTL/eviction logic (Global
// Constraint for this feature: no invalidation needed, content-hashed).
//
// photoRef is still treated as untrusted input for filename-construction
// purposes -- defense in depth against unexpected characters (path
// separators, "..", etc.) even though the doc says it's server-generated
// and presumably safe -- so the on-disk filename is a SHA-256 hash of
// photoRef rather than photoRef used verbatim as a path component.
class ContactPhotoCache
{
public:
    // cacheDir is created if it doesn't already exist, and made owner-only.
    // If it cannot be BOTH, the cache disables itself: every store() and
    // cachedPathFor() call then returns an empty string, so photos are
    // re-fetched rather than written somewhere other users can read them.
    explicit ContactPhotoCache(const QString& cacheDir);

    // Absolute path to the cached file for photoRef, or an empty string if
    // nothing is cached yet (or photoRef is empty).
    QString cachedPathFor(const QString& photoRef) const;

    // Writes bytes to disk under photoRef's cache file, overwriting any
    // existing file at that path -- safe since photoRef is content-hashed,
    // so a "different bytes under the same key" collision would indicate a
    // corrupt/misbehaving server response, not a legitimate content update.
    // Returns the absolute path written, or an empty string if photoRef/
    // bytes is empty or the write fails.
    QString store(const QString& photoRef, const QByteArray& bytes) const;

private:
    QString fileNameFor(const QString& photoRef) const;

    QDir m_dir;
    bool m_available = false;
};
