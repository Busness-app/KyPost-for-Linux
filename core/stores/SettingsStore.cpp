#include "stores/SettingsStore.h"

namespace {
constexpr auto kThemeIdKey = "appearance/themeId";
constexpr auto kPreferredModeKey = "interface/preferredMode";
constexpr auto kTrayIconEnabledKey = "general/trayIconEnabled";
constexpr auto kMinimizeToTrayOnCloseKey = "general/minimizeToTrayOnClose";
constexpr auto kHostileLocationKey = "security/hostileLocationProtection";
constexpr auto kDeliveryModeKey = "push/deliveryMode";
constexpr auto kPullEndpointKey = "push/pullEndpoint";
constexpr auto kTransportKey = "push/transport";

const QString kDefaultThemeId = QStringLiteral("Patina Ky");
const QString kDefaultPreferredMode = QStringLiteral("auto");

QString keywordVisibleKey(const QString& keyword)
{
    return QStringLiteral("keywords/%1").arg(keyword);
}
} // namespace

SettingsStore::SettingsStore(const QString& filePath)
    : m_settings(filePath, QSettings::IniFormat)
{
}

QString SettingsStore::themeId() const
{
    return m_settings.value(kThemeIdKey, kDefaultThemeId).toString();
}

void SettingsStore::setThemeId(const QString& themeId)
{
    m_settings.setValue(kThemeIdKey, themeId);
}

QString SettingsStore::preferredMode() const
{
    return m_settings.value(kPreferredModeKey, kDefaultPreferredMode).toString();
}

bool SettingsStore::setPreferredMode(const QString& mode)
{
    m_settings.setValue(kPreferredModeKey, mode);
    m_settings.sync();
    return m_settings.status() == QSettings::NoError;
}

bool SettingsStore::trayIconEnabled() const
{
    return m_settings.value(kTrayIconEnabledKey, true).toBool();
}

void SettingsStore::setTrayIconEnabled(bool enabled)
{
    m_settings.setValue(kTrayIconEnabledKey, enabled);
}

bool SettingsStore::minimizeToTrayOnClose() const
{
    return m_settings.value(kMinimizeToTrayOnCloseKey, false).toBool();
}

void SettingsStore::setMinimizeToTrayOnClose(bool enabled)
{
    m_settings.setValue(kMinimizeToTrayOnCloseKey, enabled);
}

bool SettingsStore::hostileLocationProtectionEnabled() const
{
    return m_settings.value(kHostileLocationKey, false).toBool();
}

bool SettingsStore::setHostileLocationProtectionEnabled(bool enabled)
{
    m_settings.setValue(kHostileLocationKey, enabled);
    // Flushed immediately, and the result CHECKED -- same rule as
    // CursorStore::flush(). The caller's very next act is to relaunch, so
    // QSettings' lazy write would otherwise be lost; and a read-only
    // settings.ini or a full disk is visible only through status() after
    // sync(). Reading the value back would prove nothing: QSettings answers
    // from its in-memory copy, which holds the new value either way.
    m_settings.sync();
    return m_settings.status() == QSettings::NoError;
}

QString SettingsStore::deliveryMode() const
{
    return m_settings.value(kDeliveryModeKey, QString()).toString();
}

void SettingsStore::setDeliveryMode(const QString& mode)
{
    m_settings.setValue(kDeliveryModeKey, mode);
}

QString SettingsStore::pullEndpoint() const
{
    return m_settings.value(kPullEndpointKey, QString()).toString();
}

void SettingsStore::setPullEndpoint(const QString& endpoint)
{
    m_settings.setValue(kPullEndpointKey, endpoint);
}

QString SettingsStore::transport() const
{
    return m_settings.value(kTransportKey, QString()).toString();
}

void SettingsStore::setTransport(const QString& transport)
{
    m_settings.setValue(kTransportKey, transport);
}

bool SettingsStore::keywordVisible(const QString& keyword) const
{
    return m_settings.value(keywordVisibleKey(keyword), true).toBool();
}

void SettingsStore::setKeywordVisible(const QString& keyword, bool visible)
{
    m_settings.setValue(keywordVisibleKey(keyword), visible);
}
