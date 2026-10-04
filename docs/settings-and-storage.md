# Settings, persistence and data protection

[Documentation index](README.md)

## Storage map

Resolve paths through [AppDataPaths](../qt/src/core/appdatapaths.h). Do not hardcode
the developer's home directory or bypass portable overrides.

| Data | Owner and persistence |
|---|---|
| Application preferences | [SettingsManager](../qt/src/core/settingsmanager.h), `settings.json` |
| Source list metadata | [SourceStore](../qt/src/core/sourcestore.h), `source-summaries.json` |
| Source connection details | `SourceStore`, `sources/<UUID>.json` protected envelopes |
| Channels, watch statistics and related state | [DatabaseService](../qt/src/core/database_service.h), application SQLite database |
| EPG | [EpgCacheService](../qt/src/core/epgcache_service.h), per-profile manifest and immutable SQLite generations |
| VOD catalogue/progress/movie and series lists | [SqliteVodStore](../qt/src/core/vod/storage/sqlitevodstore.h), additive `vod_*` tables |
| VOD artwork | Protected URL registry and decoded JPEG cache under the VOD artwork directory |

VOD JPEG posters persist across restarts with a fixed 256-MiB LRU budget and no
time-based expiry. Catalogue refresh retains them; changed artwork URLs create
new references. Evicted/unreadable images may be downloaded again on demand.
Local cache lookup runs on workers before catalogue/detail publication. Source
removal/reconciliation cleans its registry and decoded images. VOD schema 10 adds durable series continuation suppression in `vod_series_state`,
cleared only by confirmed playback from a new session after bulk manual Watched.
Bulk episode statuses, suppression and To Watch removal share one transaction.
Schema 9 adds the durable last-played episode pointer in `vod_series_history`,
updated atomically with playback progress; manual status edits do not move it.
Migration backfills older episode history and preserves movie catalogues/artwork.
Schema 8 supplies independent Movie/Series To Watch/Favourites membership; catalogue
eviction and credential edits preserve it, while source removal deletes it. Schema 7
transactionally rebuilds base-letter title keys without discarding watch history;
see the [VOD storage contract](../qt/src/core/vod/storage/README.md).

The Windows portable ZIP includes `OKILTV-portable.json` beside the executable.
Its default data root is the sibling `data/` directory; an absolute override can
be saved through Portable settings. The installer does not include this marker
and uses the normal application data directory. Portable data is still bound to
the operating-system account for secret protection.

## Settings edits and source mutations

[AppSettings](../qt/src/core/models.h) defines persisted values;
[SettingsController](../qt/src/app/settingscontroller.h) owns editable drafts.
For a new setting, inspect model defaults, JSON serialization/normalization,
controller dirty tracking and Save/reload, runtime application, QML and tests.
Some backend settings intentionally have no UI control.

Save must handle failure without losing drafts or falsely publishing runtime
success. Async source loading/refresh preserves pending edits. Runtime state such
as shutdown volume is independent of the draft form; save volume before backend
teardown, including zero/muted.

`AppSettings.vodLibrarySidebarWidth` stores the library sidebar preference in
logical pixels (`0` means automatic, manual values are at least 208). Missing or
invalid JSON values use automatic sizing. `ShellController` exposes the notifying
property and `setVodLibrarySidebarWidth(int)`; completed drags save atomically
through `SettingsManager`, independently of Settings drafts. Failed writes roll
back the value and log the error. Window-size clamps never overwrite the stored
preference. The value is global across sources and needs no database migration.

Use `SettingsManager` profile mutators for add/edit/remove. Source summaries are
not full profiles and must not be used for credential resolution. VOD source
changes require its mutation/stop/checkpoint barrier before source files change.
Availability-only VOD changes bypass the credential stop barrier and preserve
credential revisions; credential edits/removal still use it. Settings/source files
and SQLite are not one atomic transaction; recovery must
reconcile actual persisted state. See [VOD](vod.md).

Live track preferences are per profile/channel and saved only after confirmed
backend selection. Xtream uses stream IDs; M3U uses a SHA-256 hash of the direct
Live URL. VOD preferences use independent content identity and progress storage.
Never persist raw connection URLs as preference keys.

## VOD source defaults and group preferences

`ServerProfile.vodEnabled` defaults to true. Protected profile JSON includes
`vodDefaultsVersion`; startup migrates each older Xtream profile to enabled and
commits version 1 with that profile, without changing credential revisions. A
failed protected write retains the old marker for retry. A migrated profile’s
later explicit false survives other profiles’ retries and subsequent startups.
The global technical `AppSettings.vodEnabled` and legacy `vodSeriesEnabled` do not
veto enabled sources. Saved Enable VOD admits both movies and series.

Existing Live preference keys remain plain source UUIDs. Movies and Series use
`<UUID>|movies` and `<UUID>|series` keys in `hiddenGroupsByProfile`,
`groupOrderByProfile` and `hideUncheckedGroupsByProfile`. Provider category
snapshots remain separate by media kind in `vod_categories`; no VOD schema change
is needed. Source deletion removes both VOD preference keys. The generalized
`SourceGroupsModel` keeps drafts keyed by source within each editor; successful
Save publishes all edited sources, failed settings writes roll back memory and
retain drafts/error state, and Discard reloads saved values. Search is independent
per editor/source within the process. These preferences are separate from watch
progress, durable movie lists and track preferences.

Source-form Save commits edited sources and group models in sequence. A failure
leaves remaining drafts editable and shows the write error; successful source
creations are removed from the pending map immediately, so retry cannot duplicate
them. Successfully changed connections are queued for refresh even if a later
source/group write fails.

## Protection boundary

[SecretProtection](../qt/src/core/secretprotection.h) uses user-scoped DPAPI on
Windows and AES-256-GCM with a random key stored in Secret Service through libsecret
on Linux. There is no plaintext fallback and no key file beside application data.
Linux requires OpenSSL Crypto, `libsecret-1.so.0` and an unlocked Secret Service.
macOS Keychain support is not implemented.

Protect full source profiles, sensitive DVR/player settings, channel `stream_url`,
`icon_url` and `catchup_source_template`, and invalid-settings backup bytes.
Source summaries expose list metadata without connection details. Model `toJson()`
helpers serialize in-memory data; they do not establish a disk protection boundary.

Use the existing redaction/logger facilities for diagnostics. Do not write resolved
media URLs/headers or provider credentials into QML, logs, test reports or docs.
This protection does not defend against the same logged-in user or erase external
backups. ffmpeg/ffprobe command arguments and user-enabled raw mpv logging remain
known exposure limits, described in [security-storage.md](../.project/security-storage.md).

## Migration and failure recovery

- Write JSON atomically and check both write and commit results. Close settings
  readers before `QSaveFile` replacement; Windows can reject replacement while a
  read handle remains open.
- Migrate source details before committing settings migration markers. Verify
  protected round trips, preserve original files on failure and allow retry.
- Encrypt legacy channel fields transactionally; use a durable cleanup marker
  through secure-delete/checkpoint/VACUUM. Clear it only after successful cleanup.
- Initialize/migrate databases explicitly, never on ordinary reads/writes. Startup
  uses the Qt worker/progress-window lifecycle in [architecture](architecture.md).
- Every new database transaction needs explicit rollback on exceptions. Protect
  channel secrets before opening the refresh write transaction.
- UI watch-stat writes use a zero busy timeout and retain failed increments for
  later flush. Timer/shutdown callbacks must not leak exceptions.
- Preserve unreadable source files and protected settings. Re-entering credentials
  under a different OS account does not decrypt old schedules/options. Recovery
  needs the original account/store or explicit recreation.
- Older applications cannot read the migrated protected format. Do not run them
  against a migrated data directory.

Source removal must invalidate pending jobs and clean related channels, EPG,
history/bookmarks and scheduled DVR state through existing owners. VOD additionally
uses durable removal intent, stop acknowledgement and idempotent cleanup.

## Validation

Use disposable data directories and local fixtures for migration, rollback,
tampering, failed writes, locked/unavailable protection and restart tests. The
`OKILTV_SECURITY_TESTS` key exists only in test binaries; production must not gain
an environment switch that bypasses the secret store. UI integration uses a
disposable unlocked keyring. Account transfer, Windows DPAPI and desktop keyring
behavior need target-platform checks in addition to unit tests.
