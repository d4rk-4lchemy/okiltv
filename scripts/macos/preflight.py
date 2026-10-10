#!/usr/bin/env python3
"""Verify pinned local target inputs and executable Linux Qt generators."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from macho import ARM64, inspect, version_number

repo = Path(__file__).resolve().parents[2]
pins = dict(line.split('=', 1) for line in (repo / 'scripts/macos/dependencies.env').read_text().splitlines()
            if line and not line.startswith('#'))
qt = Path(os.environ['QT_MACOS_SDK_ROOT'])
host = Path(os.environ['QT_HOST_PATH'])
sdk = Path(os.environ['MACOS_SDK_ROOT'])
deps = Path(os.environ['MACOS_DEPS_ROOT'])
floor = version_number(os.environ['MACOS_MIN_VERSION'])
if floor < version_number(pins['OKILTV_MACOS_MIN_VERSION']):
    raise SystemExit('The pinned arm64_sequoia dependency stack requires macOS 15.0 or newer')
for root in (qt, host, sdk, deps, Path(os.environ['OSXCROSS_ROOT'])):
    if not root.is_dir():
        raise SystemExit(f'Missing cross-build input: {root}')
for root in (qt, host):
    # Official macOS frameworks keep headers inside the framework.
    candidates = [root / 'include/QtCore/qtcoreversion.h', root / 'lib/QtCore.framework/Headers/qtcoreversion.h']
    version = next((path.read_text() for path in candidates if path.is_file()), '')
    if not re.search(r'QTCORE_VERSION_STR\s+"' + re.escape(pins['OKILTV_MACOS_QT_VERSION']) + '"', version):
        raise SystemExit(f'Qt host and target must both match pinned version {pins["OKILTV_MACOS_QT_VERSION"]}: {root}')
for relative in ('libexec/moc', 'libexec/rcc', 'libexec/qmlcachegen', 'libexec/qmltyperegistrar', 'libexec/qmlimportscanner', 'bin/qsb'):
    path = host / relative
    with path.open('rb') as stream:
        if stream.read(4) != b'\x7fELF' or not os.access(path, os.X_OK):
            raise SystemExit(f'Host Qt tool is not executable ELF: {path}')
manifest = json.loads((deps / 'manifest.json').read_text())
lock = json.loads((repo / 'scripts/macos/dependency-lock.json').read_text())
actual_packages = {name: {key: package[key] for key in ('version', 'revision', 'bottle_tag', 'sha256', 'source')}
                   for name, package in manifest['packages'].items()}
if actual_packages != lock['packages']:
    raise SystemExit('Installed media dependency manifest differs from dependency-lock.json')
sdk_version = json.loads((sdk / 'SDKSettings.json').read_text())['Version']
if sdk_version != pins['OKILTV_MACOS_SDK_VERSION']:
    raise SystemExit(f'Expected pinned Apple SDK {pins["OKILTV_MACOS_SDK_VERSION"]}: {sdk_version}')
for name, expected in [('mpv', pins['OKILTV_MACOS_MPV_VERSION']),
                       ('ffmpeg', pins['OKILTV_MACOS_FFMPEG_VERSION']),
                       ('openssl@3', pins['OKILTV_MACOS_OPENSSL_VERSION'])]:
    if manifest['packages'][name]['version'] != expected:
        raise SystemExit(f'Unexpected pinned {name} version')
for name, package in manifest['packages'].items():
    if package['bottle_tag'] != pins['OKILTV_MACOS_BOTTLE_TAG']:
        raise SystemExit(f'Wrong target bottle tag: {name}')
    archive = Path(package['archive'])
    if not archive.is_file():
        raise SystemExit(f'Missing frozen dependency archive: {archive}')
    with archive.open('rb') as stream:
        if hashlib.file_digest(stream, 'sha256').hexdigest() != package['sha256']:
            raise SystemExit(f'Dependency SHA-256 mismatch: {name}')
for path in [deps / 'opt/mpv/lib/libmpv.dylib', deps / 'opt/ffmpeg/bin/ffmpeg',
             deps / 'opt/ffmpeg/bin/ffprobe', deps / 'opt/openssl@3/lib/libcrypto.dylib']:
    info = inspect(path)
    if ARM64 not in info['architectures'] or info['platform'] != 1 or info['minimum'] > floor:
        raise SystemExit(f'Target architecture/minimum OS mismatch: {path}')
wrappers = list((Path(os.environ['OSXCROSS_ROOT']) / 'bin').glob('arm64-apple-darwin*-clang++'))
if len(wrappers) != 1:
    raise SystemExit('Expected exactly one arm64 OSXCross compiler wrapper')
prefix = str(wrappers[0]).removesuffix('clang++')
symbols = subprocess.check_output([prefix + 'nm', '-g', '-U', str(deps / 'opt/mpv/lib/libmpv.dylib')], text=True)
required = set(re.findall(r'resolve\(m_api->\w+, "(mpv_\w+)"', (repo / 'qt/src/player/mpvplayer.cpp').read_text()))
for symbol in sorted(required):
    if not re.search(r'\b_' + re.escape(symbol) + r'$', symbols, re.MULTILINE):
        raise SystemExit(f'libmpv lacks application API export: {symbol}')
for tool in ('cmake', 'ninja', 'ffmpeg', 'rcodesign'):
    if not shutil.which(tool):
        raise SystemExit(f'Missing Linux host tool: {tool}')
signer_version = subprocess.check_output(['rcodesign', '--version'], text=True).strip()
if pins['OKILTV_RCODESIGN_VERSION'] not in signer_version.split():
    raise SystemExit(f'Expected pinned rcodesign {pins["OKILTV_RCODESIGN_VERSION"]}: {signer_version}')
print(f'Preflight PASS: matching Qt, {len(manifest["packages"])} verified bottles, arm64/macOS {os.environ["MACOS_MIN_VERSION"]}')
