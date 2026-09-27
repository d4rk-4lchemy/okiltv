add_library(OKILTVVodApplication STATIC
    vodjobrunner.cpp vodservices.cpp vodprogressservice.cpp vodcontroller.cpp vodmodule.cpp
    ../playback/legacyplaybackadapter.cpp ../playback/playbackcoordinator.cpp
    ../playback/vodplaybacksession.cpp)
target_link_libraries(OKILTVVodApplication PUBLIC OKILTVVodDomain PRIVATE Qt6::Concurrent)
target_compile_features(OKILTVVodApplication PUBLIC cxx_std_20)
