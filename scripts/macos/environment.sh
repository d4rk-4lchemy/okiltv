# Shared by Linux build/package entry points. No downloads or target execution.
if [[ -z "${QT_MACOS_SDK_ROOT:-}" && -f "${OKILTV_MACOS_ENV_FILE:-/opt/okiltv-macos/env.sh}" ]]; then
    declare -A okiltv_macos_overrides=()
    for variable in QT_HOST_PATH MACOS_SDK_ROOT OSXCROSS_ROOT MACOS_DEPS_ROOT MACOS_MIN_VERSION OKILTV_BUILD_JOBS MACOS_PYTHON; do
        if [[ -n "${!variable:-}" ]]; then
            okiltv_macos_overrides[$variable]="${!variable}"
        fi
    done
    source "${OKILTV_MACOS_ENV_FILE:-/opt/okiltv-macos/env.sh}"
    for variable in "${!okiltv_macos_overrides[@]}"; do
        export "$variable=${okiltv_macos_overrides[$variable]}"
    done
    unset okiltv_macos_overrides
fi
for variable in QT_MACOS_SDK_ROOT QT_HOST_PATH MACOS_SDK_ROOT OSXCROSS_ROOT MACOS_DEPS_ROOT MACOS_MIN_VERSION; do
    if [[ -z "${!variable:-}" ]]; then
        echo "Missing $variable; see docs/macos-build.md" >&2
        return 2
    fi
done
export PATH="$OSXCROSS_ROOT/bin:$QT_HOST_PATH/bin:$PATH"
export MACOSX_DEPLOYMENT_TARGET="$MACOS_MIN_VERSION"
export MACOS_PYTHON="${MACOS_PYTHON:-python3}"
if ! "$MACOS_PYTHON" -c 'import macholib' >/dev/null 2>&1; then
    echo "Host Python requires macholib==1.16.4; activate the cross-build venv or set MACOS_PYTHON." >&2
    return 2
fi
