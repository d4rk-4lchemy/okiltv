# UI Test Model (Linux)

## Summary

Linux UI automation uses two cooperating components:

1. **In-app bridge** (env-gated, localhost-only, token-protected)
2. **External runner** (`ui-tests/linux-ui-test-runner.py`) for OS session lifecycle and input

This model validates real playback under `Xvfb` + `Openbox`. It does not rely on `OKILTV_HEADLESS_TEST`.

## Environment Contract

Required when enabling the bridge:

- `OKILTV_UI_TEST=1`
- `OKILTV_UI_TEST_TOKEN=<random>`
- `OKILTV_UI_TEST_PORT=<port>`
- `OKILTV_UI_TEST_RUN_DIR=/tmp/okiltv-ui-...`
- `OKILTV_DEBUG_FILE=$RUN_DIR/app.log`
- `XDG_DATA_HOME=$RUN_DIR/appdata`
- `XDG_CONFIG_HOME=$RUN_DIR/appdata`
- `XDG_CACHE_HOME=$RUN_DIR/appdata/.cache`

## HTTP API

Bridge binds to `127.0.0.1` only.

Token auth:

- `Authorization: Bearer <token>`
- or `?token=<token>`

Endpoints:

- `GET /health`
- `GET /state`
- `GET /regions`
- `GET /elements`
- `GET /logs?cursor=<n>`
- `GET /events` (SSE, supports `cursor`)
- `POST /capture` (JSON body: `label`, optional `subdir`)

## Snapshot Shape

`GET /state` returns:

- `window`: dimensions, X11 window id, overlay/layout/focus state
- `regions`: named regions with absolute + normalized geometry
- `elements`: semantic UI elements/state
- `playback`: current channel/playback/timeshift/debug overlay snapshot
- `inventory`: visible text-bearing controls/labels
- `networkMap`: layered network observations

Core region names:

- `main_window`
- `video_canvas`
- `left_pane`
- `right_pane`
- `bottom_controls`
- `timeshift_timeline`
- `guide_overlay`
- `guide_grid`
- `settings_overlay`
- `settings_rail`
- `settings_content`
- `debug_bubble`

## Event Stream

SSE event types:

- `log`
- `input.key`
- `input.mouse`
- `network.request`
- `network.reply`
- `state.changed`
- `capture.requested`
- `capture.saved`
- `capture.failed`

`state.changed` events are lightweight metadata records in `events.ndjson` (`snapshotDir`, `snapshotFile`, `digest`). Full snapshot content is persisted in `state-snapshots/` and available via `GET /state`.

## Logging + Network

- `DebugLogger` exposes cursor-based backlog + subscription for bridge log streaming.
- `NetworkAccess` emits request/reply observations (URL, category, status, duration, payload size, error).
- `PlayerController` emits playback URL changes.
- `TimeshiftController` emits local playback URL mapping + localhost HTTP request observations.

## Redaction Rules

Bridge artifacts/events redact by default:

- Xtream `username/password` query fields
- Xtream `/live/<user>/<pass>/...` path segments
- text/url patterns containing `user/pass/username/password`

Do not rely on raw credentials in bridge outputs.

## Artifact Layout

Under one run directory (for example `/tmp/okiltv-ui-live-playback-smoke-<utc>/`):

- `app.log`
- `events.ndjson`
- `network.ndjson`
- `state-snapshots/`
- `screenshots/`
- `appdata/`
- `xvfb.log`
- `openbox.log`
- `summary.json`
- `runner.log`
- `runner-events.ndjson`

## Runner Responsibilities

`linux-ui-test-runner.py` owns:

- launching inside `dbus-run-session`
- starting/stopping `Xvfb` + `Openbox`
- isolated appdata bootstrap supplied by each local-media scenario (`Runner.seed_settings`)
- disposable Secret Service keyring and D-Bus session supplied by `scripts/ci/run_ui_test.sh`
- launching app with bridge env vars
- selecting largest `OKILTV` window
- input injection via `xdotool`
- consuming SSE stream for live event monitoring
- writing `summary.json`
- optional EPG readiness gating (`epg_entries` and/or `epg/*.cache` size)
- optional guide probe flow (open Guide, delayed capture)
