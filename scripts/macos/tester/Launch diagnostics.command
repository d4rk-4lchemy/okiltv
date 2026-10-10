#!/bin/bash
set -euo pipefail
test_root="$(cd -- "$(dirname -- "$0")" && pwd)"
diagnostics="$test_root/diagnostics"
mkdir -p "$diagnostics"
/usr/bin/sw_vers > "$diagnostics/system.txt"
/usr/bin/uname -m >> "$diagnostics/system.txt"
echo "Launching OKILTV; stdout/stderr go to $diagnostics/launch.log"
echo "Use the app's explicit diagnostic dump for its redacted playback/context log."
"$test_root/OKILTV.app/Contents/MacOS/OKILTV" > "$diagnostics/launch.log" 2>&1
