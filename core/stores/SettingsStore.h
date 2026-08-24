#pragma once

#include <QSettings>
#include <QString>
#include <optional>

// Thin typed wrapper around QSettings for app-wide preferences. The
// Connection and Keywords sections from the plan have no fields yet (see
// task notes) and are intentionally not represented here. Construct with an
// explicit file path so callers (real app or tests) control where settings
// live; core/ never decides the real on-disk location itself.
class SettingsStore
{
public:
    explicit SettingsStore(const QString& filePath);

    // Appearance
    QString themeId() const;
    void setThemeId(const QString& themeId);

    // Interface mode: "auto" (defer to QT_QUICK_CONTROLS_MOBILE), "desktop",
    // or "mobile" -- an explicit value here overrides the env var. Read once
    // at startup (see main.cpp's convergent root selection); changing it
    // takes effect on next launch, not live.
    QString preferredMode() const;
    void setPreferredMode(const QString& mode);

    // System tray (desktop mode only -- see GeneralController::isDesktopMode).
    bool trayIconEnabled() const;
    void setTrayIconEnabled(bool enabled);

    bool minimizeToTrayOnClose() const;
    void setMinimizeToTrayOnClose(bool enabled);

    // Hostile Location Protection: no mail, contacts, folders or attachments
    // cached on disk. Deliberately an ordinary preference here rather than a
    // SecureStore field (unlike the app lock's own flags, see
    // core/security/AppLockStore.h): the UI only lets it be toggled behind an
    // already-enabled, SecureStore-protected PIN, so an attacker who can edit
    // settings.ini still cannot reach anything this would have protected.
    //
    // Read by main.cpp BEFORE Database is constructed -- it decides whether
    // the database opens ":memory:" or the real file -- so changing it takes
    // effect only on the next launch, which is why toggling it relaunches.
    //
    // The setter returns false when the flag did not reach the disk. It has
    // to: the caller's next two acts are to erase the profile and relaunch,
    // and a flag that only ever existed in memory brings the replacement
    // process up unprotected -- over mail the erase already destroyed.
    bool hostileLocationProtectionEnabled() const;
    [[nodiscard]] bool setHostileLocationProtectionEnabled(bool enabled);

    // pushServerBaseUrl (the embedded ntfy subscriber's server, default
    // https://ntfy.sh) lived here until 2026-07-26. It went away with that
    // tier -- see core/domain/TransportStateMachine.h. The key it wrote,
    // "notifications/pushServerBaseUrl", is simply left unread in any
    // settings.ini that still has it; nothing migrates or removes it.

    // Push delivery (set by DeviceRegistrationService on successful (re-)registration)
    QString deliveryMode() const;      // "push" or "pull", empty if never registered
    void setDeliveryMode(const QString& mode);

    QString pullEndpoint() const;
    void setPullEndpoint(const QString& endpoint);

    QString transport() const;         // server-normalized value from the last successful registration
    void setTransport(const QString& transport);

    // Keywords -- per-keyword inbox-tab visibility, sparse map with no upper
    // bound on keyword count. "Never toggled" is distinct from "explicitly
    // hidden": true is the default, matching KeywordSettingsStore.swift's
    // isVisible default (KeywordSettings::visible's own struct-level `false`
    // default is unrelated -- that's just the zero-value for the type, not
    // this store's default-visible policy).
    bool keywordVisible(const QString& keyword) const; // true if never toggled
    void setKeywordVisible(const QString& keyword, bool visible);

private:
    QSettings m_settings;
};
