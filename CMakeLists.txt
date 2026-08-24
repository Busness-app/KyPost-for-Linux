cmake_minimum_required(VERSION 3.21)
# VERSION is the single source of truth for the app version. It reaches C++
# as KYPOST_VERSION (a PUBLIC compile definition on kypostcore, set in
# core/CMakeLists.txt) and is currently consumed by the User-Agent header in
# core/net/HttpClient.cpp. Keep it in step with the <release> entries in
# packaging/flatpak/com.kysecurity.mail.metainfo.xml and with the release
# tag -- scripts/verify-version.sh enforces this in CI.
project(KyPost VERSION 0.2.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Warnings and hardening, applied to every target in the project.
#
# This code does raw EVP_* calls with manual length arithmetic
# (core/security/CredentialCipher.cpp), reinterpret_cast<quint32*> over
# QByteArray buffers, and indexes a function-pointer table by a value read
# out of the database (core/db/Database.cpp). Building that at default
# warning level is choosing not to know.
#
# Not -Werror: automoc/autorcc-generated translation units and Qt's own
# headers are outside this project's control, so a Qt point release could
# break the build for a warning nobody here can fix. CI runs the sanitizer
# job below instead, which catches the class of defect that actually
# matters here.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual)
  # Hardening. _FORTIFY_SOURCE needs optimization to do anything and warns
  # noisily at -O0, hence the non-Debug guard; _GLIBCXX_ASSERTIONS is
  # cheap enough to leave on everywhere and turns container UB into an
  # abort with a message.
  add_compile_options(-fstack-protector-strong -fno-common)
  add_compile_definitions(_GLIBCXX_ASSERTIONS $<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=3>)
  add_link_options(-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)
endif()

# -DKYPOST_SANITIZE=ON builds everything (including the ctest targets) under
# ASan+UBSan. Not a default: it roughly triples test runtime and needs a
# matching libasan at runtime.
#
# Deliberately ASan+UBSan only, with no ThreadSanitizer option. TSan cannot
# produce a usable signal against an uninstrumented Qt -- every payload
# crossing a queued connection is reported as a race, and the suppression
# sets that quieten those also hide genuine ones. Measured numbers and the
# affinity check that replaces it are in docs/THREADING.md.
option(KYPOST_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# Where scripts/build-sqlcipher.sh installed SQLCipher.
#
# Empty means "no SQLCipher": the app builds and runs exactly as before,
# Database::open() refuses any request for an encrypted database rather than
# quietly opening a plaintext one, and the encryption tests report that they
# were not run instead of passing vacuously.
#
# Never resolved from the system paths. A find_library(NAMES sqlite3) with
# default paths would happily return the ordinary system SQLite, which
# accepts `PRAGMA key` and silently ignores it -- see Database::open() for
# what that costs. If it is not the copy we built, we do not want it.
set(KYPOST_SQLCIPHER_ROOT "" CACHE PATH "Prefix containing a SQLCipher built by scripts/build-sqlcipher.sh")

# A shipped build without SQLCipher is a plaintext mail cache, and nothing
# about it looks wrong: it builds, it runs, every test passes, and the
# encryption tests report a skip nobody reads. One dropped -D in a release or
# packaging job is all it takes. So a Release build STOPS here instead.
#
# Debug and unspecified build types are left alone deliberately -- that is
# where somebody is working on something unrelated and has no reason to build
# SQLCipher first -- and a Release build that really does mean it can say so
# with -DKYPOST_ALLOW_PLAINTEXT_DB=ON.
option(KYPOST_ALLOW_PLAINTEXT_DB "Permit a Release build with no SQLCipher, which stores mail unencrypted" OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT KYPOST_SQLCIPHER_ROOT AND NOT KYPOST_ALLOW_PLAINTEXT_DB)
  message(FATAL_ERROR
    "Release build with no SQLCipher: this binary would store the user's mail unencrypted on disk.\n"
    "  Build it:   ./scripts/build-sqlcipher.sh /tmp/sqlcipher\n"
    "  Point here: -DKYPOST_SQLCIPHER_ROOT=/tmp/sqlcipher\n"
    "If a plaintext Release build is genuinely what you want, say so: -DKYPOST_ALLOW_PLAINTEXT_DB=ON")
endif()
if(KYPOST_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network Sql Quick Qml QuickControls2 Test)
find_package(KF6Kirigami REQUIRED)

# Task 48: KI18n translation infrastructure. Found once here, same split as
# KF6Kirigami above -- app/CMakeLists.txt links KF6::I18n/KF6::I18nQml
# without a second find_package call, and po/CMakeLists.txt's ki18n_install()
# call relies on KF6I18nMacros.cmake, which KF6I18nConfig.cmake includes
# automatically as part of this find_package().
find_package(KF6I18n REQUIRED)

add_subdirectory(core)
add_subdirectory(app)
add_subdirectory(po)
enable_testing()
add_subdirectory(tests)
