# VOD movie and series libraries

[Documentation index](README.md)

VOD F3 uses the shared playback-relative forward reserve, with one decimal place,
`≈` for estimated observations and `N/A` for stale/unavailable data. Load and seek
invalidate previous observations; details are in
[buffer telemetry](playback.md#buffer-telemetry-and-live-recovery).

## Status and boundaries

The backend has Xtream movie/episode resolution, SQLite persistence and native mpv
integration. Local integration coverage exists; Windows hardware acceptance is
pending. Do not describe VOD as production-ready. The library exposes Xtream movies and series through the shared VOD stack.
M3U VOD and recommendations remain outside scope. The [VOD design document](../.project/OKILTV_ARCHITEKTURA_VOD.md) records the
broader design and acceptance status.

`V` opens movies and `B` opens series in the exclusive `vod` overlay and respects each protected source’s saved
`ServerProfile.vodEnabled`. Xtream sources default to enabled, including existing
sources after the per-profile `vodDefaultsVersion=1` migration. Neither shortcut changes
source availability. The older global technical flag does not block an enabled
source; the legacy global series flag no longer gates catalogue import or episode playback.
Library source selection
is independent of Live source selection. The standalone Movies/Live TV/Series
switch also opens the libraries. Opening pauses active VOD, including via V/B;
closing to playback resumes only the same session paused by the library. Prior
manual pauses remain paused and switching libraries retains pause ownership.
New Play, pause/resume actions, stop/end, source invalidation, DVR interruption and shutdown
cancel automatic resume. Browsing/details never start or stop playback;
Play/Resume is explicit. Live TV explicitly checkpoints/stops VOD, flushes its
progress and returns to the last primary channel through the existing handoff.

## Code map

| Area | Entry points |
|---|---|
| Domain identities, ports and errors | [core/vod](../qt/src/core/vod) |
| Xtream and HTTP adapters | [providers](../qt/src/core/vod/providers) |
| SQLite migrations/repositories | [storage](../qt/src/core/vod/storage) |
| Bounded jobs, facade and progress lane | [app/vod](../qt/src/app/vod) |
| Source/lifecycle integration | [vodruntime.cpp](../qt/src/app/vod/vodruntime.cpp) |
| Library model, details and poster requests | [vodcatalogmodel.cpp](../qt/src/app/vod/vodcatalogmodel.cpp) |
| Session state and playback handoff | [vodplaybacksession.cpp](../qt/src/app/playback/vodplaybacksession.cpp), [playbackcoordinator.cpp](../qt/src/app/playback/playbackcoordinator.cpp) |
| Engine port and mpv adapter | [iplaybackengine.h](../qt/src/player/iplaybackengine.h), [mpvplaybackengine.cpp](../qt/src/player/mpvplaybackengine.cpp) |
| Shared movie/series overlay | [VodLibraryPage.qml](../qt/qml/screens/VodLibraryPage.qml) |
| Season/episode selections and presentation | [vodepisodesmodel.cpp](../qt/src/app/vod/vodepisodesmodel.cpp), [VodEpisodesList.qml](../qt/qml/components/VodEpisodesList.qml) |
| Shared VOD scrollbars | [VodScrollBar.qml](../qt/qml/components/VodScrollBar.qml), [Theme.js](../qt/qml/theme/Theme.js) |

The domain target depends only on Qt Core. Keep UI, HTTP, SQL and native mpv
details in their adapters. Fake repositories/engines are test infrastructure,
never a runtime fallback.

## Identity, publication and source edits

Content identity includes profile UUID, persistent catalogue namespace, content
kind, provider item ID and optional parent namespace. Provider IDs are strings;
do not derive identity from titles, URLs or numeric casts.

Changing endpoint/credentials advances request revisions without changing the
namespace or deleting history. Source mutation barriers checkpoint/stop active
VOD before changing credentials, reject stale jobs and reconcile against the
actually persisted protected source configuration after a failed write. Never
roll back a request fence or automatically resume after a failed edit.

Catalogues/categories publish complete staged snapshots transactionally. Recheck
source revision, cancellation, deadline and request identity at publication.
Movie/series catalogue and category jobs have a 90-second deadline, starting at
submission and including queue time, HTTP retries, processing and publication.
Sequential selected-category requests share the catalogue job's deadline. Source
synchronization creates a fresh deadline for each category/catalogue stage, so
the whole synchronization can take longer. The VOD HTTP transfer inactivity
timeout is also 90 seconds; the job deadline still bounds the entire operation.
Other VOD jobs retain their 30-second default deadline and artwork retains its
10-second limit.
Valid empty snapshots are distinct from cache misses; failed refresh keeps prior
data. Generation-bound page cursors must not mix snapshots. Explicit source
removal invalidates work and deletes its VOD state/artwork; cache eviction alone
must retain durable progress.

VOD uses additive `vod_*` tables in the application database and its own migration
marker. Schema 10 adds durable series continuation suppression after a bulk manual
Watched action; schema 9 adds a durable per-series last-played episode pointer, atomically updated
with playback checkpoints. Migration preserves movie data and backfills existing
episode history. Schema 8 supplies the independent Movie/Series To Watch and
Favourites identities. Schema 7 rebuilds stored base-letter title keys and advances catalogue
generations; schema 6 adds confirmed track preferences to progress. Earlier additions
include source mutation recovery, category snapshots and artwork references. See
the [storage contract](../qt/src/core/vod/storage/README.md) before altering schema
or repository behavior. Workers must be joined before runtime storage is destroyed.

## Source availability and category synchronization

Settings → Sources places **Enable VOD** below the archive safety margin. M3U
shows a disabled switch and an unsupported-source hint. A saved Xtream source
with its draft switch enabled exposes independent **Live TV**, **Movies** and
**Series** group editors. Both media kinds import independent complete catalogues. Seasons and episodes
are fetched lazily when opening series details or playing a series. `SourceGroupsModel` uses media-specific loaders for
the two VOD editors and retains independent per-source drafts, search, order and
Hide Unchecked. Category lists include empty/not-yet-imported provider groups.

`VodRuntime::synchronizeSource()` coordinates scope, movie categories, series
categories and independent movie/series snapshot publication through bounded `VodController` jobs.
`AppController::sourceRefreshRequested` connects the common manual/automatic
source refresh path without waiting for Live or EPG. Adding/enabling a source
and saving changed movie or series selections also synchronize; ordering/search/Hide
Unchecked do not import catalogue items. Errors/loading are independent of Live/EPG and
are exposed in the VOD group editors and library. Series category failure does
not prevent movie synchronization. Drafts survive category refresh.

Unique current provider categories for each media kind form the denominator. Below 30% selected,
requests use `get_vod_streams&category_id=…` or `get_series&category_id=…` sequentially; at 30% or more, one full
catalogue request runs. Zero selections issue no stream request and publish a
valid empty snapshot for that kind. New categories follow Live’s rule: auto-select up to
50 provider categories; above 50 leave new categories unchecked. Responses are
filtered locally even if the server ignores `category_id`, and duplicate full
movie identities are merged. All chosen categories form one atomic snapshot;
any failed category retains the prior catalogue. Valid empty responses succeed.

`CatalogQuery.allowedCategories` applies SQL membership restrictions before title
search, sorting and pagination, for both catalogue models, Continue watching,
To Watch and Favourites. Special groups remain available with eligible contents.
Filtered-out films cannot start through stale details. Saving source/category
policy cancels obsolete imports, details, artwork, probes and pending starts;
a process-local policy generation is checked at worker execution and under a
publication mutex through the movie/category SQL commit.

Disabling a source removes it from library selection and cancels new work while
retaining catalogue/cache, history, lists and track preferences. Availability
changes do not advance credential revisions or stop the current movie. The owning
session token keeps pause, seek, same-session recovery and checkpoints; the recovery
exception expires on stop and cannot start another film. Recovery uses a reserved
bounded job lane so a policy edit does not cancel an already pending recovery;
credential/removal barriers and shutdown cancel that lane. New-start admission
is checked again after any delayed Legacy resource release. Unchecking the playing
category likewise leaves the session intact. Credential edits and removal retain
the existing checkpoint/stop barrier.

## Library behavior

- Opening either library before VOD storage is ready shows a loading spinner and
  waits up to 15 seconds. Initialization failures during that window trigger
  bounded asynchronous retries and stay out of the library error banner. Readiness
  resumes catalogue loading automatically; expiry shows an error with Retry.
  Closing the library or switching sources cancels its wait; Retry starts a new
  15-second window.
- The entire library, including details, slides from the bottom on opening and
  returns below the viewport on closing (240 ms, `Easing.OutCubic`, no fade).
  V, the close button, final Escape and successful playback start share the
  animated close path. The exclusive overlay and catalogue stay present through
  closing; input is blocked throughout either transition. The Movies/Live TV/Series
  switch stays fixed above the animated library. Live navigation finishes the
  library close before revealing playback panels, retaining the switch throughout
  the close, stop/progress barrier and panel reveal. Already running background
  Live/timeshift keeps its position without a seek or reload. Focus and initial
  selection wait for opening completion, while closing rejects delayed focus
  changes. Reopening retains browse state. These animations do not start, pause
  or stop playback; the existing playback and Back actions retain their behavior.
- The virtualized grid uses local substring search, A–Z/Z–A sorting, categories
  (changing the library category clears the search field and title filter)
  and 100-item pages. Continue watching is a category and an All movies shelf of
  up to 12 items with View all. Its query covers the source's full saved catalogue.
  Both the shelf and full category order by saved progress update time, newest
  first (left to right on the shelf), independently of the A–Z/Z–A selector.
- Title sorting and prefix/substring search share locale-independent Unicode
  base-letter keys: case folding, NFKD decomposition and removal of combining
  marks, plus Latin mappings for ł/l, ø/o, đ/ð/d, ħ/h, ı/i, æ/ae, œ/oe, þ/th and
  ß/ss. Thus Łotr sorts between Lato and Lumina, and `lotr` finds Łotr. Original
  titles remain intact; other alphabets are retained rather than transliterated.
  SQL applies ordering before pagination, with content identity breaking ties.
- The library group sidebar can be resized by dragging its right edge (8 logical
  pixel hit area). Its minimum is 208 px and maximum is one third of the window
  width. A completed drag saves one global preferred width across sources and
  restarts; shrinking the window clamps the displayed width without overwriting
  the preference. Below 760 px, the category selector replaces the sidebar.
  Before the first adjustment, widths remain 208 px below 1200 px and 268 px
  otherwise. Cancelled gestures and disabled/closing overlays do not commit.
  The same sidebar serves details; playback panels have independent geometry.
- Library categories, playback groups and movie/series rows, episode lists and
  scrolling details reserve the vertical scrollbar width plus a 12 px right gap.
  VOD scrollbars are hidden when the content fits and stay visible at rest
  without pointer hover when it overflows. Hidden bars retain their geometry and
  reserved gaps, so visibility alone never changes poster columns or text width. The main library viewport reserves the scrollbar width plus a 24 px gap
  beside posters, Continue watching and View all. All VOD thumbs use the same
  muted opaque grey (`#929292`, hover `#a0a0a0`, pressed `#ababab`), including
  categories, grid/shelf, details, playback panels/groups and selector popups.
  Popups show a persistent scrollbar when their options overflow; the shelf's
  horizontal bar is visible only when its contents overflow. Series details
  retain independent information/episode scrolling with no outer scrollbar.
- Overflowing titles below movie/series grid and Continue watching posters use
  `VodMarqueeTitle` after 1000 ms of uninterrupted keyboard indication or card hover.
  `VodMarqueeController` chooses one target across the grid/shelf using the most
  recently used input. Actual pointer movement takes over from keyboard navigation;
  scrolling/reflow under a stationary pointer does not. Titles repeat continuously
  to the left at 35 logical px/s with a 32 px gap, without pauses between cycles.
  Short/inactive titles remain static with right elision. Losing indication,
  hiding/disabling the view, leaving the viewport, opening details/popups or changing
  identity/text/font/width resets the animation and delay. Progress/artwork updates
  retain the shelf delegates by content identity and do not restart the marquee.
  In series playback, the same behavior applies to indicated episode rows in the
  right panel, never merely to a playing/visible row. The series header and episode
  rows in library details remain static; episode buttons and activation are unchanged.
- Library poster frames range from 11/15 of 289.5 × 414 px (approximately 212.3 × 303.6 px)
  to approximately 250.7 × 358.5 logical px (289.5 × 414 multiplied by sqrt(0.75),
  reducing the maximum area by 25%), preserving proportions and 3 px image insets. The grid
  and Continue watching shelf share dimensions, with fixed 16 px horizontal gaps.
  After subtracting the sidebar, layout margins, reserved scrollbar width and 24 px scrollbar gap,
  choose the maximum number of minimum-width posters that fit, then enlarge them
  to use the row up to the maximum; no gap is reserved after the last column.
  For usable width W, columns = max(1, floor((W + 16) / (289.5 × 11/15 + 16)));
  poster width = clamp((W - (columns - 1) × 16) / columns, 289.5 × 11/15, 289.5 × sqrt(0.75)).
  Fractional dimensions are retained. Window/sidebar width changes recalculate
  geometry immediately; adding a column reduces poster sizes, while height-only
  changes do not scale them. A viewport narrower than the minimum clips one column.
  Cards retain 54 px for captions and unchanged typography, badges and progress
  bars. Poster To Watch/Favourites buttons, icon padding, corner radii and edge
  insets scale by poster width / 289.5: approximately 36.4 × 36.4 px with 20.8 × 20.8 px
  icons at the maximum, down to 30.8 × 30.8 px with 17.6 × 17.6 px icons at the minimum. Details controls retain
  their fixed sizes. Selection/focus survive resizing without resetting browsing to the
  beginning. Details and playback posters retain their separate sizes.
  Use the neutral overlay palette. Library
  buttons, navigation and selectors use neutral grey interaction states.
  The primary Play/Resume button uses a blue background; active list icons use
  the same blue accent. Play/Resume has `play.svg` followed
  by Play or Resume · HH:MM; its tooltip repeats the label. Other detail actions
  are 42 × 42 px icon buttons: `start-from-beginning.svg`, `mark-watched.svg` or
  `mark-unwatched.svg`, with tooltips describing their actions. Their icons use
  a 24 × 24 px area (9 px padding, no extra inset), matching To Watch/Favourites.
- On the first library visit in a process, wait for the selected source’s Continue
  watching query and focus its first (most recently watched) poster in All movies.
  `continueMoviesLoaded` distinguishes pending history from a valid empty result
  or terminal query error; a missing catalogue snapshot remains pending until
  refresh. Empty/error history falls back to the first grid poster, if any.
  User interaction cancels automatic selection. Later visits retain browsing
  state; shelf `movieKey` identities preserve selection when its order changes.
- Left/Right navigate the shelf without wrapping; Down enters the remembered grid
  selection, and Up from the first grid row returns to the remembered shelf item.
  Enter opens details; Back/Escape restore the originating browse area and scroll.
  The details Back control uses `left-arrow.svg` followed by bold “Back” with
  4 px layout spacing (a compact, single-space-sized gap), with no
  background or border in any interaction state.
  An empty/hidden shelf leaves grid navigation independent. Wheel input over the
  shelf scrolls horizontally (down/right, up/left), including touchpad and horizontal
  deltas, without changing selection or scrolling the grid. No Escape footer is shown.
- All five library dropdowns (Source, Sort, compact Categories, Audio, Subtitles)
  keep content-independent geometry. Audio/Subtitles split the available width
  equally, or use full-width separate rows in compact layouts. Popup width follows
  the control; option height and font follow that specific control (44 px/14 px,
  or compact 22 px/12 px). Long labels elide with full-text tooltips; long popups scroll.
- Resolution badges classify by standard width **or** height: 480p (including
  lower), 720p, 1080p, 1440p and 4K. Unknown resolution hides the badge.
- Visible cards request cached/provider details with at most two concurrent
  requests. Browsing never starts media probes; details/playback take priority
  over pending thumbnail work.
- Details initially use provider video metadata. During playback, load-tagged mpv
  telemetry supplies actual dimensions, duration and audio/subtitle metadata to
  both library and playback models, and enriches the existing details cache. This
  uses the already open media connection; switching from the left panel never
  launches ffprobe. Outside active VOD, details run bounded asynchronous ffprobe
  when no technical metadata newer than seven days is cached. Backend/probe
  dimensions and tracks take precedence. Probe failure leaves playback available.
  Play/Resume and Play from beginning are disabled for the first five seconds of
  a metadata probe request, or until it finishes if sooner (including failure).
  After five seconds, the controls unlock without cancelling the probe; it keeps
  running until completion, its normal deadline or explicit playback. Leaving
  details clears the gate; a new probe request starts a fresh five-second gate.
  The model also rejects direct Play calls during the gate. A playback
  request cancels all outstanding media probes and waits asynchronously for their
  workers to finish; the probe worker kills and reaps its ffprobe process before
  acknowledging completion. Only then does a precise two-second cooldown begin,
  followed by playback resolution and loading. New probes are suppressed during
  this handoff. Cancellation, replacement, source changes and shutdown invalidate
  deferred starts. With no outstanding probe/cooldown, playback starts normally.
- Audio/subtitle selectors allow Default, subtitles Off and specific tracks before
  playback. Match to the real backend list using normalized metadata, equivalent
  language codes, codec and per-type order; probe IDs are not stable mpv IDs.

## Series, episodes and autoplay

`VodCatalogModel` is configured by `CatalogKind`. `VodLibraryPage.qml` contains
shared poster/sidebar/details layout; `VodMoviesPage.qml` remains a compatibility
wrapper. The two library instances preserve independent source, category, search,
sort, selection and scroll state. B/V close their own library or switch the open
library kind; browsing never changes playback.

`VodEpisodesModel` has independent instances in library details and `VodRuntime`.
Both use `VodEpisodesList.qml`, sharing `VodSelector.qml` with library selectors,
with a season selector and virtualized episode rows
(number/title, duration, progress, watched state, available image and description).
Series details have a fixed Back control, an independently scrolling information
area capped at half the remaining details height, and the season/episode area
filling the rest to the bottom of the details viewport. Episode content never
increases the outer details height, including compact layouts and long descriptions.
The movie details layout keeps its existing outer scrolling behavior.

`VodEpisodesModel.rows` is a constant `QAbstractItemModel` with a `modelData` map
role. Selection, status, progress and artwork use row updates rather than model
resets. History reloads keep season/selection/focus and scroll unchanged. Structural
refreshes preserve the visible episode identity and its pixel offset (or the nearest
remaining row), while first load and keyboard navigation reveal the selection.
Status edits are serialized and conflicting episode/series edits are disabled until
acknowledgement and history refresh; stale callbacks are rejected by load generation.

The first episode start persists the lazy episode set before loading media, so
an immediate completion checkpoint can assess Series To Watch after cache eviction.
Series details focus the episode list on opening, including return from playback,
without requiring Tab. Up/Down move exactly one episode without wrapping and keep
the selected row visible; Enter/Return plays the model-selected episode. In library
details, Left/Right change to the previous/next season in selector order (including
Specials), without wrapping, selecting the first episode in the new season. A
single season is unchanged; an empty season clears selection and disables Play.
The episode list retains focus and open popups keep their own key handling. Library
clicks select; Play/Enter plays or resumes. A single click/Enter in the
right playback panel starts the episode. Right focuses its episode list; Up/Down
select and Enter activates, while Left returns to the series list. Selection and season changes alone never
change playback. Browsing library/playback lists never probes media. Outside active
VOD, series details may probe only the first available ordinary episode in the
series queue, when it is selected and no technical metadata newer than seven days
is cached. Later episodes, continuation targets beyond the first episode and
Specials never start a details probe. Changing selection cancels obsolete
detail/probe/cache requests and loads the selected episode's cached/provider metadata.
Episode details and season-track reads do not set the catalogue's general busy
state. Series title, poster, description, cast, genres, duration and resolution
stay attached to the series; episode selection updates playback actions and
track controls without clearing or replacing the series information.
The first episode retains the same probe, Play gate and teardown rules as movies.
Audio/subtitle choices are editable before playback only with measured ffprobe/mpv
metadata belonging to the selected episode. `movie.trackOptionsEditable` gates
both selectors and model mutation methods. When the episode has no measured
metadata, the newest cached measurement in the same series/season supplies its
option labels; equal timestamps prefer the earlier episode in queue order.
`VodController::cachedSeasonMetadata` uses the bounded worker lane and the
repository's cache-only `readSeasonMediaMetadata` read, respecting existing detail
cache validity and opening no provider or media connection. These populated
selectors are completely disabled, including popup opening. Indices use the
selected episode's saved preferences, falling back to Default; donor preferences
are never copied. Own measurements always override borrowed ones, even when older.
Borrowed metadata is presentation-only: it never updates the selected episode's
cache, progress, resolution or playback preferences. A missing donor or failed
cache read leaves empty, disabled controls without blocking Play. Cancellation and current
selection checks fence late replies; failed probing still permits Play. Active VOD uses cached
or backend metadata without opening another media connection. Technical metadata
and confirmed track preferences belong to the episode. The target's saved
preferences win; otherwise confirmed preferences from
the previous episode in the same series are matched through the existing backend.

The shared domain queue orders available episodes by season number/order, episode
number and provider order. Gaps do not end the queue; watched episodes are included.
Season zero is Specials: manually selectable and resumable through Continue
watching, but excluded from Previous/Next and autoplay. Each navigation button
starts its successor/predecessor at zero; list/Continue activation resumes by the
normal VOD rules. Buttons keep the channel button icons/geometry and disable at
queue boundaries. The timeline shows series title, SxxExx and episode title.

Schema 9's durable last-played pointer changes only with confirmed playback
progress, in the same transaction. Manual marking of another episode never moves
it. Continue watching projects one poster per series, with the target episode and
its progress, ordered by last playback time. An incomplete remembered episode is
the target; a watched remembered episode targets the immediately next available
ordinary episode, even if already watched (then starting at zero). No successor
removes the series from Continue watching. A successful lazy details refresh can
reveal new successors without changing history. Specials can remain their own
incomplete continuation; completed Specials have no automatic successor.

Watched means strictly over 95% of known duration, or manual marking; exactly 95%
and unknown duration do not auto-complete. To Watch/Favourites apply to Series;
The series Watched status is derived from all currently known ordinary episodes,
including unavailable ones: every ordinary episode must be Watched. Specials do
not block this aggregate; an empty or Specials-only series has no completed aggregate.
The main details action marks/unmarks every currently known episode across all
seasons, including Specials and unavailable episodes. Each episode row has its
own top-right Watched/unwatched button in both details and the playback panel;
that button does not select or start the episode. Both views use 25.2 × 25.2 px
buttons with 14.4 × 14.4 px icons, 5.4 px padding and an 8 px top/right inset.
Only row text reserves space for the button; the progress bar extends beneath it
to the row's 12 px right content margin.
Status/list writes and history/catalogue refreshes keep loaded action icons and
Play visually stable while conflicting input remains disabled, in both libraries.
Unwatched clears saved positions
and preserves confirmed track preferences. New provider episodes remain unwatched.
A completion event clears Series To Watch only after all known ordinary episodes
are watched. A bulk Watched action clears it in the same transaction. Favourites remains. Re-adding a completed series survives ordinary
later checkpoints until a new completion event.

Schema 10's independent `vod_series_state` hides a bulk-marked series from both
Continue watching views without moving its last-played episode or history time.
The state survives restart, cache eviction, refresh and credential edits; source
removal deletes it. Checkpoints from the session active at marking time retain
suppression. The first confirmed playback checkpoint of a different session clears
it; opening/browsing, manual episode edits and bulk Unwatched do not. Explicit Play
can still start a hidden series. Newly discovered episodes change the aggregate
without clearing this suppression. Bulk writes and suppression/list changes commit
atomically; a failure rolls back all rows and the active-session manual override.

Runtime autoplay waits for natural end (near the duration when known), the final serial
progress write acknowledgement, and loaded episode metadata before starting the
next ordinary episode through the existing controller/coordinator. A generation,
session token and handled-end token fence replacements, delayed/repeated EOF and
pending starts. Stop, failures/early EOF, source policy/credential/removal changes,
DVR and shutdown cancel transitions. The stop acknowledgement used by an owned
episode replacement preserves its pending start. The shared VOD spinner covers
this handoff. Specials return to details without automatic advance. A confirmed
natural end with unknown duration may advance, while leaving that episode
InProgress. After the last episode, runtime requests the correct series
details; Main defers that return while Settings/modal interaction owns the UI.
Starting another material cancels the deferred return.

Series playback uses the same left list/group picker and narrow/wide chrome as
movies; the right panel presents seasons/episodes with a fixed Settings button.
Successful episode changes dismiss playback chrome while preserving an open
library/Settings overlay and its focus. In narrow layouts, Right or
the information button defers episode focus until the shared panel animation ends.
Back and the playback Stop button checkpoint/stop playback and open the owning
series details, with the stopped episode and its season selected. Stop cancels
pending episode starts/autoplay; the existing return waits for stop acknowledgement
and progress writes, and defers navigation while Settings/modal interaction owns
the UI. Settings keeps playback active. Opening B/V while playing retains the
current session.

## To Watch and Favourites

Both libraries and the playback group picker offer Continue watching, To Watch,
Favourites and provider categories (All movies/All series remains first). Lists are
scoped to one source and use the full Movie/Series identity; an item can belong to both. Both
lists support the existing substring search, title ordering and 100-item pages,
with SQL filtering before pagination. They remain visible when empty.

Details have 42-pixel bookmark/favourites buttons beside Play. Grid and Continue
watching posters reveal the same buttons on pointer hover, at bottom left and
bottom right respectively. Clicking them changes only membership, without opening
details or starting playback/probes. The original SVG colors mean unmarked;
marked icons use `Theme.accent`. Tooltips are exactly "Add to plan to watch" /
"Remove from plan to watch" and "Add to favourites" / "Remove from favourites".
The library's left category rows and playback group rows show matching icons;
Continue watching uses `play.svg`.

Schema 8 stores `vod_movie_lists` independently of catalogue and progress caches.
Refresh, disappearance, eviction and credential edits retain membership; source
removal deletes it. Repository reads accompany local catalogue pages. Writes use
the shared serial progress lane; both models receive successful changes, fence
stale replies and disable repeated writes for the same item. A failed write
preserves the prior state and displays an error. Removing the current result
refreshes its list without closing details or changing playback.

A new effective Watched event (strictly more than 95%, or a manual Mark as watched)
atomically checkpoints progress and clears To Watch, leaving Favourites intact.
The automatic completion event is consumed once per playback session. An already
watched film may be added again without resetting progress; later checkpoints of
that session retain it, while a subsequent viewing can clear it again. Manual
Mark as unwatched neither adds nor removes membership and retains its existing
session override. Unknown duration does not auto-complete.

## Progress and transport

Progress writes use a serial lane, a session UUID and increasing sequence numbers.
A newer backward seek wins over an older larger position. Store confirmed backend
state, not requested seeks or unconfirmed track choices.

Strictly more than 95% means watched; exactly 95% remains in progress. Unknown
duration does not auto-complete. Saved positions below 60 seconds show Play
and start at zero; in-session recovery retains the observed position. Resume labels
show hours:minutes. Manual watched/unwatched actions preserve track preferences;
unwatched clears saved position. Manual status stays authoritative for the current
session without seeking/stopping it; a new session restores automatic tracking.

VOD uses the existing video surface and bottom timeline. Scrubbing seeks exact
absolute media time without minute rounding and is disabled without duration or
seekability. Active VOD hides Live rails/Guide affordances; the Back arrow outside the
left movie/group panel opens the library and stops playback through the normal progress-checkpoint path.
The `V` shortcut opens/closes the library without changing playback.
The shared central playback spinner follows VOD opening, resume seeking, recovery
and buffering (except a user pause), and hides on playback, stop or failure.
During probe teardown/cooldown the library retains its Starting state and busy
indicator until playback is handed to the video surface.

Playback chrome includes two edge-attached movie panels. A separate
`vodPlaybackCatalog` instance of `VodCatalogModel` follows the active source and
uses its own A–Z query, 100-item pages and debounced substring search; full-library
filters remain independent. The left panel uses the shared `PlaybackSearchHeader`
for Search movies and ← Groups, matching Live's search/switch geometry; below it
are 124 px rows with a small poster/placeholder and title. Back is a separate
40×40 px control, 24 px to the right of the panel and 24 px below the reserved
windowed title-bar inset. Fullscreen panel backgrounds reach the top edge; Back
sits at 24 px, receiving extra clearance only if it crosses the media switch.
Playback controls use local `topContentInset` for that same narrow-window collision in every mode,
without shifting panel backgrounds. It follows the movie/group panel's animation and visibility,
locks auto-hide on hover and rejects input during chrome transitions. It is
hidden for track pickers, exclusive overlays and the narrow information-only view.
A single movie-row click selects; double-click or Enter plays with the existing
saved-progress/track restoration policy. Confirming
the current film only dismisses chrome. Successful switching checkpoints the old
session and hides chrome; a failed request leaves the list and error visible.

← Groups or another Left from the focused movie list opens
the shared Live-style group picker, highlighting and centering the selected group.
It lists All movies, Continue watching, To Watch, Favourites and provider categories from the playing
source. Search groups filters names independently of movie search. Up/Down browse;
Right, Enter or a single click apply `vodPlaybackCatalog.selectCategory()` and return
to movies without stopping/reloading playback or probing media. Movies → returns
without confirming. Escape hides the picker and chrome without stopping playback.
Movie search is retained within the selected group. Category/search stay separate
from Live and full-library filters; switching movies within the source preserves
the category, and switching source resets to All movies. Category rows omit Live
channel counts. Empty categories keep playback available. In-flight movie queries
cannot be activated before the new list finishes loading.

The right panel reads `playingMovie`, independently of the library's selected
`movie`, and shows the active film's poster, title, year, duration, genres,
description and cast. The poster is left-aligned, leaving room for the same
46 × 46 px Settings button as Live, 12 px from the right and 14 px from the top
of the panel. The button remains fixed while content scrolls and opens the shared
Settings overlay without stopping playback. It follows panel hover, visibility
and animation/input gating. Missing fields are omitted and long content scrolls. Metadata
and artwork replies are matched to the active content identity. `VodRuntime`
retains the confirmed title/year even when the playing item is outside the list
page or search results; browsing the list
does not start media probes. Poster downloads reuse the bounded protected cache.
Panels use the normal reveal, auto-hide, inactivity and animation/input gating.
Group and audio/subtitle pickers temporarily replace the left panel. When the window cannot
fit both panels plus 240 px of central video, only one is shown: the list by
default, with the transport information button or Left/Right selecting the panel.
Tab/Ctrl+F focus movie search, Down moves into the list, arrows select and Enter
plays; Escape releases focus and dismisses visible chrome without stopping playback.
With no exclusive overlay or playback chrome, Escape and Backspace perform the
Back action: checkpoint/stop, then return to the movie library or owning series
details with the stopped episode selected. Escape consumes this action before
fullscreen exit; Backspace in search retains normal text editing.
Numeric Live tuning and Live source/group/recording/multiview actions are disabled
during VOD. Back checkpoints/stops VOD and opens the full library.
F3 and track pickers use active VOD
telemetry. Explicit confirmed track choices restore on resume, recovery and Play
from beginning, independently of Live preferences.

VOD inherits advanced user mpv options, but per-load on-demand invariants control
loops, keep-open, external media, headers, cookies, seeking and start/end behavior.
Reject unknown/conflicting invariant options before loading. Resume waits for
current-load backend events/telemetry, not a fixed GUI timer. Paused telemetry
checkpoints on entry into Paused; completed seeks and track changes still checkpoint.

## Remote MP4 transport and recovery

For HTTP(S) MP4/MOV playback, `MpvPlaybackEngine` prepares a `VodRangeStream`
before handing the load to mpv. Its opaque `okiltv-vod` stream callback retains
up to 16 MiB of 256-KiB byte blocks in memory. Distant audio/video chunks can then
reuse previously read ranges instead of opening a connection for every packet.
Sequential cache misses consume one open-ended HTTP response (`Range: bytes=N-`);
256 KiB is the internal cache block size, not a separate HTTP request. The worker
retains at most one active response and closes it before an uncached distant jump.
Replacing an unfinished response includes a 100-ms teardown grace period because
Qt aborts replies before its HTTP thread finishes closing the socket. Provider-side
slot-release latency is not observable; retryable rejections still use session recovery.
A bounded reply buffer applies backpressure while the reader is paused. Distant
uncached audio/video regions can still require switching that single connection.
The cache is scoped to one load, uses no disk, and does not rewrite the media.
A trailing MP4 index still has to be fetched; ffprobe remains a separate bounded
metadata request and does not populate this cache.

Set `OKILTV_TRACE_VOD_RANGE=1` for opt-in byte-transport timings. Each fetched
block reports milliseconds since stream creation, offset, byte count, whether it
continues the current response, time to validated headers and first consumed
byte, total fetch time and validation outcome. A header time of `-1` means no new
successful header validation occurred during that fetch (normally an already
validated continuing response). These diagnostics contain no media
URLs, redirect targets or credentials. Compare them with the loadfile and
FILE_LOADED events to distinguish network/index transfer from demuxer startup;
they do not measure the first rendered frame or provider slot-release timing.

A Qt-owned worker owns networking. Reads run on mpv's IO thread, with cancellation
checked every 25 ms during a read and a 30-second deadline for each block.
Cancellation also closes an idle/backpressured response; shutdown joins the worker. Only one reader owns a stream. HTTP ranges,
length and available strong ETag/Last-Modified validators are checked before bytes
are published; resource changes or incomplete responses fail the load rather than
mix bytes or report a natural end. Servers that decline ranges (200/416 at the
initial request), other formats and advanced native HTTP/TLS/stream options retain
native mpv transport. Only the configured User-Agent is forwarded; cookies and
arbitrary headers remain disabled. Redirects are bounded and cannot downgrade TLS. The validated final media URL is
retained for subsequent ranges in the same load, avoiding repeated provider
redirect/session setup. A new recovery load resolves the original URL again.

Retryable transport failures have at most two retries, after two and five seconds,
with load/session identity fences and observed progress preserved. Stop during a
retry delay cancels the retry. Initial resume waits for a matching backend seek
completion, with a 60-second failure deadline; the loading indicator remains shown.
A three-second remote seek is not treated as a playback failure.

## Artwork protection and validation

[VodArtworkCache](../qt/src/app/vod/vodartworkcache.h) exposes opaque references and
decoded local JPEG paths. Remote URLs are encrypted in per-source `.url` files;
do not expose them to QML. Downloads use same-origin redirects, no source
credentials/headers, a 10-second/8-MiB limit and a 16-megapixel decode limit. The
decoded disk cache has a 256-MiB LRU budget; failures keep placeholders.

Decoded posters persist across visits and process restarts without a time-based
expiry. Catalogue pages (including Continue watching) and details are enriched
with validated local paths on the existing worker before model publication.
Cached posters therefore do not wait for progress reads or thumbnail request
timers. Local lookup never downloads; only missing visible/detail artwork goes
through the bounded download jobs. The library and playback models share the
cache, with at most one download per source/artwork ID. Waiting callers retain
their own cancellation/deadline; a cancelled downloader releases ownership for
another caller to retry. Disk hits update LRU access time. Unreadable images are
cache misses and are removed. Refreshing a catalogue does not invalidate artwork;
a changed URL creates a new opaque reference. Images can be downloaded again
after LRU eviction or source removal. Local paths are transient and never saved
in the catalogue database.

Use the `vod` CTest label for domain, contract, provider, storage, mpv, runtime and
QML coverage where available. Under the `qml` label,
`OKILTVQtPlaybackSearchHeaderTests` covers shared header input and switching in
Live/movie/group modes. `OKILTVQtVodPlaybackUiTests` covers sidebar selection,
search, focus, group requests and pagination.
`UI-12-vod-movies` covers the library, playback panels and switching with local
fixtures. Tests must not depend on `.secrets` or real provider accounts.
