# EPG search validation

UI validation date: 2026-10-06. Base revision:
`15ee2f5be4f407e90733b8d9a4291646b8027070`. The feature changes are in the working
tree; this record does not identify an additional commit or a published release.
Fixtures are generated local M3U/XMLTV/media/HTTP data, with disposable application
storage and keyrings. No provider accounts or repository secrets are used.

## Implementation under test

Live `Ctrl+F` opens a modal programme search over the current video surface.
The compact UI opens with only a fixed-position 28 px query field with `Search...`
and a close button aligned with the field's top edge, 12 px to its right.
All, Now, Upcoming and Past use tabs with an active underline. There is no
result-count summary; pagination and error/Retry controls remain available.
The query field is borderless, including while focused. It does not
dim the video. Live rail chrome and global transparency
apply to the field/results/actions; clearing hides filters, results and details.
The selected result expands inline; descriptions are bounded and actions wrap.
Actions use 28 × 28 px icon buttons with 18 px SVG images and the original labels
in accessible names and hover tooltips (400 ms delay). Play/Watch live/Resume use
`play.svg`, restart uses `start-from-beginning.svg`, DVR uses `dvr.svg`, and downloads use `download.svg`. Unscheduled past airings
hide Schedule recording; an existing job retains Cancel recording.
The shared channel field keeps its 14 px typography and original behavior.
Channel search remains on Tab; Guide, Settings, VOD and protected dialogs keep
their input ownership. Inside EPG search, Tab cycles All → Now → Upcoming → Past
→ All and focuses the query without changing its text/cursor/selection.
Shift+Tab retains backwards control navigation; Ctrl+Tab remains unbound.
Filter changes also work before typing and during result loading; protected
interactions, IME composition and held-key repeats do not change filters.
Queries cover the active source's eligible channels,
independently of the rail filter and Guide horizon. Selection does not tune.
Live, archive, DVR and download actions use the existing application adapters.
Archive playback in grid multiview reports its unavailable destination policy.

The immutable SQLite search schema is version 2, with normalization version 1.
The ordinary EPG metadata remains version 1 for rollback compatibility. FTS5
uses chronological document IDs and bounded time-group reads. Start date precedes
match quality; exact/prefix/subtitle quality only breaks equal-time ties. All
results group current and upcoming before past, with nearest upcoming/newest past
first; SQL and memory readers share this order across page boundaries. Mapping, scope filtering and deduplication precede visible pages of 50.
Queries have a frozen UTC clock and explicit request/source/snapshot fences.
Punctuation separates words; every token in a valid query matches a word prefix,
including one-character fragments (`Spider-M` matches `Spider-Man`). At least
one token must still contain two characters. This is a query/highlight change;
the normalized index content and schema versions remain compatible.

Search and details run asynchronously. Initial XMLTV import and legacy index
preparation expose Preparing; a refresh retains a usable prior snapshot. Import
publication is atomic, and failed/cancelled preparation retains ordinary EPG.
Shutdown fences late work, cancels tokens and joins the import pool. FTS population
checks cancellation between prepared inserts and commits every 1000 documents.

## Executed checks

Compact-UI checks below are dated 2026-10-06. Full regressions,
benchmark and Windows checks remain the 2026-10-05 baseline, explicitly labelled;
they do not establish acceptance of the revised UI on Windows.

| Check | Result | Evidence/scope |
| --- | --- | --- |
| Linux Debug application build | Passed | Qt 6.10.3; application QML compilation and linkage |
| Full Linux non-UI regression | Baseline passed (2026-10-05): 31/31 | C++, offscreen QML, playback, downloads, DVR/application adapters and VOD; 1093.69 s |
| Full-window Linux regression | Baseline passed (2026-10-05): 11/11 across the batch and isolated reruns | Final UI-07: 41.38 s; UI-10: 87.56 s; UI-12: 390.98 s; final UI-14: 121.16 s; corrected execution failures are described below |
| EPG search CTest group | Passed: 3/3 | Date-first ordering across all filters and page boundaries, store/model fences, cached-logo file URLs with spaces/URL punctuation, compact QML layout, selected-header collapse/reopen, expanded/collapsed double-click, clearing, logos, transparency, undimmed bright-background pixels, outside-click blocking, selected-only details, button/double-click isolation and small-window action/Retry access; Ctrl+Enter/Return restart/default routing, including stale resume availability; the final QML rerun passed after adapting the tooltip assertion to the 400 ms delay |
| Store Qt Test suite | Passed; benchmark skipped in ordinary run | Actual QSQLITE FTS5, Unicode/highlights, query limits/operators, SQL/memory parity, shared EPG IDs, scope before pagination, all time/rank buckets, exact-index/fallback parity, migration/retry, population cancellation and shutdown; ten punctuation-prefix cases check title/subtitle matches and original highlight spans, with nonmatches and the single-letter limit |
| Production AppController adapters | Passed: 8 | Unstarted/resumable/completed/sub-minute archive restart visibility, hidden past recording scheduling, visible current-airing DVR/restart, scope/actions, deferred catch-up fences after scope/EPG/source changes, initial import readiness and retained snapshots |
| QML component checks | Passed | Compact EPG suite uses `failOnWarning` and a Qt Quick Test executable embedding the five real SVG resources; it verifies loaded icon mappings, 28 px buttons, hover tooltips, accessible names and hidden past scheduling. Tab cycles filters from query/list/filter/description/action/close controls and query-only/loading states, preserving text/cursor/selection; Shift+Tab traverses visible controls backwards, including from details to the results focus scope. Modified Tab, held-key repeats and blocked/closed input do not cycle filters. Shared channel header passed. Broader QML batch passed 12/13 before the icon change; the VOD suite passed its isolated rerun without code changes (18.63 s). |
| Full-window UI-14 | Passed: 103.79 s | Two complete Tab filter cycles preserve query/focus/playback, Shift+Tab leaves the editor inside the popup, and Ctrl+Tab does not change the filter; unwatched archive hides redundant restart and Schedule recording; native Ctrl+Return default activation; rendered station-logo pixel checks, 64 px result headers, fixed field/diagonal X, compact inline icon actions, collapsing on clear, X close/reopen, preserved rail filter/playback, pagination, shortcut/dialog ownership, four window sizes, fullscreen Escape and native double-click activation across reflow/detail loading |
| Native punctuation-prefix smoke | Passed | Actual query-field typing: `Spider-M`, `Spider.M`, `Spider (M)` and quoted `Spider-M` find both `Spider-Man` channel results; `Spider-X` finds none; closing retains playback. Unicode-dash typing was excluded after xdotool dropped the dash in the Xvfb keyboard map; Unicode dashes and accents pass the SQL/memory regression cases. |
| Extended full-window smoke | Passed | Two PiP/grid streams, selected tile, pause and render/backend bindings retained; pending grid candidate cancelled; source with no EPG; real 257-character limit/error and recovery |
| Delayed XMLTV smoke | Passed | Real Preparing → current results, no old actions, explicit query-limit error and recovery |
| Native action-shortcut smoke | Passed | Real Main/Live routing: unstarted archive has no duplicate restart control; Ctrl+Return and Ctrl+keypad-Enter fall back to the primary action. Controller tests cover fresh restart availability and expired-resume fallback. |
| Native pointer smoke | Passed | Selected-header collapse/reopen without tuning, expanded/collapsed selected-row double-click, rendered station-logo pixel checks and real unselected-row double click separated by 100 ms, preserved header identity through expansion and deferred activation until selected details are ready |
| 200% scale smoke | Passed | Compact query/close bounds and focus at 800×500 and 426×240 logical pixels |
| `qmllint_okiltv` | Passed | No warnings in the target output |
| Focused core `clang-tidy` | Passed | No product diagnostics for `epgsearchtypes.cpp`, `epgstore.cpp` and `epgservice.cpp` (including shared `epgsearch_p.h`) after the punctuation-prefix correction |
| Full-source `clang-tidy` and focused follow-up | Baseline passed (2026-10-05); 0 errors | 82 deduplicated product translation units using the repository policy; one existing unique `performance-no-automatic-move` warning in `channelnumber.h` remains |
| Windows Release cross-build | Baseline passed (2026-10-05) | MinGW app, StoreTests and ModelTests with Qt 6.10.3 Windows SDK |
| Windows StoreTests under Wine 10 | Baseline passed (2026-10-05): 16; benchmark skipped | Actual Windows SDK `sqldrivers/qsqlite.dll`, including indexed search, cache/migration and cancellation tests; isolated Wine prefix |
| Windows ModelTests under Wine 10 | Baseline passed (2026-10-05): 10 | Controller/model race and selection tests; native Windows acceptance remains separate |

The baseline full-window regression initially passed 8/11 scenarios. UI-07 could not start
because a concurrent Wine/Xvfb run occupied its display; its isolated rerun
passed. UI-12 still used the former Live Ctrl+F channel-search path; UI-10's
replacement Tab moved an already focused editor to media navigation. The steps
now use Tab to enter Live search and verify that modified Enter retains editor
focus; UI-10 passed. The next UI-12 run reached
its final Settings checks, but a caption click used an earlier rail position
and selected Player instead of Sources. Caption clicks now wait for stable
on-screen bounds, and the complete UI-12 rerun passed. The first full-source
analysis ran during header edits and produced transient parse errors; acceptance
uses a separate frozen-source pass.
The frozen pass flagged the search backend's by-value snapshot ownership;
that ownership intentionally pins the immutable generation for the operation
and now has a scoped, explained annotation. A focused rerun of `epgservice.cpp`
reported no diagnostics. All three full-source partitions exited successfully;
the remaining warning is existing channel-number code outside this feature.

## Performance measurements

These measurements are the 2026-10-05 match-rank-first baseline, before the
current date-first ordering. They have not been rerun for the chronological reader.

Reference environment: Intel Core i9-13900H, four available logical CPUs, 4 GiB
RAM, Debian 13 container. Product code is Linux Debug; the Qt SDK libraries are
release Qt 6.10.3. Queries use Qt QSQLITE's SQLite 3.51.3, rather than the system
sqlite CLI. Temporary databases are on the container's `/tmp` tmpfs filesystem.

Each fixture has 1000 eligible channels and 100,000 or 1,000,000 generated airings
spanning Now/Upcoming/Past. Measurements use a first read after building the
database, then five warm reads. The empirical warm p95 is the maximum of those
five samples; this is a small local sample, not a CI latency guarantee. Times
cover backend first-page execution, excluding the 150 ms debounce and UI
publication. No operating-system cache eviction was performed, so the first
read is **not** a true cold-cache measurement.

| Query class | 100k first / warm p95 | 1m first / warm p95 |
| --- | ---: | ---: |
| Rare title | 4 / 4 ms | 5 / 5 ms |
| Popular two-character prefix | 4 / 4 ms | 14 / 14 ms |
| Multiple word prefixes | 11 / 14 ms | 84 / 92 ms |
| No matches | 2 / 2 ms | 3 / 4 ms |
| Rare exact title among common word matches, Upcoming | 12 / 12 ms | 63 / 74 ms |

| Storage/build measurement | 100k | 1m |
| --- | ---: | ---: |
| Build time | 3685 ms | 40,917 ms |
| Complete database | 48,709,632 B | 495,407,104 B |
| Incremental search storage | 34,582,528 B | 351,580,160 B |
| Compact ordinary EPG baseline | 14,127,104 B | 143,826,944 B |
| Peak benchmark process RSS | 30,600 KiB | 34,296 KiB |
| Search cancellation after 65 checks | 3 ms | 3 ms |

The SDK omits SQLite's `dbstat` module. Incremental search storage is the full
database size minus a compact copied fixture with search tables removed and
`VACUUM` applied; it includes search-related file overhead and is not an exact
per-table FTS byte count. RSS measures the benchmark process, excluding the
kernel's filesystem cache and separate UI processes.

A temporary, uncommitted hook around public Qt SQL calls measured the final
million-airing build phases. Prepared FTS population took 6242 ms in total; the
longest individual insert was 3911 µs and the longest commit 26,611 µs. Remaining
whole SQL phases included chronological ordering at 2972 ms, FTS integrity at
1321 ms, `quick_check` at 1969 ms and FTS optimization at 528 ms. Cancellation is
checked between these phases; shutdown can wait for an executing SQL statement.
The measurements do not establish an instantaneous global cancellation bound.

Reproduce the benchmark explicitly; ordinary CTest skips it:

```bash
OKILTV_EPG_SEARCH_BENCHMARK=100000 \
  qt/out/build/qt-linux-debug/qt/tests/OKILTVQtEpgSearchStoreTests benchmark
OKILTV_EPG_SEARCH_BENCHMARK=1000000 \
  qt/out/build/qt-linux-debug/qt/tests/OKILTVQtEpgSearchStoreTests benchmark
```

## Actual screenshots

These captures come from the full application and local fixtures with station logos.
The Results capture is checked for rendered pixels from two distinct channel logos.
They are
copied into the repository so the report does not depend on temporary build
artifacts.

The stored captures precede removal of the query focus border. Initial, Results,
Empty results and the four 100% window-size captures show the current icon
actions. The video, preparation, PiP and 200% captures are earlier
2026-10-06 layout references with text action buttons; those specific smoke runs
have not been repeated for the icon change.

| State/layout | Capture |
| --- | --- |
| Initial query only | [Initial](screenshots/epg-search/initial.png) |
| Results and inline selected details | [Results](screenshots/epg-search/results.png) |
| Punctuation prefix finds Spider-Man on both channels | [Punctuation](screenshots/epg-search/punctuation-prefix.png) |
| Station logos over decoded generated video | [Video](screenshots/epg-search/results-video.png) |
| No matching programmes | [Empty results](screenshots/epg-search/empty-results.png) |
| No local EPG in the selected source | [No EPG](screenshots/epg-search/no-epg.png) |
| Real initial XMLTV preparation | [Preparing](screenshots/epg-search/preparing.png) |
| Results after initial import | [Ready](screenshots/epg-search/preparing-ready.png) |
| Real input-limit error | [Query limit](screenshots/epg-search/query-limit-error.png) |
| Search with retained PiP state | [PiP](screenshots/epg-search/pip.png) |
| 1920×1080 | [Wide](screenshots/epg-search/layout-1920x1080.png) |
| 1280×720 | [Medium](screenshots/epg-search/layout-1280x720.png) |
| 800×600 | [Compact list](screenshots/epg-search/layout-800x600.png) |
| 426×240 | [Small viewport](screenshots/epg-search/layout-426x240.png) |
| 200%, 800×500 logical | [Scaled](screenshots/epg-search/scale-200-800x500.png) |
| 200%, 426×240 logical | [Scaled compact](screenshots/epg-search/scale-200-426x240.png) |

## Outstanding acceptance

- Native Windows packaged-app UI, keyboard/IME, native file dialog and hardware
  rendering tests have not been run. Wine execution validates the actual Windows
  SQL plugin and binaries, but does not establish native Windows UI acceptance.
- Some Xvfb captures have black video pixels despite available decoded-video
  metadata; the final pointer capture shows decoded generated video under search.
  Tests verify playback identity, pause, stream retention and render/backend
  bindings. Real-display visual acceptance remains pending; synthetic fixture
  frames do not establish hardware playback quality. No player refactor was included.
- True OS-cold reads, frame-time/input-latency profiling during playback, native
  IME composition and screen-reader acceptance have not been measured. Automated
  scale checks cover 100% and 200%, not every intermediate scale factor.
- Capability errors are explicit in the implementation; the installed Linux and
  Windows SDKs both support FTS5. An actual delivered plugin without FTS5 was not
  available for a platform run, and no system-sqlite scan fallback is used.

See [Sources and EPG](sources-and-epg.md#live-epg-search),
[UI and interaction](ui-and-interaction.md#epg-search-interaction) and
[the UI scenario guide](../ui-tests/tests/README.md#live-programme-search) for the
permanent contracts and isolated smoke commands.
