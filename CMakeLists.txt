cmake_minimum_required(VERSION 3.25)

project(OKILTVWorkspace LANGUAGES CXX)

if (OKILTV_BUILD_TESTS)
    enable_testing()
endif ()

add_subdirectory(qt)
