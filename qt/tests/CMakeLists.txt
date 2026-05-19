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
    ../src/core/epgservice.cpp
    ../src/core/epgservice.h
    ../src/core/m3uservice.cpp
    ../src/core/m3uservice.h
    ../src/core/models.cpp
    ../src/core/models_json.cpp
    ../src/core/models_variant.cpp
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
    ../src/app/playercontroller.cpp
    ../src/app/playercontroller.h
    ../src/app/profilesmodel.cpp
    ../src/app/profilesmodel.h
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
    ../src/core/epgservice.cpp
    ../src/core/epgservice.h
    ../src/core/iconcache_service.cpp
    ../src/core/iconcache_service.h
    ../src/core/m3uservice.cpp
    ../src/core/m3uservice.h
    ../src/core/models.cpp
    ../src/core/models_json.cpp
    ../src/core/models_variant.cpp
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
    ../src/core/xtreamservice.cpp
    ../src/core/xtreamservice.h
    ../src/player/mpvplayer.cpp
    ../src/player/mpvplayer.h
)

target_include_directories(OKILTVQtAppTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

target_link_libraries(OKILTVQtAppTests PRIVATE
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
