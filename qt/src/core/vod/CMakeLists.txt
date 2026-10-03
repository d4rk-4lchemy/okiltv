add_library(OKILTVVodDomain STATIC vodmodels.cpp)
target_link_libraries(OKILTVVodDomain PUBLIC Qt6::Core)
target_include_directories(OKILTVVodDomain PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_compile_features(OKILTVVodDomain PUBLIC cxx_std_20)

add_library(OKILTVVodInfrastructure STATIC providers/httptransport.cpp providers/xtreamvodprovider.cpp
    storage/sqlitevodstore.cpp storage/storagecodec.cpp)
target_link_libraries(OKILTVVodInfrastructure PUBLIC OKILTVVodDomain PRIVATE Qt6::Network Qt6::Sql ${OKILTV_ZLIB_LINK_LIB})
target_compile_features(OKILTVVodInfrastructure PUBLIC cxx_std_20)
