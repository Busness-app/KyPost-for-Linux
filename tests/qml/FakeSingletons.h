#pragma once

#include <QColor>
#include <QObject>
#include <QQmlEngine>
#include <QString>

// Minimal stand-ins for the "com.urlxl.mail" QML singletons, so components
// under test can be instantiated without main.cpp's whole composition root.
//
// Only the members the components actually bind to are here. Anything a test
// needs to drive (AppLock.locked, Pairing.reregistrationRejected) is
// writable from QML; the rest are constants.

class FakeAppLock : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool locked MEMBER m_locked NOTIFY lockedChanged)
    Q_PROPERTY(bool lockEnabled MEMBER m_lockEnabled NOTIFY lockedChanged)
    Q_PROPERTY(bool credentialsUnavailable MEMBER m_credentialsUnavailable NOTIFY lockedChanged)
    Q_PROPERTY(int remainingLockoutSeconds MEMBER m_remainingLockoutSeconds NOTIFY lockedChanged)
    Q_PROPERTY(int failedAttempts MEMBER m_failedAttempts NOTIFY lockedChanged)
    Q_PROPERTY(int minimumPinLength MEMBER m_minimumPinLength CONSTANT)

public:
    using QObject::QObject;

    Q_INVOKABLE bool tryUnlock(const QString& pin)
    {
        if (pin != QStringLiteral("419273"))
            return false;
        m_locked = false;
        emit lockedChanged();
        return true;
    }
    Q_INVOKABLE QString pinRejectionReason(const QString& pin) const
    {
        return pin.size() >= m_minimumPinLength ? QString() : QStringLiteral("too short");
    }

signals:
    void lockedChanged();

private:
    bool m_locked = false;
    bool m_lockEnabled = false;
    bool m_credentialsUnavailable = false;
    int m_remainingLockoutSeconds = 0;
    int m_failedAttempts = 0;
    int m_minimumPinLength = 6;
};

class FakePairing : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool reregistrationRejected MEMBER m_reregistrationRejected NOTIFY changed)
    Q_PROPERTY(bool isPaired MEMBER m_isPaired NOTIFY changed)

public:
    using QObject::QObject;

signals:
    void changed();

private:
    bool m_reregistrationRejected = false;
    bool m_isPaired = true;
};

// Just enough of the theme for components that bind colours/fonts.
class FakeTheme : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QColor bg MEMBER m_colour CONSTANT)
    Q_PROPERTY(QColor panel MEMBER m_colour CONSTANT)
    Q_PROPERTY(QColor line MEMBER m_colour CONSTANT)
    Q_PROPERTY(QColor ink MEMBER m_colour CONSTANT)
    Q_PROPERTY(QColor inkStrong MEMBER m_colour CONSTANT)
    Q_PROPERTY(QColor accent MEMBER m_colour CONSTANT)
    Q_PROPERTY(QColor dangerColor MEMBER m_colour CONSTANT)
    Q_PROPERTY(QColor readableOnAccent MEMBER m_colour CONSTANT)
    Q_PROPERTY(QString fontUi MEMBER m_font CONSTANT)
    Q_PROPERTY(QString fontMono MEMBER m_font CONSTANT)
    Q_PROPERTY(int shapeField MEMBER m_shape CONSTANT)
    Q_PROPERTY(int shapeSheet MEMBER m_shape CONSTANT)
    Q_PROPERTY(int shapeButton MEMBER m_shape CONSTANT)

public:
    using QObject::QObject;

private:
    QColor m_colour = QColor(QStringLiteral("#202020"));
    QString m_font = QStringLiteral("sans-serif");
    int m_shape = 4;
};

class FakeGeneral : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool isDesktopMode MEMBER m_isDesktopMode CONSTANT)

public:
    using QObject::QObject;

private:
    bool m_isDesktopMode = true;
};

inline void registerFakeSingletons(QQmlEngine*)
{
    qmlRegisterSingletonType<FakeAppLock>(
        "com.urlxl.mail", 1, 0, "AppLock",
        [](QQmlEngine*, QJSEngine*) -> QObject* { return new FakeAppLock; });
    qmlRegisterSingletonType<FakePairing>(
        "com.urlxl.mail", 1, 0, "Pairing",
        [](QQmlEngine*, QJSEngine*) -> QObject* { return new FakePairing; });
    qmlRegisterSingletonType<FakeTheme>(
        "com.urlxl.mail", 1, 0, "Theme",
        [](QQmlEngine*, QJSEngine*) -> QObject* { return new FakeTheme; });
    qmlRegisterSingletonType<FakeGeneral>(
        "com.urlxl.mail", 1, 0, "General",
        [](QQmlEngine*, QJSEngine*) -> QObject* { return new FakeGeneral; });
}
