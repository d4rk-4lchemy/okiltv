qt_add_executable(OKILTVQtDatabaseStartupTests
    tst_databasestartup.cpp
    ../src/app/databasestartup.cpp
    ../src/app/databasestartup.h)
target_link_libraries(OKILTVQtDatabaseStartupTests PRIVATE Qt6::Widgets Qt6::Test)
add_test(NAME OKILTVQtDatabaseStartupTests COMMAND OKILTVQtDatabaseStartupTests)
set_tests_properties(OKILTVQtDatabaseStartupTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(OKILTVQtCoreTests
    tst_core.cpp
    ../src/core/appdatapaths.cpp
    ../src/core/appdatapaths.h
    ../src/core/catchupurlresolver.cpp
    ../src/core/catchupurlresolver.h
    ../src/core/database_service.cpp
    ../src/core/database_service.h
    ../src/core/debuglogger.cpp
    ../src/core/debuglogger.h
    ../src/core/epgcache_service.cpp
    ../src/core/epgcache_service.h
    ../src/core/epgstore.cpp
    ../src/core/epgstore.h
    ../src/core/epgservice.cpp
    ../src/core/epgservice.h
    ../src/core/m3uservice.cpp
    ../src/core/m3uservice.h
    ../src/core/datetimeformat.cpp
    ../src/core/datetimeformat.h
    ../src/core/models.cpp
    ../src/core/models_json.cpp
    ../src/core/models_variant.cpp
    ../src/core/sourcegrouppreferences.h
    ../src/core/models.h
    ../src/core/networkaccess.cpp
    ../src/core/networkaccess.h
    ../src/core/portablebootstrap.cpp
    ../src/core/portablebootstrap.h
    ../src/core/processutils.cpp
    ../src/core/processutils.h
    ../src/core/redaction.cpp
    ../src/core/redaction.h
    ../src/core/settingsmanager.cpp
    ../src/core/settingsmanager.h
    ../src/core/secretprotection.cpp
    ../src/core/sourcestore.cpp
    ../src/core/sourcestore.h
    ../src/core/xtreamservice.cpp
    ../src/core/xtreamservice.h
)

target_include_directories(OKILTVQtCoreTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

target_link_libraries(OKILTVQtCoreTests PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    Qt6::Test
    ${OKILTV_ZLIB_LINK_LIB}
)

add_test(NAME OKILTVQtCoreTests COMMAND OKILTVQtCoreTests)
set_tests_properties(
    OKILTVQtCoreTests
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;OKILTV_HEADLESS_TEST=1"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

qt_add_executable(OKILTVQtAppTests
    tst_app_models.cpp
    ../src/app/catchupdownloadcontroller.cpp
    ../src/app/catchuparchivetransfer.cpp
    ../src/app/catchuparchivetransfer.h
    ../src/app/catchupdownloadtransfer.cpp
    ../src/app/catchupdownloadtransfer.h
    ../src/app/catchupdownloadcontroller.h
    ../src/app/updatecheckcontroller.cpp
    ../src/app/updatecheckcontroller.h
    ../src/app/appcontroller.cpp
    ../src/app/appcontroller.h
    ../src/app/channellistmodel.cpp
    ../src/app/channellistmodel.h
    ../src/app/dvrcontroller.cpp
    ../src/app/dvrcontroller.h
    ../src/app/epggridmodel.cpp
    ../src/app/epggridmodel.h
    ../src/app/guidestatemodel.cpp
    ../src/app/guidestatemodel.h
    ../src/app/multiviewcontroller.cpp
    ../src/app/multiviewcontroller.h
    ../src/app/nownextmodel.cpp
    ../src/app/nownextmodel.h
    ../src/app/portableruntimecontroller.cpp
    ../src/app/portableruntimecontroller.h
    ../src/app/playback/playbackbuffering.cpp
    ../src/app/playback/playbackbuffering.h
    ../src/app/playback/playbackrecovery.cpp
    ../src/app/playback/playbackrecovery.h
    ../src/app/playback/catchupplaybacksession.cpp
    ../src/app/playback/catchupplaybacksession.h
    ../src/app/playback/catchupstandbytransition.cpp
    ../src/app/playback/catchupstandbytransition.h
    ../src/app/playercontroller.cpp
    ../src/app/playercontroller.h
    ../src/app/profilesmodel.cpp
    ../src/app/profilesmodel.h
    ../src/app/datetimeformatter.cpp
    ../src/app/datetimeformatter.h
    ../src/app/settingscontroller.cpp
    ../src/app/settingscontroller.h
    ../src/app/shellcontroller.cpp
    ../src/app/shellcontroller.h
    ../src/app/sourcegroupsmodel.cpp
    ../src/app/sourcegroupsmodel.h
    ../src/app/timeshiftcontroller.cpp
    ../src/app/timeshiftcontroller.h
    ../src/core/appdatapaths.cpp
    ../src/core/appdatapaths.h
    ../src/core/catchupurlresolver.cpp
    ../src/core/catchupurlresolver.h
    ../src/core/database_service.cpp
    ../src/core/database_service.h
    ../src/core/debuglogger.cpp
    ../src/core/debuglogger.h
    ../src/core/epgcache_service.cpp
    ../src/core/epgcache_service.h
    ../src/core/epgstore.cpp
    ../src/core/epgstore.h
    ../src/core/epgservice.cpp
    ../src/core/epgservice.h
    ../src/core/iconcache_service.cpp
    ../src/core/iconcache_service.h
    ../src/core/m3uservice.cpp
    ../src/core/m3uservice.h
    ../src/core/datetimeformat.cpp
    ../src/core/datetimeformat.h
    ../src/core/models.cpp
    ../src/core/models_json.cpp
    ../src/core/models_variant.cpp
    ../src/core/sourcegrouppreferences.h
    ../src/core/models.h
    ../src/core/networkaccess.cpp
    ../src/core/networkaccess.h
    ../src/core/portablebootstrap.cpp
    ../src/core/portablebootstrap.h
    ../src/core/processutils.cpp
    ../src/core/processutils.h
    ../src/core/redaction.cpp
    ../src/core/redaction.h
    ../src/core/settingsmanager.cpp
    ../src/core/settingsmanager.h
    ../src/core/secretprotection.cpp
    ../src/core/sourcestore.cpp
    ../src/core/sourcestore.h
    ../src/core/xtreamservice.cpp
    ../src/core/xtreamservice.h
    ../src/player/catchupstreamsession.cpp
    ../src/player/catchupstreamsession.h
    ../src/player/catchuptsjoiner.cpp
    ../src/player/catchuptsjoiner.h
    ../src/player/mpegtstimestampnormalizer.cpp
    ../src/player/mpegtstimestampnormalizer.h
    ../src/player/livebuffertuner.cpp
    ../src/player/livebuffertuner.h
    ../src/player/cachepolicy.cpp
    ../src/player/mpvplayer.cpp
    ../src/player/mpvplayer.h
    ../src/player/mpvvideoitem.cpp
    ../src/player/mpvvideoitem.h
)

target_include_directories(OKILTVQtAppTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

target_link_libraries(OKILTVQtAppTests PRIVATE
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::OpenGL
    Qt6::Quick
    Qt6::Sql
    Qt6::Test
    ${OKILTV_ZLIB_LINK_LIB}
)

add_test(NAME OKILTVQtAppTests COMMAND OKILTVQtAppTests)
set_tests_properties(
    OKILTVQtAppTests
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;OKILTV_HEADLESS_TEST=1"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

qt_add_executable(OKILTVQtCatchupTests
    tst_catchup_stream.cpp
    ../src/player/catchupstreamsession.cpp
    ../src/player/catchuptsjoiner.cpp
    ../src/player/mpegtstimestampnormalizer.cpp
    ../src/core/catchupurlresolver.cpp
    ../src/core/datetimeformat.cpp
    ../src/core/datetimeformat.h
    ../src/core/models.cpp
    ../src/core/debuglogger.cpp
    ../src/core/redaction.cpp
    ../src/core/appdatapaths.cpp
)
target_include_directories(OKILTVQtCatchupTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(OKILTVQtCatchupTests PRIVATE Qt6::Core Qt6::Network Qt6::Concurrent Qt6::Test)
add_test(NAME OKILTVQtCatchupTests COMMAND OKILTVQtCatchupTests)

if (WIN32)
    qt_add_executable(OKILTVQtWindowsProcessJobTests
        tst_windowsprocessjob.cpp
        ../src/core/processutils.cpp
        ../src/core/processutils.h)
    target_link_libraries(OKILTVQtWindowsProcessJobTests PRIVATE Qt6::Core Qt6::Test)
    add_dependencies(OKILTVQtAppTests OKILTVQtWindowsProcessJobTests)
    add_test(NAME OKILTVQtWindowsProcessJobTests COMMAND OKILTVQtWindowsProcessJobTests)
    add_test(NAME OKILTVQtWindowsDvrTests COMMAND OKILTVQtAppTests
        dvrControllerWindowsStoppedJobWithoutFinishedFinalizes
        dvrControllerWindowsWaitsForDescendants)
    set_tests_properties(OKILTVQtWindowsDvrTests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;OKILTV_HEADLESS_TEST=1")
endif ()

# Component-level interaction tests run without provider credentials or media playback.
# Cross builds cannot execute the target SDK's QML runner on the host.
if (NOT CMAKE_CROSSCOMPILING)
    find_program(OKILTV_QMLTESTRUNNER qmltestrunner HINTS "${Qt6_DIR}/../../../bin" REQUIRED)
    if (OKILTV_QMLTESTRUNNER)
        add_test(NAME OKILTVQtUiTransparencyTests
            COMMAND "${OKILTV_QMLTESTRUNNER}" -input "${CMAKE_CURRENT_SOURCE_DIR}/qml/tst_uitransparency.qml")
        set_tests_properties(OKILTVQtUiTransparencyTests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
        add_test(NAME OKILTVQtProgrammeTimelineTests
            COMMAND "${OKILTV_QMLTESTRUNNER}" -input "${CMAKE_CURRENT_SOURCE_DIR}/qml/tst_programmetimeline.qml")
        set_tests_properties(OKILTVQtProgrammeTimelineTests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
        add_test(NAME OKILTVQtEpgTimelineTests
            COMMAND "${OKILTV_QMLTESTRUNNER}" -input "${CMAKE_CURRENT_SOURCE_DIR}/qml/tst_epgtimeline.qml")
        add_test(NAME OKILTVQtSourceGroupsListTests
            COMMAND "${OKILTV_QMLTESTRUNNER}" -input "${CMAKE_CURRENT_SOURCE_DIR}/qml/tst_sourcegroupslist.qml")
        set_tests_properties(OKILTVQtSourceGroupsListTests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
        set_tests_properties(OKILTVQtEpgTimelineTests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
    endif ()
endif ()

foreach(target OKILTVQtCoreTests OKILTVQtAppTests)
    target_link_libraries(${target} PRIVATE ${OKILTV_SECRET_LIB})
    target_compile_definitions(${target} PRIVATE OKILTV_SECURITY_TESTS)
endforeach()

qt_add_executable(OKILTVQtPlaybackTests
    tst_playback_components.cpp
    ../src/app/playback/playbackbuffering.cpp
    ../src/app/playback/playbackrecovery.cpp
    ../src/app/playback/catchupplaybacksession.cpp
    ../src/app/playback/catchupstandbytransition.cpp
    ../src/player/cachepolicy.cpp
    ../src/player/livebuffertuner.cpp
    ../src/core/models.cpp
    ../src/core/models_variant.cpp
    ../src/core/datetimeformat.cpp
    ../src/core/catchupurlresolver.cpp
    ../src/core/debuglogger.cpp
    ../src/core/appdatapaths.cpp
    ../src/core/redaction.cpp
)
target_include_directories(OKILTVQtPlaybackTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(OKILTVQtPlaybackTests PRIVATE OKILTV_APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(OKILTVQtPlaybackTests PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::Test)
add_test(NAME OKILTVQtPlaybackTests COMMAND OKILTVQtPlaybackTests)
set_tests_properties(OKILTVQtPlaybackTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;OKILTV_HEADLESS_TEST=1")

# Stable group names shared by local CTest runs and the PR workflow matrix.
set_tests_properties(OKILTVQtCoreTests OKILTVQtDatabaseStartupTests PROPERTIES LABELS core)
set_tests_properties(OKILTVQtAppTests PROPERTIES LABELS app TIMEOUT 600)
set_tests_properties(OKILTVQtPlaybackTests OKILTVQtCatchupTests PROPERTIES LABELS playback TIMEOUT 300)
if (WIN32)
    set_tests_properties(OKILTVQtWindowsProcessJobTests OKILTVQtWindowsDvrTests
        PROPERTIES LABELS windows TIMEOUT 120)
endif ()
if (NOT CMAKE_CROSSCOMPILING)
    set_tests_properties(OKILTVQtUiTransparencyTests OKILTVQtProgrammeTimelineTests
        OKILTVQtEpgTimelineTests OKILTVQtSourceGroupsListTests PROPERTIES LABELS qml TIMEOUT 120)
endif ()

option(OKILTV_BUILD_UI_TESTS "Register Linux UI tests using Xvfb and a disposable keyring" OFF)
if (OKILTV_BUILD_UI_TESTS)
    if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "OKILTV_BUILD_UI_TESTS requires a native Linux build")
    endif ()
    find_program(OKILTV_TEST_BASH bash REQUIRED)
    foreach(scenario
        05-channel-selection
        06-overlay-inactivity
        07-guide-groups
        08-stationary-pointer-navigation
        08-ui-transparency
        09-catchup-download
        10-multiview-keyboard
        11-multiview-controls)
        add_test(NAME "UI-${scenario}"
            COMMAND "${OKILTV_TEST_BASH}" "${CMAKE_SOURCE_DIR}/scripts/ci/run_ui_test.sh"
                "${CMAKE_SOURCE_DIR}/ui-tests/tests/${scenario}.py"
                "$<TARGET_FILE:OKILTVQt>"
                "${CMAKE_BINARY_DIR}/ui-artifacts/${scenario}")
        set_tests_properties("UI-${scenario}" PROPERTIES
            LABELS ui TIMEOUT 300 RESOURCE_LOCK okiltv_ui_desktop)
    endforeach()
endif ()

qt_add_executable(OKILTVQtDownloadTests
    ../src/core/m3uservice.cpp
    ../src/core/networkaccess.cpp
    tst_catchup_downloads.cpp
    ../src/core/epgcache_service.cpp
    ../src/core/epgstore.cpp
    ../src/player/catchuptsjoiner.cpp
    ../src/core/database_service.cpp
    ../src/app/catchupdownloadcontroller.cpp
    ../src/app/catchuparchivetransfer.cpp
    ../src/app/catchuparchivetransfer.h
    ../src/app/catchupdownloadtransfer.cpp
    ../src/app/catchupdownloadtransfer.h
    ../src/core/appdatapaths.cpp
    ../src/core/catchupurlresolver.cpp
    ../src/core/datetimeformat.cpp
    ../src/core/debuglogger.cpp
    ../src/core/models.cpp
    ../src/core/models_json.cpp
    ../src/core/models_variant.cpp
    ../src/core/portablebootstrap.cpp
    ../src/core/processutils.cpp
    ../src/core/redaction.cpp
    ../src/core/secretprotection.cpp
    ../src/core/settingsmanager.cpp
    ../src/core/sourcestore.cpp)
target_include_directories(OKILTVQtDownloadTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(OKILTVQtDownloadTests PRIVATE OKILTV_SECURITY_TESTS)
target_link_libraries(OKILTVQtDownloadTests PRIVATE Qt6::Widgets Qt6::Core Qt6::Network Qt6::Sql Qt6::Test ${OKILTV_SECRET_LIB})
add_test(NAME OKILTVQtDownloadTests COMMAND OKILTVQtDownloadTests)
# Real 10/30-second retry cooldowns and the 60-second stall case put this suite
# near nine minutes locally. Leave room for slower hosted runners.
set_tests_properties(OKILTVQtDownloadTests PROPERTIES LABELS app TIMEOUT 900
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;OKILTV_HEADLESS_TEST=1")

if (NOT CMAKE_CROSSCOMPILING AND OKILTV_QMLTESTRUNNER)
    add_test(NAME OKILTVQtDownloadUiTests COMMAND "${OKILTV_QMLTESTRUNNER}"
        -input "${CMAKE_CURRENT_SOURCE_DIR}/qml/tst_catchupdownloads.qml")
    set_tests_properties(OKILTVQtDownloadUiTests PROPERTIES LABELS qml TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
endif ()

if (NOT CMAKE_CROSSCOMPILING AND OKILTV_QMLTESTRUNNER)
    add_test(NAME OKILTVQtUpdateUiTests COMMAND "${OKILTV_QMLTESTRUNNER}"
        -input "${CMAKE_CURRENT_SOURCE_DIR}/qml/tst_updateavailable.qml")
    set_tests_properties(OKILTVQtUpdateUiTests PROPERTIES LABELS qml TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
endif ()
