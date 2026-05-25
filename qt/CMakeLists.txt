cmake_minimum_required(VERSION 3.25)

project(OKILTVQt VERSION 0.4.1 LANGUAGES CXX)

option(OKILTV_BUILD_TESTS "Build Qt test targets" ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(OKILTV_QT_COMPONENTS
    Concurrent
    Core
    Gui
    Network
    OpenGL
    Qml
    Quick
    QuickControls2
    Svg
    Sql
    Widgets
)

if (OKILTV_BUILD_TESTS)
    list(APPEND OKILTV_QT_COMPONENTS Test)
endif ()

find_package(Qt6 6.10 REQUIRED COMPONENTS ${OKILTV_QT_COMPONENTS})

set(OKILTV_ZLIB_LINK_LIB "")
find_package(ZLIB QUIET)
if (ZLIB_FOUND)
    set(OKILTV_ZLIB_LINK_LIB ZLIB::ZLIB)
elseif (MINGW OR (WIN32 AND CMAKE_CROSSCOMPILING))
    # Qt MinGW SDK exposes bundled zlib headers (QtZlib/) but may not provide FindZLIB metadata.
    # In that setup QtCore already resolves zlib internally, so do not hard-fail configure.
    message(STATUS "ZLIB package not found; using Qt bundled zlib path for MinGW cross-build.")
else ()
    message(FATAL_ERROR "ZLIB is required to build EPG gzip/zlib decompression support.")
endif ()

if (UNIX AND NOT APPLE)
    find_package(Qt6 REQUIRED COMPONENTS DBus)
endif()

qt_standard_project_setup()

qt_add_executable(OKILTVQt
    src/app/appcontroller.cpp
    src/app/appcontroller.h
    src/app/channellistmodel.cpp
    src/app/channellistmodel.h
    src/app/displaysleepblocker.cpp
    src/app/displaysleepblocker.h
    src/app/dvrcontroller.cpp
    src/app/dvrcontroller.h
    src/app/epggridmodel.cpp
    src/app/epggridmodel.h
    src/app/guidestatemodel.cpp
    src/app/guidestatemodel.h
    src/app/main.cpp
    src/app/multiviewcontroller.cpp
    src/app/multiviewcontroller.h
    src/app/nownextmodel.cpp
    src/app/nownextmodel.h
    src/app/portableruntimecontroller.cpp
    src/app/portableruntimecontroller.h
    src/app/profilesmodel.cpp
    src/app/profilesmodel.h
    src/app/playercontroller.cpp
    src/app/playercontroller.h
    src/app/settingscontroller.cpp
    src/app/settingscontroller.h
    src/app/shellcontroller.cpp
    src/app/shellcontroller.h
    src/app/sourcegroupsmodel.cpp
    src/app/sourcegroupsmodel.h
    src/app/traycontroller.cpp
    src/app/traycontroller.h
    src/app/timeshiftcontroller.cpp
    src/app/timeshiftcontroller.h
    src/app/uitestbridge.cpp
    src/app/uitestbridge.h
    src/app/uitestcapturecontroller.cpp
    src/app/uitestcapturecontroller.h
    src/core/appdatapaths.cpp
    src/core/appdatapaths.h
    src/core/catchupurlresolver.cpp
    src/core/catchupurlresolver.h
    src/core/database_service.cpp
    src/core/database_service.h
    src/core/debuglogger.cpp
    src/core/debuglogger.h
    src/core/epgcache_service.cpp
    src/core/epgcache_service.h
    src/core/epgservice.cpp
    src/core/epgservice.h
    src/core/iconcache_service.cpp
    src/core/iconcache_service.h
    src/core/m3uservice.cpp
    src/core/m3uservice.h
    src/core/models.cpp
    src/core/models_json.cpp
    src/core/models_variant.cpp
    src/core/models.h
    src/core/networkaccess.cpp
    src/core/networkaccess.h
    src/core/portablebootstrap.cpp
    src/core/portablebootstrap.h
    src/core/processutils.cpp
    src/core/processutils.h
    src/core/redaction.cpp
    src/core/redaction.h
    src/core/settingsmanager.cpp
    src/core/settingsmanager.h
    src/core/sourcestore.cpp
    src/core/sourcestore.h
    src/core/xtreamservice.cpp
    src/core/xtreamservice.h
    src/player/catchupstreamsession.cpp
    src/player/catchupstreamsession.h
    src/player/mpvplayer.cpp
    src/player/mpvplayer.h
    src/player/mpvvideoitem.cpp
    src/player/mpvvideoitem.h
)

qt_add_resources(OKILTVQt app_assets
    PREFIX "/"
    FILES
        resources/icons/app.png
        resources/icons/audio-track.svg
        resources/icons/closed-caption.svg
        resources/icons/appearance.svg
        resources/icons/channel-error.svg
        resources/icons/close.svg
        resources/icons/diagnostics.svg
        resources/icons/dvr.svg
        resources/icons/epg-hide.svg
        resources/icons/epg-show.svg
        resources/icons/epg.svg
        resources/icons/favourites.svg
        resources/icons/fullscreen-close.svg
        resources/icons/fullscreen-enter.svg
        resources/icons/next-channel.svg
        resources/icons/on-top.svg
        resources/icons/pause.svg
        resources/icons/play.svg
        resources/icons/player.svg
        resources/icons/previous-channel.svg
        resources/icons/save.svg
        resources/icons/settings.svg
        resources/icons/sources.svg
        resources/icons/start-from-beginning.svg
        resources/icons/stop.svg
        resources/icons/timeshift.svg
        resources/icons/group-placeholder.svg
        resources/icons/multiview.svg
        resources/icons/volume-below-50.svg
        resources/icons/volume-muted.svg
        resources/icons/volume-over-50.svg
        resources/fonts/VCR_OSD_MONO_1.001.ttf
)

set_target_properties(OKILTVQt PROPERTIES
    OUTPUT_NAME OKILTV
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/app
    WIN32_EXECUTABLE TRUE
)

target_include_directories(OKILTVQt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/player
)

target_compile_definitions(OKILTVQt PRIVATE
    QT_DISABLE_DEPRECATED_BEFORE=0x060A00
)

if (UNIX AND NOT APPLE)
    target_compile_definitions(OKILTVQt PRIVATE
        OKILTV_HAS_DBUS_SLEEP_BLOCKER=1
    )
endif ()

target_link_libraries(OKILTVQt PRIVATE
    Qt6::Concurrent
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::OpenGL
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Svg
    Qt6::Sql
    Qt6::Widgets
    ${OKILTV_ZLIB_LINK_LIB}
)

if (MINGW)
    # Qt's official MinGW packages are built with stack-protector enabled.
    target_compile_options(OKILTVQt PRIVATE -fstack-protector-strong)
    target_link_options(OKILTVQt PRIVATE -fstack-protector-strong)
endif ()

if (UNIX AND NOT APPLE)
    target_link_libraries(OKILTVQt PRIVATE Qt6::DBus)
endif ()

if (WIN32)
    set(app_icon_rc "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/app_icon.rc")
    target_sources(OKILTVQt PRIVATE
        ${app_icon_rc}
    )
    set_source_files_properties(${app_icon_rc} PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/app.ico")
endif ()

set(OKILTV_QML_FILES
    qml/Main.qml
    qml/components/AppButton.qml
    qml/components/ChannelBadge.qml
    qml/components/FormComboBox.qml
    qml/components/FormSpinBox.qml
    qml/components/FormSwitch.qml
    qml/components/FormTextField.qml
    qml/components/EpgHoverBubble.qml
    qml/components/GlassPanel.qml
    qml/components/IconActionButton.qml
    qml/components/OverlaySectionPanel.qml
    qml/components/PathBrowseField.qml
    qml/components/SettingsRailButton.qml
    qml/components/WindowChromeBar.qml
    qml/components/WindowResizeHandles.qml
    qml/components/SourceManagerPane.qml
    qml/screens/GuidePage.qml
    qml/screens/LiveTvPage.qml
    qml/screens/SettingsPage.qml
)

qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

qt_add_qml_module(OKILTVQt
    URI OKILTV
    VERSION 1.0
    QML_FILES
        ${OKILTV_QML_FILES}
    RESOURCES
        qml/assets/fonts/LiberationSans-Bold.ttf
        qml/assets/fonts/LiberationSans-Regular.ttf
        qml/theme/Theme.js
)

find_program(OKILTV_QMLLINT_EXECUTABLE qmllint)
if (OKILTV_QMLLINT_EXECUTABLE)
    add_custom_target(qmllint_okiltv
        COMMAND
            ${OKILTV_QMLLINT_EXECUTABLE}
            -I ${CMAKE_CURRENT_BINARY_DIR}
            ${OKILTV_QML_FILES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running qmllint for OKILTV QML files"
    )
endif()

if (OKILTV_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()
