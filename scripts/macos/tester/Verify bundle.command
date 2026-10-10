#!/bin/bash
set -euo pipefail
test_root="$(cd -- "$(dirname -- "$0")" && pwd)"
cd "$test_root"
/usr/bin/shasum -a 256 -c SHA256SUMS
/usr/bin/codesign --verify --deep --strict --verbose=2 OKILTV.app
"$test_root/OKILTV.app/Contents/MacOS/ffmpeg" -version
"$test_root/OKILTV.app/Contents/MacOS/ffprobe" -version
echo "Checks passed. Finder launch and playback still need the tester checklist."
