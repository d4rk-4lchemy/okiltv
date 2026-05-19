#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$repo_root/qt/out/build/qt-win64-release"
publish_dir="$repo_root/publish"
app_version="${APP_VERSION:-$(sed -n 's/^project(OKILTVQt VERSION \([0-9.]*\).*/\1/p' "$repo_root/qt/CMakeLists.txt" | head -n 1)}"

QT_WIN_SDK_ROOT="${QT_WIN_SDK_ROOT:-/opt/Qt/6.10.3/mingw_64}"
QT_HOST_PATH="${QT_HOST_PATH:-/opt/Qt/6.10.3/gcc_64}"

export QT_WIN_SDK_ROOT
export QT_HOST_PATH
build_jobs="$("$repo_root/scripts/build_jobs.sh")"

if [[ ! -f "$QT_WIN_SDK_ROOT/lib/cmake/Qt6/qt.toolchain.cmake" ]]; then
    echo "Qt Windows SDK toolchain not found at: $QT_WIN_SDK_ROOT/lib/cmake/Qt6/qt.toolchain.cmake" >&2
    echo "Set QT_WIN_SDK_ROOT to a valid Qt MinGW SDK root." >&2
    exit 1
fi
if [[ ! -d "$QT_HOST_PATH/bin" ]]; then
    echo "Qt host path is invalid (missing bin/): $QT_HOST_PATH" >&2
    echo "Set QT_HOST_PATH to a valid host Qt install." >&2
    exit 1
fi

needs_reconfigure=0
if [[ ! -f "$build_dir/CMakeCache.txt" ]]; then
    needs_reconfigure=1
elif [[ ! -f "$build_dir/build.ninja" ]]; then
    needs_reconfigure=1
elif ! rg -q '^CMAKE_MAKE_PROGRAM:FILEPATH=' "$build_dir/CMakeCache.txt"; then
    needs_reconfigure=1
elif ! rg -q '^CMAKE_TOOLCHAIN_FILE:FILEPATH=' "$build_dir/CMakeCache.txt"; then
    needs_reconfigure=1
else
    cached_toolchain="$(sed -n 's#^CMAKE_TOOLCHAIN_FILE:FILEPATH=##p' "$build_dir/CMakeCache.txt" | head -n 1)"
    expected_toolchain="$QT_WIN_SDK_ROOT/lib/cmake/Qt6/qt.toolchain.cmake"
    if [[ -z "$cached_toolchain" || ! -f "$cached_toolchain" || "$cached_toolchain" != "$expected_toolchain" ]]; then
        needs_reconfigure=1
    fi
fi

if [[ "$needs_reconfigure" -eq 1 ]]; then
    cmake --fresh --preset qt-win64-release
fi

cmake --build --preset qt-win64-release -j"$build_jobs"
APP_VERSION="$app_version" PACKAGE_QT_WIN64_MODE=zip scripts/package_qt_win64.sh "$build_dir"

zip_path="$publish_dir/OKILTV-qt-win-x64-${app_version}.zip"

test -f "$zip_path"

7z l "$zip_path" | sed -n '1,24p'
ls -lh "$zip_path"
