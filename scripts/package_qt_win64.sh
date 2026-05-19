#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

build_dir="${1:-$repo_root/qt/out/build/qt-win64-release}"
stage_root="${2:-$repo_root/qt/out/package/qt-win64-release/app}"
publish_dir="${3:-$repo_root/publish}"

stage_parent="$(dirname "$stage_root")"
cache_file="$build_dir/CMakeCache.txt"
qml_dir="$repo_root/qt/qml"
mpv_dll_path="${MPV_DLL_PATH:-$repo_root/build/mpv/mpv-2.dll}"
wine_prefix="${WINEPREFIX:-$repo_root/qt/out/wineprefix-qt-win64}"
wine_debug="${WINEDEBUG:--all}"
app_version="${APP_VERSION:-$(sed -n 's/^project(OKILTVQt VERSION \([0-9.]*\).*/\1/p' "$repo_root/qt/CMakeLists.txt" | head -n 1)}"
zip_path="${publish_dir}/OKILTV-qt-win-x64-${app_version}.zip"
installer_path="${publish_dir}/OKILTV-qt-win-x64-setup-${app_version}.exe"
portable_path="${publish_dir}/OKILTV-qt-win-x64-portable-${app_version}.exe"
package_mode="${PACKAGE_QT_WIN64_MODE:-full}"

require_file() {
    local path="$1"
    if [[ ! -f "$path" ]]; then
        echo "Required file not found: $path" >&2
        exit 1
    fi
}

require_command() {
    local name="$1"
    if ! command -v "$name" >/dev/null 2>&1; then
        echo "Required command not found: $name" >&2
        exit 1
    fi
}

cache_value() {
    local key="$1"
    awk -F= -v key="$key" '$1 ~ "^" key ":" { print $2; exit }' "$cache_file"
}

copy_runtime_dll() {
    local compiler="$1"
    local dll_name="$2"
    local dll_path

    dll_path="$("$compiler" -print-file-name="$dll_name")"
    if [[ -z "$dll_path" || "$dll_path" == "$dll_name" || ! -f "$dll_path" ]]; then
        echo "Unable to locate runtime DLL: $dll_name" >&2
        exit 1
    fi

    cp -f "$dll_path" "$stage_root/$dll_name"
}

stage_app_bundle() {
    local toolchain_file qt_host_path c_compiler qt_win_sdk_root windeployqt_exe exe_path optional_path

    require_file "$cache_file"

    toolchain_file="$(cache_value CMAKE_TOOLCHAIN_FILE)"
    qt_host_path="$(cache_value QT_HOST_PATH)"
    c_compiler="$(cache_value CMAKE_C_COMPILER)"

    if [[ -z "$toolchain_file" || -z "$qt_host_path" || -z "$c_compiler" ]]; then
        echo "Missing required values in $cache_file" >&2
        exit 1
    fi

    if [[ "$c_compiler" != /* ]]; then
        c_compiler="$(command -v "$c_compiler")"
    fi

    require_file "$c_compiler"

    qt_win_sdk_root="$(cd "$(dirname "$toolchain_file")/../../.." && pwd)"
    windeployqt_exe="$qt_win_sdk_root/bin/windeployqt.exe"
    exe_path="$build_dir/app/OKILTV.exe"

    require_file "$windeployqt_exe"
    require_file "$exe_path"
    require_file "$mpv_dll_path"

    rm -rf "$stage_parent"
    mkdir -p "$stage_root"

    cp -f "$exe_path" "$stage_root/OKILTV.exe"
    cp -f "$mpv_dll_path" "$stage_root/mpv-2.dll"

    WINEPREFIX="$wine_prefix" WINEDEBUG="$wine_debug" wine "$windeployqt_exe" \
        --release \
        --no-translations \
        --qmldir "$qml_dir" \
        "$stage_root/OKILTV.exe"

    cat > "$stage_root/qt.conf" <<'EOF'
[Paths]
Prefix=.
Plugins=.
QmlImports=qml
Imports=qml
EOF

    for dll_name in \
        libgcc_s_seh-1.dll \
        libstdc++-6.dll \
        libwinpthread-1.dll \
        libssp-0.dll
    do
        copy_runtime_dll "$c_compiler" "$dll_name"
    done

    for optional_dll in dxcompiler.dll dxil.dll; do
        optional_path="$(find "$qt_win_sdk_root" -name "$optional_dll" -print -quit)"
        if [[ -n "$optional_path" && -f "$optional_path" ]]; then
            cp -f "$optional_path" "$stage_root/$optional_dll"
        fi
    done
}

package_zip() {
    require_command 7z
    mkdir -p "$publish_dir"
    rm -f "$zip_path"
    (
        cd "$stage_parent"
        7z a -tzip "$zip_path" app >/dev/null
    )
}

package_installer() {
    local config_template config_path

    require_command cpack
    require_command makensis
    mkdir -p "$publish_dir"

    config_template="$repo_root/scripts/windows/cpack_nsis_config.cmake.in"
    config_path="$stage_parent/CPackConfig-installer.cmake"
    require_file "$config_template"

    sed \
        -e "s#@PACKAGE_NAME@#OKILTV#g" \
        -e "s#@PACKAGE_VERSION@#${app_version}#g" \
        -e "s#@PACKAGE_FILE_NAME@#$(basename "$installer_path" .exe)#g" \
        -e "s#@OUTPUT_DIR@#${publish_dir}#g" \
        -e "s#@LICENSE_FILE@#${repo_root}/scripts/windows/installer_license.txt#g" \
        -e "s#@STAGE_ROOT@#${stage_root}#g" \
        "$config_template" > "$config_path"

    rm -f "$installer_path"
    cpack --config "$config_path"
}

package_portable() {
    local nsis_script app_icon

    require_command makensis
    mkdir -p "$publish_dir"
    nsis_script="$repo_root/scripts/windows/portable_launcher.nsi"
    app_icon="${PORTABLE_APP_ICON_PATH:-$repo_root/qt/resources/icons/app.ico}"
    require_file "$nsis_script"
    require_file "$app_icon"
    echo "Portable launcher icon: $app_icon"

    rm -f "$portable_path"
    makensis \
        -DAPP_DIR="$stage_root" \
        -DAPP_VERSION="$app_version" \
        -DAPP_ICON="$app_icon" \
        -DOUTPUT_FILE="$portable_path" \
        "$nsis_script" >/dev/null
}

verify_outputs() {
    if [[ "$package_mode" == "full" || "$package_mode" == "zip" ]]; then
        require_file "$zip_path"
    fi
    if [[ "$package_mode" == "full" ]]; then
        require_file "$installer_path"
        require_file "$portable_path"
    elif [[ "$package_mode" == "portable" ]]; then
        require_file "$portable_path"
    fi
}

cleanup_publish() {
    rm -rf "$publish_dir/qt-win-x64" "$publish_dir/qt-win-x64-redesign"
}

cleanup_generated() {
    rm -rf "$repo_root/_CPack_Packages"
}

stage_app_bundle
if [[ "$package_mode" == "full" || "$package_mode" == "zip" ]]; then
    package_zip
fi
if [[ "$package_mode" == "full" ]]; then
    package_installer
    package_portable
elif [[ "$package_mode" == "portable" ]]; then
    package_portable
elif [[ "$package_mode" != "zip" ]]; then
    echo "Unsupported PACKAGE_QT_WIN64_MODE: $package_mode" >&2
    exit 1
fi
verify_outputs
cleanup_publish
cleanup_generated

echo "Stage bundle: $stage_root"
if [[ "$package_mode" == "full" || "$package_mode" == "zip" ]]; then
    echo "ZIP: $zip_path"
fi
if [[ "$package_mode" == "full" ]]; then
    echo "Installer: $installer_path"
    echo "Portable: $portable_path"
elif [[ "$package_mode" == "portable" ]]; then
    echo "Portable: $portable_path"
fi
