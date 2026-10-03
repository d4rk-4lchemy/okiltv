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

## Verification focus

Cover stable M3U IDs, reordered/empty imports, source edits/removal during jobs,
reversed completion order, failed cache fallback, EPG cancellation/publication,
pinned generation retirement and stale details. Use local XMLTV/M3U/HTTP fixtures.
Guide pointer tests must distinguish real pointer movement from delegate entry
caused by scrolling under a stationary pointer. See [build and testing](build-and-testing.md).
