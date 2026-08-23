#include "security/PrivatePath.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

// The startup hardening in app/main.cpp used to be a mkpath() and a
// setPermissions() with both results dropped, under a comment asserting that
// the mail database, the contact photos and the push endpoint were protected.
// A directory somebody else created, or one on a filesystem with no
// permission bits, made that comment the only protection there was.
//
// These tests are what turns the comment into a fact the caller can act on.
class PrivatePathTest : public QObject
{
    Q_OBJECT

private slots:
    void aFreshDirectoryIsCreatedOwnerOnly();
    void anExistingWorldReadableDirectoryIsTightened();
    void aDirectoryThatCannotBeCreatedIsReportedAsNotCreated();
    void aDirectoryThatCannotBeTightenedIsReportedAsNotPrivate();
    void aWorldReadableFileIsTightened();
    void anAbsentFileIsNotPrivate();
};

void PrivatePathTest::aFreshDirectoryIsCreatedOwnerOnly()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("fresh"));

    QCOMPARE(PrivatePath::ensureDirectory(path), PrivatePath::Status::Ready);
    QVERIFY(QFileInfo(path).isDir());
    QVERIFY(PrivatePath::isPrivate(path));
}

// The case that matters most in the field: a data directory left behind by an
// older build, created under a loose umask before anything tightened it.
void PrivatePathTest::anExistingWorldReadableDirectoryIsTightened()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("legacy"));
    QVERIFY(QDir().mkpath(path));
    QVERIFY(QFile::setPermissions(path,
                                   QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                                       | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                       | QFileDevice::ReadOther | QFileDevice::ExeOther));
    QVERIFY(!PrivatePath::isPrivate(path)); // the state this exists to fix

    QCOMPARE(PrivatePath::ensureDirectory(path), PrivatePath::Status::Ready);
    QVERIFY(PrivatePath::isPrivate(path));
}

// A file where the directory should be. Fails for root too, unlike chmod.
void PrivatePathTest::aDirectoryThatCannotBeCreatedIsReportedAsNotCreated()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("in-the-way"));
    QFile blocker(path);
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.close();

    QCOMPARE(PrivatePath::ensureDirectory(path), PrivatePath::Status::NotCreated);
}

// The status the callers fail closed on, and the only one that cannot be
// built out of files this process owns: chmod succeeds on anything we own, so
// the premise has to be a directory belonging to somebody else. /tmp is the
// one such directory that is world-readable AND writable on every Linux box,
// which matters -- if this guard were removed, a store would really land in
// there rather than failing for an unrelated reason.
void PrivatePathTest::aDirectoryThatCannotBeTightenedIsReportedAsNotPrivate()
{
    if (::geteuid() == 0)
        QSKIP("running as root: chmod would succeed, and would chmod /tmp");

    const QString shared = QStringLiteral("/tmp");
    const QFileInfo sharedInfo(shared);
    if (!sharedInfo.isDir() || sharedInfo.ownerId() == ::geteuid()
        || !(sharedInfo.permissions() & QFileDevice::ReadOther)) {
        QSKIP("no world-readable directory owned by another user on this machine");
    }

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("someone-elses"));
    QVERIFY(QFile::link(shared, path));

    QCOMPARE(PrivatePath::ensureDirectory(path), PrivatePath::Status::NotPrivate);
    QCOMPARE(QFileInfo(shared).permissions(), sharedInfo.permissions()); // and nothing was changed
}

// KUnifiedPush writes its client state with plain QSettings, so it lands 0644
// after a default umask -- and the endpoint inside it is a bearer capability
// anyone who can read the file can use.
void PrivatePathTest::aWorldReadableFileIsTightened()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("kunifiedpush-state"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("endpoint=https://push.example/very-secret");
    file.close();
    QVERIFY(QFile::setPermissions(path,
                                   QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup
                                       | QFileDevice::ReadOther));
    QVERIFY(!PrivatePath::isPrivate(path));

    QVERIFY(PrivatePath::ensureFile(path));
    QVERIFY(PrivatePath::isPrivate(path));
}

// Not "vacuously fine": a caller asking this wants a protected file, and
// there is nothing here to protect.
void PrivatePathTest::anAbsentFileIsNotPrivate()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("never-written"));

    QVERIFY(!PrivatePath::ensureFile(path));
    QVERIFY(!PrivatePath::isPrivate(path));
}

QTEST_GUILESS_MAIN(PrivatePathTest)
#include "PrivatePathTest.moc"
