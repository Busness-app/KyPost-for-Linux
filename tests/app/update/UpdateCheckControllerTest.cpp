#include "update/UpdateCheckController.h"

#include "version/VersionCompare.h"

#include <QTest>

class UpdateCheckControllerTest : public QObject
{
    Q_OBJECT

private slots:
    void reportsAnUpdateOnlyWhenTheServerVersionIsStrictlyNewer_data();
    void reportsAnUpdateOnlyWhenTheServerVersionIsStrictlyNewer();
    void installedVersionIsTheCompiledInConstant();
};

void UpdateCheckControllerTest::reportsAnUpdateOnlyWhenTheServerVersionIsStrictlyNewer_data()
{
    QTest::addColumn<QString>("latest");
    QTest::addColumn<QString>("installed");
    QTest::addColumn<bool>("expected");

    QTest::newRow("newer") << "0.3.0" << "0.2.0" << true;
    QTest::newRow("equal") << "0.2.0" << "0.2.0" << false;
    QTest::newRow("older") << "0.1.0" << "0.2.0" << false;
    // The server sends an empty string when it has nothing to report. That
    // must never render as an update.
    QTest::newRow("nothing reported") << "" << "0.2.0" << false;
}

void UpdateCheckControllerTest::reportsAnUpdateOnlyWhenTheServerVersionIsStrictlyNewer()
{
    QFETCH(QString, latest);
    QFETCH(QString, installed);
    QFETCH(bool, expected);
    QCOMPARE(VersionCompare::isNewer(latest, installed), expected);
}

// The left-hand side of the comparison must come from the build, not from a
// second hand-maintained copy. A constant that drifts from the tag makes a
// current install nag forever -- see the spec and server_version.go:12-28.
void UpdateCheckControllerTest::installedVersionIsTheCompiledInConstant()
{
    QCOMPARE(UpdateCheckController::compiledInVersion(), QStringLiteral(KYPOST_VERSION));
}

QTEST_MAIN(UpdateCheckControllerTest)
#include "UpdateCheckControllerTest.moc"
