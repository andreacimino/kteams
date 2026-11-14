cmake_minimum_required(VERSION 3.20)
project(kollega VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# No WebEngine: the client is fully native (sign-in with the OAuth device code flow).
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Qml Quick QuickControls2 Network WebSockets DBus Multimedia LinguistTools)
# Native calls: WebRTC through GStreamer (video frames go to Qt Multimedia's QVideoSink).
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 gstreamer-video-1.0 gstreamer-app-1.0)
# Tray icon.
find_package(KF6StatusNotifierItem REQUIRED)
# Trouter bodies can be gzip-compressed.
find_package(ZLIB REQUIRED)
# Refresh token storage in KWallet / Secret Service.
find_package(Qt6Keychain REQUIRED)
# Desktop notifications for new messages.
find_package(KF6Notifications REQUIRED)
# Idle detection for the "Away" presence: keyboard and mouse input on the whole desktop.
find_package(KF6IdleTime REQUIRED)

# Everything but main() is a static library, so the tests link the same code as the app.
add_library(kollega_core STATIC
    src/backend.cpp
    src/backend.h
    src/avatarcache.cpp
    src/avatarcache.h
    src/chatparser.cpp
    src/chatparser.h
    src/content.cpp
    src/content.h
    src/mediacache.cpp
    src/mediacache.h
    src/mediadevices.cpp
    src/mediadevices.h
    src/trouter.cpp
    src/trouter.h
    src/trouterframes.cpp
    src/trouterframes.h
    src/fileuploader.cpp
    src/fileuploader.h
    src/desktopintegration.cpp
    src/desktopintegration.h
    src/screencast.cpp
    src/screencast.h
    src/sdpdialect.cpp
    src/sdpdialect.h
    src/demo.h
    src/callengine.cpp
    src/callengine.h
    src/calljson.cpp
    src/calljson.h
    src/nativecall.cpp
    src/nativecall.h
    src/oauth.cpp
    src/oauth.h
    src/presenceinfo.cpp
    src/presenceinfo.h
    src/statecache.cpp
    src/statecache.h
    src/teamsclient.cpp
    src/teamsclient.h
    src/models.cpp
    src/models.h
)
target_include_directories(kollega_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(kollega_core PUBLIC
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Network Qt6::WebSockets Qt6::Multimedia
    ZLIB::ZLIB
    Qt6Keychain::Qt6Keychain
    KF6::Notifications
    KF6::IdleTime
    KF6::StatusNotifierItem
    PkgConfig::GST
    Qt6::DBus
)

add_executable(kollega
    src/main.cpp
    qml/qml.qrc
)

# Source strings are English; translations/*.ts hold the other languages (Qt Linguist),
# compiled and embedded at build time, picked by the system locale at start-up.
# Refresh the .ts files after changing strings: cmake --build native/build --target update_translations
# (the QML files are listed explicitly: lupdate does not look inside the .qrc)
file(GLOB KOLLEGA_QML_SOURCES CONFIGURE_DEPENDS qml/*.qml)
qt_add_translations(kollega
    SOURCE_TARGETS kollega kollega_core
    SOURCES ${KOLLEGA_QML_SOURCES}
    TS_FILES
        translations/kollega_it.ts
        translations/kollega_de.ts
        translations/kollega_fr.ts
        translations/kollega_es.ts
        translations/kollega_pt_BR.ts
        translations/kollega_zh_CN.ts
        translations/kollega_ja.ts
        translations/kollega_ru.ts
        translations/kollega_ko.ts
        translations/kollega_nl.ts
        translations/kollega_pl.ts
    RESOURCE_PREFIX "/i18n")

target_link_libraries(kollega PRIVATE kollega_core)

# Developer tool, off by default: Teams web in a QtWebEngine window with "--capture",
# which records the calling protocol (see docs/calling-protocol.md). Not used by the app.
option(BUILD_CALL_HELPER "Build kollega-capture, the Teams web protocol recorder (needs QtWebEngine)" OFF)
if(BUILD_CALL_HELPER)
find_package(Qt6 6.8 REQUIRED COMPONENTS WebEngineCore WebEngineQuick)
add_executable(kollega-capture
    call/main.cpp
    call/call.qrc
)
target_link_libraries(kollega-capture PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::WebEngineCore Qt6::WebEngineQuick
)
endif()

# cmake --install build --prefix ~/.local  (menu entry, icon, both programs)
include(GNUInstallDirs)
install(TARGETS kollega RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(BUILD_CALL_HELPER)
install(TARGETS kollega-capture RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
# The menu launcher's PATH may not include the install prefix (~/.local/bin): absolute Exec.
configure_file(data/io.github.andreacimino.kollega.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/io.github.andreacimino.kollega.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/io.github.andreacimino.kollega.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/io.github.andreacimino.kollega.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES data/kollega.notifyrc DESTINATION ${CMAKE_INSTALL_DATADIR}/knotifications6)
install(FILES data/io.github.andreacimino.kollega.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

# Media pipeline check without a call (not built by default): cmake --build native/build --target kollega-offer-test
add_executable(kollega-offer-test EXCLUDE_FROM_ALL tests/offer_dump.cpp)
target_link_libraries(kollega-offer-test PRIVATE kollega_core)

# Regression tests (Qt Test, run by ctest): cmake -DBUILD_TESTING=ON, then ctest --test-dir native/build.
# With -DKOLLEGA_COVERAGE=ON the "coverage" target runs them and writes the gcovr report
# (native/build/coverage/index.html, summary on the terminal); see tests/README.md.
include(CTest)
option(KOLLEGA_COVERAGE "Compile with gcov instrumentation and add the coverage target" OFF)
if(KOLLEGA_COVERAGE)
    target_compile_options(kollega_core PUBLIC --coverage -O0 -g)
    target_link_options(kollega_core PUBLIC --coverage)
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
