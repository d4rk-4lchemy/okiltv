#!/bin/bash
set -euo pipefail
test_root="$(cd -- "$(dirname -- "$0")" && pwd)"
mkdir -p "$test_root/diagnostics"
"$test_root/OKILTV.app/Contents/MacOS/OKILTVQtMacosPlatformTests" -v2 2>&1 | tee "$test_root/diagnostics/platform-tests.log"
