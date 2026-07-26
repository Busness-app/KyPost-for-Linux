cmake_minimum_required(VERSION 3.21)
# VERSION is the single source of truth for the app version. It reaches C++
# as KYPOST_VERSION (a PUBLIC compile definition on kypostcore, set in
# core/CMakeLists.txt) and is currently consumed by the User-Agent header in
# core/net/HttpClient.cpp. Keep it in step with the <release> entries in
# packaging/flatpak/com.urlxl.mail.metainfo.xml.
project(KyPost VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

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
