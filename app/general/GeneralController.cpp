#include "general/GeneralController.h"

#include "stores/SettingsStore.h"

GeneralController::GeneralController(SettingsStore& settingsStore, bool isDesktopMode, QObject* parent)
    : QObject(parent)
    , m_settingsStore(settingsStore)
    , m_isDesktopMode(isDesktopMode)
{
}

QString GeneralController::preferredMode() const
{
    return m_settingsStore.preferredMode();
}

void GeneralController::setPreferredMode(const QString& mode)
{
    if (mode == preferredMode())
        return;
    if (!m_settingsStore.setPreferredMode(mode))
        return;
    emit preferredModeChanged();
    emit relaunchRequired();
}

bool GeneralController::isDesktopMode() const
{
    return m_isDesktopMode;
}

bool GeneralController::trayIconEnabled() const
{
    return m_settingsStore.trayIconEnabled();
}

void GeneralController::setTrayIconEnabled(bool enabled)
{
    if (enabled == trayIconEnabled())
        return;
    m_settingsStore.setTrayIconEnabled(enabled);
    emit trayIconEnabledChanged(enabled);
}

bool GeneralController::minimizeToTrayOnClose() const
{
    return m_settingsStore.minimizeToTrayOnClose();
}

void GeneralController::setMinimizeToTrayOnClose(bool enabled)
{
    if (enabled == minimizeToTrayOnClose())
        return;
    m_settingsStore.setMinimizeToTrayOnClose(enabled);
    emit minimizeToTrayOnCloseChanged(enabled);
}
