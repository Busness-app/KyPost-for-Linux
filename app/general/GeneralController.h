#pragma once

#include <QObject>
#include <QString>

class SettingsStore;

// QML-facing wrapper around SettingsStore's small, domain-less app-shell
// preferences: the Desktop/Mobile interface-mode choice and the desktop-only
// system tray options. Neither concern belongs on ThemeController (theme-
// specific) or any other existing controller, so they share this one
// singleton rather than each getting their own near-empty class. Registered
// as a QML singleton in main.cpp ("General"), same convention as Theme/
// Pairing.
class GeneralController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString preferredMode READ preferredMode NOTIFY preferredModeChanged)
    Q_PROPERTY(bool isDesktopMode READ isDesktopMode CONSTANT)
    Q_PROPERTY(bool trayIconEnabled READ trayIconEnabled NOTIFY trayIconEnabledChanged)
    Q_PROPERTY(bool minimizeToTrayOnClose READ minimizeToTrayOnClose NOTIFY minimizeToTrayOnCloseChanged)

public:
    // isDesktopMode reflects the mode THIS process already resolved to at
    // startup (see main.cpp's convergent root selection) -- fixed for the
    // life of the process. Changing preferredMode requests a relaunch so the
    // replacement process can select the other root component.
    explicit GeneralController(SettingsStore& settingsStore, bool isDesktopMode, QObject* parent = nullptr);

    QString preferredMode() const;
    Q_INVOKABLE void setPreferredMode(const QString& mode); // "auto" | "desktop" | "mobile"

    bool isDesktopMode() const;

    bool trayIconEnabled() const;
    Q_INVOKABLE void setTrayIconEnabled(bool enabled);

    bool minimizeToTrayOnClose() const;
    Q_INVOKABLE void setMinimizeToTrayOnClose(bool enabled);

signals:
    void preferredModeChanged();
    void relaunchRequired();
    void trayIconEnabledChanged(bool enabled);
    void minimizeToTrayOnCloseChanged(bool enabled);

private:
    SettingsStore& m_settingsStore;
    bool m_isDesktopMode;
};
