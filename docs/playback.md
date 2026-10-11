# Playback and recording

[Documentation index](README.md)

## Ownership and state

[PlayerController](../qt/src/app/playercontroller.h) owns backend objects and
executes decisions from independent policy components in
[app/playback](../qt/src/app/playback). `isPlaying` follows observed mpv state;
QML must never flip it optimistically. Browse selection does not tune a stream.

The media-switch Live TV action uses `VodRuntime::returnToLive` to cancel pending VOD
starts/autoplay, wait for matching stop acknowledgement and flush serial progress
writes before emitting `liveRequested`. `AppController::returnToLive` then resolves
the last primary channel identity against the current source/model or durable
channel cache. It never substitutes the browsed/first channel. Without in-session
history it uses the saved last channel of the active Live source. Missing channels
leave Live idle; active Live/multiview avoids a duplicate retune. Buffered Live and
timeshift keep their existing position, pause state and refill policy during library
return, without a seek or reload. GO LIVE remains a separate transport action.
Catch-up uses its existing return-to-live control, with engine-bound catch-up
recordings kept protected. Opening Movies/Series leaves Live/catch-up/multiview running; only VOD
gets a session-owned temporary library pause.

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

`MpvPlayer::State` is allocated in the constructor and retained until destruction;
unload clears its handles and flags without resetting that allocation. Its
`initialized` flag stays atomic for the render thread's fast path. Scope-only
configuration, buffering, hardware-decoding, deinterlace and initialization
blocks use reference-based `std::lock_guard` on the existing `QMutex` to avoid
MinGW GCC 13's false null-state diagnostics through `QMutexLocker`. Lock scope,
atomic memory ordering and playback behavior are unchanged.

macOS requests OpenGL 3.2 Core before application creation and logs the actual
render context/profile and current CGL availability. It retains the existing
Qt-owned FBO/render API and `hwdec=auto-safe`; VideoToolbox/fallback, audio and
visible frames require Apple Silicon testing. mpv resolves versioned bundled
`Contents/Frameworks/libmpv*.dylib` after an explicit library override. Packaged
ffmpeg/ffprobe live beside the bundle executable and use existing helper lookup.
IOKit sleep assertions follow the existing playback/window/settings policy.
See [macOS build/acceptance](macos-build.md) for tested build scope and pending
runtime gates.

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

## Buffer telemetry and Live recovery

Ordinary primary Live temporarily slows playback when delivery learning raises
the effective reserve target and a fresh playback-relative reserve is below that
new target. `PlaybackBuffering` owns the decision and monotonic played-time
budget; `PlayerController` executes it through temporary `MpvPlayer` speed
ownership. The 30-second progressing-playback window includes a two-second linear
ramp from 1.00 to 0.90, a 26-second plateau and a two-second return to 1.00. It saves
approximately 2.8 seconds of media consumption relative to normal speed, assuming
uninterrupted playback. Actual reserve growth still depends on incoming media.
Pause, cache pause, frozen/missing playback clocks, stale reserve and sampling gaps
over one second do not count towards that budget. Further target increases do not
extend or queue another window; completion/cancellation imposes a 30-second
monotonic cooldown. A new tune resets the policy and cooldown.

The backend enables `audio-pitch-correction` before changing speed and restores
its previous value after returning to normal speed. mpv handles synchronized
audio/video stretching; slower tempo can remain audible, particularly with music.
Custom non-1.00 speeds and compressed audio passthrough suppress this adaptation.
Stop, seek, retune, media-period replacement, recovery/refill holds, backend
handoff and timeshift/catch-up transitions cancel speed ownership. Manual pause
retains the same window without auto-resuming. Catch-up, local timeshift,
PiP/grid playback and VOD do not use it. `MultiViewController` disables it for all
visible/retained multiview sessions and re-enables only the single primary session.
Speed controls never initialize a dormant backend. F3 shows the temporary
requested speed and remaining progressing-playback
seconds; diagnostics log start/completion/cancellation without source URLs.
Deterministic policy and real-mpv null-output tests verify the transitions,
cancellation, audio progress and option restoration. Audible quality and visible
transition smoothness on Windows remain pending manual acceptance.

Live read-ahead has no application time ceiling. Initialization, policy reset
and adaptive retuning set both `cache-secs` and `demuxer-readahead-secs` to the
largest finite double accepted by mpv (`DBL_MAX`; mpv rejects infinity).
`demuxer-max-bytes` and `demuxer-max-back-bytes` retain their existing budgets;
Live hysteresis remains zero so reads resume as memory becomes available.
The configured/learned reserve still controls startup/refill readiness, not how
far ahead media may be downloaded. `cacheBudgetSecondsForBufferTarget()` retains
`clamp(max(3 * reserve, reserve + 8), 10, 120)` solely as the reference duration
for the unchanged byte-budget calculation, plus the existing 30-second history
allowance. The reference duration is never passed to Live's mpv time options.
The diagnostic snapshot's Live forward-limit reports `Memory limited`, with no numeric
time ceiling. Catch-up retains its active 90/85-second and standby 30/25-second
bands; typed on-demand loads explicitly preserve their previous reserve-derived
time window rather than inheriting the Live backend default.

`MpvPlayer::playbackBufferSnapshot()` reports the forward reserve from the current
playback position to the observed continuous media end. It uses the minimum known
active audio/video end from `demuxer-cache-state/ts-per-stream`, translated into
the aggregate cache/playback clock before subtraction; subtitles do not limit
reserve. Some mpv versions rebase only the aggregate timestamps, leaving stream
timestamps in the original MPEG-TS epoch. The translation is inferred from
matching stream/aggregate duration and endpoint observations in the same cache
state. Missing or ambiguous correspondence uses the already rebased aggregate
clock as an explicit estimate; raw stream PTS must never enter display or Live
recovery as forward seconds. Overlapping seekable ranges are merged, and a later disconnected
range is excluded. The final GOP may extend beyond the seekable range's last
keyframe. There is no display cap at the configured reserve or byte-budget
reference duration.
The raw `demuxer-cache-duration` remains available for guarded startup and
archive transport policies and diagnostics. Ordinary Live refill, reconnect refill
and Live health watchdogs use the same playback-relative observation as F3.

Fast telemetry samples the position and cache state together approximately every
100 ms. Buffer observations expire after one second and are fenced by load
generation; stop, retune, MPEG-TS period replacement and seek invalidate them.
Seek observations resume after backend playback restart. Slow metadata publication
does not overwrite the fast clock/cache sample. Inconsistent clocks produce no
reserve; duration-only or aggregate-clock fallbacks are marked as estimates.

F3 uses this observation for Live, catch-up and VOD, displays one decimal place,
prefixes estimates with `≈`, and displays `N/A` for unavailable/stale observations.
Catch-up diagnostics no longer clamp an observation to 90 seconds. Timeshift's
separate distance to the live edge retains its meaning.
During a Live refill hold, F3 shows that hold's frozen goal, including reconnect
refill, even when the configured or learned target changes in the background.

Ordinary Live keeps playing through delivery gaps while forward reserve remains.
A reserve pause requires distinct fresh observations at or below 0.2 seconds for
at least 500 ms; a single zero, a repeated cached sample, stale/missing telemetry,
seek/load invalidation, reader idle or EOF does not confirm depletion. Manual
pause and guarded startup remain independent. The refill goal is frozen at pause
entry (`max(3 seconds, configured reserve, learned reserve)`); subsequent delivery
learning can expand download capacity without raising that hold's goal. Resume
permits a 0.3-second tolerance. After at least one second, reader idle with more
than one second of reserve releases a capacity-limited hold. EOF releases the
remaining tail, and a 30-second absolute hold deadline releases partial or unknown
reserve. Freshly observed reserve at or below 0.05 seconds on EOF/deadline enters
recovery; unavailable telemetry alone never requests it. Incomplete ordinary
refill releases retain a ten-second retry cooldown, without resetting delivery
learning. Stop/seek/retune cancel confirmation, hold and cooldown.

Live reconnect refill uses the same confirmation, frozen goal, tolerance and
30-second maximum per hold, including initial paused reserve rebuilding. EOF and
hold deadlines drain usable media before trying another connection. Plain Live
keeps mpv automatic cache pauses disabled after refill/recovery, so an independent
raw-duration gate cannot override the application reserve policy. No-refill
recovery requires three fresh observations of exhausted reserve (at most 0.05 s)
without media growth or healthy observed playback progress; positive incoming
bytes alone do not prove usable refill.
A stalled decoder with usable reserve takes the separate decoder/frame watchdog
path, including audio-only playback. Intentional/manual pauses do not enter that
path. Transition diagnostics include forward and raw reserve, freshness, frozen
goal, elapsed hold time and release reason, without provider URLs.

Live reconnect stabilization requires 12 seconds of observed progressing playback
with healthy backend/frame state and fresh buffer observations. A high stable
reserve (within 0.25 seconds of the effective configured/learned Live reserve)
qualifies without requiring net buffer growth. Growth, advancing cache end and
positive input rate provide delivery evidence over a five-second window, allowing
normal provider bursts and brief reserve dips above 0.5 seconds. Sustained draining
without delivery does not finish stabilization, but playable cached media drains
before reconnect. Stabilization/total deadlines do not stop freshly observed
progressing playback with usable reserve. Missing observations/manual pause do
not count toward healthy time; real backend/frame failures and exhausted reserve
retain recovery. Catch-up keeps its independent playback-based stabilization and
does not adopt Live reserve thresholds.

Deterministic playback tests cover plateaus, bursts, lost delivery, pauses, stale
observations, distinct-sample depletion confirmation, frozen refill goals, tolerant
resume, capacity/EOF/deadline tail draining, audio-only decoder stalls, A/V clocks,
rebased MPEG-TS epochs, subtitles and disconnected ranges. App regressions inject
contradictory raw/forward metrics and verify F3 and Live decisions agree.
App tests cover F3 formatting/generation/age gates and real mpv reserve/seek
behavior for WAV and MPEG-TS with a 40,000-second timestamp origin, with rebasing
enabled and disabled. Real mpv checks cover reads beyond the former 11-second
ceiling, stopping and refilling at a finite byte budget, and restoration of Live
options after a catch-up policy. Windows hardware
display and buffering acceptance remains a manual check.

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

Only HTTP MPEG-TS paths use the owned catch-up stream protocol. HLS and other
media retain native HTTP URLs, allowing mpv to resolve relative playlist/segment
URIs against the provider response and redirects.

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
chance to recover before generic player reconnect. Automatic generation
continuation requires a later live edge and clamps its target to at least the
current edge; exhausted failed generations cannot return to retained older data.
DVR ingest, timeshift preflight/ingest and reconnect use `Core::mediaInputOptions`
from the shared media request headers (User-Agent, custom headers and referrer).
These HTTP-only input options precede the input and are omitted for local/UDP media.

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

DVR and manual recording have separate output/remux settings. Both retain the
source TS unless FFmpeg exits normally with code zero and worker-side ffprobe
verification confirms a nonempty output whose duration matches the source within
one second. Failed probes or partial output never authorize source deletion. Never implicitly
stop recordings to start VOD. Scheduled DVR defaults to checkpointing/stopping VOD
with a notification; `dvrStopVodBeforeRecording=false` permits independent DVR/VOD
and prevents automatic DVR tap handoff from replacing VOD.

Archive downloads require an explicitly selected, fully published ended programme.
They use finite provider URLs and do not share player, DVR, timeshift or archive
playback-session state. One Qt Network worker transfers at a time; queue-wide pause
retains source bytes. ffmpeg remuxes complete local input to a verified MKV without
transcoding. Finite HLS support and resume validation are detailed in
[source-feature-parity.md](../.project/source-feature-parity.md). Preserve usable
partial output and collision-safe filenames through error paths. HLS failures
retain the root manifest and its complete downloaded resource graph at their
original paths (including local remux/validation failure), and report that location.
Success and explicit cancellation remove the graph after workers stop.

Use [playback tests](../qt/tests/tst_playback_components.cpp) for deterministic
policy decisions, Catchup/App suites for transport and real backend integration,
and Download tests for the separate transfer pipeline. Windows visual/audio
continuity still requires manual acceptance; see [build and testing](build-and-testing.md).
