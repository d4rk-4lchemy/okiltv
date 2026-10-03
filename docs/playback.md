# Playback and recording

[Documentation index](README.md)

## Ownership and state

[PlayerController](../qt/src/app/playercontroller.h) owns backend objects and
executes decisions from independent policy components in
[app/playback](../qt/src/app/playback). `isPlaying` follows observed mpv state;
QML must never flip it optimistically. Browse selection does not tune a stream.

| Component | Policy responsibility |
|---|---|
| `PlaybackBuffering` | Startup reserve, Live adaptation, cache budgets and refill holds |
| `PlaybackRecovery` | Reconnect phases, stability, attempt budgets and deadlines |
| `CatchupPlaybackSession` | Archive position, seek/reload, publication wait, bookmarks and recovery alignment |
| `CatchupStandbyTransition` | Standby preparation, readiness, alignment and cutover decisions |
| `CatchupStreamSession` | HTTP delivery, bounded queues, MPEG-TS continuity and media periods |

Policy components receive explicit clocks/observations and return decisions. They
have no reference or private-state access to `PlayerController`. Each PiP/grid
player gets independent policy state. The detailed ownership/transition map is in
[playback-refactoring.md](../.project/playback-refactoring.md).

## Backend and rendering contracts

[MpvPlayer](../qt/src/player/mpvplayer.h) wraps libmpv;
[MpvVideoItem](../qt/src/player/mpvvideoitem.h) integrates the render API with Qt
Quick. Keep OpenGL startup configuration and `video-timing-offset=0`, which avoids
mpv timing adjustments fighting the host's vsync.

- Use named `loadfile` node arguments (`url`, `flags`, `options`) for compatibility
  with mpv 0.37 and newer. Do not assume the newer positional index argument.
- The local FFI value for `MPV_EVENT_PROPERTY_CHANGE` is 22; 16 is client-message.
  Observed pause/cache events must reach Qt signals.
- Idle volume/audio configuration and stop do not initialize a dormant backend.
  Apply deferred settings when the backend initializes or reinitializes.
- Audio-only MP3, AAC and TS channels become ready without a video frame. TS
  programmes without video use a supported audio PID for their continuity clock.
- Shared/replaced backend callbacks must verify sender identity before changing a
  reused slot. Tolerate transient layout/slot-count differences during teardown.
- Use `playbackChannelActivated` for tune/shared-adoption affordances;
  `currentChannelChanged` also fires for metadata-only updates.
- Activating the already active/in-flight primary channel suppresses retuning.
  A stopped/failed channel may still be retried explicitly.

Global picture presets and smoothing apply to Live, catch-up, standby, multiview
and retained VOD backends. Standard removes only the application-owned shader;
user-provided mpv shaders/options remain independent.

## Provider catch-up

[AppController](../qt/src/app/appcontroller.cpp) validates the channel/programme
and [CatchupUrlResolver](../qt/src/core/catchupurlresolver.cpp) resolves provider
URLs. Preserve the canonical archive URL and exact original Live URL even when an
initial redirect is resolved. Xtream timestamps use the source server timezone,
with UTC fallback. Publication eligibility includes the source safety margin.

All application entrypoints start supported archives as continuous delayed-channel
sessions, including ended programmes. Media time selects the displayed programme.
Crossing an EPG boundary updates metadata/timeline without reconnecting or stopping.
Missing EPG uses a generic catch-up timeline and does not interrupt delivery.

Catch-up is independent of local timeshift. Do not attach `TimeshiftController`,
create local HLS or apply ordinary Live startup/refill policy to it. Preserve manual
pause ownership through automatic refill and recovery.

Seeks use eligible observed cache where possible; otherwise use the existing
stop-acknowledgement/reload path. Recovery drains verified media and resumes at the
watched point. Standby cutover uses observed forward cache and aligns its position;
stale URL endpoints must not cause repeated early cutovers or replay. Clear a timed
out standby attempt before fallback recovery.

Bookmarks require confirmed, settled playback and observed position, not a pending
seek target. Startup restoration is separate from Guide bookmarks: restore an
eligible saved catch-up session only with more than five minutes remaining,
otherwise restore its channel Live. Activating another source during catch-up is
stop-first.

## Local timeshift and multiview

[TimeshiftController](../qt/src/app/timeshiftcontroller.h) owns the local rolling
buffer, helper processes, timeline and disk quota. It is disabled by default and
requires ffmpeg/ffprobe. Enable/disable applies to current single Live playback;
storage/window/quota changes apply on the next stream. Local starvation gets a
chance to recover before generic player reconnect.

[MultiViewController](../qt/src/app/multiviewcontroller.h) owns PiP/grid transitions.
Closing PiP preserves the visible primary stream without reload; swapping PiP
streams does not reconnect them. Grid promotion may retain background streams
when configured. Stop in an active grid targets its focused tile. Stop while a
promoted retained grid is hidden restores the original grid, closes the promoted
tile and focuses a remaining tile. `Ctrl+O` clears retained background streams or
closes an active grid while preserving committed playback.

Windows startup currently performs an experimental idle auxiliary-backend
initialization/retirement. `OKILTV_STARTUP_PIP_CLEANUP=0` disables it; `1` enables it
on other platforms. It must not change playback/layout, and its reported memory
benefit is not an established explanation of decoder/render teardown.

## Recording and finite downloads

| Mode | Owner and contract |
|---|---|
| Manual recording | Player recording path; `Ctrl+R` falls back to it when no programme hover target exists |
| Scheduled DVR | [DvrController](../qt/src/app/dvrcontroller.h); persisted schedules, offsets, ingest and optional remux |
| Archive download | [CatchupDownloadController](../qt/src/app/catchupdownloadcontroller.h); independent session-only FIFO queue |

DVR and manual recording have separate output/remux settings. Never implicitly
stop recordings to start VOD. Scheduled DVR defaults to checkpointing/stopping VOD
with a notification; `dvrStopVodBeforeRecording=false` permits independent DVR/VOD
and prevents automatic DVR tap handoff from replacing VOD.

Archive downloads require an explicitly selected, fully published ended programme.
They use finite provider URLs and do not share player, DVR, timeshift or archive
playback-session state. One Qt Network worker transfers at a time; queue-wide pause
retains source bytes. ffmpeg remuxes complete local input to a verified MKV without
transcoding. Finite HLS support and resume validation are detailed in
[source-feature-parity.md](../.project/source-feature-parity.md). Preserve usable
partial output and collision-safe filenames through error paths.

Use [playback tests](../qt/tests/tst_playback_components.cpp) for deterministic
policy decisions, Catchup/App suites for transport and real backend integration,
and Download tests for the separate transfer pipeline. Windows visual/audio
continuity still requires manual acceptance; see [build and testing](build-and-testing.md).
