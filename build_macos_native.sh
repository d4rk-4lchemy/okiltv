#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$repo_root"
if [[ "$(uname -s)" != Darwin || "$(uname -m)" != arm64 ]]; then
    echo "The native macOS beta build requires an Apple Silicon Mac." >&2
    exit 2
fi
: "${QT_MACOS_SDK_ROOT:?Supply the macOS Qt SDK; see docs/macos-build.md}"
source scripts/macos/dependencies.env
export OKILTV_MACOS_QT_VERSION
export QT_HOST_PATH="$QT_MACOS_SDK_ROOT"
MACOS_DEPS_ROOT="$(brew --prefix)"
MACOS_SDK_ROOT="$(xcrun --sdk macosx --show-sdk-path)"
export MACOS_DEPS_ROOT MACOS_SDK_ROOT
export MACOS_MIN_VERSION="$OKILTV_MACOS_MIN_VERSION"
export MACOS_PYTHON="${MACOS_PYTHON:-python3}"
export PATH="$QT_MACOS_SDK_ROOT/bin:$MACOS_DEPS_ROOT/opt/ffmpeg/bin:$PATH"
"$MACOS_PYTHON" -c 'import macholib'
qt_version="$("$QT_MACOS_SDK_ROOT/bin/qtpaths" --qt-version)"
if [[ "$qt_version" != "$OKILTV_MACOS_QT_VERSION" ]]; then
    echo "Expected Qt $OKILTV_MACOS_QT_VERSION, found $qt_version." >&2
    exit 2
fi
preset=qt-macos-arm64-native-release
log_dir="$repo_root/qt/out/build/$preset/logs"
mkdir -p "$log_dir"
cmake --fresh --preset "$preset" 2>&1 | tee "$log_dir/configure.log"
cmake --build --preset "$preset" --target OKILTVQt OKILTVQtMacosPlatformTests \
    -j"$(scripts/build_jobs.sh)" 2>&1 | tee "$log_dir/build.log"
package_args=(--native --platform-test "$repo_root/qt/out/build/$preset/qt/tests/OKILTVQtMacosPlatformTests")
if [[ -n "${APP_VERSION:-}" ]]; then
    package_args+=(--expected-version "$APP_VERSION")
fi
"$MACOS_PYTHON" scripts/macos/package.py "${package_args[@]}" 2>&1 | tee "$log_dir/package.log"
