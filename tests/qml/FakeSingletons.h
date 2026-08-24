#pragma once

#include <QColor>
#include <QObject>
#include <QQmlEngine>
#include <QString>

// Minimal stand-ins for the "com.kysecurity.mail" QML singletons, so components
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
    Q_PROPERTY(bool storeUnavailable MEMBER m_storeUnavailable NOTIFY lockedChanged)
    Q_PROPERTY(bool wipeIncomplete MEMBER m_wipeIncomplete NOTIFY lockedChanged)
    Q_PROPERTY(bool databaseUnencrypted MEMBER m_databaseUnencrypted NOTIFY lockedChanged)
    Q_PROPERTY(bool databaseMemoryOnly MEMBER m_databaseMemoryOnly NOTIFY lockedChanged)
    Q_PROPERTY(bool dataDirectoryUnprotected MEMBER m_dataDirectoryUnprotected NOTIFY lockedChanged)

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
    bool m_storeUnavailable = false;
    bool m_wipeIncomplete = false;
    bool m_databaseUnencrypted = false;
    bool m_databaseMemoryOnly = false;
    bool m_dataDirectoryUnprotected = false;
};

class FakePairing : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool reregistrationRejected MEMBER m_reregistrationRejected NOTIFY changed)
    Q_PROPERTY(bool isPaired MEMBER m_isPaired NOTIFY changed)
    Q_PROPERTY(bool certificateMismatch MEMBER m_certificateMismatch NOTIFY changed)
    Q_PROPERTY(QString expectedCertificateFingerprint MEMBER m_expectedFingerprint NOTIFY changed)
    Q_PROPERTY(QString observedCertificateFingerprint MEMBER m_observedFingerprint NOTIFY changed)

public:
    using QObject::QObject;

    // Present so a root that binds it still loads under test. The dialog's
    // own test spies on its reconnectRequested signal instead: connecting
    // that signal to this call is the ROOT's job, not the dialog's, and a
    // test should not assert across a seam it isn't exercising.
    Q_INVOKABLE void reconnectToServer() { }

signals:
    void changed();

private:
    bool m_reregistrationRejected = false;
    bool m_isPaired = true;
    bool m_certificateMismatch = false;
    QString m_expectedFingerprint = QStringLiteral("AA:BB");
    QString m_observedFingerprint = QStringLiteral("CC:DD");
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
    // DangerButton binds these two. Absent until the certificate-trust
    // dialog became the first test to instantiate one, at which point every
    // run logged "Unable to assign [undefined] to QColor" and the button
    // under test rendered with no fill or border at all.
    Q_PROPERTY(QColor dangerFillColor MEMBER m_colour CONSTANT)
    Q_PROPERTY(QColor dangerBorderColor MEMBER m_colour CONSTANT)
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

class FakePgpEnrollment : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int state MEMBER m_state NOTIFY changed)
    Q_PROPERTY(QString status MEMBER m_status NOTIFY changed)
    Q_PROPERTY(QString verificationCode MEMBER m_code NOTIFY changed)
    Q_PROPERTY(bool busy MEMBER m_busy NOTIFY changed)

public:
    using QObject::QObject;
    Q_INVOKABLE void start() { m_state = 2; m_busy = true; m_code = QStringLiteral("5R9K6FWA18A8YP"); emit changed(); }
    Q_INVOKABLE void checkAgain() { m_state = 2; m_busy = true; emit changed(); }
    Q_INVOKABLE void cancel() { m_state = 0; m_busy = false; m_code.clear(); emit changed(); }

signals:
    void changed();

private:
    int m_state = 0;
    QString m_status;
    QString m_code;
    bool m_busy = false;
};

inline void registerFakeSingletons(QQmlEngine*)
{
    qmlRegisterSingletonType<FakeAppLock>(
        "com.kysecurity.mail", 1, 0, "AppLock",
        [](QQmlEngine*, QJSEngine*) -> QObject* { return new FakeAppLock; });
    qmlRegisterSingletonType<FakePairing>(
        "com.kysecurity.mail", 1, 0, "Pairing",
        [](QQmlEngine*, QJSEngine*) -> QObject* { return new FakePairing; });
    qmlRegisterSingletonType<FakeTheme>(
        "com.kysecurity.mail", 1, 0, "Theme",
        [](QQmlEngine*, QJSEngine*) -> QObject* { return new FakeTheme; });
    qmlRegisterSingletonType<FakeGeneral>(
        "com.kysecurity.mail", 1, 0, "General",
        [](QQmlEngine*, QJSEngine*) -> QObject* { return new FakeGeneral; });
    qmlRegisterSingletonType<FakePgpEnrollment>(
        "com.kysecurity.mail", 1, 0, "PgpEnrollment",
        [](QQmlEngine*, QJSEngine*) -> QObject* { return new FakePgpEnrollment; });
}
