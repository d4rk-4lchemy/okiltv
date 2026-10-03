# VOD and the test movie library

[Documentation index](README.md)

## Status and boundaries

The backend has Xtream movie/episode resolution, SQLite persistence and native mpv
integration. Local integration coverage exists; Windows hardware acceptance is
pending. Do not describe VOD as production-ready. The current library UI exposes
movies only: no series browser, M3U VOD, recommendations or permanent navigation
entry. The [VOD design document](../.project/OKILTV_ARCHITEKTURA_VOD.md) records the
broader design and acceptance status.

`V` opens the exclusive `vod` overlay. First use lazily initializes the runtime
and opts the selected Xtream source in for this process. Persisted global/source
VOD and global series flags remain disabled by default. Library source selection
is independent of Live source selection. Browsing/opening details does not start
or stop playback; Play/Resume is explicit.

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
| Movie overlay | [VodMoviesPage.qml](../qt/qml/screens/VodMoviesPage.qml) |

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
Valid empty snapshots are distinct from cache misses; failed refresh keeps prior
data. Generation-bound page cursors must not mix snapshots. Explicit source
removal invalidates work and deletes its VOD state/artwork; cache eviction alone
must retain durable progress.

VOD uses additive `vod_*` tables in the application database and its own migration
marker. Schema 8 adds durable per-movie To Watch/Favourites membership. Schema 7 rebuilds stored base-letter title keys and advances catalogue
generations; schema 6 adds confirmed track preferences to progress. Earlier additions
include source mutation recovery, category snapshots and artwork references. See
the [storage contract](../qt/src/core/vod/storage/README.md) before altering schema
or repository behavior. Workers must be joined before runtime storage is destroyed.

## Library behavior

- The entire library, including details, slides from the bottom on opening and
  returns below the viewport on closing (240 ms, `Easing.OutCubic`, no fade).
  V, the close button, final Escape and successful playback start share the
  animated close path. The exclusive overlay and catalogue stay present through
  closing; input is blocked throughout either transition. Focus and initial
  selection wait for opening completion, while closing rejects delayed focus
  changes. Reopening retains browse state. These animations do not start, pause
  or stop playback; the existing playback and Back actions retain their behavior.
- The virtualized grid uses local substring search, A–Z/Z–A sorting, categories
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
- Posters remain 199 × 282 px with 16 px horizontal spacing. Resizing changes the
  column count, never card dimensions. Use the neutral overlay palette. Library
  buttons, navigation and selectors use neutral grey interaction states.
  The primary Play/Resume button uses a blue background; active list icons use
  the same blue accent. Play/Resume has `play.svg` followed
  by Play or Resume · HH:MM; its tooltip repeats the label. Other detail actions
  are 42 × 42 px icon buttons: `start-from-beginning.svg`, `mark-watched.svg` or
  `mark-unwatched.svg`, with tooltips describing their actions.
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

## To Watch and Favourites

The movie library and playback group picker offer Continue watching, To Watch,
Favourites and provider categories (All movies remains first). Lists are scoped to
one source and use the full content identity; a film can belong to both. Both
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
stale replies and disable repeated writes for the same movie. A failed write
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
title-bar inset. It follows the movie/group panel's animation and visibility,
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
description and cast. Missing fields are omitted and long content scrolls. Metadata
and artwork replies are matched to the active content identity. `VodRuntime`
retains the confirmed title/year even when the playing item is outside the list
page or search results; browsing the list
does not start media probes. Poster downloads reuse the bounded protected cache.
Panels use the normal reveal, auto-hide, inactivity and animation/input gating.
Group and audio/subtitle pickers temporarily replace the left panel. When the window cannot
fit both panels plus 240 px of central video, only one is shown: the list by
default, with the transport information button or Left/Right selecting the panel.
Tab/Ctrl+F focus movie search, Down moves into the list, arrows select and Enter
plays; Escape releases focus and dismisses chrome without stopping playback.
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
