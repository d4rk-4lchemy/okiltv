#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
OUT_DIR="$ROOT/publish/qt-win-x64"
MPV_DLL="$SCRIPT_DIR/mpv/mpv-2.dll"
QT_SDK_VERSION="${QT_SDK_VERSION:-6.10.3}"
QT_HOST_PATH="${QT_HOST_PATH:-/home/dev/Qt/$QT_SDK_VERSION/gcc_64}"
QT_WIN_SDK_ROOT="${QT_WIN_SDK_ROOT:-/home/dev/Qt/$QT_SDK_VERSION/mingw_64}"
MINGW_CXX="${MINGW_CXX:-x86_64-w64-mingw32-g++-posix}"

ensure_mpv_dll() {
  if [ -f "$MPV_DLL" ]; then
    echo "Using local mpv: $MPV_DLL"
    return
  fi

  echo "mpv-2.dll not found at $MPV_DLL"
  echo "Attempting download..."

  MPV_URL="https://sourceforge.net/projects/mpv-player-windows/files/libmpv/mpv-dev-x86_64-v3-20240317-git-a62a0b3.7z/download"
  TMP_DIR=$(mktemp -d)
  trap 'rm -rf "$TMP_DIR"' EXIT

  curl -fL "$MPV_URL" -o "$TMP_DIR/mpv.7z"
  mkdir -p "$SCRIPT_DIR/mpv"
  7z e "$TMP_DIR/mpv.7z" "mpv-2.dll" -o"$SCRIPT_DIR/mpv/" -y

  if [ ! -f "$MPV_DLL" ]; then
    echo "Failed to obtain mpv-2.dll. Place it manually at $MPV_DLL and re-run." >&2
    exit 1
  fi
}

deploy_qt_runtime() {
  local app_dir="$OUT_DIR/app"
  local qt_bin="$QT_WIN_SDK_ROOT/bin"
  local qt_plugins="$QT_WIN_SDK_ROOT/plugins"
  local qt_qml="$QT_WIN_SDK_ROOT/qml"

  if [ ! -d "$qt_bin" ] || [ ! -d "$qt_plugins" ] || [ ! -d "$qt_qml" ]; then
    echo "Windows Qt SDK at $QT_WIN_SDK_ROOT is missing expected runtime folders." >&2
    exit 1
  fi

  mkdir -p "$app_dir/plugins" "$app_dir/qml"

  cp "$qt_bin"/Qt6*.dll "$app_dir/"
  copy_mingw_runtime_dll "$app_dir" libgcc_s_seh-1.dll
  copy_mingw_runtime_dll "$app_dir" libssp-0.dll
  copy_mingw_runtime_dll "$app_dir" libstdc++-6.dll
  copy_mingw_runtime_dll "$app_dir" libwinpthread-1.dll

  if [ -f "$qt_bin/d3dcompiler_47.dll" ]; then
    cp "$qt_bin/d3dcompiler_47.dll" "$app_dir/"
  fi

  if [ "${OKILTV_INCLUDE_SOFTWARE_OPENGL:-0}" = "1" ] && [ -f "$qt_bin/opengl32sw.dll" ]; then
    cp "$qt_bin/opengl32sw.dll" "$app_dir/"
  fi

  for plugin_dir in iconengines imageformats platforms sqldrivers styles tls; do
    if [ -d "$qt_plugins/$plugin_dir" ]; then
      mkdir -p "$app_dir/plugins/$plugin_dir"
      cp -R "$qt_plugins/$plugin_dir/." "$app_dir/plugins/$plugin_dir/"
    fi
  done

  for qml_dir in QtCore QtQml QtQuick; do
    if [ -d "$qt_qml/$qml_dir" ]; then
      mkdir -p "$app_dir/qml/$qml_dir"
      cp -R "$qt_qml/$qml_dir/." "$app_dir/qml/$qml_dir/"
    fi
  done

  cat > "$app_dir/qt.conf" <<'EOF'
[Paths]
Prefix=.
Plugins=plugins
QmlImports=qml
Imports=qml
EOF
}

copy_mingw_runtime_dll() {
  local destination_dir="$1"
  local dll_name="$2"
  local dll_path

  dll_path="$("$MINGW_CXX" -print-file-name="$dll_name")"
  if [ -z "$dll_path" ] || [ "$dll_path" = "$dll_name" ] || [ ! -f "$dll_path" ]; then
    echo "Required MinGW runtime DLL not found: $dll_name" >&2
    exit 1
  fi

  cp "$dll_path" "$destination_dir/"
}

ensure_mpv_dll

if [ ! -d "$QT_WIN_SDK_ROOT" ]; then
  echo "Windows Qt SDK not found at $QT_WIN_SDK_ROOT" >&2
  echo "Install it there or set QT_WIN_SDK_ROOT before running this script." >&2
  exit 1
fi

export QT_WIN_SDK_ROOT
export QT_HOST_PATH

cmake --fresh --preset qt-win64-release
cmake --build --preset qt-win64-release

APP_DIR="$ROOT/qt/out/build/qt-win64-release/app"
EXE="$APP_DIR/OKILTV.exe"

if [ ! -f "$EXE" ]; then
  echo "Qt build completed but expected executable was not found at $EXE" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
rm -rf "$OUT_DIR/app"
mkdir -p "$OUT_DIR/app"
cp -R "$APP_DIR/"* "$OUT_DIR/app/"

deploy_qt_runtime

if [ -f "$MPV_DLL" ]; then
  cp "$MPV_DLL" "$OUT_DIR/app/mpv-2.dll"
else
  echo "Warning: build/mpv/mpv-2.dll is missing; deployed folder will not play video until it is supplied." >&2
fi

ARCHIVE="$OUT_DIR/OKILTV-qt-win-x64.zip"
rm -f "$ARCHIVE"
(
  cd "$OUT_DIR/app"
  7z a -tzip "$ARCHIVE" ./* >/dev/null
)

echo ""
echo "Qt folder deployment complete -> $OUT_DIR/app"
echo "Zip artifact -> $ARCHIVE"
