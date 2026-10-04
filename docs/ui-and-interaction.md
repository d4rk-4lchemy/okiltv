# QML UI and interaction

[Documentation index](README.md)

## Shell and visual rules

[Main.qml](../qt/qml/Main.qml) owns the application window and global shortcuts.
[LiveTvPage.qml](../qt/qml/screens/LiveTvPage.qml) stays rendered behind overlays.
`ShellController.activeOverlay` is `none`, `guide`, `settings` or `vod`; these are
exclusive overlays, not routes to replacement playback pages.

| Screen | Role |
|---|---|
| [LiveTvPage](../qt/qml/screens/LiveTvPage.qml) | Video, Live rails, transport, hover details and multiview presentation |
| [GuidePage](../qt/qml/screens/GuidePage.qml) | Programme grid, selection and explicit tuning/catch-up actions |
| [SettingsPage](../qt/qml/screens/SettingsPage.qml) | Draft settings and source management in a right-anchored overlay |
| [VodLibraryPage](../qt/qml/screens/VodLibraryPage.qml) | Shared full-window movie/series library and details; VodMoviesPage is a compatibility wrapper |

Use [Theme.js](../qt/qml/theme/Theme.js) for colors, dimensions, typography and
animation tokens. All new overlays use the shared black/grey `overlayBackground`,
`overlaySidebar`, `overlaySurface*`, `overlayBorder` and `overlayText*` palette.
Large backgrounds, cards and controls remain neutral; color belongs to small
selection accents, status indicators and semantic badges. VOD library buttons, navigation and selectors use neutral grey interaction states,
including keyboard focus. The primary movie Play/Resume button is the explicit
exception: blue background, play icon and Play or Resume · HH:MM text. Other movie
detail actions use icon buttons with descriptive tooltips. Watched/unwatched,
Play from beginning, To Watch and Favourites share 24 × 24 px icon areas inside
42 × 42 px buttons. The details return
control shows `left-arrow.svg` and bold “Back” with a compact, single-space-sized
gap (4 px layout spacing), without background or border;
hover and keyboard focus underline its label. To Watch and Favourites
buttons also appear at the bottom left/right of hovered grid and Continue watching
posters; they consume clicks without opening details. Active list icons use the
same blue `Theme.accent`; inactive icons preserve their SVG colors. The corresponding
library category rows and playback groups use bookmark/favourites icons; Continue
watching uses `play.svg`. Membership
writes disable both controls for the affected movie until completion. Library dropdown option
width, height and font follow their own control, with stable geometry across choices.

Apply `Theme.uiBackground()` to translucent backgrounds. The global transparency
setting scales each background's original transparency: 0 is opaque, 100 is the
default appearance. Do not apply it twice or fade text/icons with the background.
The implementation uses alpha translucency, not real blur.

Keep video dominant and Live rails rectangular/edge-attached. Navigation follows
transient playback chrome, without floating outer gaps or residual dark scrims after
chrome hides. Extend the existing bottom timeline for playback modes rather than
adding a competing scrubber.

## Media mode navigation

`MediaModeChrome` centers `MediaModeSwitch`: Movies, Live TV, Series, independently
of the title bar in windowed, maximized and fullscreen modes.
The neutral 300 × 34 px capsule has three equal segments, 3 px insets and a
rounded grey selection that slides in 180 ms with OutCubic easing. It shrinks to
216 px in narrow windows. It selects by click or keyboard, without dragging.
The custom title bar retains its original 33 px height, 18 px app icon, bold 13 px
title and 30 × 30 px window buttons. It hides in fullscreen. Empty title-bar areas
retain native system move, double-click maximization and Windows Snap.

`Main.qml` owns the confirmed selection. An open library takes precedence;
otherwise a movie selects Movies, an episode selects Series and Live/idle selects
Live TV. Clicking an already open library is idempotent; clicking its segment
during playback opens that library. V/B retain toggle behavior and independent
library browsing state. Settings, modal/download interactions, library/title-bar
transitions and pending Live handoffs disable navigation. Library errors and
source opt-outs retain their existing presentation without enabling VOD.

Opening either library, including via V/B, pauses only an active VOD session.
`VodRuntime` remembers the session it paused and resumes it after the library
finishes closing to playback. A prior manual pause remains paused; changing
libraries does not resume. Explicit pause/resume actions, new Play, stop/end,
source invalidation, DVR interruption, Live navigation and shutdown discard the
automatic resume intent. Replacing a library with another overlay keeps the
session paused until returning to playback. Live/catch-up/multiview are unaffected
by library opening.

Live TV closes the library, waits for acknowledged VOD stop and durable progress
flush, then invokes `AppController::returnToLive`. It restores the last primary
channel rather than a browsed selection; startup fallback uses the existing saved
channel of the active Live source. Missing/removed channels leave idle Live.
Already active TV is not retuned or sought: buffered Live and timeshift keep their
position, pause state and refill policy. GO LIVE remains a separate transport
action. Catch-up returns to direct Live through its existing transport; an
engine-bound catch-up recording prevents that switch.

The standalone switch follows revealed playback chrome in every window mode,
using a transparent 50 px control area below the windowed title bar or at the
fullscreen top edge. `Main.topBarReservedHeight` is zero in fullscreen: Live/VOD
rails and library, Guide and Settings frames extend to the top edge. Only
windowed/maximized chrome reserves the shared 33 px title-bar inset.
The switch stays parented to the window above the library, keeping its position
and visibility through opening, closing and Movies/Series changes. On a Live TV
request, the library first finishes sliding down; playback panels reveal only
after that close and the VOD stop/progress barrier. The switch remains visible
through both animations and the handoff, with navigation disabled until settled.
`navigationTopInset` keeps the library toolbar below it while the category
sidebar remains full height. Guide, its collapse control and centered numeric/
multiview HUDs move down locally;
Settings content and narrow playback controls crossing the centered switch receive
a local content inset without moving panel backgrounds.
`mediaNavigationHeight`/`mediaNavigationWidth` describe this control area separately
from the title-bar inset. Disabled segments consume clicks so underlying overlays
remain open. Hover/focus
holds chrome open; hidden/animating navigation cannot receive input. From Live
search, Tab enters the switch; native Tab/Shift+Tab moves focus, Left/Right chooses
a candidate, Enter/Space requests it and Escape returns focus to playback.

## Selection, playback and focus

The movie/series library shows a spinner while startup VOD storage is unavailable,
waiting up to 15 seconds before showing a retryable error. Series details obtain
audio/subtitle choices from the selected episode's cache. Only the first available
ordinary episode may run a details media probe before playback; later episodes
and Specials do not probe. Changing episodes cancels the previous probe. The
existing five-second Play gate and playback handoff apply to episode probes too.

Browse state and playback state are separate. Hover/keyboard navigation previews
channels; explicit Return, transport Play or a Live channel double-click tunes.
Picker rows confirm on a single click. Avoid retuning when selection metadata or
EPG changes. Preserve the two NowNext models and the independently browsed Guide.

Global shortcuts must respect text editors, modal interactions and overlay input
ownership. Modified Tab is not plain Tab; `Ctrl+Tab` is unbound. Do not implement
`Meta+Arrow` snapping: Windows handles it through native window styles.

| Input | Contract |
|---|---|
| `Space` | Active transport play/pause; single Live may enter enabled timeshift |
| `F` | Fullscreen, or favourite toggle in left-pane keyboard channel navigation |
| `F1` / `F2` / `F3` | Active audio picker / subtitle picker / diagnostic bubble |
| `F6` | Always-on-top toggle |
| `Tab`, `Ctrl+F` | Live search; movie overlay uses `Ctrl+F` for its own search |
| `Ctrl+S`, `Ctrl+G` | Live source/group picker |
| `Ctrl+Up` | Guide outside grid selection; grid tile selection when applicable |
| `Ctrl+P`, `Ctrl+Shift+P` | PiP toggle / swap |
| `Ctrl+O`, `Ctrl+Enter` | Grid toggle / promotion; Guide `Ctrl+Enter` starts eligible catch-up |
| `Ctrl+R` | Toggle hovered programme DVR schedule, otherwise manual recording |
| `Ctrl+D` | Download explicitly selected ended programme after validation |
| `V` / `B` | Toggle movies / series; opening pauses active VOD, closing restores library-owned pause; the other key switches library kind |

The full precedence and Escape hierarchy remain in
[AGENTS.md, keyboard contract](../AGENTS.md#12-keyboard--shortcut-contract).
Do not infer a shortcut's availability from this summary alone. In search fields,
character keys must enter text instead of triggering playback/fullscreen/volume.
Numeric Live tune is disabled in overlays, editors and source/group pickers.

The shared VOD season selector and library selectors use `VodSelector.qml` for
neutral backgrounds, popup typography and focus borders. During Series playback,
Right focuses episodes; Up/Down select and Enter plays. Left returns to Series.
Focused season selectors retain native combo keyboard handling and Escape closes
their popup before dismissing playback chrome.

Changing categories in the movie library clears its search field and title filter,
including sidebar choices, the compact selector and Continue watching's View all.

Movie Escape closes an open selector, returns from details to the originating
shelf/grid, then closes the library. On first use, loaded Continue watching history
selects its newest film; empty/error history selects the first grid film. Subsequent
visits retain selection, and user interaction prevents delayed focus changes.
Left/Right navigate the shelf; Down enters the remembered grid selection; Up from
the first grid row returns to the shelf. Enter opens details. The shelf consumes
wheel input as horizontal scrolling, preserving selection and grid scroll. The
Escape footer is absent. The movie
overlay never auto-hides and blocks Live tuning/recording/multiview shortcuts.
Live channel, movie playback and group searches share
[PlaybackSearchHeader.qml](../qt/qml/components/PlaybackSearchHeader.qml).
The 42 px header has a flexible search field and a 78 px view switch separated by
10 px, with matching rail insets (4 px left, 8 px right, 16 px top). The component
exposes the field for focus handling and forwards edits, key/focus events and
switch requests; each consumer keeps its own model and input policy. Live retains
its existing palette; movie and movie-group headers use the neutral overlay palette.
The Live search field explicitly forwards Ctrl+Up/G/S/F to the window handler
before native text editing consumes them; Ctrl+Left/Right retain cursor editing.
The list headers show ← Groups; the group header shows Channels → or Movies →.
During movie playback, the 40×40 px Back arrow sits outside the left panel,
24 px to its right and 24 px below the reserved windowed title-bar inset (24 px
from the fullscreen top edge), shifted down only if it would cross the switch. It follows the
visible movie/group panel's slide and fade, disables input during chrome animation,
and keeps chrome visible on hover. It hides with the panel, when a track picker
replaces it, or when the narrow layout shows only movie information. Back opens
the library while stopping the movie with a progress checkpoint. The left list
uses independent search over the playing source; click selects and double-click
or Enter plays/resumes. Left focuses the movie list, and another Left opens the
shared group picker with the current group highlighted and centered. It offers
All movies, Continue watching, To Watch, Favourites and provider categories, with a separate Search groups field. Up/Down browse;
Right, Enter or a single click confirm and return to movies without changing
playback. Movies → returns without confirming. Escape hides the picker and chrome.
Movie search is preserved within the selected category; Live and library filters
remain independent. Search fields retain normal cursor-key handling.
The right panel always summarizes the playing film, not
the browsed selection. Its poster is left-aligned beside a fixed Settings button
with the same geometry, appearance and overlay action as Live's right panel;
opening Settings keeps the movie playing. Both panels follow playback chrome
visibility and transparency.
At narrow widths the transport information button switches between list and
summary. Tab/Ctrl+F target movie search; Escape dismisses visible chrome without
stopping. On video without chrome or an exclusive overlay, Escape/Backspace
perform playback Back, checkpointing/stopping and opening the movie library or
owning series details. Search Backspace remains text editing.
The central Live playback spinner also indicates VOD opening, resume seeking,
recovery and buffering; it does not persist during an ordinary user pause or
following stop/failure. Movie Play actions unlock after five seconds of metadata
probing, or earlier when probing finishes; the probe continues in the background
after unlocking. The Starting state covers confirmed probe shutdown and the
two-second handoff.
See [VOD](vod.md#progress-and-transport) for data and switching contracts.

VOD scrollbars appear only while their content overflows the available viewport,
remaining visible at rest and without pointer hover. Empty and fitting content
hides the bar; its reserved geometry and gaps remain stable. Library categories, playback groups
and movie/series rows, episode lists and scrolling details reserve the scrollbar
width plus a 12 px gap on the right, so text, progress bars and status buttons stay
clear of it. The main library viewport reserves the scrollbar width plus a 24 px
gap beside posters, the Continue watching shelf and View all. All VOD scrollbars
share a muted, opaque grey thumb (`#929292`), with subtle hover (`#a0a0a0`) and
pressed (`#ababab`) states, through `VodScrollBar` and shared Theme tokens. This
includes library categories/grid/details, Continue watching, playback lists and
information/episodes, dropdown popups and the VOD playback group picker. Popup
scrollbars stay visible while their options overflow. The Continue watching
shelf's horizontal scrollbar follows the same overflow rule.
Series details keep the outer viewport non-scrolling and use
separate overflow-dependent scrollbars for information and episodes.

## Series interaction

B and V share shortcut gating, overlay slide animation and input ownership.
Separate instances of the shared library retain independent browsing state. Series
details add seasons and a virtualized episode list: clicks select, Play/Enter
starts/resumes. Opening details or returning from playback focuses episodes
without Tab. Up/Down move one row, clamp at list boundaries and reveal selection;
Enter/Return starts the model-selected episode. Left/Right in the details list
change seasons in selector order, clamp at the ends and select the first episode;
no season change occurs with only one season. Empty seasons disable Play. This
behavior is enabled by `VodEpisodesList.seasonKeyNavigation` only in the library;
the playback panel retains Left/Right focus navigation. Open popups own their keys.
Changing episodes keeps the series information, layout, focus and scroll stable.
Only playback actions and track presentation follow the selected episode. Its
own measured metadata enables Audio/Subtitles; borrowed metadata from the newest
cached measurement in the same season fills completely disabled controls.
Borrowed data does not change preferences, resolution or progress.
Native list key navigation is
disabled so it cannot independently move the highlight. Back stays fixed;
information scrolls within at most half the remaining height and the episode area fills the rest to the bottom. The outer
series details never need scrolling to accommodate episodes. Row data changes
preserve episode scroll, selection, season and focus. Keyboard navigation reveals
the selected row; structural refreshes preserve the visible episode and offset.
The main Watched action changes all known episodes, including Specials. Series
Watched is derived from all ordinary episodes, excluding Specials. Top-right
row buttons independently toggle an episode's status, consuming the click without
selection/playback. Details and the right playback panel use 25.2 × 25.2 px
buttons with 14.4 × 14.4 px icons, 5.4 px padding and an 8 px top/right inset.
In both views the progress bar extends beneath the button to the row's 12 px
right content margin; only text reserves space for the button. Conflicting
status edits are disabled until the write and history refresh finish. Season/episode browsing does not change playback or probe media.
Loaded VOD action icons and Play retain their normal appearance while status/list
writes and subsequent history/catalogue refreshes disable interaction. This avoids
flashing unrelated episode/movie controls; confirmed status still changes the icon.
Unavailable playback and metadata-probe gating retain the disabled appearance.
During episode playback the left rail contains series/groups and the right rail
seasons/episodes of the playing series; a single right-row click/Enter plays.
Settings remains fixed at the panel's top right. Existing narrow-panel switching,
focus, auto-hide and Escape rules apply. Previous/Next use the channel icons and
button geometry, start at zero and disable at boundaries. The timeline includes
series title, SxxExx and episode title. Back and the playback Stop button
checkpoint/stop and open the owning series details with the stopped episode and
season selected, after stop acknowledgement and progress writes. Natural final
end requests that same return; Settings/modal ownership defers it and another
start cancels it. See [VOD](vod.md#series-episodes-and-autoplay).

Overflowing movie/series poster captions in the grid and Continue watching shelf,
and episode-row titles in the right playback panel, use `VodMarqueeTitle`.
The single target chosen by `VodMarqueeController` starts after one full second of
keyboard indication or hover. The last input wins: keyboard navigation ignores
stationary hover, while actual pointer movement takes ownership. A playing episode
or a visible panel alone never starts scrolling. The title moves left continuously
at 35 logical px/s, repeating after a 32 px gap without further pauses. Loss of
indication, viewport visibility or input readiness resets it; content/font/width
changes start a fresh delay. Shelf progress/artwork updates preserve delegates and
ongoing animation. Short/inactive titles retain elision. The series panel header,
library details episode titles, year labels and poster placeholders stay static.
Existing row activation and status/list buttons retain their input contracts.

## Pointer and animation behavior

Frameless window resize handles sit above the movie overlay and title bar, so
all edges and corners remain draggable while the library or details are open,
including during their slide transitions. They remain disabled in maximized and
fullscreen windows; download interactions and modal dialogs retain higher layers.

The whole movie library slides up from below its viewport on opening and down on
closing, using `Theme.transitionMs + 60` (240 ms) and `Easing.OutCubic`, without
fading. Its clipped viewport retains the windowed title-bar inset and reaches
the top edge in fullscreen, with the media switch inside the library in every
window mode. Normal closes
keep the page mounted and the exclusive `vod` state until the animation completes;
only then does catalogue cleanup run and focus return to playback. Controls and
pointer/wheel input to the revealed video are blocked during transitions, and
repeated V/Escape is consumed. Initial library focus and delayed history selection
wait for input readiness. Replacement by another overlay cancels VOD presentation
without clearing that new overlay.

Chrome disables input during transitions; visible geometry alone does not prove a
control is ready. Hover selection must follow actual pointer movement, not a new
delegate appearing under a stationary pointer after keyboard scrolling.

Keep the row-to-hover-bubble bridge stable at viewport edges. Auto-hide/inactivity
handling must respect keyboard locks, mouse pinning and open overlays. Pointer
tests wait for `enabled` and `hovered`; they acknowledge `pinned` after a pin click.

The movie library's group separator has an 8 px drag target, horizontal resize
cursor and neutral hover/drag highlight. Its layout width is clamped between
208 px and one third of the window width. Drag coordinates are measured in the
page, and pointer capture continues outside the separator. Losing the gesture,
hiding/disabling the page or entering compact layout cancels the preview. Release
commits only a changed width. Window/sidebar width changes resize library poster
frames between approximately 212.3 × 303.6 and 250.7 × 358.5 logical px, preserving their proportions,
3 px image insets and 16 px horizontal gaps. The grid and Continue watching share
geometry: maximize the number of minimum-size posters that fit, then enlarge them
to use the row up to the maximum, with no trailing gap. Reserve scrollbar width plus its 24 px gap
independently of its visibility to avoid layout oscillation. Height-only changes
do not scale posters. Resizing preserves selection/focus and does not reset
browsing to the beginning; captions and badges retain their sizes. Poster To Watch
and Favourites controls scale proportionally with the shared poster width, from
approximately 36.4 × 36.4 px to 30.8 × 30.8 px, including icon padding, corner radii
and edge insets. Scaling retains the original 289.5 px reference width; the new
maximum poster area is 75% of the previous maximum, with unchanged minimum size.
Details controls retain their fixed sizes.
Details/playback posters retain independent geometry; see [VOD](vod.md).

Attached scrollbars need one geometry owner per axis. For custom placement, choose
an explicit parent and anchor to its viewport rather than mixing conflicting
attached geometry and anchors.

## Settings and formatting

Settings edits are drafts. Async source work preserves unsaved drafts. Save applies
changes; discard/reload restores persisted state. UI transparency previews live,
while draft date/time choices affect only their preview until Save.

Use [datetimeformat.h](../qt/src/core/datetimeformat.h) and the `dateTimeFormatter`
context object for user-facing dates and times. Date order and 12/24-hour preference
are independent, with persisted `system` defaults. English names and local time-zone
conversion remain consistent. Do not change technical ISO/UTC timestamps, provider
URLs, programme identity or elapsed durations when presentation preferences change.
Bind to notifying patterns rather than retaining timer-formatted clock strings.

Native file/folder dialogs in [PathBrowseField](../qt/qml/components/PathBrowseField.qml)
are created lazily; set `parentWindow` from `Window.window` before opening. Eager
creation can stall Settings, and missing window ownership can break Windows dialogs.

## Registration and validation

F3 buffer diagnostics show the forward reserve relative to actual playback,
using the current continuous audio/video cache bounds. Values have one decimal
place, estimates carry `≈`, and stale/unavailable observations show `N/A`.
Configured cache targets do not cap displayed reserve. Live, catch-up and VOD
share this presentation; local Timeshift additionally reports distance to its
live edge. See [buffer telemetry and recovery](playback.md#buffer-telemetry-and-live-recovery).

Register every new QML file in `qt_add_qml_module()` in
[qt/CMakeLists.txt](../qt/CMakeLists.txt), and add resources through the existing
resource declarations. A working source-tree preview does not prove packaging.
Keep local qmllint suppressions narrow and adjacent to context-property aliases.

Run QML lint and relevant component/UI tests described in
[build and testing](build-and-testing.md). Validate layout, focus and pointer
behavior at narrow/wide sizes. Guide palette/overflow, native dialogs and window
behavior also need packaged Windows checks.

## Source media group editors

In Settings → Sources, Enable VOD sits immediately below Archive safety margin
(minutes) and uses `FormSwitch`. M3U disables it with a short support hint. Saved
Xtream sources with the draft setting enabled show Live TV / Movies / Series
segments above Groups, initially Live TV. Each segment has independent source
selection/order/search/Hide Unchecked drafts. Switching media/source cancels any
active group drag. Movies/Series include empty provider categories, without Live
channel counts. The group toolbar fits narrow layouts; search and filtered
selection retain their behavior. Save processes all media editors and all changed
sources; errors retain unsaved drafts and display inline. Discard reloads saved
values. No-groups warning considers edited types/sources, not just the visible
Live editor. Continue watching, To Watch and Favourites stay in VOD navigation,
with SQL filtering restricting their contents to selected movie/series categories.

`V`/`B` open movie/series libraries using saved source availability; it no longer opts a
source into VOD for the session. Disabling VOD does not close the playing film/episode;
transport remains available through that session’s token.
