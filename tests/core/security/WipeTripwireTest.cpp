#include "security/WipeTripwire.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h> // geteuid -- the root check in the permissions test

class WipeTripwireTest : public QObject
{
    Q_OBJECT

private slots:
    void armThenDisarmRoundTrips();
    void armIsIdempotentAndSurvivesRestart();
    void disarmOnAnUnarmedTripwireSucceeds();
    void armReportsFailureWhenTheMarkerCannotBeWritten();
    void anEmptyPathIsNeverArmedAndNeverClaimsToBe();
};

void WipeTripwireTest::armThenDisarmRoundTrips()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString marker = dir.filePath(QStringLiteral("wipe-pending"));

    WipeTripwire tripwire(marker);
    QVERIFY(!tripwire.isArmed());

    QVERIFY(tripwire.arm());
    QVERIFY(tripwire.isArmed());
    QVERIFY(QFile::exists(marker));

    QVERIFY(tripwire.disarm());
    QVERIFY(!tripwire.isArmed());
    QVERIFY(!QFile::exists(marker));
}

// The whole point is surviving a process that never came back. A second
// WipeTripwire over the same path is what the next launch sees.
void WipeTripwireTest::armIsIdempotentAndSurvivesRestart()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString marker = dir.filePath(QStringLiteral("wipe-pending"));

    {
        WipeTripwire tripwire(marker);
        QVERIFY(tripwire.arm());
        QVERIFY(tripwire.arm()); // re-arming is not a second event
    }

    WipeTripwire afterRelaunch(marker);
    QVERIFY(afterRelaunch.isArmed());
}

// The post-condition is "no marker", and it already holds. Returning false
// here would make an ordinary clean shutdown look like a bookkeeping failure.
void WipeTripwireTest::disarmOnAnUnarmedTripwireSucceeds()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    WipeTripwire tripwire(dir.filePath(QStringLiteral("wipe-pending")));
    QVERIFY(!tripwire.isArmed());
    QVERIFY(tripwire.disarm());
}

// A read-only profile directory is exactly the kind of failure that also
// breaks the wipe, so arm() must report it rather than let the caller
// believe an interruption would be caught.
void WipeTripwireTest::armReportsFailureWhenTheMarkerCannotBeWritten()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString readOnlySubdir = dir.filePath(QStringLiteral("locked"));
    QVERIFY(QDir().mkpath(readOnlySubdir));

    // Strip write permission from the directory the marker would go in.
    QVERIFY(QFile::setPermissions(readOnlySubdir,
                                   QFileDevice::ReadOwner | QFileDevice::ExeOwner));

    WipeTripwire tripwire(QDir(readOnlySubdir).filePath(QStringLiteral("wipe-pending")));
    const bool armed = tripwire.arm();

    // Restore before asserting, so a failure here cannot leave an
    // unremovable temporary directory behind.
    QVERIFY(QFile::setPermissions(readOnlySubdir,
                                   QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));

    // Running as root defeats permission bits entirely -- report that rather
    // than assert something the environment made untrue.
    if (armed) {
        QVERIFY2(::geteuid() == 0, "arm() reported success on a directory it could not write to");
        QSKIP("running as root: directory permissions do not apply");
    }
    QVERIFY(!armed);
}

void WipeTripwireTest::anEmptyPathIsNeverArmedAndNeverClaimsToBe()
{
    // Braces, not parens: `WipeTripwire tripwire(QString())` declares a
    // function. Most vexing parse, caught by the compiler here.
    WipeTripwire tripwire{ QString() };
    QVERIFY(!tripwire.isArmed());
    QVERIFY(!tripwire.arm());
    QVERIFY(!tripwire.disarm());
}

QTEST_GUILESS_MAIN(WipeTripwireTest)
#include "WipeTripwireTest.moc"
