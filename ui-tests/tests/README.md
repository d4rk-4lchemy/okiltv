# Linux UI regression tests

All scenarios use local generated MPEG-TS, M3U and XMLTV fixtures. No IPTV
account, private capture or repository secret is needed.

| Scenario | Coverage |
| --- | --- |
| `05-channel-selection.py` | Hover, pinning, keyboard selection, Guide/Settings handoff, PiP swap/close |
| `06-overlay-inactivity.py` | Input resets timeout; idle chrome collapses; Guide/Settings remain open |
| `07-guide-groups.py` | Selected groups, empty groups and Guide navigation |
| `08-stationary-pointer-navigation.py` | Keyboard scrolling under a stationary pointer |
| `08-ui-transparency.py` | Preview, keyboard/drag changes, Save/discard and playback preservation |
| `09-catchup-download.py` | Guide/right-pane downloads, pause/resume, filenames and partial archives |
| `10-multiview-keyboard.py` | Grid selection, cancellation and keyboard priority |
| `11-multiview-controls.py` | Focused transport and independent backends |
| `12-vod-movies.py` | Movie library, transport, source policies and overflowing Continue watching |
| `13-vod-series.py` | B/V, episodes, keyboard/pointer panels, Settings, Back and responsive layouts |
| `14-epg-search.py` | Live Ctrl+F, source-wide local airings, modal keyboard isolation, stale actions, filters, narrow layouts and fullscreen Escape |

Configure and run the group:

```bash
export PATH=/opt/Qt/6.10.3/gcc_64/bin:$PATH
export CMAKE_PREFIX_PATH=/opt/Qt/6.10.3/gcc_64
cmake --preset qt-linux-debug -DOKILTV_BUILD_UI_TESTS=ON
cmake --build --preset qt-linux-debug --target OKILTVQt -j"$(scripts/build_jobs.sh)"
ctest --preset qt-linux-debug -L '^ui$' --output-on-failure --no-tests=error
```

Single scenario, with an isolated D-Bus session and disposable unlocked keyring:

```bash
bash scripts/ci/run_ui_test.sh ui-tests/tests/05-channel-selection.py   qt/out/build/qt-linux-debug/app/OKILTV /tmp/okiltv-channel-selection
```

Dependencies: Python 3, ffmpeg, libmpv2, libsecret-1-0, gnome-keyring,
dbus-run-session, Xvfb, Openbox, xdotool, x11-utils and Mesa software OpenGL.
The helper does not use the user's keyring or change the application security
backend. Run scenarios sequentially; CTest enforces a shared UI resource lock.

CTest writes scenario logs, snapshots and screenshots under
`qt/out/build/qt-linux-debug/ui-artifacts/`. CI retains reports and selected
UI diagnostics for seven days, excluding generated media and application data.

Wait for overlay visibility/geometry and the effective `enabled` state before
pointer input; the Settings test also waits for `hovered` to confirm pointer
delivery to `ui.live.settingsButton`. The application
intentionally ignores input during slide animations; fixed sleeps alone do not
establish that a control is ready. The channel-selection helper repeats real mouse
movement while waiting, because movement during auto-hide animations is ignored.
Channel targeting uses the rendered `ui.live.channelName.<id>` bounds and waits
for that row to be enabled, hovered and selected. Pinning steps additionally wait
for the actual `pinned` state after the click, so a missed click cannot silently
pass as hover selection. Picker rows are targeted by rendered labels. Timeout
errors include selected/playing channel IDs and hovered/pinned rows; the full
state is also retained in `failure.json`.
Guide channel targeting excludes hidden Live delegates with identical labels and
waits for enabled Guide content after chrome animation. PiP assertions inspect
the actual layout and both tile channel IDs on open/assignment/swap, and require
layout `off` on close; unchanged primary playback alone cannot satisfy them.

## Catch-up download regression

`09-catchup-download.py` uses generated local media and XMLTV to check `Ctrl+D` from Guide and the right EPG pane, cancelling the Save File dialog, channel/date/time/title MKV filenames using explicit saved date/time settings, filename collisions, preservation of incomplete archives as `.partial.mkv` and cancellation cleanup. Requires ffmpeg/ffprobe and the existing Xvfb/Openbox/xdotool stack. Run through `scripts/ci/run_ui_test.sh` for an isolated Secret Service session.

### Grid keyboard selection

`10-multiview-keyboard.py` uses local media and separate Ctrl key-down/key-up events
to verify candidate movement, release-to-commit, edge wrapping, empty tiles,
cancellation and keyboard priority. Run it through `scripts/ci/run_ui_test.sh`.
The bridge exposes read-only `multiview` mode, focused index, candidate index,
selection activity, gesture availability and input context for these assertions.
The window snapshot also reports activation so tests wait for focus restoration
after minimizing, rather than sending input into an inactive window.
PiP selection in the multiview soak script uses a mouse click; grid selection
holds Ctrl across arrows and commits on release. Ctrl+Shift+O is removed.

### Focused multiview controls

`11-multiview-controls.py` verifies Space and transport buttons against the actual
pause state of two independent local backends, focused-channel stepping, browse
selection versus playback, EPG changes, empty tiles and focused Stop.
The multiview snapshot includes focused playback state and per-tile channel,
pause, position and video-surface/backend identity; provider URLs are not exposed.

### Test Movies library

`12-vod-movies.py` uses a local Xtream API and generated media. It exercises V,
text entry (including V), Ctrl+F, details, Escape, catalog reopening, playback
handoff and session-only activation. It also checks hidden Live rails/Guide during
VOD, the playing title and duration, exact click/drag seeks while paused, title
stability while browsing another movie and restored Live rails after Stop. The
local media endpoint supports HTTP byte ranges for real mpv seeks. Run through `scripts/ci/run_ui_test.sh` for
an isolated unlocked keyring; CTest registers it as `UI-12-vod-movies` when
`OKILTV_BUILD_UI_TESTS=ON`.

The VOD movie scenario also covers Settings → Sources: Enable VOD below the
archive margin, Live TV/Movies/Series segments, empty provider movie categories,
Series category configuration, independent M3U support hints, drafts across source
switches, Discard and persisted opt-out removing the source from the library.
All API/media requests use its local fixture; it never needs a provider account.

### Series library

`13-vod-series.py` shares the movie scenario's local media/keyring fixture and
adds lazy series metadata, season gaps, B/V switching without loading media,
right-panel selection/Enter/single-click activation, Settings during playback,
Back to the owning details and narrow/wide captures. CTest registers
`UI-13-vod-series` with the same desktop resource lock.

### Live programme search

`14-epg-search.py` generates local media/XMLTV and two source profiles, including
shared EPG IDs, hidden groups, local station logos and enough airings to exceed
a page. Result-capture pixel checks verify that both channel logos render beside
the airing text, including the application icon-cache path. It checks
Ctrl+F from the channel editor without changing its filter, current-result action
fences, normal Tab channel search, popup Tab cycling through All/Now/Upcoming/Past
with wrapping and preserved query/focus/playback, Shift+Tab control navigation,
unbound Ctrl+Tab, isolated popup shortcut behavior, Unicode
prefix matching, unavailable archives, hover/auto-hide isolation, explicit tune
outside the rail filter, protected Guide and single-Escape fullscreen behavior.
It also checks query-only opening, a close button 12 px to the right with its top
edge aligned to the query field, fixed field
position, inline selected actions, collapsing on clear, closing with X and a
real double click separated by 100 ms across an inline layout change. An
unwatched archive has neither a duplicate Play from beginning control nor an
unavailable Schedule recording control; Ctrl+Enter
activates the current channel outside the retained rail filter.
Captures cover initial/results/empty states and 1920×1080, 1280×720, 800×600 and
426×240 layouts. The semantic bridge exposes `epgSearch` session/result identities
and the window's active focus object; no provider credentials are needed.
The focused `OKILTV_EPG_SEARCH_SMOKE=actions` run checks the unstarted archive
control and native Ctrl+Return/Ctrl+keypad-Enter fallback in Main/Live.
The focused `OKILTV_EPG_SEARCH_SMOKE=pointer` run uses the same isolated fixture
to verify selected-header collapse/reopen without tuning, double-click activation
from expanded and collapsed states, reflow and asynchronous detail loading.
The focused `OKILTV_EPG_SEARCH_SMOKE=punctuation` run types hyphen, dot,
parenthesis and quoted variants of `Spider-M` into the actual query field,
checks both channel results for `Spider-Man`, rejects `Spider-X` and preserves
playback. Unicode dashes and accent normalization are covered by the store suite;
xdotool drops Unicode dash keysyms with the fixture's Xvfb keyboard map.
It uses the same isolated application/keyring runner:

```bash
OKILTV_EPG_SEARCH_SMOKE=punctuation bash scripts/ci/run_ui_test.sh \
  ui-tests/tests/14-epg-search.py qt/out/build/qt-linux-debug/app/OKILTV \
  qt/out/build/qt-linux-debug/ui-artifacts/14-epg-search-punctuation
```
