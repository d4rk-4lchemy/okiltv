#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/ci/dependencies.env
source "$repo_root/scripts/ci/dependencies.env"
build_dir="${1:-$repo_root/qt/out/build/qt-linux-release}"
stage_parent="$repo_root/qt/out/package/qt-linux-release"
app_dir="$stage_parent/AppDir"
tools_dir="$repo_root/qt/out/tools/appimage"
publish_dir="$repo_root/publish"
app_version="$(sed -n 's/^project(OKILTVQt VERSION \([0-9.]*\).*/\1/p' "$repo_root/qt/CMakeLists.txt")"
qt_root="${QT_LINUX_SDK_ROOT:?Set QT_LINUX_SDK_ROOT to the Qt Linux SDK directory}"

fetch_tool() {
    local name="$1" url="$2" digest="$3"
    if [[ ! -f "$tools_dir/$name" ]]; then
        curl --fail --location --retry 3 "$url" -o "$tools_dir/$name.part"
        mv "$tools_dir/$name.part" "$tools_dir/$name"
    fi
    printf '%s  %s\n' "$digest" "$tools_dir/$name" | sha256sum --check --status
    chmod +x "$tools_dir/$name"
}

system_library() {
    local path
    # libmpv and libsecret are loaded via QLibrary, so ELF scanning cannot find them.
    path="$(/sbin/ldconfig -p | awk -v name="$1" '$1 == name && /x86-64/ && !found {print $NF; found=1}')"
    if [[ ! -f "$path" ]]; then
        echo "Required runtime library not found: $1" >&2
        exit 1
    fi
    printf '%s\n' "$path"
}

test "$(uname -m)" = x86_64
test -x "$build_dir/app/OKILTV"
test -x "$qt_root/bin/qmake"
mkdir -p "$tools_dir" "$publish_dir" "$stage_parent"
fetch_tool linuxdeploy-x86_64.AppImage \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/$LINUXDEPLOY_VERSION/linuxdeploy-x86_64.AppImage" \
    "$LINUXDEPLOY_SHA256"
fetch_tool linuxdeploy-plugin-qt-x86_64.AppImage \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/$LINUXDEPLOY_QT_VERSION/linuxdeploy-plugin-qt-x86_64.AppImage" \
    "$LINUXDEPLOY_QT_SHA256"

rm -rf "$app_dir"
mkdir -p "$app_dir/usr/share/icons/hicolor/256x256/apps"
convert "$repo_root/qt/resources/icons/app.png" -resize 256x256 \
    "$app_dir/usr/share/icons/hicolor/256x256/apps/OKILTV.png"

# The SDK also ships drivers for Oracle, Mimer, MySQL, etc. linuxdeploy deploys
# all SQL plugins, but OKILTV only uses SQLite; those other clients may not exist.
plugin_dir="$stage_parent/qt-plugins"
rm -rf "$plugin_dir"
cp -a "$qt_root/plugins" "$plugin_dir"
find "$plugin_dir/sqldrivers" -type f ! -name libqsqlite.so -delete
test -f "$plugin_dir/sqldrivers/libqsqlite.so"
install -m 0755 "$repo_root/scripts/ci/qmake_appimage.sh" "$stage_parent/qmake-appimage"

export PATH="$tools_dir:$qt_root/bin:$PATH"
export LD_LIBRARY_PATH="$qt_root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export OKILTV_REAL_QMAKE="$qt_root/bin/qmake"
export OKILTV_QT_PLUGIN_DIR="$plugin_dir"
export QMAKE="$stage_parent/qmake-appimage"
export QML_SOURCES_PATHS="$repo_root/qt/qml"
export EXTRA_QT_MODULES="svg;sql;network"
export APPIMAGE_EXTRACT_AND_RUN=1
export ARCH=x86_64
export VERSION="$app_version"
export OUTPUT="$publish_dir/OKILTV-qt-linux-x86_64-$app_version.AppImage"

"$tools_dir/linuxdeploy-x86_64.AppImage" \
    --appdir "$app_dir" \
    --executable "$build_dir/app/OKILTV" \
    --library "$(system_library libmpv.so.2)" \
    --library "$(system_library libsecret-1.so.0)" \
    --desktop-file "$repo_root/scripts/linux/OKILTV.desktop" \
    --icon-file "$app_dir/usr/share/icons/hicolor/256x256/apps/OKILTV.png" \
    --plugin qt --output appimage

test -s "$OUTPUT"
test -f "$app_dir/usr/lib/libmpv.so.2"
test -f "$app_dir/usr/lib/libsecret-1.so.0"
echo "AppImage: $OUTPUT"
