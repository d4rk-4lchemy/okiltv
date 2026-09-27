# VOD storage contract

`IVodMigrations`, `IVodCatalogRepository`, `IVodProgressRepository` and
`IVodSourceAccess` are ports. `SqliteVodStore` implements them with per-operation,
thread-owned connections, additive versioned migrations, consistent `VACUUM INTO`
backups, staging and transactional publication. `VodRuntime` wires these ports
into the application; B4 integration is still undergoing verification.
`qt/tests/vod/fakes/vodfakes.h` remains an in-memory executable reference
contract, never an application fallback. The logical table inventory is in
`.project/OKILTV_ARCHITEKTURA_VOD.md`, section 8.

## Identity and schema version 6

Use the existing application database, additive `vod_*` tables, and an independent
`vod_schema_migrations(version INTEGER PRIMARY KEY)` marker. Migration runs on a
worker before first repository access. A newer version, failed migration or
unavailable secret protection disables VOD without resetting Live data. Repeated
prepare calls are idempotent and serialized. Back up consistently before B2
migration and test rollback/restart with the real database service.

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
validated fields, format version and UTC fetch time. Signed artwork URLs require
secret protection; playback descriptors and headers are never persisted.

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

Local page tokens carry generation and a stable `(sort_key, identity)` cursor.
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
auto-completes. The serial application progress lane also handles manual status
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
`CatalogQuery.titleContains` adds normalized substring matching on worker SQL
queries; the existing title-prefix contract and generation-tagged keyset pages
remain available.
