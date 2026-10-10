# Linux host tools, Darwin libraries. Do not chainload OSXCross's launcher-only
# toolchain: discover the installed SDK-specific wrappers instead.
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR arm64)

foreach(name OSXCROSS_ROOT MACOS_SDK_ROOT QT_MACOS_SDK_ROOT QT_HOST_PATH MACOS_DEPS_ROOT MACOS_MIN_VERSION)
    if ("$ENV{${name}}" STREQUAL "")
        message(FATAL_ERROR "macOS cross build requires ${name}; see docs/macos-build.md")
    endif ()
endforeach()

file(GLOB OKILTV_DARWIN_COMPILERS "$ENV{OSXCROSS_ROOT}/bin/arm64-apple-darwin*-clang++")
list(LENGTH OKILTV_DARWIN_COMPILERS OKILTV_DARWIN_COMPILER_COUNT)
if (NOT OKILTV_DARWIN_COMPILER_COUNT EQUAL 1)
    message(FATAL_ERROR "Expected one arm64 Darwin clang++ wrapper in OSXCROSS_ROOT/bin")
endif ()
list(GET OKILTV_DARWIN_COMPILERS 0 CMAKE_CXX_COMPILER)
set(CMAKE_OBJCXX_COMPILER "${CMAKE_CXX_COMPILER}")
string(REGEX REPLACE "clang\\+\\+$" "" OKILTV_DARWIN_TOOL_PREFIX "${CMAKE_CXX_COMPILER}")
set(CMAKE_C_COMPILER "${OKILTV_DARWIN_TOOL_PREFIX}clang")
set(CMAKE_AR "${OKILTV_DARWIN_TOOL_PREFIX}ar")
set(CMAKE_RANLIB "${OKILTV_DARWIN_TOOL_PREFIX}ranlib")
set(CMAKE_INSTALL_NAME_TOOL "${OKILTV_DARWIN_TOOL_PREFIX}install_name_tool")

set(CMAKE_OSX_SYSROOT "$ENV{MACOS_SDK_ROOT}" CACHE PATH "Apple SDK")
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Target architecture")
set(CMAKE_OSX_DEPLOYMENT_TARGET "$ENV{MACOS_MIN_VERSION}" CACHE STRING "Verified dependency OS floor")
set(CMAKE_FIND_ROOT_PATH "$ENV{QT_MACOS_SDK_ROOT};$ENV{MACOS_DEPS_ROOT};$ENV{MACOS_SDK_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_PREFIX_PATH "$ENV{QT_MACOS_SDK_ROOT};$ENV{MACOS_DEPS_ROOT}/opt/openssl@3" CACHE STRING "Darwin packages")
set(OPENSSL_ROOT_DIR "$ENV{MACOS_DEPS_ROOT}/opt/openssl@3" CACHE PATH "Darwin OpenSSL")
set(QT_HOST_PATH "$ENV{QT_HOST_PATH}" CACHE PATH "Linux Qt generators")
set(QT_HOST_PATH_CMAKE_DIR "$ENV{QT_HOST_PATH}/lib/cmake" CACHE PATH "Linux Qt tool packages")

# Wrappers use the installed libc++ and linker; all host tools remain executable
# on Linux. Keep the same environment available to compiler/linker invocations.
