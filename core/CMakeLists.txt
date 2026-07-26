add_library(kypostcore STATIC
  models/StandardFolder.cpp
  models/Email.h
  models/Contact.h
  models/ContactFieldJson.h
  models/StandardFolder.h
  models/KeywordSettings.h
  models/MfaChallenge.h
  models/PushNotification.h
  models/Group.h
  models/MailFolder.h
  domain/DevicePairing.h
  domain/PairingStore.cpp
  domain/PairingStore.h
  domain/DeviceRegistrationService.cpp
  domain/DeviceRegistrationService.h
  domain/MailRepository.cpp
  domain/MailRepository.h
  domain/FolderRepository.cpp
  domain/FolderRepository.h
  domain/PgpMessageState.cpp
  domain/PgpMessageState.h
  domain/PgpComposeState.cpp
  domain/PgpComposeState.h
  domain/KeywordRepository.cpp
  domain/KeywordRepository.h
  domain/ContactSyncReconciliation.cpp
  domain/ContactSyncReconciliation.h
  domain/NativeContactReconciliation.cpp
  domain/NativeContactReconciliation.h
  domain/ContactSyncRepository.cpp
  domain/ContactSyncRepository.h
  domain/GroupsRepository.cpp
  domain/GroupsRepository.h
  domain/PgpQrRepository.cpp
  domain/PgpQrRepository.h
  domain/ContactPhotoRepository.cpp
  domain/ContactPhotoRepository.h
  domain/PushRepository.cpp
  domain/PushRepository.h
  domain/TransportStateMachine.cpp
  domain/TransportStateMachine.h
  db/Database.cpp
  db/Database.h
  db/EmailDao.cpp
  db/EmailDao.h
  db/ContactDao.cpp
  db/ContactDao.h
  db/GroupDao.cpp
  db/GroupDao.h
  db/FolderDao.cpp
  db/FolderDao.h
  db/PushDao.cpp
  db/PushDao.h
  db/PendingContactChangeDao.cpp
  db/PendingContactChangeDao.h
  db/NativeContactLinkDao.cpp
  db/NativeContactLinkDao.h
  db/SecurityWipe.cpp
  db/SecurityWipe.h
  db/SqlUtil.h
  security/AppLockStore.cpp
  security/AppLockStore.h
  security/CredentialCipher.cpp
  security/CredentialCipher.h
  security/LockoutPolicy.cpp
  security/LockoutPolicy.h
  stores/SettingsStore.cpp
  stores/SettingsStore.h
  stores/CursorStore.cpp
  stores/CursorStore.h
  stores/SecureStore.h
  stores/SecureStoreFile.cpp
  stores/SecureStoreFile.h
  stores/ContactPhotoCache.cpp
  stores/ContactPhotoCache.h
  net/NetworkError.cpp
  net/NetworkError.h
  net/RelayAuth.h
  net/HttpClient.cpp
  net/HttpClient.h
  net/NativeRegistrationClient.cpp
  net/NativeRegistrationClient.h
  net/PushNotificationClient.cpp
  net/PushNotificationClient.h
  net/MfaResponseClient.cpp
  net/MfaResponseClient.h
  net/DeregisterClient.cpp
  net/DeregisterClient.h
  net/ContactSyncClient.cpp
  net/ContactSyncClient.h
  net/FolderClient.cpp
  net/FolderClient.h
  net/GroupsClient.cpp
  net/GroupsClient.h
  net/ContactPhotoClient.cpp
  net/ContactPhotoClient.h
  net/PgpQrClient.cpp
  net/PgpQrClient.h
  net/PgpBootstrapClient.cpp
  net/PgpBootstrapClient.h
  net/RelayMailSource.cpp
  net/RelayMailSource.h
  net/NtfySubscriber.cpp
  net/NtfySubscriber.h
  vcard/VCardContact.cpp
  vcard/VCardContact.h
  theme/ThemePalette.cpp
  theme/ThemePalette.h
  theme/AppTheme.cpp
  theme/AppTheme.h
  theme/SemanticColors.h
  theme/Shape.h
  theme/ThemeManager.cpp
  theme/ThemeManager.h
)

# CONFIGURE_DEPENDS: adding db/migrations/NNN_*.sql should be enough to pick
# up a new migration on the next build, without touching this file.
file(GLOB KYPOST_MIGRATION_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/db/migrations/*.sql)
list(SORT KYPOST_MIGRATION_FILES)

set(KYPOST_MIGRATION_FUNCTIONS "")
set(KYPOST_MIGRATION_TABLE_ENTRIES "")
set(KYPOST_MIGRATION_COUNT 0)

foreach(migration_file ${KYPOST_MIGRATION_FILES})
  get_filename_component(migration_stem ${migration_file} NAME_WE)
  string(REGEX MATCH "^([0-9]+)_" _ ${migration_stem})
  set(migration_number ${CMAKE_MATCH_1})
  if(NOT migration_number)
    message(FATAL_ERROR "Migration file ${migration_file} must start with a numeric prefix like NNN_")
  endif()

  file(READ ${migration_file} migration_sql)

  string(APPEND KYPOST_MIGRATION_FUNCTIONS
    "inline QString kypostMigrationSql${migration_number}()\n"
    "{\n"
    "    return QStringLiteral(R\"SQL(${migration_sql})SQL\");\n"
    "}\n\n")
  string(APPEND KYPOST_MIGRATION_TABLE_ENTRIES "    &kypostMigrationSql${migration_number},\n")
  math(EXPR KYPOST_MIGRATION_COUNT "${KYPOST_MIGRATION_COUNT} + 1")
endforeach()

configure_file(db/MigrationSql.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/MigrationSql.h @ONLY)

target_include_directories(kypostcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(kypostcore PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

# core/security/CredentialCipher.cpp needs AES-256-GCM, which Qt does not
# expose anywhere in its public API. libcrypto is what Qt itself links for
# TLS, is in org.kde.Platform, and is on every target distro -- see that
# file's header comment. PRIVATE: nothing in kypostcore's public headers
# mentions an OpenSSL type, so consumers don't inherit the include path.
find_package(OpenSSL REQUIRED)

target_link_libraries(kypostcore PUBLIC
  Qt6::Core
  Qt6::Network
  Qt6::Sql
)

target_link_libraries(kypostcore PRIVATE OpenSSL::Crypto)

# Surfaces the root project(VERSION) to C++. PUBLIC so test binaries that
# link kypostcore see the same value the library was built with.
target_compile_definitions(kypostcore PUBLIC KYPOST_VERSION="${PROJECT_VERSION}")
