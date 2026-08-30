#include "general/GeneralController.h"

#include "stores/SettingsStore.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

class GeneralControllerTest : public QObject
{
    Q_OBJECT

private slots:
    void changingModeRequestsRelaunch();
};

void GeneralControllerTest::changingModeRequestsRelaunch()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SettingsStore store(dir.filePath(QStringLiteral("settings.ini")));
    GeneralController controller(store, true);
    QSignalSpy relaunchSpy(&controller, &GeneralController::relaunchRequired);

    controller.setPreferredMode(QStringLiteral("mobile"));
    QCOMPARE(relaunchSpy.count(), 1);

    controller.setPreferredMode(QStringLiteral("mobile"));
    QCOMPARE(relaunchSpy.count(), 1);
}

QTEST_GUILESS_MAIN(GeneralControllerTest)
#include "GeneralControllerTest.moc"
