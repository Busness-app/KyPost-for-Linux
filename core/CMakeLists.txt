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
  domain/LocalDataWipe.cpp
  domain/TrackedWipe.cpp
  domain/LocalDataWipe.h
  domain/PgpQrRepository.cpp
  domain/PgpQrRepository.h
  domain/ContactPhotoRepository.cpp
  domain/ContactPhotoRepository.h
  domain/PushRepository.cpp
  domain/PushRepository.h
  domain/TransportStateMachine.cpp
  domain/TransportStateMachine.h
  domain/PairingStoreCredentialSealer.cpp
  domain/PairingStoreCredentialSealer.h
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
  db/DatabaseEncryptionMigration.cpp
  db/ProfileDatabase.cpp
  pgp/OpenPgpDecryptor.cpp
  pgp/MimeBodyReader.cpp
  pgp/PgpMimeWriter.cpp
  pgp/OpenPgpKeyImporter.cpp
  pgp/OpenPgpEncryptor.cpp
  pgp/PgpSendPlanner.cpp
  pgp/EncryptedMessageReader.cpp
  security/DatabaseKeyStore.cpp
  security/LockoutPolicy.cpp
  security/WipeTripwire.cpp
  security/LockoutPolicy.h
  security/PinPolicy.cpp
  security/PinPolicy.h
  security/CredentialSealer.h
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
  net/CertificatePinSink.cpp
  net/CertificatePinSink.h
  net/NetworkExecutor.cpp
  net/NetworkExecutor.h
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
  net/PgpRecipientChecker.cpp
  net/PgpRecipientChecker.h
  net/PgpPayloadClient.cpp
  net/PgpPayloadClient.h
  net/PgpResolveClient.cpp
  net/PgpResolveClient.h
  net/RelayMailSource.cpp
  net/RelayMailSource.h
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

# Argon2id for the credential seal's key derivation.
#
# The PIN is a six-digit number and, with the credential gate on, it is the
# key-encryption key for the relay device secret -- so anyone holding a copy
# of the keychain entry attacks it offline, where the runtime backoff and
# wipe-after-ten policy do not exist. PBKDF2-HMAC-SHA256 at 150k iterations
# is ~1.5e11 HMACs for the whole 10^6 keyspace, which one consumer GPU
# recovers in hours: PBKDF2 is trivially parallel and needs no memory.
# Argon2id at 64 MiB per guess is memory-hard, which is what actually costs a
# GPU attacker, and libargon2 is packaged on every target distro and in
# org.kde.Platform. See core/security/CredentialCipher.h.
find_package(PkgConfig REQUIRED)
pkg_check_modules(ARGON2 REQUIRED IMPORTED_TARGET libargon2)

target_link_libraries(kypostcore PUBLIC
  Qt6::Core
  Qt6::Network
  Qt6::Sql
)

# gpgme's C library, NOT the gpgmepp C++ wrapper.
#
# The wrapper looked like the obvious choice and broke CI: KDE neon's
# libgpgmepp-dev requires gpgme >= 2.0.0 while Ubuntu noble carries gpgme
# 1.x, and CI layers both archives -- so its CMake config found its own
# headers and then failed on its own dependency. The C API is what the
# wrapper wraps, is present wherever gpgme is, and needs nothing newer than
# a decade-old release. See core/pgp/OpenPgpDecryptor.cpp.
pkg_check_modules(GPGME REQUIRED IMPORTED_TARGET gpgme)

target_link_libraries(kypostcore PRIVATE OpenSSL::Crypto PkgConfig::ARGON2 PkgConfig::GPGME)

# SQLCipher, when a prefix was given.
#
# Linked into kypostcore rather than loaded as some separate driver, because
# there is nothing to load: Qt's own stock QSQLITE plugin drives SQLCipher
# unchanged. The plugin has a DT_NEEDED on libsqlite3.so.0, and the library
# scripts/build-sqlcipher.sh produces carries exactly that SONAME -- so by
# the time Qt dlopens the plugin, its sqlite is already resolved to ours and
# the system copy is never mapped. Verified with LD_DEBUG=libs: one
# `calling init` line for libsqlite3, and it is this one.
#
# That is also why no vendored Qt driver, no private Qt SQL headers and no
# custom plugin appear anywhere in this repo. The first design assumed all
# three were needed.
if(KYPOST_SQLCIPHER_ROOT)
  find_library(KYPOST_SQLCIPHER_LIBRARY
    NAMES sqlite3
    PATHS "${KYPOST_SQLCIPHER_ROOT}/lib"
    NO_DEFAULT_PATH  # see the cache variable's comment: the system copy is not a fallback
    REQUIRED)
  # Proof, at configure time, that the library really is SQLCipher rather
  # than an ordinary SQLite that happens to sit at that prefix. sqlite3_key
  # exists only in SQLCipher. Without this the mistake surfaces at RUNTIME,
  # as Database::open() refusing to open anything -- correct, but a long way
  # from the person who pointed KYPOST_SQLCIPHER_ROOT at the wrong place.
  include(CheckLibraryExists)
  check_library_exists("${KYPOST_SQLCIPHER_LIBRARY}" sqlite3_key "" KYPOST_SQLCIPHER_HAS_KEY)
  if(NOT KYPOST_SQLCIPHER_HAS_KEY)
    message(FATAL_ERROR
      "${KYPOST_SQLCIPHER_LIBRARY} has no sqlite3_key: this is a plain SQLite, not SQLCipher. "
      "Build one with scripts/build-sqlcipher.sh and point KYPOST_SQLCIPHER_ROOT at its prefix.")
  endif()

  target_link_libraries(kypostcore PUBLIC "${KYPOST_SQLCIPHER_LIBRARY}")
  # So the running binary finds it without LD_LIBRARY_PATH, and finds it
  # BEFORE the Qt plugin asks for a libsqlite3 of its own.
  set(CMAKE_BUILD_RPATH "${KYPOST_SQLCIPHER_ROOT}/lib" PARENT_SCOPE)
  set(CMAKE_INSTALL_RPATH "${KYPOST_SQLCIPHER_ROOT}/lib" PARENT_SCOPE)
  target_compile_definitions(kypostcore PUBLIC KYPOST_HAVE_SQLCIPHER=1)
  message(STATUS "SQLCipher: ${KYPOST_SQLCIPHER_LIBRARY}")
else()
  message(STATUS "SQLCipher: not configured (KYPOST_SQLCIPHER_ROOT unset) -- "
                  "encrypted databases unavailable")
endif()

# Surfaces the root project(VERSION) to C++. PUBLIC so test binaries that
# link kypostcore see the same value the library was built with.
target_compile_definitions(kypostcore PUBLIC KYPOST_VERSION="${PROJECT_VERSION}")
