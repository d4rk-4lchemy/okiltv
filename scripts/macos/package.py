#!/usr/bin/env python3
"""Assemble and ad-hoc sign an Apple Silicon beta bundle on Linux or macOS."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tarfile
import tempfile

from check_bundle import check_bundle
from macho import ARM64, inspect, is_macho, system_reference

REPO = Path(__file__).resolve().parents[2]


def run(*args, **kwargs):
    return subprocess.check_output([str(arg) for arg in args], text=True, **kwargs)


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def source_fingerprint():
    files = run('git', '-C', REPO, 'ls-files', '-z', '--cached', '--others', '--exclude-standard').split('\0')
    digest = hashlib.sha256()
    for relative in sorted(set(files)):
        path = REPO / relative
        if relative and path.is_file():
            digest.update(relative.encode() + b'\0' + hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def native_packages(deps):
    """Inventory the installed Homebrew runtime closure, not today's formula versions."""
    cellar = (deps / 'Cellar').resolve(strict=True)
    names = sorted(set(['mpv', 'ffmpeg', 'openssl@3'] +
                       run('brew', 'deps', '--installed', '--union',
                           'mpv', 'ffmpeg', 'openssl@3').splitlines()))
    packages = {}
    for name in names:
        prefix = Path(run('brew', '--prefix', '--installed', name).strip()).resolve(strict=True)
        if not prefix.is_relative_to(cellar):
            raise ValueError(f'Homebrew package outside the selected Cellar: {name}')
        receipt = prefix / 'INSTALL_RECEIPT.json'
        installed = json.loads(receipt.read_text())
        packages[name] = {
            'version': prefix.name, 'prefix': str(prefix),
            'source': f'https://formulae.brew.sh/formula/{name}',
            'formula_commit': installed.get('source', {}).get('tap_git_head'),
            'receipt_sha256': hashlib.sha256(receipt.read_bytes()).hexdigest(),
        }
    return packages


def sign_native(app):
    """Sign inner code before frameworks and the outer app, using Apple's ad-hoc signer."""
    log = []
    binaries = sorted(path for path in app.rglob('*') if is_macho(path))
    # License copies retain their original paths, including *.framework
    # directories. Only ancestors of actual code need framework signatures.
    frameworks = sorted({parent for binary in binaries for parent in binary.parents
                         if parent.suffix == '.framework' and parent.is_relative_to(app)},
                        key=lambda path: (-len(path.parts), str(path)))
    for path in [*binaries, *frameworks, app]:
        log.append(run('codesign', '--force', '--sign', '-', '--timestamp=none', path,
                       stderr=subprocess.STDOUT))
    log.append(run('codesign', '--verify', '--deep', '--strict', '--verbose=2', app,
                   stderr=subprocess.STDOUT))
    return ''.join(log)


class Packager:
    def __init__(self, build, stage, platform_test=None, native=False):
        self.qt = Path(os.environ['QT_MACOS_SDK_ROOT']).resolve()
        self.deps = Path(os.environ['MACOS_DEPS_ROOT']).resolve()
        self.host = Path(os.environ['QT_HOST_PATH']).resolve()
        self.platform_test = platform_test
        self.source_app = build / 'app/OKILTV.app'
        self.app = stage / 'OKILTV.app'
        self.contents = self.app / 'Contents'
        self.frameworks = self.contents / 'Frameworks'
        if native:
            self.prefix = ''
        else:
            wrappers = list((Path(os.environ['OSXCROSS_ROOT']) / 'bin').glob('arm64-apple-darwin*-clang++'))
            if len(wrappers) != 1:
                raise ValueError('Expected exactly one arm64 Darwin compiler wrapper')
            self.prefix = str(wrappers[0]).removesuffix('clang++')
        self.pending = []
        self.copies = {}
        self.destinations = {}
        self.packages = (native_packages(self.deps) if native else
                         json.loads((self.deps / 'manifest.json').read_text())['packages'])

    def copy_binary(self, source, destination):
        source = source.resolve(strict=True)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        destination.chmod(destination.stat().st_mode | 0o700)
        info = inspect(source)
        if ARM64 not in info['architectures']:
            raise ValueError(f'Target library lacks arm64: {source}')
        if info['architectures'] != [ARM64]:
            temporary = destination.with_name(destination.name + '.arm64')
            run(self.prefix + 'lipo', destination, '-thin', 'arm64', '-output', temporary)
            temporary.replace(destination)
        self.copies[source] = destination
        self.destinations[destination] = source
        self.pending.append((source, destination))
        return destination

    def library(self, source):
        source = source.resolve(strict=True)
        if source in self.copies:
            return self.copies[source]
        if not source.is_relative_to(self.qt) and not source.is_relative_to(self.deps):
            raise ValueError(f'Dependency outside target SDKs: {source}')
        framework = next((parent for parent in source.parents if parent.suffix == '.framework'), None)
        if framework:
            name = framework.stem
            version = source.relative_to(framework).parts[1]
            root = self.frameworks / framework.name
            binary = root / source.relative_to(framework)
            resources = framework / 'Versions' / version / 'Resources'
            if resources.is_dir():
                shutil.copytree(resources, root / 'Versions' / version / 'Resources', symlinks=True,
                                ignore=shutil.ignore_patterns('_CodeSignature', '*.prl', '*.cmake'))
            (root / 'Versions/Current').symlink_to(version)
            (root / name).symlink_to('Versions/Current/' + name)
            (root / 'Resources').symlink_to('Versions/Current/Resources')
        else:
            binary = self.frameworks / source.name
        if binary in self.destinations:
            raise ValueError(f'Dependency basename collision: {source}, {self.destinations[binary]}')
        return self.copy_binary(source, binary)

    def resolve(self, reference, source, info):
        if reference.startswith('/'):
            candidates = [Path(reference)]
        elif reference.startswith('@loader_path/'):
            candidates = [source.parent / reference.removeprefix('@loader_path/')]
        elif reference.startswith('@executable_path/'):
            candidates = [self.source_app / 'Contents/MacOS' / reference.removeprefix('@executable_path/')]
        elif reference.startswith('@rpath/'):
            suffix = reference.removeprefix('@rpath/')
            candidates = []
            for rpath in info['rpaths']:
                expanded = rpath.replace('@loader_path', str(source.parent))
                expanded = expanded.replace('@executable_path', str(self.source_app / 'Contents/MacOS'))
                if not expanded.startswith('@'):
                    candidates.append(Path(expanded) / suffix)
            candidates += [self.qt / 'lib' / suffix, self.deps / 'lib' / suffix]
        else:
            raise ValueError(f'Unknown Mach-O dependency: {source}: {reference}')
        for candidate in candidates:
            if candidate.is_file():
                return candidate.resolve()
        raise ValueError(f'Unresolved target dependency: {source}: {reference}')

    def assemble(self):
        if not (self.source_app / 'Contents/MacOS/OKILTV').is_file():
            raise ValueError('Build OKILTVQt before packaging')
        shutil.copytree(self.source_app, self.app, symlinks=True,
                        ignore=shutil.ignore_patterns('_CodeSignature'))
        self.frameworks.mkdir(exist_ok=True)
        self.copy_binary(self.source_app / 'Contents/MacOS/OKILTV', self.contents / 'MacOS/OKILTV')
        for helper in ('ffmpeg', 'ffprobe'):
            self.copy_binary(self.deps / 'opt/ffmpeg/bin' / helper, self.contents / 'MacOS' / helper)
        if self.platform_test:
            self.copy_binary(self.platform_test, self.contents / 'MacOS/OKILTVQtMacosPlatformTests')
        mpv = self.library(self.deps / 'opt/mpv/lib/libmpv.dylib')
        # Qt's OpenSSL plugin resolves these dynamically; it may have no linked
        # load command that would otherwise seed dependency collection.
        for name in ('libssl.dylib', 'libcrypto.dylib'):
            self.library(self.deps / 'opt/openssl@3/lib' / name)
        if mpv.name != 'libmpv.dylib':
            (self.frameworks / 'libmpv.dylib').symlink_to(mpv.name)

        scanner = self.host / 'libexec/qmlimportscanner'
        imports = json.loads(run(scanner, '-rootPath', REPO / 'qt/qml', '-importPath', self.qt / 'qml'))
        optional = set()
        for qmldir in (self.qt / 'qml').rglob('qmldir'):
            optional.update(re.findall(r'^optional import (\S+)', qmldir.read_text(), re.MULTILINE))
        for module in imports:
            if (module.get('type') == 'module' and module.get('name') != 'OKILTV'
                    and module.get('name') not in optional and not module.get('path')):
                raise ValueError(f'Unresolved QML import: {module["name"]}')
        write_json(self.app.parent / 'qml-imports.json', imports)
        # Controls select style imports at runtime. The installed QML runtime is
        # small; retain its complete module tree, including transitive styles.
        shutil.copytree(self.qt / 'qml', self.contents / 'Resources/qml', symlinks=True,
                        ignore=shutil.ignore_patterns('_CodeSignature', '*.debug', '*.a', '*.prl', '*.o', 'objects-*'))
        plugin_paths = [self.qt / 'plugins/platforms/libqcocoa.dylib',
                        self.qt / 'plugins/sqldrivers/libqsqlite.dylib']
        for category in ('imageformats', 'iconengines', 'tls', 'networkinformation', 'styles'):
            plugin_paths.extend(sorted((self.qt / 'plugins' / category).glob('*.dylib')))
        for plugin in plugin_paths:
            self.copy_binary(plugin, self.contents / 'PlugIns' / plugin.relative_to(self.qt / 'plugins'))
        for path in sorted((self.contents / 'Resources/qml').rglob('*')):
            if is_macho(path):
                self.copy_binary(self.qt / 'qml' / path.relative_to(self.contents / 'Resources/qml'), path)
        (self.contents / 'Resources/qt.conf').write_text(
            '[Paths]\nPrefix = ..\nPlugins = PlugIns\nQmlImports = Resources/qml\nTranslations = Resources/translations\n')

        while self.pending:
            source, destination = self.pending.pop(0)
            info = inspect(source)
            args = [self.prefix + 'install_name_tool']
            for reference in sorted(set(info['dependencies'])):
                if system_reference(reference):
                    continue
                target = self.library(self.resolve(reference, source, info))
                relative = os.path.relpath(target, destination.parent)
                args += ['-change', reference, '@loader_path/' + relative]
            if info['id']:
                args += ['-id', '@rpath/' + str(destination.relative_to(self.frameworks))
                         if destination.is_relative_to(self.frameworks) else '@rpath/' + destination.name]
            for rpath in sorted(set(info['rpaths'])):
                args += ['-delete_rpath', rpath]
            args += ['-add_rpath', '@loader_path/' + os.path.relpath(self.frameworks, destination.parent)]
            run(*args, destination)
        self.notices()

    def notices(self):
        notices = self.contents / 'Resources/third-party-notices'
        notices.mkdir()
        for package in self.packages.values():
            root = Path(package['prefix'])
            for path in root.rglob('*'):
                if path.is_file() and (path.name.upper().startswith(('LICENSE', 'COPYING', 'COPYRIGHT', 'NOTICE'))
                                       or 'licenses' in path.parts):
                    relative = path.relative_to(root)
                    target = notices / root.parent.name / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(path, target)
        for path in self.qt.glob('LICENSE*'):
            if path.is_file():
                shutil.copy2(path, notices / path.name)
        shutil.copytree(REPO / 'scripts/macos/licenses', notices / 'Qt-license-texts')
        if (self.qt / 'sbom').is_dir():
            shutil.copytree(self.qt / 'sbom', notices / 'Qt-SBOM')
        (notices / 'README.txt').write_text(
            'This beta bundle contains Qt and the mpv/FFmpeg media dependency stack.\n'
            'Dependency versions, sources and available provenance hashes are in build-manifest.json beside the app.\n'
            'Redistribution license acceptance and corresponding-source preparation for a public release remain pending.\n')


def fixtures(stage):
    root = stage / 'fixtures'
    root.mkdir()
    # Native host FFmpeg generates files. Target ffmpeg is never executed here.
    host_ffmpeg = shutil.which('ffmpeg')
    if not host_ffmpeg:
        raise ValueError('Native host ffmpeg is required to generate tester fixtures')
    run(host_ffmpeg, '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi',
        '-i', 'testsrc2=size=640x360:rate=25', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
        '-t', '12', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-threads', '2', '-c:a', 'aac',
        '-movflags', '+faststart', root / 'H264-AAC.mp4')
    (root / 'sample subtitles.srt').write_text('1\n00:00:01,000 --> 00:00:06,000\nOKILTV macOS — subtitle test\n\n')
    (root / 'sample.xml').write_text('<?xml version="1.0"?><tv><channel id="local.fixture">'
        '<display-name>Local video fixture</display-name></channel>'
        '<programme start="20261008000000 +0000" stop="20261009000000 +0000" channel="local.fixture">'
        '<title>Local playback test</title></programme></tv>\n')
    for name in ('Launch diagnostics.command', 'Prepare fixtures.command', 'Verify bundle.command'):
        shutil.copy2(REPO / 'scripts/macos/tester' / name, stage / name)
        (stage / name).chmod(0o755)
    shutil.copy2(REPO / 'scripts/macos/tester/README.txt', stage / 'README.txt')
    shutil.copy2(REPO / 'docs/macos-tester-checklist.md', stage / 'TESTER-CHECKLIST.md')


def package(build, stage, publish, platform_test=None, native=False, expected_version=None):
    # Only the canonical package subtree may be replaced.
    preset = 'qt-macos-arm64-native-release' if native else 'qt-macos-arm64-cross-release'
    canonical = REPO / 'qt/out/package' / preset
    if stage != canonical:
        raise ValueError(f'Use canonical staging directory: {canonical}')
    stage.mkdir(parents=True, exist_ok=True)
    plist = plistlib.loads((build / 'app/OKILTV.app/Contents/Info.plist').read_bytes())
    if expected_version and plist['CFBundleShortVersionString'] != expected_version:
        raise ValueError('Built application version differs from the validated release version')
    assembler = Packager(build, stage, platform_test, native)
    assembler.assemble()
    fixtures(stage)
    if platform_test:
        shutil.copy2(REPO / 'scripts/macos/tester/Run platform tests.command', stage / 'Run platform tests.command')
        (stage / 'Run platform tests.command').chmod(0o755)
    check_bundle(assembler.app, require_signatures=False)
    signer = shutil.which('codesign' if native else 'rcodesign')
    if not signer:
        raise ValueError('An ad-hoc code signing tool is required')
    signing_log = (sign_native(assembler.app) if native else
                   run(signer, 'sign', '--timestamp-url', 'none', assembler.app, stderr=subprocess.STDOUT))
    (stage / 'signing.log').write_text(signing_log)
    report = check_bundle(assembler.app)
    write_json(stage / 'bundle-check.json', report)
    plist = plistlib.loads((assembler.contents / 'Info.plist').read_bytes())
    commit = run('git', '-C', REPO, 'rev-parse', '--short=12', 'HEAD').strip()
    manifest = {
        'application': 'OKILTV', 'version': plist['CFBundleShortVersionString'], 'commit': commit,
        'working_tree_modified': bool(run('git', '-C', REPO, 'status', '--porcelain').strip()),
        'source_tree_sha256': source_fingerprint(),
        'architecture': 'arm64', 'minimum_macos': plist['LSMinimumSystemVersion'],
        'qt_version': os.environ.get('OKILTV_MACOS_QT_VERSION', '6.10.3'),
        'sdk': Path(os.environ['MACOS_SDK_ROOT']).name,
        'compiler': run(assembler.prefix + 'clang++', '--version').splitlines()[0],
        'signer': 'Apple codesign' if native else run(signer, '--version').strip(),
        'signing': 'ad-hoc; unnotarized beta build', 'channel': 'beta',
        'beta_reason': 'Developer ID signing/notarization and full macOS acceptance are pending',
        'build_host': 'macOS' if native else 'Linux (OSXCross)',
        'input_versions': ({'qt': os.environ['OKILTV_MACOS_QT_VERSION'], 'media': 'installed Homebrew formulae'}
                           if native else dict(line.split('=', 1) for line in
                           (REPO / 'scripts/macos/dependencies.env').read_text().splitlines()
                           if line and not line.startswith('#'))),
        'dependencies': {name: {key: value for key, value in package.items() if key not in {'prefix', 'archive'}}
                         for name, package in assembler.packages.items()},
        'features': ['Live/EPG', 'VOD', 'multiview', 'subtitles', 'ffmpeg/ffprobe', 'DVR', 'timeshift', 'downloads', 'Keychain', 'IOKit sleep assertions'],
        'native_platform_test_included': bool(platform_test),
        'validation': {'native_link' if native else 'cross_link': 'PASS',
                       'bundle_static_checks': 'PASS', 'ad_hoc_code_page_digests': 'PASS',
                       'macos_Finder_launch': 'PENDING', 'Keychain_runtime': 'PENDING', 'video_audio': 'PENDING',
                       'hardware_decoding': 'PENDING', 'full_feature_acceptance': 'PENDING',
                       'Apple_codesign': 'PASS' if native else 'PENDING'},
    }
    write_json(stage / 'build-manifest.json', manifest)
    file_hashes = []
    for path in sorted(stage.rglob('*')):
        if path.is_file() and not path.is_symlink():
            file_hashes.append(hashlib.sha256(path.read_bytes()).hexdigest() + '  ' + str(path.relative_to(stage)))
    (stage / 'SHA256SUMS').write_text('\n'.join(file_hashes) + '\n')
    publish.mkdir(exist_ok=True)
    suffix = '' if native else f'-{commit}'
    name = f"OKILTV-qt-macos-arm64-{manifest['version']}-beta{suffix}.tar.gz"
    archive = publish / name
    with tarfile.open(archive, 'w:gz', compresslevel=6) as tar:
        for path in sorted(stage.iterdir()):
            tar.add(path, arcname=path.name)
    # Confirm extraction preserves the framework links, permissions and signatures.
    with tempfile.TemporaryDirectory(dir=REPO / 'qt/out/package') as temporary:
        restored = Path(temporary)
        with tarfile.open(archive) as tar:
            tar.extractall(restored, filter='data')
        check_bundle(restored / 'OKILTV.app')
        for relative in ('OKILTV.app/Contents/MacOS/OKILTV', 'OKILTV.app/Contents/MacOS/ffmpeg',
                         'OKILTV.app/Contents/MacOS/ffprobe', 'Launch diagnostics.command'):
            if not os.access(restored / relative, os.X_OK):
                raise ValueError(f'Archive lost executable permission: {relative}')
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix(archive.suffix + '.sha256').write_text(digest + '  ' + archive.name + '\n')
    print(f'Package PASS: {archive}\nSHA-256: {digest}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', action='store_true', help='Use native macOS/Homebrew tools instead of OSXCross')
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--expected-version', help='Require the built Info.plist to match the release version')
    parser.add_argument('--platform-test', type=Path, help='Native Keychain/SQLite/helper integration test')
    args = parser.parse_args()
    preset = 'qt-macos-arm64-native-release' if args.native else 'qt-macos-arm64-cross-release'
    stage = REPO / 'qt/out/package' / preset
    if stage.exists():
        shutil.rmtree(stage)
    build = args.build_dir or REPO / 'qt/out/build' / preset
    package(build.resolve(), stage, REPO / 'publish', args.platform_test, args.native, args.expected_version)
