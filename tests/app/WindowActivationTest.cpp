#include "WindowActivation.h"

#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>

// What happens when the user launches an already-running KyPost.
//
// KDBusService(Unique) means the second process does not become a second
// app: it relays its argv to the running instance over D-Bus and exits.
// main.cpp's activateRequested handler used to log the argument count and
// route deep links, and nothing else -- so an ordinary launch, carrying no
// kypost:// URL, did literally nothing visible. Reproduced end to end
// against the real Flatpak build: the second process relayed and exited,
// the running instance logged exactly one activateRequested, and no window
// ever appeared.
//
// From the user's side that is "clicking the app in the start menu does
// nothing", and it is unrecoverable once the window has been closed -- the
// process keeps running, and on a desktop with no StatusNotifier host there
// is no tray icon to click either. The launcher is the only way back in, and
// it was the thing that did not work.
//
// Hence this: activation must put the window in front, from any of the
// states it can be in.
class WindowActivationTest : public QObject
{
    Q_OBJECT

private:
    // A root Window, exactly the shape both QML roots have.
    static QQmlApplicationEngine* loadWindow(QQmlApplicationEngine& engine)
    {
        engine.loadData(R"(
            import QtQuick
            import QtQuick.Window
            Window { width: 200; height: 150; visible: true }
        )");
        return &engine;
    }

    static QQuickWindow* windowOf(QQmlApplicationEngine& engine)
    {
        const auto roots = engine.rootObjects();
        return roots.isEmpty() ? nullptr : qobject_cast<QQuickWindow*>(roots.first());
    }

private slots:
    void aClosedWindowIsShownAgain();
    void aMinimisedWindowIsRestored();
    void anAlreadyVisibleWindowStaysVisible();
    void aNullEngineIsSurvivable();
    void anEngineWithNoWindowIsSurvivable();
};

// The case that stranded the user: window closed, process still alive, and
// the launcher the only way back.
void WindowActivationTest::aClosedWindowIsShownAgain()
{
    QQmlApplicationEngine engine;
    loadWindow(engine);
    QQuickWindow* window = windowOf(engine);
    QVERIFY2(window != nullptr, "the fixture must produce a real QQuickWindow");

    window->setVisible(false);
    QVERIFY(!window->isVisible());

    activateMainWindow(&engine);

    QVERIFY2(window->isVisible(), "activating must bring a closed window back on screen");
}

void WindowActivationTest::aMinimisedWindowIsRestored()
{
    QQmlApplicationEngine engine;
    loadWindow(engine);
    QQuickWindow* window = windowOf(engine);
    QVERIFY(window != nullptr);

    window->setWindowStates(Qt::WindowMinimized);
    QVERIFY(window->windowStates() & Qt::WindowMinimized);

    activateMainWindow(&engine);

    QVERIFY2(!(window->windowStates() & Qt::WindowMinimized),
             "activating must un-minimise, not just re-show behind everything");
    QVERIFY(window->isVisible());
}

void WindowActivationTest::anAlreadyVisibleWindowStaysVisible()
{
    QQmlApplicationEngine engine;
    loadWindow(engine);
    QQuickWindow* window = windowOf(engine);
    QVERIFY(window != nullptr);
    QVERIFY(window->isVisible());

    activateMainWindow(&engine);

    QVERIFY2(window->isVisible(), "activating a visible window must not hide or destroy it");
}

// activateRequested can fire before the engine exists -- KDBusService is
// constructed first on purpose, so the pointer is genuinely null for a
// window of time. Crashing there would turn a cosmetic bug into a crash on
// every second launch.
void WindowActivationTest::aNullEngineIsSurvivable()
{
    activateMainWindow(nullptr);
}

void WindowActivationTest::anEngineWithNoWindowIsSurvivable()
{
    QQmlApplicationEngine engine; // nothing loaded: no root objects at all
    activateMainWindow(&engine);
}

QTEST_MAIN(WindowActivationTest)
#include "WindowActivationTest.moc"
