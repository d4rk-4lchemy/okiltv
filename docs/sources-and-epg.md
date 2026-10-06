# Sources, channels and EPG

[Documentation index](README.md)

## Data and source access

[models.h](../qt/src/core/models.h) defines `ServerProfile`, `Channel`, `EpgEntry`,
`DvrScheduleEntry` and `AppSettings`. A profile has a UUID and type: Xtream,
M3U URL or M3U file. Channel identity includes its profile, so equal provider
channel IDs from different sources are distinct.

Read full credentials and timezone through `SettingsManager::profileById()` or
`activeProfile()`. `current().profiles` contains summary mirrors, not resolver-grade
connection details. Use the profile mutators; direct summary mutation bypasses
the protected source store. See [settings and storage](settings-and-storage.md).

| Change | Main code |
|---|---|
| Source loading/refresh and stale-result rejection | [appcontroller.cpp](../qt/src/app/appcontroller.cpp) |
| Xtream authentication, categories and streams | [xtreamservice.cpp](../qt/src/core/xtreamservice.cpp) |
| M3U metadata and relative references | [m3uservice.cpp](../qt/src/core/m3uservice.cpp) |
| Channel persistence and refresh reconciliation | [database_service.cpp](../qt/src/core/database_service.cpp) |
| Display/filter/selection and direct numeric tune | [channellistmodel.cpp](../qt/src/app/channellistmodel.cpp) |
| Group order, visibility and favourites | [sourcegroupsmodel.cpp](../qt/src/app/sourcegroupsmodel.cpp) |

Xtream channel IDs come from provider `stream_id`. M3U initially assigns IDs from
zero and retains them by exact stream URL, including duplicate occurrences, across
refreshes. New M3U IDs use a durable per-source sequence. Reordering changes
`sortOrder`, not identity; changed URLs create new identities. ID zero is valid.

`channelNumber` is a canonical positive decimal string. M3U reads `tvg-chno` and
Xtream reads `num`; display formatting can use the locale separator. Do not cast
decimal channel numbers to integers or conflate them with database IDs.

## Import and group contracts

Live TV source requests (Xtream authentication, categories and streams, and remote
M3U playlists) use a 90-second total HTTP request timeout. Sequential requests
each receive their own limit, so a complete source refresh can take longer.

- Xtream category/stream endpoints must return arrays. Reject malformed shapes
  with bounded, redacted diagnostics. Persist human-readable category names.
- M3U supports header defaults and per-entry overrides, relative stream/logo/XMLTV
  references and `#EXTGRP` fallback. A direct HLS manifest is one channel; its
  segments or variants are not separate channels.
- Explicit XMLTV configuration overrides discovered `url-tvg`/`x-tvg-url` hints.
  Discovery is persisted in protected profile details and survives failed refresh.
- Refresh replacement is transactional and prunes removed channels. Preserve
  retained channel identities, favourites, watch history and startup restoration.
- Use existing category normalization helpers. Group visibility/order and manual
  favourite exclusions are per source.
- Automatic favourites become eligible at 360 full watched minutes. Explicit
  removal persists an exclusion; later watching must not silently re-add them.
- Periodic watch updates preserve selection and scroll. Membership/order changes
  use row insertion/removal/moves instead of resetting unchanged lists.

M3U is a catalogue format and HLS is a transport; do not infer transport features
from the source type. The [source feature matrix](../.project/source-feature-parity.md)
contains the detailed parity and archive-template rules.

## VOD source refresh

Xtream’s saved Enable VOD setting controls library availability and defaults to
true after a per-profile protected migration. The shared source-refresh entrypoint
emits `sourceRefreshRequested` to `VodRuntime`; movie/series category import and
selected movie synchronization run independently of channel and EPG work. Adding
or enabling a source also starts this flow. Series import is category-only and
independent of the global series playback flag. M3U VOD remains unsupported.

Settings group editors retain independent media/source selection, order, search
and Hide Unchecked drafts, including provider categories without cached titles.
Category reconciliation uses Live’s up-to-50 auto-enable rule. Selected movie
categories determine the sequential category requests below 30% or full catalogue
request at/above 30%; local membership filtering and a single atomic publication
protect against ignored server filters and partial failures. See [VOD](vod.md).

## EPG pipeline

```text
XMLTV network/local input
  -> bounded streaming XML/gzip/zlib import
  -> new EpgStore SQLite generation
  -> validation and atomic cache-manifest publication
  -> immutable EpgService snapshot
  -> background range/summary/detail queries
  -> independent browse/playback NowNext models and Guide
```

[epgservice.cpp](../qt/src/core/epgservice.cpp),
[epgstore.cpp](../qt/src/core/epgstore.cpp) and
[epgcache_service.cpp](../qt/src/core/epgcache_service.cpp) own this pipeline.
Production snapshots pin disk generations; they do not materialize every
programme in memory. Fixture helpers may still use small in-memory snapshots.

The importer retains all accepted provider channels/dates, independently of the
playlist and visible Guide range. Malformed/truncated XML fails the import;
`<tv/>` is a valid empty result. Multiple XMLTV sources use the first normalized
channel/start identity. Matching to channels uses `tvgId`.

Only successful complete imports replace the published manifest. Failed refresh
keeps the last usable generation and reports failure. Publication and cancellation
are synchronized so obsolete work cannot overwrite newer data. Old generations
remain readable until their last readers finish.

There are two read workers and one importer. Capture one snapshot per job; never
mix generations. Database reads must not run inside QML getters or model `data()`.
The query LRU is bounded per generation; it is not a total process memory limit.

## Guide and detail loading

[EpgGridModel](../qt/src/app/epggridmodel.h) asynchronously loads visible rows/time
ranges with bounded prefetch. [NowNextModel](../qt/src/app/nownextmodel.h) keeps
browse and playback presentation separate. Programme summaries may have
`detailsPending`; hydrate details for selection/hover and before taking DVR,
download or catch-up action snapshots.

`epgCacheBootstrapPending` describes startup cache bootstrap only. It must not
become a generic “network loading” flag. `AppController.isBusy == false` means
profile loading finished; tests still need to wait for the relevant EPG result.

Guide history/look-ahead settings accept 1–999 hours. Render only the viewport
and bounded prefetch, even for a large configured range. Same-identity EPG rebuilds
preserve selection and both scroll axes. Channel/source structural changes can
reset the model. Live group filtering affects Guide channels; Guide search remains
independent of Live search.

Display date/time changes reformat retained timestamps without changing provider
time, programme identity or playback. Use the shared formatting helpers described
in [UI and interaction](ui-and-interaction.md).

## Live EPG search

`Ctrl+F` in the Live context opens the exclusive `epg-search` overlay. Search
covers all eligible channels in the active source, excluding hidden groups,
independently of the channel rail's query/category and the Guide time horizon.
It searches the complete published local EPG generation, without provider calls
while typing. One result represents one airing on one playback channel; shared
XMLTV IDs produce separate channel results and exact duplicates are removed.

[EpgSearchController](../qt/src/app/epgsearchcontroller.h) owns the session,
150 ms debounce, one search worker and the latest queued request. Its separate
[EpgSearchModel](../qt/src/app/epgsearchmodel.h) publishes bounded pages of 50
results on the UI thread. Query/filter/context edits immediately invalidate old
actions. Request, source, channel revision and EPG generation checks reject stale
responses; closing cancels work and shutdown joins the worker. Details are loaded
only for the selected result using the existing generation-checked detail API.
The model publishes cached station logos as `file:` URLs via `QUrl::fromLocalFile`,
preserving spaces and URL punctuation; uncached logos retain their source URL.

The core contract lives in [epgsearchtypes.h](../qt/src/core/epgsearchtypes.h).
Normalization folds case and accents (including Polish ł), preserves other
alphabets and splits punctuation into word boundaries. Every token must match
in the title/subtitle as a word prefix, including single-character fragments in
an otherwise valid query: `Spider-M` finds `Spider-Man`. Hyphens, Unicode dashes,
dots, slashes, colons, parentheses and quotes act as word separators rather than
literal search operators. At least one token must have two characters, so `M`
alone does not start a search. Queries exceeding 256 input
characters or 16 tokens show a validation error. User text never becomes raw
FTS syntax. Highlight ranges map back to the original Unicode text.

SQLite FTS5 indexes normalized titles/subtitles in each new immutable generation.
Search schema 2 and normalization version 1 are separate from the ordinary EPG
metadata version 1, which remains readable by rollback builds. Chronological FTS
row IDs support forward/backward reads within time groups; filtering and
shared-channel mapping happen before the bounded result heap cuts a page.
Every title/subtitle match is read in chronological FTS row-ID order, without
separate match-quality buckets; the existing exact-title index remains in schema 2
for cache compatibility but no longer controls result ordering. FTS population checks cancellation
per document and commits in batches of 1000; SQL sorting/integrity statements
finish before their next cancellation check.
Older generations remain available to Guide while a local background replacement
builds the index; only a verified complete replacement may update the manifest.
At application shutdown, `shutdownSearchPreparations()` fences new imports and
upgrades, cancels their tokens and joins the import pool before Qt SQL teardown.
A missing FTS5 capability reports a search error. Linux and Windows acceptance
must exercise the shipped QSQLITE plugin rather than the system sqlite CLI.

All results group Now, Upcoming, then Past. Start dates are the primary order
within each group: Now/Upcoming ascend (upcoming nearest first), while Past
descends (newest first). Individual time filters use the same date order. Only
equal start dates use exact normalized titles, all-title prefix matches and then
subtitle matches as tiebreakers, followed by channel order and stable airing
identity. The indexed reader consumes every boundary timestamp before cutting a
page, so a later exact title cannot displace an earlier prefix/subtitle match.
Pages share a frozen UTC clock and request context. Channel mapping,
filtering and deduplication precede visible pagination. Current action availability
is reevaluated at activation, independently of the frozen list ordering.

Past means ended programmes retained in local EPG, not guaranteed archive
availability. Live, catch-up, DVR and downloads use AppController's existing
validation. Past airings expose Play from beginning only with a valid saved resume
point (primary Resume); unstarted/completed/expired or sub-minute progress uses
primary Play at zero without a duplicate restart action. The selected action map
sets `recordingVisible` false for unscheduled past airings, avoiding a disabled
Schedule recording control; existing scheduled jobs retain their cancellation. Current airings retain
their available catch-up restart. Ctrl+Enter routes through
`EpgSearchController::activateSelectedFromBeginningOrDefault`, which revalidates
the current restart availability before choosing restart or the primary action;
a stale/expired resume point cannot suppress the fallback. Future/unavailable airings open details. Explicit Live activation
resolves source/channel identity without changing rail filters and follows the
focused multiview destination. Archive playback is visibly unavailable in grid
multiview because the legacy archive path would dismantle the grid; PiP retains
its existing destination policy. Ctrl+R in search never falls back to manual
recording. Opening, closing and selection do not tune or pause any player.
Native download completion revalidates the selected airing and request identity
before enqueueing. Deferred catch-up handoff also checks the source, channel scope
and EPG generation before accepting playback.

## Verification focus

Cover stable M3U IDs, reordered/empty imports, source edits/removal during jobs,
reversed completion order, failed cache fallback, EPG cancellation/publication,
pinned generation retirement and stale details. Use local XMLTV/M3U/HTTP fixtures.
Guide pointer tests must distinguish real pointer movement from delegate entry
caused by scrolling under a stationary pointer. See [build and testing](build-and-testing.md).
