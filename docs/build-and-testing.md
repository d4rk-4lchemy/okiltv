# Build, validation and packaging

[Documentation index](README.md)

Run commands from the repository root. Use [CMakePresets.json](../CMakePresets.json)
and the canonical `qt/out/build/<preset>` trees. Avoid ad hoc build directories
unless the task explicitly calls for one. Use
[scripts/build_jobs.sh](../scripts/build_jobs.sh) for adaptive parallelism.

Movie-list coverage verifies schema 7-to-8 migration, durable source-scoped flags,
filter-before-pagination, atomic watched removal and re-adding completed movies.
Contract tests exercise the shared progress lane and once-per-session completion;
application tests synchronize independent library/sidebar models without media
requests. QML checks hover controls and click isolation on grid/shelf posters,
detail actions, tooltips and write gating. UI-12 verifies details, automatic removal,
re-adding watched films and the new playback groups using local fixtures.

## Dependencies and native Linux build

The project uses C++20, CMake 3.25+, Ninja and Qt 6.10+. Required Qt modules are
declared in [qt/CMakeLists.txt](../qt/CMakeLists.txt); Qt Test is needed only with
tests enabled. Linux also needs Qt DBus, zlib and OpenSSL Crypto. Runtime playback
uses libmpv; protected storage needs libsecret and a desktop Secret Service.
ffmpeg/ffprobe support timeshift, remux/probing and media integration tests.

The preferred local Qt SDK is `/opt/Qt/6.10.3/gcc_64`; adjust this installation
path if necessary without changing preset build directories.

```bash
export QT_LINUX_SDK_ROOT=/opt/Qt/6.10.3/gcc_64
export PATH="$QT_LINUX_SDK_ROOT/bin:$PATH"
export CMAKE_PREFIX_PATH="$QT_LINUX_SDK_ROOT${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"

cmake --fresh --preset qt-linux-debug
cmake --build --preset qt-linux-debug -j"$(scripts/build_jobs.sh)"
ctest --preset qt-linux-debug
```

The executable is `qt/out/build/qt-linux-debug/app/OKILTV`. After initial
configuration, incremental builds can reuse the tree. Linux Debug enables test
targets by default; Linux Release and Windows base presets disable them. To build
only the application, use `--target OKILTVQt`. `./build_linux.sh` is the repository's
Linux release helper.

## Select validation by affected contract

Test definitions and labels live in [qt/tests/CMakeLists.txt](../qt/tests/CMakeLists.txt).
Inspect registered tests rather than relying on a fixed suite count:

```bash
ctest --preset qt-linux-debug --show-only
ctest --preset qt-linux-debug -L '^core$' --output-on-failure
ctest --preset qt-linux-debug -L '^app$' --output-on-failure
ctest --preset qt-linux-debug -L '^playback$' --output-on-failure
ctest --preset qt-linux-debug -L '^qml$' --output-on-failure
ctest --preset qt-linux-debug -L vod --output-on-failure
```

| Area | Main coverage |
|---|---|
| Import, settings, storage, EPG | `OKILTVQtCoreTests`, `OKILTVQtDatabaseStartupTests` |
| Controllers, source changes, tracks, DVR/timeshift | `OKILTVQtAppTests` |
| Explicit-clock playback policy | `OKILTVQtPlaybackTests` |
| TS continuity and archive HTTP transport | `OKILTVQtCatchupTests` |
| Independent finite downloads | `OKILTVQtDownloadTests`, download QML suite |
| VOD ports and adapters | Domain, Contract, Provider, Storage, Mpv and Runtime tests with `vod` label |
| QML controls/interaction | Timeline, source group, channel number, transparency, update, download, VOD library and VOD playback-panel suites |
| Windows process ownership | Windows process-job and DVR cases, registered on Windows |

VOD contract tests cover confirmed probe teardown, the subsequent two-second
cooldown, cancellation, replacement, source changes and shutdown. The application
catalogue test uses real ffprobe against a stalled local HTTP response to verify
the five-second Play gate, early completion unlock, continued background probing,
connection closure before playback, plus VOD loading/buffering/pause indicators.
The catalogue test also covers playback-sidebar category/search isolation, latest
filter publication, pagination and unchanged playback/media-probe counts while
browsing groups. `OKILTVQtPlaybackSearchHeaderTests` covers shared search/switch
event forwarding, text-cursor behavior and disabled input in Live/movie/group modes.
`ui-tests/tests/12-vod-movies.py` exercises the Groups/Movies buttons, external Back
arrow geometry/visibility at minimum size and fullscreen, repeated Left, group
confirmation with Right/Enter/click, group search/empty results, empty categories,
All movies, cancellation/reopening, narrow layout and track pickers using a local
provider. It checks that group browsing preserves the paused position and media
request count; the Live channel-selection scenario covers the shared picker’s
existing behavior.

`OKILTVQtVodUiTests` covers first-visit history selection, empty/error fallback,
user-interaction cancellation, reopening, shelf/grid focus transfers, identity-based
shelf selection, details return, horizontal wheel input, dropdown geometry/options
at wide and compact sizes, icon actions and the blue icon/text Play button.
The library QML suite also exercises sidebar dragging, minimum/maximum width,
window resizing, compact mode, reopening, gesture cancellation and disabled input.
The app suite checks sidebar preference persistence, invalid/legacy values,
failed-write rollback/retry and independence from Settings drafts. The local
`UI-12-vod-movies` scenario verifies the real separator and bounded geometry.
The catalog application test verifies `continueMoviesLoaded` and shelf identities.
It also verifies cached poster paths at row publication, independent of progress
reads, and reuse after a new runtime starts without additional catalogue/image
requests. `vodArtworkPersistentConcurrentCache` uses a stalled local HTTP fixture
to cover one download for concurrent callers, waiter cancellation, disk-only and
offline reuse after reopening, LRU access-time updates, corrupt-image removal and
repair, changed URL identity and source removal during a download. Storage tests
cover multilingual base-letter search/sorting, composed/decomposed Unicode,
keyset pages in both directions, retained staging and schema-6-to-7 migration
rollback/retry across batch boundaries, preserved history and stale cursors.
The library QML suite also checks deferred focus while controls are disabled and
rejects delayed history focus changes during closing. `UI-12-vod-movies` checks
intermediate slide geometry, disabled controls and retained exclusive ownership,
repeated transition keys, all library close paths and restored browsing focus.
`UI-12-vod-movies` also checks shelf keyboard navigation and mouse-wheel scrolling
in the packaged application with three locally played films.

`OKILTVQtVodRangeTests` verifies sequential reads across cache eviction using one
HTTP response, one active socket during distant jumps on the local fixture,
redirects and reuse of the final media URL, pause/backpressure, idle cancellation, distant-track byte reuse,
cache eviction, the final partial block, rejected/changed/truncated ranges and
cancellation during preparation or a blocked read. Native mpv tests cover MP4
playback through the cache and fallback
when a server ignores ranges. Contract tests cover delayed remote resume and the
bounded recovery delays. These fixtures use only generated/local data.

Optional private slow-start diagnostics can use `OKILTV_TRACE_VOD_RANGE=1` to
measure individual byte reads without exposing URLs or credentials. Run provider
experiments sequentially when the account has a single connection slot, and keep
private harnesses, captures and measurements uncommitted. FILE_LOADED timing is
distinct from seek completion and the first rendered frame/audio.

Some tests are conditional on platform/tools. QML tests need `qmltestrunner`;
rendering integration uses Xvfb/software rendering where configured. Respect CTest
timeouts: download tests exercise real retry/stall windows. Report skipped tests
and unavailable dependencies instead of presenting partial execution as full coverage.

Use synthetic media, local HTTP, M3U and XMLTV. Tests must not require `.secrets`,
provider accounts or private recordings. Optional private diagnostic fixtures must
stay opt-in and uncommitted. EPG tests wait for asynchronous model/detail completion;
profile idle alone does not mean Guide data is ready.

## QML and C++ analysis

```bash
cmake --build --preset qt-linux-debug --target qmllint_okiltv
scripts/run_clang_tidy_src.sh qt/out/build/qt-linux-debug
```

The clang-tidy wrapper needs `compile_commands.json`, `clang-tidy`, `jq` and `rg`.
It deduplicates product translation units and prefers application compile commands.
Keep suppressions narrow; do not weaken [.clang-tidy](../.clang-tidy) to hide new
findings. QML policy is in [.qmllint.ini](../qt/qml/.qmllint.ini) and formatting in
[.qmlformat.ini](../qt/qml/.qmlformat.ini). Every QML component must be registered
in the module before validating packaged behavior.

For documentation-only edits, check links, commands and whitespace; a full C++
rebuild is unnecessary. For code changes, run relevant tests and the repository's
required checks, broadening to full regression when affected boundaries warrant it.

## Linux UI automation

```bash
cmake --preset qt-linux-debug -DOKILTV_BUILD_UI_TESTS=ON
cmake --build --preset qt-linux-debug -j"$(scripts/build_jobs.sh)"
ctest --preset qt-linux-debug -L '^ui$' --output-on-failure
```

[scripts/ci/run_ui_test.sh](../scripts/ci/run_ui_test.sh) supplies an isolated
D-Bus session and disposable keyring. The harness uses Xvfb, Openbox, xdotool and
generated local media; setup is documented in
[ui-tests/linux-ui-tests.md](../ui-tests/linux-ui-tests.md) and
[scripts/ci/README.md](../scripts/ci/README.md). CTest serializes desktop scenarios.
Each harness invocation keeps a unique appdata/artifact directory; do not reuse
encrypted fixture data with a new keyring.

The [scenario directory](../ui-tests/tests) covers channel selection, inactivity,
Guide groups, stationary pointer behavior, transparency, downloads, multiview and
VOD. Wait for enabled/hovered controls and acknowledged state changes, not only
screen coordinates or fixed sleeps.

## Windows cross-build and packages

```bash
export QT_WIN_SDK_ROOT=/opt/Qt/6.10.3/mingw_64
export QT_HOST_PATH=/opt/Qt/6.10.3/gcc_64
cmake --fresh --preset qt-win64-release
cmake --build --preset qt-win64-release --target OKILTVQt -j"$(scripts/build_jobs.sh)"
scripts/package_qt_win64.sh
```

`./build_windows.sh` is the end-to-end helper. Packaging uses MinGW, Wine,
windeployqt, CPack/NSIS and 7z through the existing scripts. Do not replace this
with an incomplete manual deployment. Outputs in `publish/` are the versioned
Windows ZIP and setup EXE. Only the ZIP receives the portable marker.

Installer update/clean-install paths preserve user settings/sources and unrelated
files. Native launch/foreground handling and maintenance behavior are covered by
[installer-testing.md](../scripts/windows/installer-testing.md). A successful
cross-build or Wine run does not replace Windows desktop playback, rendering,
DPAPI, native-dialog and installer checks.

## CI and releases

[pr-tests.yml](../.github/workflows/pr-tests.yml) owns grouped test execution.
[release.yml](../.github/workflows/release.yml) builds release assets; packaging
does not implicitly execute tests. Release tags must match the version in
`qt/CMakeLists.txt`; dependency releases are separate.

[package_linux_appimage.sh](../scripts/package_linux_appimage.sh) bundles Qt/QML,
libmpv and libsecret for the Linux package. The host still provides desktop
services and optional ffmpeg/ffprobe. Tool versions/checksums and CI setup are in
[scripts/ci](../scripts/ci); use that configuration instead of inventing download
locations. AppImage compatibility and Windows acceptance remain platform-specific.
