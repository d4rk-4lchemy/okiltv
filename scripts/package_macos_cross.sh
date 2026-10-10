#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
source scripts/macos/environment.sh
"$MACOS_PYTHON" scripts/macos/preflight.py
exec "$MACOS_PYTHON" scripts/macos/package.py "$@"
