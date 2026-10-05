# Application architecture

[Documentation index](README.md)

OKILTV is a desktop IPTV application for Xtream Codes and M3U sources. Qt Quick
renders its interface; libmpv renders video through an OpenGL surface. SQLite
stores channel data, programme generations and VOD state. The application also
provides catch-up playback, local timeshift, recording, PiP/multiview and an
experimental Xtream movie and series libraries.

## Layers and entry points

| Area | Responsibility and entry points |
|---|---|
| QML | [Main.qml](../qt/qml/Main.qml), screens and components present state and invoke application actions |
| Application | [app](../qt/src/app) owns controllers, Qt models, orchestration and lifecycle |
| Core | [core](../qt/src/core) implements provider access, data models, storage and formatting without QML ownership |
| Player | [player](../qt/src/player) adapts libmpv, rendering and stream transport |
| VOD domain | [core/vod](../qt/src/core/vod) defines independent identities and ports; its domain target links only Qt Core |
| Build and tests | [qt/CMakeLists.txt](../qt/CMakeLists.txt) registers product sources/resources; [qt/tests](../qt/tests) holds regression coverage |

Use [main.cpp](../qt/src/app/main.cpp) to trace construction and wiring.
`constructCoreServices`, `constructAppServices`, `wireInterControllerSignals` and
`registerQmlContextProperties` divide startup assembly. VOD runtime/catalog
objects are also wired there. C++ communicates with QML through properties,
signals, slots and invokable methods; controllers do not manipulate QML objects.

## Ownership map

| Owner | Responsibilities |
|---|---|
| `AppController` | Active source loading, channel/EPG orchestration, catch-up validation, startup restoration and watch tracking |
| `ShellController` | Exclusive overlays, shell visibility, layout and interaction state |
| `PlayerController` | A player's backend objects, observed playback state and execution of playback policy decisions |
| `MultiViewController` | PiP/grid slots, promotion, retained streams and backend ownership transitions |
| `SettingsController` | Editable drafts, validation, Save/reload and presentation settings |
| `SettingsManager` / `SourceStore` | Persisted settings and source summaries/protected details |
| `DvrController` / `TimeshiftController` | Scheduled recording and rolling local media sessions respectively |
| `CatchupDownloadController` | Independent finite archive download queue, owned by `AppController` |
| `VodRuntime` / `VodCatalogModel` | Source policy/category synchronization, VOD session integration, movie/series libraries, independent playback sidebar and episode autoplay |
| `UpdateCheckController` | One asynchronous startup release check and user notification actions |

`ChannelListModel`, `ProfilesModel`, `SourceGroupsModel`, `EpgGridModel` and
`GuideStateModel` present domain data. There are intentionally two `NowNextModel`
instances: `nowNextModel` follows browse selection, while
`playbackNowNextModel` follows the playing channel. Never merge them.

The QML player context property is `appPlayerController`, not `playerController`.
Other important names include `appController`, `shellController`, `vodRuntime`,
`vodCatalog`, `vodSeriesCatalog`, `vodPlaybackCatalog`, `movieSourceGroupsModel`, `seriesSourceGroupsModel`, `settingsController`, `dateTimeFormatter`, `dvrController` and
`multiViewController`. Verify the full list in `main.cpp` before adding bindings.
`MpvVideoItem` is registered in the `OKILTV 1.0` QML module.

`VodEpisodesModel` owns independent library and runtime episode selections and
stable list models; row data updates preserve viewport and focus. The shared
domain `seriesWatched` function derives completion from all known ordinary
episodes, excluding Specials. Bulk manual status changes use the serial progress
lane and one repository transaction.
Domain queue functions in `vodmodels.cpp` serve continuation, Previous/Next and
autoplay. `VodRuntime` owns transition cancellation and deferred return to series
details; autoplay never depends on the library being mounted. Playback stays in
one `VodPlaybackSession` and the existing controller/coordinator. Schema 9 updates
series continuation alongside confirmed episode progress in one SQLite transaction.
Schema 10 adds durable suppression after bulk Watched, without changing playback
history; a confirmed checkpoint of a new session clears it.

Uploaded subtitles use `IVodSubtitleRepository`/`SqliteVodStore` (schema 11) and
`VodSubtitleCache` for durable local files. `VodRuntime` owns a serialized worker
pool, source cancellation, file-dialog pause ownership and shared UI publication.
Playback descriptors carry trusted attachments to `IPlaybackEngine`; actual mpv
track IDs never become durable import identities. Requested pre-play selection is
separate from confirmed session progress. Shutdown cancels and joins subtitle
workers before SQL/Qt teardown; source reconciliation also cleans orphan copies.

## Startup and shutdown

1. Set up platform/bootstrap paths, Fusion style and the OpenGL rendering backend.
2. Load settings and source summaries; initialize/migrate the database on a
   Qt-owned worker before ordinary database access.
3. Construct models/controllers, connect signals and expose the QML context.
4. Load the main window, initialize source/channel data and restore eligible
   playback. Opening the shell is not permission to auto-play browsed content.
5. After QML loads, check for a newer stable release asynchronously. Headless/UI
   tests suppress automatic update checks; network failures are log-only.

[databasestartup.cpp](../qt/src/app/databasestartup.cpp) uses `QThread::create`.
SQLite connections belong to that worker, and completion joins native/thread-local
teardown before returning or rethrowing. Do not replace it with an adopted
`std::async` thread. A database rebuild displays a modal progress window while
keeping the GUI loop responsive; a fresh/already migrated database does not.

Capture the system channel decimal separator before setting `LC_NUMERIC` to `C`
after application construction. mpv needs C numeric parsing; UI channel numbering
still uses the captured locale. Keep desktop OpenGL and the Qt Quick OpenGL
backend: the video item requires them.

Shutdown checkpoints startup playback and VOD progress before stopping visible
transport and disposing backends. Worker jobs must finish before their stores or
controllers disappear. Timeshift/DVR helpers use bounded termination paths;
Windows also has a shutdown watchdog. Preserve this order to prevent ghost audio,
late callbacks and writes into destroyed services.

## Asynchronous work

- VOD library models wait for runtime storage readiness for up to 15 seconds,
  retrying initialization asynchronously during that window. Closing/source changes
  cancel the wait; successful reconciliation resumes catalogue requests.
- `AppController` generation counters reject obsolete profile, EPG and programme
  detail results. Check identity/generation again when publishing a result.
- Cancellation alone is insufficient: already completed callbacks may still be
  queued. Source changes/removal must fence old results and publication.
- Worker code receives snapshots rather than reading mutable GUI state. Create
  and destroy SQLite connections on the thread that uses them.
- Startup group reconciliation reads group metadata only. Full channel loading
  and decryption stay in the active-profile worker.
- EPG has bounded import/read workers and immutable disk generations. VOD has
  bounded jobs, publication barriers and a serial progress lane.
- Each player, including PiP, owns independent buffering/recovery policy state.
  Avoid global counters or one player's timers influencing another player.

Read [playback](playback.md), [sources and EPG](sources-and-epg.md) and
[VOD](vod.md) before changing these ownership boundaries.
