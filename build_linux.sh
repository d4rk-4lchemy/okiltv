#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$repo_root/qt/out/build/qt-linux-release"
build_jobs="$("$repo_root/scripts/build_jobs.sh")"

if [[ -z "${QT_LINUX_SDK_ROOT:-}" ]]; then
    if [[ -d "/opt/Qt/6.10.3/gcc_64" ]]; then
        QT_LINUX_SDK_ROOT="/opt/Qt/6.10.3/gcc_64"
    elif [[ -d "/home/dev/Qt/6.10.3/gcc_64" ]]; then
        QT_LINUX_SDK_ROOT="/home/dev/Qt/6.10.3/gcc_64"
    fi
fi

if [[ -n "${QT_LINUX_SDK_ROOT:-}" ]]; then
    export QT_LINUX_SDK_ROOT
    export PATH="$QT_LINUX_SDK_ROOT/bin:$PATH"
    export CMAKE_PREFIX_PATH="${QT_LINUX_SDK_ROOT}${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
fi

needs_reconfigure=0
if [[ ! -f "$build_dir/CMakeCache.txt" ]]; then
    needs_reconfigure=1
elif [[ ! -f "$build_dir/build.ninja" ]]; then
    needs_reconfigure=1
elif ! rg -q '^CMAKE_MAKE_PROGRAM:FILEPATH=' "$build_dir/CMakeCache.txt"; then
    needs_reconfigure=1
elif [[ -n "${QT_LINUX_SDK_ROOT:-}" ]]; then
    cached_prefix="$(sed -n 's#^CMAKE_PREFIX_PATH:UNINITIALIZED=##p' "$build_dir/CMakeCache.txt" | head -n 1)"
    if [[ -z "$cached_prefix" || "$cached_prefix" != *"$QT_LINUX_SDK_ROOT"* ]]; then
        needs_reconfigure=1
    fi
fi

if [[ "$needs_reconfigure" -eq 1 ]]; then
    cmake --fresh --preset qt-linux-release
fi

cmake --build --preset qt-linux-release -j"$build_jobs"
