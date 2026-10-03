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
| [VodMoviesPage](../qt/qml/screens/VodMoviesPage.qml) | Full-window movie library and details |

Use [Theme.js](../qt/qml/theme/Theme.js) for colors, dimensions, typography and
animation tokens. All new overlays use the shared black/grey `overlayBackground`,
`overlaySidebar`, `overlaySurface*`, `overlayBorder` and `overlayText*` palette.
Large backgrounds, cards and controls remain neutral; color belongs to small
selection accents, status indicators and semantic badges. VOD library buttons, navigation and selectors use neutral grey interaction states,
including keyboard focus. The primary movie Play/Resume button is the explicit
exception: blue background, play icon and Play or Resume · HH:MM text. Other movie
detail actions use icon buttons with descriptive tooltips. To Watch and Favourites
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

Keep video dominant and Live rails rectangular/edge-attached. Do not bring back a
persistent navigation shell, floating outer gaps or residual dark scrims after
chrome hides. Extend the existing bottom timeline for playback modes rather than
adding a competing scrubber.

## Selection, playback and focus

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
| `V` | Toggle movie library without changing playback |

The full precedence and Escape hierarchy remain in
[AGENTS.md, keyboard contract](../AGENTS.md#12-keyboard--shortcut-contract).
Do not infer a shortcut's availability from this summary alone. In search fields,
character keys must enter text instead of triggering playback/fullscreen/volume.
Numeric Live tune is disabled in overlays, editors and source/group pickers.

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
The list headers show ← Groups; the group header shows Channels → or Movies →.
During movie playback, the 40×40 px Back arrow sits outside the left panel,
24 px to its right and 24 px below the reserved title-bar inset. It follows the
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
the browsed selection. Both follow playback chrome visibility and transparency.
At narrow widths the transport information button switches between list and
summary. Tab/Ctrl+F target movie search; Escape dismisses chrome without stopping.
The central Live playback spinner also indicates VOD opening, resume seeking,
recovery and buffering; it does not persist during an ordinary user pause or
following stop/failure. Movie Play actions unlock after five seconds of metadata
probing, or earlier when probing finishes; the probe continues in the background
after unlocking. The Starting state covers confirmed probe shutdown and the
two-second handoff.
See [VOD](vod.md#progress-and-transport) for data and switching contracts.

## Pointer and animation behavior

Frameless window resize handles sit above the movie overlay and title bar, so
all edges and corners remain draggable while the library or details are open,
including during their slide transitions. They remain disabled in maximized and
fullscreen windows; download interactions and modal dialogs retain higher layers.

The whole movie library slides up from below its viewport on opening and down on
closing, using `Theme.transitionMs + 60` (240 ms) and `Easing.OutCubic`, without
fading. Its clipped viewport retains the reserved title-bar inset. Normal closes
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
commits only a changed width. Resizing preserves browsing state and fixed posters.

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

Register every new QML file in `qt_add_qml_module()` in
[qt/CMakeLists.txt](../qt/CMakeLists.txt), and add resources through the existing
resource declarations. A working source-tree preview does not prove packaging.
Keep local qmllint suppressions narrow and adjacent to context-property aliases.

Run QML lint and relevant component/UI tests described in
[build and testing](build-and-testing.md). Validate layout, focus and pointer
behavior at narrow/wide sizes. Guide palette/overflow, native dialogs and window
behavior also need packaged Windows checks.
