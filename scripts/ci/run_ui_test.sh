#!/usr/bin/env bash
set -euo pipefail

scenario="$(realpath "${1:?Pass the UI scenario}")"
app="$(realpath "${2:?Pass the application binary}")"
run_dir="$(realpath -m "${3:?Pass an artifact directory}")"
session_dir="$(mktemp -d)"
trap 'rm -rf "$session_dir"' EXIT

# Real application storage uses Secret Service. Give each test a separate bus,
# unlocked disposable keyring and XDG directories instead of bypassing storage.
export XDG_DATA_HOME="$session_dir/data"
export XDG_CONFIG_HOME="$session_dir/config"
export XDG_CACHE_HOME="$session_dir/cache"
export XDG_RUNTIME_DIR="$session_dir/runtime"
mkdir -m 0700 "$XDG_RUNTIME_DIR"
export QT_QPA_PLATFORM=xcb
export LIBGL_ALWAYS_SOFTWARE=1
unset QT_QUICK_BACKEND OKILTV_HEADLESS_TEST

# The positional arguments are intentionally expanded in the inner shell.
# shellcheck disable=SC2016
dbus-run-session -- bash -euo pipefail -c '
    printf "%s" "disposable-ci-keyring" | gnome-keyring-daemon --unlock --components=secrets
    exec python3 -u "$1" --app-bin "$2" --run-dir "$3"
' _ "$scenario" "$app" "$run_dir"
