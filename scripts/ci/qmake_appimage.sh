#!/usr/bin/env bash
set -euo pipefail

# Give linuxdeploy a private plugin tree, keeping the installed SDK untouched.
# All other qmake paths still point at the real SDK (QML, libraries, tools, etc.).
: "${OKILTV_REAL_QMAKE:?}"
: "${OKILTV_QT_PLUGIN_DIR:?}"
if [[ "$#" -eq 1 && "$1" == -query ]]; then
    "$OKILTV_REAL_QMAKE" -query | awk -v plugins="$OKILTV_QT_PLUGIN_DIR" '
        /^QT_INSTALL_PLUGINS:/ { print "QT_INSTALL_PLUGINS:" plugins; next }
        { print }
    '
else
    exec "$OKILTV_REAL_QMAKE" "$@"
fi
