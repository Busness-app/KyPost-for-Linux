add_executable(kypost
  main.cpp
  WindowActivation.cpp
  qml/qml.qrc
)

target_include_directories(kypost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(kypost PRIVATE
  kypostcore
  Qt6::Quick
  Qt6::Qml
  Qt6::QuickControls2
  KF6::Kirigami
)

# EmailDetail.qml's body WebEngineView (Task 35) -- Qt6::WebEngineQuick
# provides the "QtWebEngine" QML module (WebEngineView/
# WebEngineNavigationRequest) at runtime; confirmed installed
# (qt6-webengine 6.11.1) this session. No app/*.cpp source touches this
# API directly -- it's QML-only, so no target_sources entry is needed here,
# just the find_package/target_link_libraries to make the plugin available
# at runtime and satisfy qmllint/IDE tooling.
find_package(Qt6 REQUIRED COMPONENTS WebEngineQuick)
target_link_libraries(kypost PRIVATE Qt6::WebEngineQuick)

# ThemeController wraps core::ThemeManager as a QML singleton (Q_PROPERTY/
# QColor/qmlRegisterSingletonInstance), so it belongs in the app executable,
# never in kypostcore -- Qt6::Gui (QColor) and Qt6::Qml (qmlRegisterSingleton*)
# are already available transitively via Qt6::Quick above, no new
# find_package/target_link_libraries needed. fonts.qrc bundles the real TTF
# files ThemeController.fontUi()/fontMono() advertise the family names of;
# same AUTORCC mechanism already used for qml/qml.qrc.
target_sources(kypost PRIVATE
  theme/ThemeController.cpp
  theme/ThemeController.h
  resources/fonts.qrc
)

# GeneralController wraps SettingsStore's Desktop/Mobile mode preference and
# desktop-only tray settings as a QML singleton ("General") -- plain QObject/
# QString/bool, no new find_package needed, same reasoning as
# ThemeController above.
target_sources(kypost PRIVATE
  general/GeneralController.cpp
  general/GeneralController.h
)

# MailController/EmailListModel (Task 32) wrap core/domain's MailRepository/
# KeywordRepository/PairingStore and core/net's RelayMailSource as a QML
# singleton ("MailApp") -- QObject/QAbstractListModel/QVariant, so app/ per
# the core/ boundary rule, same as ThemeController above. No extra
# find_package needed: QMimeDatabase/QStandardPaths/QFile are already part of
# Qt6::Core, transitively available via Qt6::Quick.
target_sources(kypost PRIVATE
  mail/EmailListModel.cpp
  mail/EmailListModel.h
  mail/MailController.cpp
  mail/MailController.h
  mail/PgpMessagePresentation.cpp
  mail/PgpMessagePresentation.h
  security/AppLockManager.cpp
  security/AppLockManager.h
  security/AppRelauncher.cpp
  security/AppRelauncher.h
)

# RemoteContentInterceptor (VibeSec fix): installed as EmailDetail.qml's
# WebEngineView.profile.urlRequestInterceptor to block CSS/media remote
# fetches that QWebEngineSettings::AutoLoadImages never gated in the first
# place -- see its own class doc comment. Needs Qt6::WebEngineCore directly
# for QWebEngineUrlRequestInterceptor/QWebEngineUrlRequestInfo (transitively
# pulled in by Qt6::WebEngineQuick above at the library level, but the C++
# headers need the component named explicitly here).
find_package(Qt6 REQUIRED COMPONENTS WebEngineCore)
target_sources(kypost PRIVATE
  mail/RemoteContentInterceptor.cpp
  mail/RemoteContentInterceptor.h
)
target_link_libraries(kypost PRIVATE Qt6::WebEngineCore)

# ContactsController/ContactListModel (Task 33) wrap core/domain's
# ContactSyncRepository as a QML singleton ("ContactsApp") -- QObject/
# QAbstractListModel/QVariant, so app/ per the core/ boundary rule, same as
# MailController/EmailListModel above. No extra find_package needed.
target_sources(kypost PRIVATE
  contacts/ContactFieldMapping.h
  contacts/ContactListModel.cpp
  contacts/ContactListModel.h
  contacts/ContactsController.cpp
  contacts/ContactsController.h
)

# PgpQrController wraps core/domain's PgpQrRepository and core/net's
# PgpQrClient as a QML singleton ("PgpQr") -- QObject/QVariant, so app/ per
# the core/ boundary rule, same as ContactsController above.
#
# QR generation/scanning uses zxing-cpp (the "ZXing::ZXing" CMake target,
# `find_package(ZXing CONFIG)`), not the originally-discussed QZXing: this
# machine already has zxing-cpp installed (a KDE-ecosystem transitive
# dependency of `prison`/gst-plugins-bad), it's actively maintained
# (v3.1.0, Apache-2.0), and it ships a header-only Qt/QtMultimedia wrapper
# (ZXing/ZXingQt.h) with both `Barcode::fromText(...).toImage()` (encode,
# used by PgpQrController::myQrImageDataUrl()) and a ready-made
# `ZXingQt::BarcodeReader` QObject (decode) that self-registers as QML type
# "ZXing"/"BarcodeReader" the moment its header is compiled into this
# binary with QT_QML_LIB defined (already true here via Qt6::Qml, linked
# transitively through Qt6::Quick) -- no separate PgpQrScanner class or
# manual qmlRegisterType needed, PgpScanContactKey.qml uses it directly.
#
# Unlike Qt5, there is no separate "MultimediaQuick" component in Qt6 --
# Qt6::Multimedia alone ships the "QtMultimedia" QML plugin (Camera/
# VideoOutput/MediaCaptureSession), auto-loaded by the QML engine at
# runtime; confirmed no Qt6MultimediaQuickConfig.cmake exists, only a
# private Qt6MultimediaQuickPrivate.
find_package(ZXing CONFIG REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Multimedia)
target_sources(kypost PRIVATE
  pgp/PgpQrController.cpp
  pgp/PgpQrController.h
  pgp/PgpQrScanner.cpp
  pgp/PgpQrScanner.h
  pgp/PgpQrTargetValidator.cpp
  pgp/PgpQrTargetValidator.h
  pgp/PgpEnrollmentController.cpp
  pgp/PgpEnrollmentController.h
)
target_link_libraries(kypost PRIVATE ZXing::ZXing Qt6::Multimedia)

# PairingController (Task 34) wraps core/domain's DeviceRegistrationService
# and PairingStore as the "Pairing" QML singleton -- QObject/QUrl parsing, so
# app/ per the core/ boundary rule, same as MailController/ContactsController
# above. No extra find_package needed: QUrlQuery is already part of Qt6::Core,
# transitively available via Qt6::Quick.
target_sources(kypost PRIVATE
  pairing/PairingController.cpp
  pairing/PairingController.h
)

# SecureStoreKeychain talks to the Secret Service over QtDBus, so it belongs
# in the app executable, never in kypostcore.
find_package(Qt6Keychain REQUIRED)
target_sources(kypost PRIVATE
  platform/SecureStoreKeychain.cpp
  platform/SecureStoreKeychain.h
)
target_link_libraries(kypost PRIVATE Qt6Keychain::Qt6Keychain)

# UnifiedPushConnector talks to the local KUnifiedPush distributor over
# D-Bus, so it belongs in the app executable, never in kypostcore. The
# KUnifiedPushConfig.cmake package exports a plain "KUnifiedPush" target
# (no namespace), confirmed against the installed .cmake files. Qt6::DBus is
# needed directly (not just transitively via KUnifiedPush) because
# UnifiedPushConnector registers its own well-known D-Bus service name for
# the distributor's callback delivery -- see UnifiedPushConnector.cpp.
find_package(KUnifiedPush REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS DBus)
target_sources(kypost PRIVATE
  push/UnifiedPushConnector.cpp
  push/UnifiedPushConnector.h
)
target_link_libraries(kypost PRIVATE KUnifiedPush Qt6::DBus)

# PushPayloadParser (pure JSON-bytes -> PushNotification parsing, no QObject)
# and NotificationDispatcher (wraps KNotification) (Task 40) live in
# app/push/, alongside UnifiedPushConnector -- KNotifications is a KF6 system
# library, never linked into kypostcore, same core/ boundary rule. The
# KF6NotificationsConfig.cmake package exports a "KF6::Notifications" target
# (confirmed via `pacman -Ql knotifications | grep cmake` and its Targets
# .cmake file this session), same naming pattern as KF6::DBusAddons below.
find_package(KF6Notifications REQUIRED)
target_sources(kypost PRIVATE
  push/NotificationDispatcher.cpp
  push/NotificationDispatcher.h
  push/PushPayloadParser.cpp
  push/PushPayloadParser.h
)
target_link_libraries(kypost PRIVATE KF6::Notifications)

# TrayController wraps KStatusNotifierItem (KF6StatusNotifierItem, confirmed
# installed this session -- kstatusnotifieritem-6.28.0) for the desktop-only
# system tray icon / minimize-to-tray feature. Its own
# KF6StatusNotifierItemConfig.cmake internally does
# find_dependency(Qt6Widgets 6.9.0) and links Qt6::Gui/Qt6::Widgets/Qt6::DBus
# itself, so no separate top-level CMakeLists.txt Widgets component is
# needed here. This is also why main.cpp now constructs QApplication instead
# of QGuiApplication: KStatusNotifierItem's standard-actions context menu is
# a QMenu (Qt Widgets), and a QWidget cannot be constructed under a plain
# QGuiApplication.
find_package(KF6StatusNotifierItem REQUIRED)
target_sources(kypost PRIVATE
  tray/TrayController.cpp
  tray/TrayController.h
)
target_link_libraries(kypost PRIVATE KF6::StatusNotifierItem)

# KDBusService (KF6DBusAddons) wraps app startup in main.cpp to claim the
# com.kysecurity.mail well-known bus name in Unique mode and emit
# activateRequested on duplicate-instance launches -- confirmed installed via
# `pacman -Qs kdbusaddons`, providing the standard KDE convenience wrapper
# over plain QDBusConnection registration for Task 11's D-Bus activation
# proof. See main.cpp for why org.freedesktop.Application/Activate isn't used.
find_package(KF6DBusAddons REQUIRED)
target_link_libraries(kypost PRIVATE KF6::DBusAddons)

# KI18n (Task 48): i18n()/i18nc() translation-catalog infrastructure.
# KF6::I18n is found once at the top level (same split as KF6::Kirigami) and
# provides the C++ i18n()/i18nc()/KLocalizedString::setApplicationDomain()
# API main.cpp uses. KF6::I18nQml (exported by that same find_package(KF6I18n)
# call -- confirmed via KF6I18nTargets.cmake, no second find_package needed)
# provides KLocalization::setupLocalizedContext(), which installs a
# KLocalizedQmlContext as the QQmlApplicationEngine's root context object so
# the same i18n()/i18nc() functions are callable from QML with no `import`
# statement (klocalizedqmlcontext.h: "the i18n functions are also available
# on the root context without KI18n.* prefix"). Deliberately NOT the older
# KLocalizedContext class -- confirmed deprecated since 6.8 on this 6.27.0
# install (klocalizedcontext.h: KI18N_DEPRECATED_VERSION(6, 8, "use
# KLocalizedQmlContext or KLocalization::setupLocalizedContext() from
# KF6::I18nQml instead")); see main.cpp for the actual wiring.
target_link_libraries(kypost PRIVATE KF6::I18n KF6::I18nQml)

include(GNUInstallDirs)
install(TARGETS kypost RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Install app icon files (Task 44): scalable SVG + rasterized PNGs at
# standard freedesktop hicolor theme sizes (16, 22, 24, 32, 48, 64, 128, 256 px).
# com.kysecurity.mail is the icon name referenced in the .desktop file.
install(FILES
  ${PROJECT_SOURCE_DIR}/packaging/flatpak/icons/hicolor/scalable/apps/com.kysecurity.mail.svg
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
)

install(FILES
  ${PROJECT_SOURCE_DIR}/packaging/flatpak/icons/hicolor/16x16/apps/com.kysecurity.mail.png
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/16x16/apps
)

install(FILES
  ${PROJECT_SOURCE_DIR}/packaging/flatpak/icons/hicolor/22x22/apps/com.kysecurity.mail.png
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/22x22/apps
)

install(FILES
  ${PROJECT_SOURCE_DIR}/packaging/flatpak/icons/hicolor/24x24/apps/com.kysecurity.mail.png
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/24x24/apps
)

install(FILES
  ${PROJECT_SOURCE_DIR}/packaging/flatpak/icons/hicolor/32x32/apps/com.kysecurity.mail.png
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/32x32/apps
)

install(FILES
  ${PROJECT_SOURCE_DIR}/packaging/flatpak/icons/hicolor/48x48/apps/com.kysecurity.mail.png
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/48x48/apps
)

install(FILES
  ${PROJECT_SOURCE_DIR}/packaging/flatpak/icons/hicolor/64x64/apps/com.kysecurity.mail.png
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/64x64/apps
)

install(FILES
  ${PROJECT_SOURCE_DIR}/packaging/flatpak/icons/hicolor/128x128/apps/com.kysecurity.mail.png
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/128x128/apps
)

install(FILES
  ${PROJECT_SOURCE_DIR}/packaging/flatpak/icons/hicolor/256x256/apps/com.kysecurity.mail.png
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
)

# Install desktop entry (Task 45): app metadata for the application menu and
# D-Bus service activation.
install(FILES
  ${PROJECT_SOURCE_DIR}/packaging/flatpak/com.kysecurity.mail.desktop
  DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)

# Refresh the desktop-file MIME cache (${CMAKE_INSTALL_DATADIR}/applications/
# mimeinfo.cache) so kypost://'s x-scheme-handler association above is
# actually discoverable, not just present on disk. install(FILES ...) alone
# only copies the .desktop file -- it never regenerates this cache, and
# without it GIO-based URL dispatch (what Chrome and most other browsers use
# to launch an external protocol handler) reports "No default application"
# and silently does nothing on a kypost://native-pair link click, even
# though xdg-mime's own raw-directory-scan fallback still resolves it fine.
# Distro packaging (deb/rpm triggers, Flatpak's export step) normally runs
# this as a post-install hook; a plain `cmake --install` has none, so it's
# done here instead. Quietly skipped if update-desktop-database isn't
# installed (optional tool, not a hard build dependency) or the DESTDIR'd
# path isn't the live system database (e.g. packaging a staging root).
find_program(UPDATE_DESKTOP_DATABASE_EXECUTABLE update-desktop-database)
if(UPDATE_DESKTOP_DATABASE_EXECUTABLE)
  install(CODE "
    execute_process(
      COMMAND ${UPDATE_DESKTOP_DATABASE_EXECUTABLE} \"\$ENV{DESTDIR}${CMAKE_INSTALL_FULL_DATADIR}/applications\"
      RESULT_VARIABLE _update_desktop_database_result
    )
    if(NOT _update_desktop_database_result EQUAL 0)
      message(STATUS \"update-desktop-database failed (non-fatal, e.g. staged DESTDIR install) -- kypost:// links may need \\`update-desktop-database\\` run manually\")
    endif()
  ")
endif()

# Install D-Bus service file (Task 45): activation metadata. The Exec= path
# (/usr/bin/kypost) is only correct once make install puts the binary
# there, which is now the case for real system installs.
install(FILES
  ${PROJECT_SOURCE_DIR}/packaging/dbus/com.kysecurity.mail.service
  DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services
)

# Install KNotifications config (Task 45): defines notification event types
# (newMail popup) that NotificationDispatcher (app/push/) can use.
install(FILES
  ${PROJECT_SOURCE_DIR}/packaging/flatpak/KyPost.notifyrc
  DESTINATION ${CMAKE_INSTALL_DATADIR}/knotifications6
)

# Install AppStream metainfo (Task 46): software-center metadata (name,
# summary, description, categories, release history) consumed by app
# stores/software centers such as KDE Discover and GNOME Software.
install(FILES
  ${PROJECT_SOURCE_DIR}/packaging/flatpak/com.kysecurity.mail.metainfo.xml
  DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo
)
