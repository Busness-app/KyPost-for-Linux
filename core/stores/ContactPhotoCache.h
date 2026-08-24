#pragma once

#include <QByteArray>
#include <QDir>
#include <QString>

// Plain QDir-based disk cache for contact photo bytes, keyed by
// Contact::photoRef. photoRef is described by the source doc as an opaque,
// content-hashed reference generated server-side (see Contact.h's own
// comment on the field) -- so it's immutable once fetched, and this cache
// deliberately implements no invalidation or TTL (Global Constraint for this
// feature: no invalidation needed, content-hashed). It DOES evict, which is
// a different question: see kMaxCacheBytes.
//
// photoRef is still treated as untrusted input for filename-construction
// purposes -- defense in depth against unexpected characters (path
// separators, "..", etc.) even though the doc says it's server-generated
// and presumably safe -- so the on-disk filename is a SHA-256 hash of
// photoRef rather than photoRef used verbatim as a path component.
class ContactPhotoCache
{
public:
    // Total disk this cache may occupy. photoRef is content-hashed, so
    // entries are never stale and nothing here expires -- but "never stale"
    // is not "never grows": every distinct photoRef the user ever views
    // deposits another file, and a relay that hands out a fresh reference per
    // request turns that into unbounded disk. Over budget, the oldest-written
    // entries are removed (mtime order -- reads do not touch it, so this is
    // oldest-first, not least-recently-used). Losing one costs a re-fetch.
    static constexpr qint64 kMaxCacheBytes = 64LL * 1024 * 1024;

    // The one place the photo cache directory is named, so that erasing it
    // and deciding whether to recreate it are the same decision. Returns an
    // empty string when the profile keeps nothing on disk -- Hostile Location
    // Protection -- which is exactly what the constructor below treats as
    // "disabled". main() erased this path and then constructed a cache over
    // it, which recreated the directory and put the next fetched face
    // straight back on the disk the mode promises holds nothing.
    static QString directoryFor(const QString& dataDir, bool persistent);

    // cacheDir is created if it doesn't already exist, and made owner-only.
    // If it cannot be BOTH, the cache disables itself: every store() and
    // cachedPathFor() call then returns an empty string, so photos are
    // re-fetched rather than written somewhere other users can read them.
    //
    // An EMPTY cacheDir disables the cache outright, and is how a caller says
    // "no disk cache at all". PrivatePath::ensureDirectory("") would refuse it
    // too -- mkpath("") fails -- so the explicit check is not what makes this
    // safe; it is what keeps a deliberate decision from logging "could not
    // create the photo cache directory" on every launch of a mode that asked
    // for no directory. Not listed in tests/guards.tsv for exactly that
    // reason: removing it does not change the outcome, only the log.
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
    void evictToBudget(const QString& keepPath) const;

    QDir m_dir;
    bool m_available = false;
};
