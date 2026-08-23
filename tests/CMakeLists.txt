# One executable per QtTest class, registered with ctest. Task 7 does a
# final consistency pass once Tasks 4-6 add their own tests here.

function(kypost_add_test test_name)
  add_executable(${test_name} ${ARGN})
  target_link_libraries(${test_name} PRIVATE kypostcore Qt6::Test)
  add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

# Like kypost_add_test() above, but for tests that live in app/ rather than
# kypostcore: needs the app/ include directory (so headers under app/ resolve
# the same way they do for the kypost target itself) and an explicit
# SOURCES list (the extra app/*.cpp files under test, compiled directly into
# the test binary rather than linked from a library), plus an optional LIBS
# list for tests that need packages beyond kypostcore/Qt6::Test (e.g.
# KF6::I18n, Qt6Keychain::Qt6Keychain). Wraps the add_executable +
# target_include_directories(PRIVATE app) + target_link_libraries + add_test
# pattern repeated below for each app/ test target.
function(kypost_add_app_test test_name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
  add_executable(${test_name} ${ARG_SOURCES})
  target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/app)
  target_link_libraries(${test_name} PRIVATE kypostcore Qt6::Test ${ARG_LIBS})
  add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

kypost_add_test(EmailTest core/models/EmailTest.cpp)
kypost_add_test(ContactTest core/models/ContactTest.cpp)
kypost_add_test(StandardFolderTest core/models/StandardFolderTest.cpp)
kypost_add_test(KeywordSettingsTest core/models/KeywordSettingsTest.cpp)
kypost_add_test(MfaChallengeTest core/models/MfaChallengeTest.cpp)
kypost_add_test(PushNotificationTest core/models/PushNotificationTest.cpp)

# core/security: PIN policy, lockout backoff, and the PIN-derived AEAD that
# backs the credential gate. All three are pure/store-only, no GUI.
kypost_add_test(LockoutPolicyTest core/security/LockoutPolicyTest.cpp)
# Links libcrypto directly (kypostcore keeps it PRIVATE): the KDF-migration
# test has to hand-build a pre-Argon2id blob in the old PBKDF2 + AES-GCM
# layout, because that is the only way to prove an existing user's sealed
# credential still opens after the format change. find_package is repeated
# here because imported targets are directory-scoped and core/'s call does
# not reach this list.
find_package(OpenSSL REQUIRED)
kypost_add_test(CredentialCipherTest core/security/CredentialCipherTest.cpp)
target_link_libraries(CredentialCipherTest PRIVATE OpenSSL::Crypto)
kypost_add_test(AppLockStoreTest core/security/AppLockStoreTest.cpp)
kypost_add_test(DatabaseKeyStoreTest core/security/DatabaseKeyStoreTest.cpp)
# PinPolicy: what counts as an acceptable PIN. Enforced in C++ because
# the PIN is the key-encryption key for the relay device secret whenever
# the credential gate is on, and QML is not a security boundary.
kypost_add_test(PinPolicyTest core/security/PinPolicyTest.cpp)

kypost_add_test(DatabaseTest core/db/DatabaseTest.cpp)
kypost_add_test(DatabaseEncryptionTest core/db/DatabaseEncryptionTest.cpp)
kypost_add_test(OpenPgpDecryptorTest core/pgp/OpenPgpDecryptorTest.cpp)
kypost_add_test(ProfileDatabaseTest core/db/ProfileDatabaseTest.cpp)
kypost_add_test(DatabaseEncryptionMigrationTest core/db/DatabaseEncryptionMigrationTest.cpp)
kypost_add_test(SecurityWipeTest core/db/SecurityWipeTest.cpp)
kypost_add_test(EmailDaoTest core/db/EmailDaoTest.cpp)
kypost_add_test(ContactDaoTest core/db/ContactDaoTest.cpp)
kypost_add_test(GroupDaoTest core/db/GroupDaoTest.cpp)
kypost_add_test(FolderDaoTest core/db/FolderDaoTest.cpp)
kypost_add_test(PushDaoTest core/db/PushDaoTest.cpp)
kypost_add_test(PendingContactChangeDaoTest core/db/PendingContactChangeDaoTest.cpp)
kypost_add_test(NativeContactLinkDaoTest core/db/NativeContactLinkDaoTest.cpp)

kypost_add_test(SettingsStoreTest core/stores/SettingsStoreTest.cpp)
kypost_add_test(CursorStoreTest core/stores/CursorStoreTest.cpp)
kypost_add_test(SecureStoreFileTest core/stores/SecureStoreFileTest.cpp)
kypost_add_test(ContactPhotoCacheTest core/stores/ContactPhotoCacheTest.cpp)

kypost_add_test(PairingStoreTest core/domain/PairingStoreTest.cpp)
kypost_add_test(DeviceRegistrationServiceTest core/domain/DeviceRegistrationServiceTest.cpp)
kypost_add_test(MailRepositoryTest core/domain/MailRepositoryTest.cpp)
kypost_add_test(FolderRepositoryTest core/domain/FolderRepositoryTest.cpp)
kypost_add_test(PairingStoreSealTest core/domain/PairingStoreSealTest.cpp)
# The two data-destruction paths, which lived as untestable lambdas inside
# main() until they were extracted into core/domain/LocalDataWipe.
kypost_add_test(LocalDataWipeTest core/domain/LocalDataWipeTest.cpp)
kypost_add_test(TrackedWipeTest core/domain/TrackedWipeTest.cpp)
kypost_add_test(WipeTripwireTest core/security/WipeTripwireTest.cpp)
kypost_add_test(PgpMessageStateTest core/domain/PgpMessageStateTest.cpp)
kypost_add_test(PgpComposeStateTest core/domain/PgpComposeStateTest.cpp)
kypost_add_test(KeywordRepositoryTest core/domain/KeywordRepositoryTest.cpp)
kypost_add_test(ContactSyncReconciliationTest core/domain/ContactSyncReconciliationTest.cpp)
kypost_add_test(NativeContactReconciliationTest core/domain/NativeContactReconciliationTest.cpp)
kypost_add_test(ContactSyncRepositoryTest core/domain/ContactSyncRepositoryTest.cpp)
kypost_add_test(GroupsRepositoryTest core/domain/GroupsRepositoryTest.cpp)
kypost_add_test(ContactPhotoRepositoryTest core/domain/ContactPhotoRepositoryTest.cpp)
kypost_add_test(PushRepositoryTest core/domain/PushRepositoryTest.cpp)
kypost_add_test(TransportStateMachineTest core/domain/TransportStateMachineTest.cpp)
kypost_add_test(PgpQrRepositoryTest core/domain/PgpQrRepositoryTest.cpp)

kypost_add_test(NetworkErrorTest core/net/NetworkErrorTest.cpp)
kypost_add_test(RelayAuthTest core/net/RelayAuthTest.cpp)
kypost_add_test(HttpClientTest core/net/HttpClientTest.cpp)
# Asserts the property the whole threading migration exists for: that the
# calling thread is never re-entered while a request is in flight.
kypost_add_test(NetworkExecutorTest core/net/NetworkExecutorTest.cpp)
# Guards the unpinned-path regression the first threading conversion caused.
kypost_add_test(CertificatePinSinkTest core/net/CertificatePinSinkTest.cpp)
target_include_directories(NetworkExecutorTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/core/net)
kypost_add_test(NativeRegistrationClientTest core/net/NativeRegistrationClientTest.cpp)
kypost_add_test(PushNotificationClientTest core/net/PushNotificationClientTest.cpp)
kypost_add_test(MfaResponseClientTest core/net/MfaResponseClientTest.cpp)
kypost_add_test(DeregisterClientTest core/net/DeregisterClientTest.cpp)
kypost_add_test(ContactSyncClientTest core/net/ContactSyncClientTest.cpp)
kypost_add_test(FolderClientTest core/net/FolderClientTest.cpp)
kypost_add_test(GroupsClientTest core/net/GroupsClientTest.cpp)
kypost_add_test(ContactPhotoClientTest core/net/ContactPhotoClientTest.cpp)
kypost_add_test(PgpQrClientTest core/net/PgpQrClientTest.cpp)
kypost_add_test(PgpBootstrapClientTest core/net/PgpBootstrapClientTest.cpp)
kypost_add_test(PgpRecipientCheckerTest core/net/PgpRecipientCheckerTest.cpp)
kypost_add_test(PgpPayloadClientTest core/net/PgpPayloadClientTest.cpp)
kypost_add_test(RelayMailSourceTest core/net/RelayMailSourceTest.cpp)

kypost_add_test(VCardContactTest core/vcard/VCardContactTest.cpp)

kypost_add_test(ThemePaletteTest core/theme/ThemePaletteTest.cpp)
kypost_add_test(AppThemeTest core/theme/AppThemeTest.cpp)
kypost_add_test(ThemeManagerTest core/theme/ThemeManagerTest.cpp)

# SecureStoreKeychain needs QtKeychain + app/ headers, so its LIBS list adds
# Qt6Keychain::Qt6Keychain on top of kypost_add_app_test()'s usual
# kypostcore/Qt6::Test.
find_package(Qt6Keychain REQUIRED)
kypost_add_app_test(SecureStoreKeychainTest
  SOURCES
    app/platform/SecureStoreKeychainTest.cpp
    ${CMAKE_SOURCE_DIR}/app/platform/SecureStoreKeychain.cpp
  LIBS Qt6Keychain::Qt6Keychain
)

# EmailListModel/MailController (Task 32) live in app/mail/, not kypostcore
# (see core/ boundary rule): app/ headers plus the app/mail/*.cpp sources
# compiled directly into the test binary. Found here rather than lower down
# because EmailListModelTest itself now needs KF6::I18n: its PGP marker roles
# pull in app/mail/PgpMessagePresentation.cpp, which is localized.
find_package(KF6I18n REQUIRED)

kypost_add_app_test(EmailListModelTest
  SOURCES
    app/mail/EmailListModelTest.cpp
    ${CMAKE_SOURCE_DIR}/app/mail/EmailListModel.cpp
    ${CMAKE_SOURCE_DIR}/app/mail/PgpMessagePresentation.cpp
  LIBS KF6::I18n
)

# Localized prose + the webmail URL builder for the four PgpMessageState
# values. Lives in app/ rather than core/ because it needs KF6::I18n, which
# core/'s QtCore/QtNetwork/QtSql-only boundary forbids.
# AppLockManager: unlock/lockout/wipe transitions and the "prove the PIN
# even to turn protection off" rule. Needs no extra LIBS -- plain QObject.
# KF6::I18n: AppLockManager::pinRejectionReason() returns user-facing copy
# explaining why a PIN was refused, so it goes through i18n()/i18np() like
# every other string a user reads.
kypost_add_app_test(AppLockManagerTest
  SOURCES
    app/security/AppLockManagerTest.cpp
    ${CMAKE_SOURCE_DIR}/app/security/AppLockManager.cpp
  LIBS
    KF6::I18n
)

kypost_add_app_test(PgpMessagePresentationTest
  SOURCES
    app/mail/PgpMessagePresentationTest.cpp
    ${CMAKE_SOURCE_DIR}/app/mail/PgpMessagePresentation.cpp
  LIBS KF6::I18n
)

kypost_add_app_test(MailControllerTest
  SOURCES
    app/mail/MailControllerTest.cpp
    ${CMAKE_SOURCE_DIR}/app/mail/EmailListModel.cpp
    ${CMAKE_SOURCE_DIR}/app/mail/MailController.cpp
    ${CMAKE_SOURCE_DIR}/app/mail/PgpMessagePresentation.cpp
  # Qt6::Gui for QDesktopServices, used by the Hostile-Location-Protection
  # ephemeral attachment path. The kypost target gets it transitively via
  # Qt6::Quick; a test binary links kypostcore + Qt6::Test only, so it has
  # to be named here.
  LIBS KF6::I18n Qt6::Gui
)

# RemoteContentInterceptor (VibeSec fix): covers shouldBlockRemoteContentRequest(),
# the pure decision logic behind interceptRequest() -- see
# RemoteContentInterceptorTest.cpp's own comment for why interceptRequest()
# itself (which needs a real QWebEngineUrlRequestInfo, no public
# constructor) isn't directly testable. Needs Qt6::WebEngineCore for
# QWebEngineUrlRequestInterceptor/QWebEngineUrlRequestInfo, and
# Qt6::WebEngineQuick for QQuickWebEngineProfile -- installOn()'s
# Q_INVOKABLE bridge (see RemoteContentInterceptor.h) takes one directly,
# since QQuickWebEngineProfile::setUrlRequestInterceptor() isn't a QML
# property in Qt6 the way it was in Qt5.
find_package(Qt6 REQUIRED COMPONENTS WebEngineCore WebEngineQuick)
kypost_add_app_test(RemoteContentInterceptorTest
  SOURCES
    app/mail/RemoteContentInterceptorTest.cpp
    ${CMAKE_SOURCE_DIR}/app/mail/RemoteContentInterceptor.cpp
  LIBS Qt6::WebEngineCore Qt6::WebEngineQuick
)

# ContactListModel/ContactsController (Task 33) live in app/contacts/, not
# kypostcore -- same app/ test pattern as EmailListModelTest/MailControllerTest
# above.
kypost_add_app_test(ContactListModelTest
  SOURCES
    app/contacts/ContactListModelTest.cpp
    ${CMAKE_SOURCE_DIR}/app/contacts/ContactListModel.cpp
)

kypost_add_app_test(ContactsControllerTest
  SOURCES
    app/contacts/ContactsControllerTest.cpp
    ${CMAKE_SOURCE_DIR}/app/contacts/ContactListModel.cpp
    ${CMAKE_SOURCE_DIR}/app/contacts/ContactsController.cpp
  LIBS KF6::I18n
)

# PairingController/MfaController (Task 34) live in app/pairing/, not
# kypostcore -- same app/ test pattern as MailControllerTest/
# ContactsControllerTest above.
kypost_add_app_test(PairingControllerTest
  SOURCES
    app/pairing/PairingControllerTest.cpp
    ${CMAKE_SOURCE_DIR}/app/pairing/PairingController.cpp
  LIBS KF6::I18n
)

kypost_add_app_test(MfaControllerTest
  SOURCES
    app/pairing/MfaControllerTest.cpp
    ${CMAKE_SOURCE_DIR}/app/pairing/MfaController.cpp
  LIBS KF6::I18n
)

# PgpQrController (PGP QR key exchange) lives in app/pgp/, not kypostcore --
# same app/ test pattern as PairingControllerTest/MfaControllerTest above.
# Needs Qt6::Gui (QImage/QBuffer) and ZXing::ZXing (plain ZXing:: C++ API,
# see app/CMakeLists.txt's PgpQrController comment) in addition to KF6::I18n.
find_package(ZXing CONFIG REQUIRED)
kypost_add_app_test(PgpQrControllerTest
  SOURCES
    app/pgp/PgpQrControllerTest.cpp
    ${CMAKE_SOURCE_DIR}/app/pgp/PgpQrController.cpp
    ${CMAKE_SOURCE_DIR}/app/pgp/PgpQrTargetValidator.cpp
  LIBS Qt6::Gui KF6::I18n ZXing::ZXing
)

# PgpQrTargetValidator (VibeSec fix, extracted from PgpQrController so its
# DNS-resolution-based link-local check is independently unit-testable with
# an injected fake HostResolver -- see PgpQrTargetValidator.h) lives in
# app/pgp/, not kypostcore -- same app/ test pattern as PgpQrControllerTest
# above, but plain QtNetwork (QHostAddress/QHostInfo/QUrl), no extra LIBS
# needed.
kypost_add_app_test(PgpQrTargetValidatorTest
  SOURCES
    app/pgp/PgpQrTargetValidatorTest.cpp
    ${CMAKE_SOURCE_DIR}/app/pgp/PgpQrTargetValidator.cpp
)

# PushPayloadParser (Task 40) lives in app/push/, not kypostcore -- same
# app/ test pattern as EmailListModelTest/MailControllerTest above. Pure C++
# over QJsonDocument (already part of Qt6::Core, transitively available via
# kypostcore), no extra LIBS needed.
#
# NotificationDispatcher's notify() itself has no test here deliberately:
# like UnifiedPushConnector (Task 10, same directory), it's a thin wrapper
# around a concrete, un-mockable system integration point (KNotification's
# real D-Bus round-trip to whatever org.freedesktop.Notifications service is
# reachable, plus the notification server's own ActionInvoked callback for
# the "View" action) -- there's no fake/injectable seam to test through
# without building a mock notification server, which is out of scope. See
# task-40-report.md. Its pure title/text fallback-selection logic
# (pickTitle/pickText) *is* covered, directly, by NotificationDispatcherTest
# below.
kypost_add_app_test(PushPayloadParserTest
  SOURCES
    app/push/PushPayloadParserTest.cpp
    ${CMAKE_SOURCE_DIR}/app/push/PushPayloadParser.cpp
)

# NotificationDispatcherTest (review follow-up to Task 40): covers only the
# pure pickTitle()/pickText() static helpers, not notify() -- see the
# PushPayloadParserTest comment above for why notify() itself stays
# untested. Needs KF6::Notifications (same find_package as app/CMakeLists.txt)
# because NotificationDispatcher.cpp still #includes <KNotification> for
# notify(), even though this test never calls it.
find_package(KF6Notifications REQUIRED)
kypost_add_app_test(NotificationDispatcherTest
  SOURCES
    app/push/NotificationDispatcherTest.cpp
    ${CMAKE_SOURCE_DIR}/app/push/NotificationDispatcher.cpp
  LIBS KF6::Notifications KF6::I18n
)

# ---- QML tests -------------------------------------------------------
#
# Qt Quick Test, added 2026-07-26. This suite previously had 73 C++ tests and
# zero QML tests, while four of the five defences standing between a hostile
# email and the user lived in .qml files -- the app-lock overlay, the email
# viewer's javascriptEnabled/autoLoadImages settings, its navigation gate,
# and the compose sanitizer. The app-lock bypass fixed in this same pass
# (pop-out Windows were never covered by the overlay) shipped exactly because
# nothing here could go red.
#
# The runner registers fake "com.urlxl.mail" singletons (tests/qml/
# FakeSingletons.h) rather than the real controllers, which would drag in a
# Database, a SecureStore and a live QNetworkAccessManager. That keeps these
# tests about QML behaviour -- bindings, gating, visibility -- which is the
# layer that had no coverage at all.
#
# The QML under test is loaded from the same qrc the app ships
# (app/qml/qml.qrc), not from source paths, so a component that builds but is
# missing from the resource bundle fails here rather than at runtime.
find_package(Qt6 REQUIRED COMPONENTS QuickTest Quick Qml)

add_executable(QmlTests
  qml/main.cpp
  qml/FakeSingletons.h
  ${CMAKE_SOURCE_DIR}/app/qml/qml.qrc
)
target_include_directories(QmlTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/qml)
# KF6::I18nQml: the pages under test call i18n(), which needs the same
# KLocalizedQmlContext main.cpp installs -- without it they fail to
# construct with ReferenceError rather than testing anything.
target_link_libraries(QmlTests PRIVATE Qt6::QuickTest Qt6::Quick Qt6::Qml KF6::I18n KF6::I18nQml)
# QUICK_TEST_SOURCE_DIR tells the runner where the tst_*.qml files live.
target_compile_definitions(QmlTests PRIVATE
  QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml"
)

# offscreen: CI has no display, and these tests only assert on bindings and
# item state, never on rendered pixels.
add_test(NAME QmlTests COMMAND QmlTests)
set_tests_properties(QmlTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# QmlTests above instantiates a handful of leaf components. It cannot load
# DesktopRoot or MobileRoot -- those need every C++ singleton -- so a property
# assignment that makes a whole root fail to load slips through it entirely.
# That is not hypothetical: it shipped, and the app would not start.
#
# qmllint reads the files instead of running them, so it needs no singletons.
# See tests/qml/qmllint-gate.sh for why it gates on one warning category.
#
# Qt's qmllint, NOT whatever `qmllint` is first on PATH -- on at least one
# distro that name belongs to an unrelated tool which exits 0 on everything.
find_program(KYPOST_QMLLINT
  NAMES qmllint
  HINTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}" "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_LIBEXECS}"
  PATHS /usr/lib/qt6/bin
  NO_DEFAULT_PATH
)
if(KYPOST_QMLLINT)
  add_test(NAME QmlPropertyLint
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/qml/qmllint-gate.sh
            ${KYPOST_QMLLINT} ${CMAKE_SOURCE_DIR}/app/qml)
else()
  message(WARNING "Qt's qmllint not found -- QmlPropertyLint will not run. "
                  "A .qml file assigning a non-existent property will not be caught.")
endif()
