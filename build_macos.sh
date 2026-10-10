#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$repo_root"
source scripts/macos/environment.sh
"$MACOS_PYTHON" scripts/macos/preflight.py
preset=qt-macos-arm64-cross-release
log_dir="$repo_root/qt/out/build/$preset/logs"
mkdir -p "$log_dir"
cmake --fresh --preset "$preset" 2>&1 | tee "$log_dir/configure.log"
cmake --build --preset "$preset" --target OKILTVQt -j"$(scripts/build_jobs.sh)" 2>&1 | tee "$log_dir/build.log"
package_args=()
if [[ "${OKILTV_MACOS_BUILD_PLATFORM_TESTS:-1}" == 1 ]]; then
    cmake --fresh --preset qt-macos-arm64-cross-debug -DOKILTV_BUILD_TESTS=ON 2>&1 | tee "$log_dir/test-configure.log"
    cmake --build --preset qt-macos-arm64-cross-debug --target OKILTVQtMacosPlatformTests \
        -j"$(scripts/build_jobs.sh)" 2>&1 | tee "$log_dir/test-build.log"
    package_args+=(--platform-test "$repo_root/qt/out/build/qt-macos-arm64-cross-debug/qt/tests/OKILTVQtMacosPlatformTests")
fi
bash scripts/package_macos_cross.sh "${package_args[@]}" 2>&1 | tee "$log_dir/package.log"
