#!/usr/bin/env python3
"""Reject incomplete, externally linked, wrong-architecture or unsigned bundles."""
import argparse
import configparser
import hashlib
import json
from pathlib import Path
import plistlib
import os

from macho import (ARM64, code_pages_valid, inspect, is_macho,
                   system_reference, version_number)


def check_bundle(app, require_signatures=True):
    app = app.resolve()
    contents = app / 'Contents'
    plist = plistlib.loads((contents / 'Info.plist').read_bytes())
    if plist['CFBundleIdentifier'] != 'org.okiltv.OKILTV':
        raise ValueError('Unexpected application identity')
    floor = version_number(plist['LSMinimumSystemVersion'])
    required = ['MacOS/OKILTV', 'MacOS/ffmpeg', 'MacOS/ffprobe',
                'Frameworks/libmpv.dylib', 'PlugIns/platforms/libqcocoa.dylib',
                'PlugIns/sqldrivers/libqsqlite.dylib', 'PlugIns/tls/libqsecuretransportbackend.dylib',
                'Resources/OKILTV.icns', 'Resources/qml/QtQuick/Controls/qmldir', 'Resources/qt.conf']
    for relative in required:
        if not (contents / relative).is_file():
            raise ValueError(f'Missing runtime input: {relative}')
    config = configparser.ConfigParser()
    config.read(contents / 'Resources/qt.conf')
    for key, value in {'Plugins': 'PlugIns', 'QmlImports': 'Resources/qml'}.items():
        if config['Paths'].get(key) != value:
            raise ValueError(f'Incorrect qt.conf {key}')
    executable = contents / 'MacOS/OKILTV'
    binaries = []
    signatures = {}
    references = 0
    for path in sorted(app.rglob('*')):
        if path.is_symlink():
            resolved = path.resolve(strict=True)
            if not resolved.is_relative_to(app):
                raise ValueError(f'External symlink: {path}')
        if not is_macho(path):
            continue
        info = inspect(path)
        if (info['architectures'] != [ARM64] or info['subtypes'] != [0] or info['platform'] != 1
                or info['minimum'] == 0 or info['minimum'] > floor):
            raise ValueError(f'Architecture/deployment floor mismatch: {path}')
        for reference in info['dependencies']:
            references += 1
            if system_reference(reference):
                continue
            if reference.startswith('@loader_path/'):
                target = path.parent / reference.removeprefix('@loader_path/')
            elif reference.startswith('@executable_path/'):
                target = executable.parent / reference.removeprefix('@executable_path/')
            else:
                raise ValueError(f'Non-relocatable load command: {path}: {reference}')
            target = target.resolve(strict=True)
            if not target.is_relative_to(app) or not is_macho(target):
                raise ValueError(f'Invalid bundled dependency: {path}: {reference}')
        for rpath in info['rpaths']:
            if not rpath.startswith(('@loader_path/', '@executable_path/')):
                raise ValueError(f'External RPATH: {path}: {rpath}')
        if info['id'] and not info['id'].startswith('@rpath/'):
            raise ValueError(f'External dylib identity: {path}: {info["id"]}')
        if require_signatures:
            signatures[path] = code_pages_valid(path)
        binaries.append(str(path.relative_to(app)))
    if require_signatures:
        for seal in app.rglob('_CodeSignature/CodeResources'):
            root = seal.parent.parent
            records = plistlib.loads(seal.read_bytes())['files2']
            for relative, expected in records.items():
                resource = root / relative
                if not resource.resolve(strict=True).is_relative_to(app):
                    raise ValueError(f'External signed resource: {resource}')
                if 'hash2' in expected and hashlib.sha256(resource.read_bytes()).digest() != expected['hash2']:
                    raise ValueError(f'Invalid signed resource digest: {resource}')
                if 'symlink' in expected and (not resource.is_symlink() or os.readlink(resource) != expected['symlink']):
                    raise ValueError(f'Invalid signed resource symlink: {resource}')
                if 'cdhash' in expected:
                    if resource.suffix == '.framework':
                        info = plistlib.loads((resource / 'Resources/Info.plist').read_bytes())
                        binary = (resource / info['CFBundleExecutable']).resolve()
                    else:
                        binary = resource.resolve()
                    if signatures[binary]['cdhash'] != expected['cdhash']:
                        raise ValueError(f'Invalid nested code identity: {resource}')
            # Bind the sealed resources and Info.plist to their CodeDirectory,
            # rather than validating an independently replaceable hash inventory.
            if root == contents:
                binary, info_path = executable, contents / 'Info.plist'
            else:
                info_path = root / 'Resources/Info.plist'
                info = plistlib.loads(info_path.read_bytes())
                binary = root / info['CFBundleExecutable']
            signature = signatures[binary.resolve()]
            for slot, payload in ((1, info_path.read_bytes()), (3, seal.read_bytes())):
                expected = signature['special'].get(slot)
                if expected != signature['digest'](payload).digest()[:signature['hash_size']]:
                    raise ValueError(f'Invalid bundle signature special slot {slot}: {binary}')
    return {'status': 'PASS', 'architecture': 'arm64', 'minimum_macos': plist['LSMinimumSystemVersion'],
            'macho_files': len(binaries), 'load_references': references,
            'signature_scope': 'Ad-hoc code-page digests, resource seals and nested code identities; Apple codesign/Gatekeeper pending',
            'binaries': binaries}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('app', type=Path)
    parser.add_argument('--unsigned', action='store_true')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    report = check_bundle(args.app, not args.unsigned)
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f"Bundle PASS: {report['macho_files']} arm64 binaries, {report['load_references']} load references")
