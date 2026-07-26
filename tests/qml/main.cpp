// Qt Quick Test runner for the QML layer.
//
// This repo had 73 C++ tests and zero QML tests, while four of the five
// defences between a hostile email and the user lived in .qml files: the
// app-lock overlay, the email viewer's javascriptEnabled/autoLoadImages
// settings, its navigation gate, and the compose sanitizer. The app-lock
// bypass found in review (pop-out windows were never covered by the overlay)
// shipped precisely because nothing here could fail.
//
// The singletons registered below are fakes, not the real controllers: the
// production ones need a Database, a SecureStore, a live QNetworkAccessManager
// and the whole main.cpp composition root. Faking them keeps these tests
// about QML behaviour -- bindings, visibility, gating -- which is exactly the
// layer that was untested. The C++ behind them is covered by the other 73.
#include <QQmlEngine>
#include <QtQuickTest>

#include <KLocalizedQmlContext>

#include "FakeSingletons.h"

class Setup : public QObject
{
    Q_OBJECT

public:
    Setup() = default;

public slots:
    void qmlEngineAvailable(QQmlEngine* engine)
    {
        // The pages under test call i18n() freely; without this they throw
        // ReferenceError and never finish constructing. Same call main.cpp
        // makes, for the same reason.
        KLocalization::setupLocalizedContext(engine);
        registerFakeSingletons(engine);
    }
};

QUICK_TEST_MAIN_WITH_SETUP(kypost_qml, Setup)

#include "main.moc"
