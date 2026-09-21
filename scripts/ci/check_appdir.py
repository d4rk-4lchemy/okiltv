#!/usr/bin/env python3
"""Check the packaged ELF dependencies and libraries invisible to ELF scanning."""
import os
from pathlib import Path
import subprocess
import sys

appdir = Path(sys.argv[1]).resolve()
libdir = appdir / "usr/lib"
required = [
    appdir / "usr/bin/OKILTV",
    libdir / "libmpv.so.2",
    libdir / "libsecret-1.so.0",
    appdir / "usr/plugins/platforms/libqxcb.so",
    appdir / "usr/plugins/sqldrivers/libqsqlite.so",
    appdir / "usr/plugins/tls/libqopensslbackend.so",
]
env = {**os.environ, "LD_LIBRARY_PATH": str(libdir)}
for binary in required:
    if not binary.is_file():
        raise SystemExit(f"Missing packaged runtime: {binary}")
    result = subprocess.run(["ldd", str(binary)], env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode or "not found" in result.stdout:
        raise SystemExit(f"Unresolved packaged runtime: {binary}\n{result.stdout}")
if not (appdir / "usr/qml/QtQuick/Controls/qmldir").is_file():
    raise SystemExit("Qt Quick Controls QML module is missing")

# A fresh process uses only the packaged library search path, without the Qt SDK.
subprocess.run([sys.executable, "-c", """
import ctypes, sys
from pathlib import Path
libdir = Path(sys.argv[1])
mpv = ctypes.CDLL(str(libdir / 'libmpv.so.2'))
mpv.mpv_client_api_version.restype = ctypes.c_ulong
assert mpv.mpv_client_api_version() >> 16 == 2, 'Unsupported libmpv ABI'
for symbol in ('mpv_create', 'mpv_render_context_create', 'mpv_stream_cb_add_ro'):
    getattr(mpv, symbol)
secret = ctypes.CDLL(str(libdir / 'libsecret-1.so.0'))
for symbol in ('secret_password_lookupv_sync', 'secret_password_storev_sync'):
    getattr(secret, symbol)
""", str(libdir)], env=env, check=True)
print("AppDir: required Qt/QML plugins, ELF dependencies and dynamic library ABI passed")
