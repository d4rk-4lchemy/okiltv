# macOS Apple Silicon beta builds

[Documentation index](README.md) · [Tester checklist](macos-tester-checklist.md)

The repository supports a Linux OSXCross build with pinned local dependencies
and a native arm64 beta job in the release workflow on GitHub's `macos-15` runner.
Both deliver an **ad-hoc signed, unnotarized beta** requiring macOS 15.0 or newer.
The `beta` filename and manifest explicitly identify the missing Developer ID
signature/notarization and full macOS acceptance. Finder launch, production
Keychain continuity, video/audio, VideoToolbox and full desktop/hardware behavior
remain manual acceptance requirements. Intel/universal builds and Developer ID
signing/notarization are outside the current workflow.

## Native release CI (beta)

[release.yml](../.github/workflows/release.yml) runs `macOS arm64 beta` alongside
the Windows/Linux jobs after version validation. A published application release
starts all jobs; `deps-*` releases are excluded. Manual dispatch only saves
Actions artifacts and does not attach packages to a Release. The first native
GitHub run is still pending; local Linux checks do not establish native execution.

The native job installs Qt at the version shared by the CI/macOS dependency pins,
with `aqtinstall` and `macholib` pinned, and installs Ninja, mpv, FFmpeg and OpenSSL
through Homebrew. It uses the runner's Xcode SDK rather than redistributing an
Apple SDK or relying on `/opt/okiltv-macos`. Homebrew formulae and runner images
can change; the Linux bottle lock does not apply to native CI. The build manifest
records each installed runtime dependency's actual keg version, formula reference,
installed formula commit when available and receipt digest. Builds are not
guaranteed to be byte-for-byte reproducible.

`build_macos_native.sh` configures `qt-macos-arm64-native-release`, then builds only
`OKILTVQt` and `OKILTVQtMacosPlatformTests`. The canonical native build and stage
directories are `qt/out/build/qt-macos-arm64-native-release` and
`qt/out/package/qt-macos-arm64-native-release`. Tests are enabled to compile that
selected platform target. Native `lipo` and `install_name_tool` reuse the same
recursive collection, arm64 thinning, QML/plugin inventory and closure checks as
the cross packager. Apple's `codesign` signs inner binaries, frameworks and the
outer app ad-hoc and verifies the assembled bundle. Packaging checks extraction
and executable modes. The validated release version must match the built Info.plist.

The job extracts the resulting archive and runs `Verify bundle.command`, the
disposable Keychain/SQLite/helper integration test, and real-bundle mutation tests.
The Python inventory/version/signing-order contract tests also run before building.
CI test results are retained in the separate diagnostics artifact; the package
manifest conservatively leaves full runtime acceptance pending. These gates do
not test Finder/Gatekeeper behavior for a browser download, visible playback,
VideoToolbox, or persistence across application upgrades.

The release asset is `OKILTV-qt-macos-arm64-<version>-beta.tar.gz`, accompanied by
its `.tar.gz.sha256` and `SHA256SUMS-macos.txt`. The Actions artifact is
`okiltv-macos-beta-<version>`. Packages/diagnostics are retained for 14 days;
only packages are published. Publishing waits for Windows, Linux and macOS and
verifies all three checksum lists. No Developer ID certificate or Apple account
secret is used. Users may need System Settings → Privacy & Security → Open Anyway
for the downloaded beta; see the [tester checklist](macos-tester-checklist.md).

Equivalent native build after installing Homebrew dependencies and Python packages:

```bash
export QT_MACOS_SDK_ROOT=/path/to/Qt/6.10.3/macos
export OKILTV_BUILD_JOBS=2
./build_macos_native.sh
```

## Linux cross-build inputs

The local installation on this development machine is activated with
`source /opt/okiltv-macos/env.sh`. The build helper uses that file when no target
Qt root was supplied; `OKILTV_MACOS_ENV_FILE` selects another environment file.
Explicitly configured installations must supply all these variables:

| Variable | Purpose |
|---|---|
| `QT_MACOS_SDK_ROOT` | Qt 6.10.3 macOS target SDK with arm64 frameworks/plugins |
| `QT_HOST_PATH` | Matching Linux Qt 6.10.3 tools, including QML generators |
| `MACOS_SDK_ROOT` | Extracted macOS 15.5 SDK |
| `OSXCROSS_ROOT` | OSXCross installation with one `bin/arm64-apple-darwin*-clang++` wrapper |
| `MACOS_DEPS_ROOT` | Relocated Darwin media prefix with `opt` aliases, retained bottles and `manifest.json` |
| `MACOS_MIN_VERSION` | Verified deployment floor, at least `15.0` for this stack |
| `OKILTV_BUILD_JOBS` | Optional parallelism override; use `2` on this development host |
| `MACOS_PYTHON` | Optional host Python executable with `macholib==1.16.4` |

[dependencies.env](../scripts/macos/dependencies.env) records Qt/SDK/OSXCross,
mpv, FFmpeg, OpenSSL and rcodesign versions.
[dependency-lock.json](../scripts/macos/dependency-lock.json) freezes all media
package versions, bottle tags, SHA-256 values and upstream archive URLs. The
installed manifest also identifies each retained archive and extracted prefix.
[preflight.py](../scripts/macos/preflight.py) checks that inventory and every
bottle checksum, Qt version equality, executable ELF host generators, target
architecture/OS floor, SDK version and the signing tool. No dependency is
downloaded or upgraded implicitly by the build/package helpers.

The native Keychain library links Security/CoreFoundation and a small ARC
Objective-C++ adapter to Foundation/LocalAuthentication. `LAContext` disables
authentication interaction explicitly; no deprecated authentication UI flag is
needed.

The prepared setup uses LLVM 19 and OSXCross commit recorded in the pins, Qt's
macOS desktop archives, 62 `arm64_sequoia` Homebrew bottles and rcodesign 0.29.0.
The Apple SDK must be supplied under its applicable terms; retain acquisition
and extraction records separately. This repository integrates an already
prepared prefix; it does not provide a source-build recipe for the media stack.
See [OSXCross](https://github.com/tpoechtrager/osxcross) and
[Qt cross-compilation](https://doc.qt.io/qt-6.10/cross-compiling-qt.html) for preparing
another installation. Lowering CMake's target cannot lower the bottles' OS floor.

## Build and package

```bash
./build_macos.sh
```

The helper runs preflight, freshly configures `qt-macos-arm64-cross-release`,
builds only `OKILTVQt`, cross-builds the small native platform integration test
in the separate Debug tree, then packages and verifies the archive. Set
`OKILTV_MACOS_BUILD_PLATFORM_TESTS=0` to omit that test executable.
Logs are saved in `qt/out/build/qt-macos-arm64-cross-release/logs/`; each build
tree retains `compile_commands.json`. The Release executable is
`qt/out/build/qt-macos-arm64-cross-release/app/OKILTV.app/Contents/MacOS/OKILTV`.
The helper does not execute any Darwin program on Linux.

Equivalent application-only commands after activating the environment:

```bash
cmake --fresh --preset qt-macos-arm64-cross-release
cmake --build --preset qt-macos-arm64-cross-release --target OKILTVQt \
  -j"$(scripts/build_jobs.sh)"
bash scripts/package_macos_cross.sh
```

The repository's explicit [Darwin toolchain](../build/toolchains/osxcross-arm64.cmake)
owns target compiler, SDK, arm64, deployment target and package/library discovery.
It discovers wrapper names from the installation rather than assuming a Darwin
suffix. `QT_HOST_PATH`/`QT_HOST_PATH_CMAKE_DIR` keep Qt generators on Linux;
target Qt packages come from `QT_MACOS_SDK_ROOT`. Do not chainload the OSXCross
launcher-only internal CMake file or execute the macOS `macdeployqt` on Linux.

Both cross presets disable tests by default. Add `-DOKILTV_BUILD_TESTS=ON` when
configuring Debug to compile selected test targets. There is deliberately no
macOS cross CTest preset: these executables cannot run on Linux.

## Bundle and archive contract

[package_macos_cross.sh](../scripts/package_macos_cross.sh) stages under
`qt/out/package/qt-macos-arm64-cross-release` and creates
`publish/OKILTV-qt-macos-arm64-<version>-beta-<commit>.tar.gz` with a companion
`.sha256` file. Packaging replaces only that canonical staging tree. The manifest includes a content fingerprint of the source tree, including
uncommitted inputs, alongside the base commit. The archive
contains the app, a synthetic H.264/AAC movie, XMLTV and subtitle samples,
`.command` helpers, a checklist, build manifest, QML scan inventory, static
verification report and per-file checksums. The playlist is generated on the
Mac after extraction so its paths match the tester's directory, including spaces.

The app contains Qt frameworks in `Contents/Frameworks`, Cocoa/SQLite/image/TLS
plugins in `Contents/PlugIns`, runtime QML modules and `qt.conf` in
`Contents/Resources`, and ffmpeg/ffprobe beside `Contents/MacOS/OKILTV`. The
scanner uses Linux Qt tools against the target QML root; optional platform style
imports are distinguished from required imports. The installed QML runtime tree
is retained conservatively for dynamic Controls style selection, excluding static
archives and build inputs. The in-app `OKILTV` module remains embedded.

Dynamic libmpv and OpenSSL libraries explicitly seed recursive Mach-O collection.
Qt universal libraries are thinned to arm64. Framework resources and internal
symlinks survive copying and archiving. Every non-system load reference is
rewritten to the actual bundled library via `@loader_path`; dylib identities use
`@rpath`, and external RPATHs are removed. Filename collisions and unresolved
dependencies fail packaging. System Apple frameworks and `/usr/lib` remain
provided by macOS. Default playback requests OpenGL; optional Vulkan rendering
is not part of this package's acceptance scope.

After assembly, rcodesign recursively ad-hoc signs the helpers, plugins,
frameworks and app. [check_bundle.py](../scripts/macos/check_bundle.py) checks
required runtime inputs, all architectures/deployment floors, dependency closure,
internal symlinks, signed code-page digests, nested code identities, sealed
resource hashes and the Info.plist/resource
special slots in each bundle signature. The real-bundle mutation checks in
[test_bundle.py](../scripts/macos/test_bundle.py) reject missing helpers, external
links/dependencies, wrong architectures and changed code/resources.
Packaging repeats the checks
after archive extraction and verifies executable permissions. The independent
digest check does not establish Apple's full resource/signature validation or
Gatekeeper trust; `Verify bundle.command` runs `codesign --verify --deep --strict`
on the Mac. The archive includes dependency notices, pinned Qt license texts
and the SDK's Qt SBOM. Public distribution/source/license review remains pending.

## Platform behavior and validation

The bundle identity is `org.okiltv.OKILTV`, and CMake supplies its version,
deployment floor and existing-artwork `.icns` icon. Protected storage uses a
Keychain generic password under service `org.okiltv.storage`, account
`encryption-key-v1`; see [settings/storage](settings-and-storage.md#protection-boundary).
Missing keys are created only when encrypting, with race handling and read-back
verification. Background storage forbids authentication UI and reports access
errors through the existing persistence paths. A locked/denied store must be
unlocked/authorized in Keychain before retrying. No plaintext/key-file fallback
or production test override exists. Signature/upgrade access continuity remains
a Mac acceptance requirement.

Qt Quick retains OpenGL and requests 3.2 Core before application creation. The
renderer logs the actual version/profile and whether a CGL context is current.
mpv discovers the bundled versioned dylib after an explicit override. Finder
launches discover ffmpeg/ffprobe beside the executable, without shell PATH changes.
IOKit owns display and idle-system sleep assertions under the existing active
playback/window policy, releasing them on stop/policy change/destruction.
macOS ignores the legacy Windows `APPDATA` path/import and uses QStandardPaths;
the resolved data directory is logged, always outside the app for ordinary
launches. Command-Q/menu/Dock quit routes through existing exit confirmations and
async shutdown; activation restores a hidden window. These behaviors are
implemented but await desktop/hardware acceptance.

`OKILTVQtMacosPlatformTests` uses unique disposable Keychain accounts, exercises
missing-key refusal, competing creation/durable reads and malformed-key retention,
then tests FTS5 through the shipped SQLite driver and helper discovery with an
empty PATH. It never accesses/deletes the production key. When included,
`Run platform tests.command` writes its result outside the app. Cocoa registrations
for the existing VOD mpv/render-runtime suites use the same Core context setup;
those broader suites require their native runner/fixtures and remain pending.
Linux Xvfb/desktop automation is not a macOS test runner.

Local validation has passed the application Release cross-build, Debug compilation
of core/app/VOD mpv tests and the native platform test, dependency/signature/resource
checks including archive extraction, real-bundle rejection mutations and clean-copy
input preservation. The affected Linux core/app/playback/VOD suites (including real
mpv FBO/runtime playback) and QML lint passed. Native Mac execution remains pending.

Run affected Linux core/app/playback/VOD regressions, `qmllint_okiltv` and product
clang-tidy after shared changes. Cross-link/static package passes are separate
from Mac test passes. Follow the [tester checklist](macos-tester-checklist.md)
for Finder launch, source persistence, visible video/audio, relocation, oldest OS,
hardware fallback, dialogs, subtitles, multiview, timeshift/DVR/downloads, sleep
assertions and shutdown. Record each remote gate as PASS/FAIL/NOT TESTED; no
provider accounts are needed for the local fixture gates.
