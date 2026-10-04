# VOD storage contract

`IVodMigrations`, `IVodCatalogRepository`, `IVodProgressRepository`, `IVodMovieListsRepository` and
`IVodSourceAccess` are ports. `SqliteVodStore` implements them with per-operation,
thread-owned connections, additive versioned migrations, consistent `VACUUM INTO`
backups, staging and transactional publication. `VodRuntime` wires these ports
into the application; B4 integration is still undergoing verification.
`qt/tests/vod/fakes/vodfakes.h` remains an in-memory executable reference
contract, never an application fallback. The logical table inventory is in
`.project/OKILTV_ARCHITEKTURA_VOD.md`, section 8.

## Identity and schema version 10

Use the existing application database, additive `vod_*` tables, and an independent
`vod_schema_migrations(version INTEGER PRIMARY KEY)` marker. Migration runs on a
worker before first repository access. A newer version, failed migration or
unavailable secret protection disables VOD without resetting Live data. Repeated
prepare calls are idempotent and serialized. Back up consistently before B2
migration and test rollback/restart with the real database service.

Schema 10 adds `vod_series_state`, keyed by full series identity with profile,
namespace, `continue_hidden` and `blocked_session`. It has no catalogue foreign
key; refresh/eviction and credential edits retain it, source removal deletes it.
`setSeriesWatched(SeriesWatchedChange, RequestContext)` writes every supplied known
episode (including Specials and unavailable items) atomically with suppression
and Series To Watch removal for Watched. Unwatched resets positions, preserves
track preferences and leaves suppression/list choices intact. Manual writes never
move `vod_series_history`; current-session writes use its established token and
sequence, other episodes get fresh manual tokens. Invalid/stale writes roll back
all changes. `SeriesProgress.continuationHidden` fences the domain projection and
SQL filters it before pagination. A confirmed playback checkpoint from a token
other than `blocked_session` clears suppression; beginning a session or changing
manual status alone does not. Migration recreates the continuation view while
preserving existing history, lists, tracks and catalogues.

Series Watched is derived from all known ordinary episodes; Specials are excluded
and at least one ordinary episode is required. The domain helper `seriesWatched`
and episode-completion SQL use the same strict 95%/manual completion criterion.

Schema 9 adds `vod_series_history`: full Series identity plus source/namespace,
provider series ID, last-played episode ID and playback update time. Checkpoints
update this pointer atomically with episode progress only when
`VodProgress.playbackCheckpoint` is true. Manual status edits set it false.
Migration backfills prior episode histories without altering movie data.
`readSeriesProgress` returns episode progress in one transaction, the remembered
episode and its projected continuation. `vod_series_continue` projects an
incomplete remembered episode or its immediate ordinary successor, using the
same season/episode/provider ordering as the domain queue. It excludes completed
Specials and series with no successor. SQL applies this projection, category,
search and list filters before keyset pagination. Artwork for Series follows the
same staged/published protected references as Movie.

Schema 8 adds `vod_movie_lists`, keyed by full content identity, for both Movie and Series with independent
`to_watch` and `favourite` flags and a source lookup index. There is no foreign key
to catalogue rows: refresh, provider disappearance, eviction and credential edits
retain the flags. Explicit source removal deletes them. Queries return membership
for their page and apply the selected movie-list filter before keyset pagination.
No provider or media requests are needed to change a list.

`checkpoint(..., completed=true)` clears To Watch in the progress transaction only
for a Movie whose effective status is Watched, after session/sequence validation.
An Episode completion clears the parent Series To Watch only when all known
ordinary episodes have Watched status. Season zero Specials do not block it.
Favourites remains. Failure rolls back progress, pointer and membership together. The application supplies a completion event once
per automatic playback session, or for an explicit Mark as watched action. Re-adding
an already completed movie is allowed and does not reset progress; later checkpoints
without a new completion event preserve it. Favourites is never auto-cleared.
Membership writes share the serial progress lane and source mutation barriers.

Schema 7 recomputes `vod_items.sort_key` and retained `vod_staging.sort_key` in
bounded 256-row batches inside the migration transaction. Keys use Unicode case
folding, NFKD and combining-mark removal, with the explicit Latin mappings
ł/l, ø/o, đ/ð/d, ħ/h, ı/i, æ/ae, œ/oe, þ/th and ß/ss. Display titles stay unchanged.
Source catalogue generations advance once to reject pre-migration cursors;
refresh timestamps, content identities, artwork, progress and track preferences
remain intact. Existing catalogues need no provider refresh. Failure rolls back
keys, generations and the schema marker together; the next prepare can retry.

Schema 6 adds `vod_progress.track_preferences`, a JSON object containing confirmed
explicit audio/subtitle choices (including subtitle off). Existing progress rows
migrate with an empty object, preserving positions. Preferences share the same
content identity, session/sequence fences and atomic checkpoint as position; they
are restored even when starting from the beginning. Track metadata and layout
support safe matching after reload; no media URL or credentials are stored.

A source row has `profile_id`, a persistent `catalog_namespace` UUID,
`credential_revision`, capabilities, sync state and deletion intent. **Changing
endpoint, domain, username or password increments credential_revision but never
changes catalog_namespace or deletes progress.** Never derive the namespace from
credentials. A newly created source gets a new namespace; explicit removal deletes
its VOD state. Source summaries contain no additional credential copy.

Schema 3 separates `configuration_revision` (the revision in protected source
configuration) from `revision` (the monotonically increasing request fence).
Before changing credentials, checkpoint and stop the active session, advance the
request fence and persist `mutation_pending=1`. Reads and writes reject that
pending state. After the configuration write, read the actual protected source
file and bind its revision to the new request fence, clearing the pending marker.
On write failure this restores access with the previously saved credentials;
namespace/history remain intact, and playback does not resume automatically.
Do not decrement the request fence: old snapshots, imports and checkpoints must
remain rejected even if a later edit reuses the same configuration revision.
Startup reconciles pending edits before enabling the source. If reconciliation
cannot read the source or update SQLite, keep it blocked and report the error.
A timed-out form does not release its barrier before its outstanding worker ends.

The logical identity tuple is `(profile_id, catalog_namespace, content_kind,
provider_item_id, parent_namespace)`. Provider IDs are TEXT, never numeric casts.
Represent missing parent separately from a present value (SQLite NULL uniqueness
alone is insufficient). `ContentRef::key()` is a versioned length-prefixed Qt Core
encoding for in-process keys; SQL should use the logical columns and explicit
uniqueness constraints. It is not a provider URL or title.

Schema 4 adds `vod_categories` keyed by `(profile, namespace, kind, id)` and
`vod_category_snapshots` with fetch time and the current refresh request ID.
Movie/series category IDs cannot collide. Categories retain provider order and
optional parent identity. The snapshot row distinguishes a complete empty response
from a cache miss. `categories(scope)` returns cached data, including stale data,
with its UTC timestamp; `categories(scope, true)` explicitly refreshes it. A failed
refresh reports its error and leaves the previous snapshot readable. Starting a
new refresh fences older responses even when the newer request fails. Publication
checks source revision, cancellation, deadline and request identity in a transaction.
Whole-kind eviction removes the category snapshot; category-filtered item eviction
retains category metadata. Source removal deletes both tables, without leaving labels.

Item/category links are many-to-many. Seasons belong to a series; episode identity
lives once in `vod_items`, with separate season/series links. Details contain
validated fields, format version and UTC fetch time. The existing `mediaProbe`
payload also stores technical metadata observed through the active mpv load:
actual dimensions and audio/subtitle tracks with UTC observation time. Playback
updates only existing details using local repository reads/writes and no extra
provider/media connection; actual duration replaces the provider hint. Signed
artwork URLs require secret protection; playback descriptors and headers are never persisted.

`readSeasonMediaMetadata(series, seasonId, context)` reads episode details in one
local read transaction using the existing seven-day detail-cache validity. It
returns the newest measured `mediaProbe` by observation time, breaking ties by
episode number/provider order within that series and season. It includes known
unavailable episodes as possible metadata donors. Source/namespace fences,
cancellation and deadlines apply; no provider, artwork or media request is made.
The result is presentation-only and must not be persisted as another episode's
measurement or track preference. This read requires no schema migration.

## Publication and local queries

A refresh token binds scope (source/namespace/movies-or-series/optional category),
source revision, latest import ID, cancellation and deadline. Stage bounded batches
outside the published generation. Inside the publication transaction recheck all
these conditions and deletion intent. Only a complete, validated import can
advance the generation. Valid empty imports replace just their scope; failed or
partial imports leave the old generation. Cancellation and source edits invalidate
publication, even when the provider still returns a payload.

`abandonRefresh` is noexcept/idempotent, including after publication; cleanup failure
must retain a staging marker for later reconciliation. Abandon never deletes the
published snapshot. Another category and lazily absent episode details are not
implicitly missing from this import.

Local page tokens carry generation and a `(sort_key, identity)` cursor. Title
queries and prefix/substring filters use the same base-letter normalized titles;
Continue watching uses the numeric UTC progress
update time, serialized in `lastSortKey`, with descending identity as a tiebreaker.
Reject stale-generation cursors instead of mixing generations. Index source,
namespace, kind, category links and normalized title with identity as tiebreaker.
Prefix search is the initial contract; no FTS or substring performance guarantee.

## Durable progress and removal

`vod_progress` uses the content identity with NO cascade from catalog/cache tables.
Store position/duration in INTEGER milliseconds, optional content revision,
watch status, session UUID, monotonic sequence and UTC update time. `beginSession`
establishes ownership before checkpoints; only that session and increasing sequence
can write. A larger position never wins over a newer backward seek. Serialize
beginSession calls in the application progress lane; delayed checkpoints from a
retired session fail. Cache eviction/provider disappearance retains history.

`CatalogQuery.continueWatchingOnly` filters on durable progress before keyset
pagination: positive position, InProgress status, and at least 5% remaining when
duration is known. Strictly greater than 95% is watched; unknown duration never
auto-completes. Results sort by `vod_progress.updated` descending, independently
of title sorting, before applying the page limit. The shelf and full category
therefore show the most recently watched movies first. The serial application
progress lane also handles manual status
changes, preserving tracks and clearing position for Mark as unwatched. Manual
choices override checkpoints for that session; a new playback session restores
automatic tracking. No schema migration is needed for these existing fields.

`prepareRemoval` durably marks intent and invalidates writers under the same barrier
as publication/checkpoints. Then cancel work, confirm playback stop, remove VOD
rows/artwork and finish intent. Cleanup is idempotent. After a crash, complete any
remaining intents before accepting new work. Settings/source files and SQLite are
not one transaction. `VodRuntime` installs the preparation hook before actual
SourceStore removal. It waits for the matching active-session stop acknowledgement
before allowing the source file to change. Credential edits additionally flush
progress; stop timeout or failed flush prevents the edit. A failed source-file
write is reconciled against persisted credentials, without resuming playback.

### Movie artwork (schema 5)

Staged and published item rows retain opaque artwork IDs/roles in an additive
`artwork` column. Details encode the same references, accepting older payloads
without artwork. Protected remote URLs live in the per-source artwork registry,
never in public catalog events or QML. Decoded JPEG cache files have a separate
256-MiB LRU budget; source deletion/reconciliation removes the associated files.
Application workers add existing validated JPEG paths to transient `ArtworkRef`
values before catalogue/detail publication. Neither cached local paths nor remote
URLs are serialized in staged/published catalogue rows.
`CatalogQuery.titleContains` adds normalized substring matching on worker SQL
queries; the existing title-prefix contract and generation-tagged keyset pages
remain available.

## Source policy and filtered queries

`CatalogQuery.allowedCategories` optionally restricts category membership before
search, ordering, special-list filters and pagination; an explicit empty list
returns no rows. `identity` permits bounded admission checks for a selected movie.
Source-scoped movie/series group preferences live in SettingsManager, keyed by
UUID plus `|movies`/`|series`, rather than a second catalogue schema.

Policy changes use a process-local generation in `RequestContext.policyCurrent`.
`publishIfCurrent` and category snapshot publication lock `policyMutex` through
final validation/commit, so disabled/obsolete selections cannot publish. A disabled
source can still produce a storage snapshot: network/new playback admission is in
VodController, while active-session progress remains writable. Credential/removal
fences and durable namespace/revision behavior are unchanged.
