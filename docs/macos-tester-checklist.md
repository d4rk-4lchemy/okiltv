# OKILTV Apple Silicon beta bundle checklist

This checklist covers the ad-hoc signed beta package built on Linux or by the native macOS release workflow and tested on an Apple Silicon Mac. See [build and bundle contracts](macos-build.md). Linux cross-link and static bundle validation have passed; the first native CI run and full hardware/desktop gates remain pending. Passing CI's disposable Keychain/SQLite/helper tests does not replace the runtime gates below.

## Package contents and machine details

The sender should supply `OKILTV.app`, generated media/M3U/XMLTV/subtitle fixtures, launch instructions, checksums and a build manifest. The manifest must identify the supported macOS floor, commit and whether ffmpeg/ffprobe and all features are included. The application should require no Qt, Homebrew or compiler installation.

The [pinned cross-build dependencies](../scripts/macos/dependencies.env) require macOS 15.0 or newer on Apple Silicon. Confirm the tester's machine meets this floor.

Record the Mac model/chip, macOS version, display resolution/scaling and whether the test uses built-in or external audio. Record the build identifier from the manifest for every result.

Before reporting a failure, retain the exact macOS message, test step and approximate time. Keep test data and diagnostic files outside the app bundle. Use synthetic sources and the delivered fixtures; provider accounts are not required.

## First launch

1. Verify the archive checksum and extract it with a tool that preserves permissions and framework symlinks.
   Run `Verify bundle.command` to verify file checksums and Apple's strict bundle signatures. If included, run `Run platform tests.command` to check disposable Keychain items, shipped SQLite FTS5 and helper discovery without PATH. These helpers write diagnostics outside the app and never delete the production Keychain key.
2. Place the app in a writable test location, then launch it from Finder. Do not install dependencies or adjust shell library paths to make it work.
3. Beta packages are ad-hoc signed and unnotarized, without Developer ID. If Gatekeeper blocks launch, record that exact message and use System Settings → Privacy & Security → Open Anyway for the intentionally supplied beta. macOS 15+ does not support the older Control-click override for unnotarized apps. A malformed-signature or damaged-bundle error should be returned to the sender for correction. Do not disable Gatekeeper globally.
4. Confirm that the window, library screens, Settings and local file dialogs open and that no Qt module, Cocoa plugin, TLS or library-load error appears.
5. If Finder launch fails, run the delivered diagnostic launcher. It should capture ordinary stdout/stderr and existing redacted diagnostics without changing bundled libraries or bypassing protection. Return any crash report associated with that launch.

The sender should distinguish packaging/signature failures from playback failures before asking for further tests. A fallback Terminal launch does not establish that Finder launch works.

## Storage and source persistence

1. Run `Prepare fixtures.command`, add `fixtures/local.m3u` and import `fixtures/local.xml`. Save and reopen Settings. The helper regenerates local media paths and current XMLTV times whenever the test directory moves.
2. Quit through the application, relaunch and confirm that the source, preferences and imported state remain readable.
3. Confirm that application data is outside `OKILTV.app`. Report the resolved data path from diagnostics rather than guessing its location.
4. If macOS prompts for Keychain access, record when it occurs and whether allowing or denying it produces a clear, recoverable result.
5. In a separately agreed storage test, verify that denying/unavailable Keychain access leaves existing files and drafts intact. Do not delete the application's Keychain key or test against valuable existing data.
6. Test a subsequent build using the same application identity. Existing source data should still open; report any new Keychain access prompt.

## First playback

1. Play the supplied H.264/AAC local media through its local M3U entry. Confirm visibly changing video and audible audio.
2. Pause/resume, seek if available for the selected playback mode, stop and start again. Confirm that stopping or quitting leaves no audio playing.
3. Resize the window repeatedly, minimize/restore, enter/leave fullscreen and move it between displays if available. Look for black frames, inverted output, scaling errors, crashes and stalled rendering.
4. Test a supplied subtitle sample and change audio/subtitle selections when the fixture has multiple tracks.
5. Return redacted diagnostics showing the actual Qt graphics backend/context, libmpv build and decoder state. Report visible output separately from a “file loaded” or “playing” status.

## Feature acceptance after the first playback passes

| Area | Check |
| --- | --- |
| Live and EPG | Local TS/HLS fixtures, channel switching, radio, Guide and EPG search; no source credentials in diagnostics |
| VOD | Supplied synthetic catalogue fixture if available: Movies/Series, metadata probing, pause ownership, stop/back, restart resume and episode transitions |
| Tracks | Embedded/default/off choices; uploaded subtitle import, restart restoration, removal and filenames with spaces/Unicode |
| Multiview | PiP and grid, promote/restore, independent audio ownership and shutdown; record actual decoder use and resource load |
| Hardware decoding | H.264/HEVC samples with normal defaults and software fallback; report observed decoder, dropped frames and CPU use |
| Helpers | Packaged ffmpeg/ffprobe resolve without Homebrew or Terminal PATH changes |
| Recording | Manual/DVR capture, concurrent sessions where covered, stop, remux verification and no surviving helper processes |
| Timeshift | Startup, rolling segments, seek/pause/live return and stop/failure cleanup |
| Downloads | Finite local fixture download, pause/cancel, destination dialog and clean exit |
| Sleep | Prevent-screen-sleep policy while playback is active, then release after stop/quit; inspect assertions if requested |
| Window controls | Frameless drag/resize, maximize/fullscreen Spaces, always-on-top and Retina/external monitor changes |
| Desktop lifecycle | Menu-bar tray, minimize/restore, Dock behavior and normal quit with recording/download confirmation |
| Keyboard and dialogs | Text editing, macOS modifier mapping, picker focus, native dialog cancellation and playback pause restoration |
| Relocation | Quit, move the app to a path containing spaces and relaunch without changing dependencies or data |
| Oldest OS | Repeat launch, storage and playback on the minimum version claimed by the manifest |

Only run rows for features included in the manifest. For a minimal rendering package without ffmpeg/ffprobe, report dependent features as unavailable rather than failed or passed.

## Result to return

Provide the build identifier, machine details, steps and one of `PASS`, `FAIL` or `NOT TESTED` for each exercised gate. Include the exact first error, relevant screenshots/crash report and the application's redacted diagnostic output. Do not send saved source files, database contents, private stream URLs or provider credentials.

A minimum successful first report confirms Finder launch, protected source persistence through restart, visible video/audio and clean stop/quit. The remaining feature rows establish broader acceptance; they should not block useful feedback about the first package.
