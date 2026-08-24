#include "stores/ContactPhotoCache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

namespace {

// The cache's own naming rule, restated: the test has to know where a photo
// WOULD have landed to prove that it did not.
QString expectedFileName(const QString& photoRef)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(photoRef.toUtf8(), QCryptographicHash::Sha256).toHex());
}

} // namespace

class ContactPhotoCacheTest : public QObject
{
    Q_OBJECT

private slots:
    void cachedPathForReturnsEmptyWhenNothingStored();
    void storeThenCachedPathForRoundTripsBytes();
    void cachedPathForOnEmptyPhotoRefReturnsEmpty();
    void storeOnEmptyBytesReturnsEmptyAndWritesNothing();
    void storeOverwritesExistingFileForSameRef();
    void constructorCreatesCacheDirIfMissing();
    void aCacheDirectoryOtherUsersCanReadDisablesTheCacheEntirely();
    void aCacheDirectoryThatCannotBeCreatedDisablesTheCache();
    void aStoredPhotoIsUnreadableToOtherUsers();
    void aProfileThatKeepsNothingOnDiskGetsNoCacheDirectoryAtAll();
    void anEmptyCacheDirectoryDisablesTheCacheEntirely();
    void theCacheDoesNotGrowPastItsBudget();
    void evictionKeepsTheEntryItJustHandedBackEvenWhenItSortsFirst();
};

void ContactPhotoCacheTest::cachedPathForReturnsEmptyWhenNothingStored()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ContactPhotoCache cache(dir.path());

    QVERIFY(cache.cachedPathFor(QStringLiteral("photo-ref-1")).isEmpty());
}

void ContactPhotoCacheTest::storeThenCachedPathForRoundTripsBytes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ContactPhotoCache cache(dir.path());

    const QByteArray bytes = QByteArrayLiteral("some-jpeg-bytes");
    const QString path = cache.store(QStringLiteral("photo-ref-1"), bytes);
    QVERIFY(!path.isEmpty());

    const QString cachedPath = cache.cachedPathFor(QStringLiteral("photo-ref-1"));
    QCOMPARE(cachedPath, path);

    QFile file(cachedPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), bytes);
}

void ContactPhotoCacheTest::cachedPathForOnEmptyPhotoRefReturnsEmpty()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ContactPhotoCache cache(dir.path());

    QVERIFY(cache.cachedPathFor(QString()).isEmpty());
}

void ContactPhotoCacheTest::storeOnEmptyBytesReturnsEmptyAndWritesNothing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ContactPhotoCache cache(dir.path());

    QVERIFY(cache.store(QStringLiteral("photo-ref-1"), QByteArray()).isEmpty());
    QVERIFY(cache.cachedPathFor(QStringLiteral("photo-ref-1")).isEmpty());
}

void ContactPhotoCacheTest::storeOverwritesExistingFileForSameRef()
{
    // photoRef is content-hashed/immutable per the doc, so this is a
    // defensive-behavior test, not a real-world expected path -- storing
    // different bytes under the same ref must still not crash or leave
    // stale bytes behind.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ContactPhotoCache cache(dir.path());

    cache.store(QStringLiteral("photo-ref-1"), QByteArrayLiteral("first-version"));
    const QString path = cache.store(QStringLiteral("photo-ref-1"), QByteArrayLiteral("second-version"));
    QVERIFY(!path.isEmpty());

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArrayLiteral("second-version"));
}

void ContactPhotoCacheTest::constructorCreatesCacheDirIfMissing()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const QString cacheDir = parent.path() + QStringLiteral("/contact-photos");
    QVERIFY(!QFile::exists(cacheDir));

    ContactPhotoCache cache(cacheDir);
    QVERIFY(QFile::exists(cacheDir));

    // Also usable immediately -- proves mkpath actually ran before any
    // store()/cachedPathFor() call, not just that the directory happens to
    // exist.
    const QString path = cache.store(QStringLiteral("photo-ref-1"), QByteArrayLiteral("bytes"));
    QVERIFY(!path.isEmpty());
}

// Finding the cache directory readable by other users used to produce a
// warning and nothing else: the cache stayed on, and pictures of everyone the
// user knows went into it under the default umask. The log line is not read
// until after the exposure, so it protected nobody.
//
// The premise is a directory belonging to somebody else -- chmod succeeds on
// anything this process owns -- and it is deliberately a WRITABLE one, so
// that removing the guard makes this test fail by actually storing the photo
// rather than by failing to write it for an unrelated reason.
void ContactPhotoCacheTest::aCacheDirectoryOtherUsersCanReadDisablesTheCacheEntirely()
{
    if (::geteuid() == 0)
        QSKIP("running as root: chmod would succeed, and would chmod /tmp");

    const QString shared = QStringLiteral("/tmp");
    const QFileInfo sharedInfo(shared);
    if (!sharedInfo.isDir() || sharedInfo.ownerId() == ::geteuid()
        || !(sharedInfo.permissions() & QFileDevice::ReadOther)) {
        QSKIP("no world-readable directory owned by another user on this machine");
    }

    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const QString cacheDir = parent.filePath(QStringLiteral("contact-photos"));
    QVERIFY(QFile::link(shared, cacheDir));

    const QString photoRef = QStringLiteral("photo-ref-shared-machine");
    const QString wouldBe = shared + QLatin1Char('/') + expectedFileName(photoRef);
    QFile::remove(wouldBe); // a run that failed here before really did leave one

    ContactPhotoCache cache(cacheDir);
    const QString stored = cache.store(photoRef, QByteArrayLiteral("a-recognisable-face"));

    // Cleaned up BEFORE anything can fail: the first version of this test put
    // the remove() after the QVERIFY, so the run that proved the guard
    // load-bearing left a face in /tmp for the next one to trip over.
    const bool leaked = QFileInfo::exists(wouldBe);
    QFile::remove(wouldBe);

    QVERIFY2(!leaked, "a contact photo was written into a directory other users can read");
    QVERIFY2(stored.isEmpty(), "the cache stayed enabled on a directory other users can read");
    // Reads are refused too: a cache that will not write must not hand out
    // paths under someone else's directory either.
    QVERIFY(cache.cachedPathFor(photoRef).isEmpty());
}

// The other way the directory can be unusable, and the one that needs no
// second user: a plain file sitting where it has to go.
void ContactPhotoCacheTest::aCacheDirectoryThatCannotBeCreatedDisablesTheCache()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const QString cacheDir = parent.filePath(QStringLiteral("contact-photos"));
    QFile blocker(cacheDir);
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.close();

    ContactPhotoCache cache(cacheDir);

    QVERIFY(cache.store(QStringLiteral("photo-ref-1"), QByteArrayLiteral("bytes")).isEmpty());
    QVERIFY(cache.cachedPathFor(QStringLiteral("photo-ref-1")).isEmpty());
    QCOMPARE(QFileInfo(cacheDir).size(), 0); // nothing written through the blocker either
}

// The directory is owner-only, and so is every file in it. A 0644 photo in a
// 0700 directory is safe only until the directory's mode is what changes.
void ContactPhotoCacheTest::aStoredPhotoIsUnreadableToOtherUsers()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ContactPhotoCache cache(dir.path());

    const QString path = cache.store(QStringLiteral("photo-ref-1"), QByteArrayLiteral("a-face"));
    QVERIFY(!path.isEmpty());

    const QFileDevice::Permissions mode = QFileInfo(path).permissions();
    QVERIFY2(!(mode & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup)), "group");
    QVERIFY2(!(mode & (QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther)), "other");

    // And nothing half-written was left beside it by QSaveFile.
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 1);
}

// The composition decision itself, not the two halves of it. main() erases
// this directory under Hostile Location Protection and then constructs a cache
// over the same path; when those were two separate expressions hundreds of
// lines apart, the second one recreated what the first had deleted and the
// next contact photo fetched was written straight back to disk. One function
// answers both questions, so they cannot disagree again.
void ContactPhotoCacheTest::aProfileThatKeepsNothingOnDiskGetsNoCacheDirectoryAtAll()
{
    const QString dataDir = QStringLiteral("/tmp/kypost-profile");

    QCOMPARE(ContactPhotoCache::directoryFor(dataDir, true),
             dataDir + QStringLiteral("/contact-photos"));
    QVERIFY2(ContactPhotoCache::directoryFor(dataDir, false).isEmpty(),
             "a non-persistent profile was handed a disk path to cache faces in");
}

// And the empty string that decision produces must actually disable the
// cache -- not fall through to QDir and resolve against the working directory,
// which would put the faces in whatever directory the app happened to start
// in.
void ContactPhotoCacheTest::anEmptyCacheDirectoryDisablesTheCacheEntirely()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString previousCwd = QDir::currentPath();
    QVERIFY(QDir::setCurrent(dir.path()));

    ContactPhotoCache cache((QString()));

    const QString path = cache.store(QStringLiteral("photo-ref-1"), QByteArrayLiteral("jpeg-bytes"));
    QVERIFY2(path.isEmpty(), "a disabled cache reported a stored path");
    QVERIFY(cache.cachedPathFor(QStringLiteral("photo-ref-1")).isEmpty());

    // Nothing landed anywhere -- including the working directory an empty
    // QDir path resolves to.
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).count(), 0);

    QVERIFY(QDir::setCurrent(previousCwd));
}

// Content-addressed keys remove staleness, not growth: every distinct photoRef
// deposits another file and nothing here ever expired. A relay handing out a
// fresh reference per request -- or simply a long-lived profile -- filled the
// user's disk one avatar at a time.
void ContactPhotoCacheTest::theCacheDoesNotGrowPastItsBudget()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ContactPhotoCache cache(dir.path());

    // 96 MiB of photos into a 64 MiB budget, oldest first. mtime has
    // one-second resolution on some filesystems, so the ordering is stamped
    // explicitly rather than assumed from write order.
    const QByteArray chunk(8 * 1024 * 1024, 'x');
    QDateTime stamp = QDateTime::currentDateTimeUtc().addSecs(-3600);
    for (int i = 0; i < 12; ++i) {
        const QString path = cache.store(QStringLiteral("photo-ref-%1").arg(i), chunk);
        QVERIFY(!path.isEmpty());
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(stamp, QFileDevice::FileModificationTime));
        file.close();
        stamp = stamp.addSecs(60);
    }

    qint64 total = 0;
    const QFileInfoList entries = QDir(dir.path()).entryInfoList(QDir::Files);
    for (const QFileInfo& entry : entries)
        total += entry.size();
    QVERIFY2(total <= ContactPhotoCache::kMaxCacheBytes,
             qPrintable(QStringLiteral("cache grew to %1 bytes").arg(total)));

    // The most recent write is the one the caller was just handed a path to,
    // so it is never the entry evicted to make room.
    QVERIFY(!cache.cachedPathFor(QStringLiteral("photo-ref-11")).isEmpty());
    QVERIFY2(cache.cachedPathFor(QStringLiteral("photo-ref-0")).isEmpty(),
             "the oldest entry survived a cache that was over budget");
}

// Eviction used to protect "the last entry in the oldest-first list" and
// call that the file store() had just written. That is only the same thing
// while every mtime is distinct and monotonic. Here the existing entries
// carry timestamps in the future -- a restored backup, a clock that stepped
// back, an rsync preserving times -- so the brand-new file sorts FIRST, and
// the positional rule deleted the very path store() was about to return.
void ContactPhotoCacheTest::evictionKeepsTheEntryItJustHandedBackEvenWhenItSortsFirst()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ContactPhotoCache cache(dir.path());

    const QByteArray chunk(8 * 1024 * 1024, 'x');
    const QDateTime future = QDateTime::currentDateTimeUtc().addSecs(3600);
    for (int i = 0; i < 8; ++i) {
        const QString path = cache.store(QStringLiteral("old-ref-%1").arg(i), chunk);
        QVERIFY(!path.isEmpty());
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(future.addSecs(i), QFileDevice::FileModificationTime));
        file.close();
    }

    // 64 MiB is already at the budget; this ninth photo puts it over, so
    // eviction has to run.
    const QString fresh = cache.store(QStringLiteral("fresh-ref"), chunk);
    QVERIFY(!fresh.isEmpty());

    QVERIFY2(QFileInfo::exists(fresh), "eviction deleted the path store() had just returned");
    QCOMPARE(cache.cachedPathFor(QStringLiteral("fresh-ref")), fresh);

    qint64 total = 0;
    const QFileInfoList entries = QDir(dir.path()).entryInfoList(QDir::Files);
    for (const QFileInfo& entry : entries)
        total += entry.size();
    QVERIFY2(total <= ContactPhotoCache::kMaxCacheBytes,
             qPrintable(QStringLiteral("cache grew to %1 bytes").arg(total)));
}

QTEST_GUILESS_MAIN(ContactPhotoCacheTest)
#include "ContactPhotoCacheTest.moc"
