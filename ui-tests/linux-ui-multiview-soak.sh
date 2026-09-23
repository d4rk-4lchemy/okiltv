#!/usr/bin/env bash
set -u
set -o pipefail

# Headless Linux UI smoke test for multiview/PiP.
# Artifacts are always written to /tmp.
#
# Usage:
#   ui-tests/linux-ui-multiview-soak.sh [primary] [pip] [grid3] [grid4] [settle_sec] [hold_sec] [hold_shot_interval_sec]
# Example:
#   ui-tests/linux-ui-multiview-soak.sh 1 2 3 4 4 60 10
#
# Optional environment:
#   OKILTV_SOAK_OVERLAY_MODE=clean|visible (default: clean)
#   OKILTV_SOAK_FINAL_DUAL_CAPTURE=0|1 (default: 0)

PRIMARY_CHANNEL="${1:-1}"
PIP_CHANNEL="${2:-2}"
GRID_CHANNEL_3="${3:-3}"
GRID_CHANNEL_4="${4:-4}"
SETTLE_SEC="${5:-4}"
HOLD_SEC="${6:-60}"
HOLD_SHOT_INTERVAL_SEC="${7:-10}"
CAPTURE_OVERLAY_MODE="${OKILTV_SOAK_OVERLAY_MODE:-clean}"
FINAL_DUAL_CAPTURE="${OKILTV_SOAK_FINAL_DUAL_CAPTURE:-0}"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
APP_BIN="${REPO_ROOT}/qt/out/build/qt-linux-release/app/OKILTV"

RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)"
RUN_DIR="/tmp/okiltv_multiview_test_${RUN_ID}"
SHOT_DIR="${RUN_DIR}/screenshots"
mkdir -p "$SHOT_DIR"

RUN_LOG="${RUN_DIR}/runner.log"
ACTION_LOG="${RUN_DIR}/actions.log"
APP_LOG="${RUN_DIR}/app.log"
EVENT_LOG="${RUN_DIR}/events.log"
SUMMARY_TXT="${RUN_DIR}/summary.txt"
CAPTURE_REQUEST_FILE="${RUN_DIR}/capture-request.txt"

is_non_negative_int() {
  [[ "$1" =~ ^[0-9]+$ ]]
}

if ! is_non_negative_int "$SETTLE_SEC"; then
  echo "ERROR: settle_sec must be a non-negative integer (got: ${SETTLE_SEC})" >&2
  exit 1
fi

if ! is_non_negative_int "$HOLD_SEC"; then
  echo "ERROR: hold_sec must be a non-negative integer (got: ${HOLD_SEC})" >&2
  exit 1
fi

if ! is_non_negative_int "$HOLD_SHOT_INTERVAL_SEC"; then
  echo "ERROR: hold_shot_interval_sec must be a non-negative integer (got: ${HOLD_SHOT_INTERVAL_SEC})" >&2
  exit 1
fi

if (( HOLD_SEC < 60 )); then
  HOLD_SEC=60
fi

if (( HOLD_SHOT_INTERVAL_SEC != 10 )); then
  HOLD_SHOT_INTERVAL_SEC=10
fi

if [[ "$CAPTURE_OVERLAY_MODE" != "clean" && "$CAPTURE_OVERLAY_MODE" != "visible" ]]; then
  echo "ERROR: OKILTV_SOAK_OVERLAY_MODE must be 'clean' or 'visible' (got: ${CAPTURE_OVERLAY_MODE})" >&2
  exit 1
fi

if [[ "$FINAL_DUAL_CAPTURE" != "0" && "$FINAL_DUAL_CAPTURE" != "1" ]]; then
  echo "ERROR: OKILTV_SOAK_FINAL_DUAL_CAPTURE must be '0' or '1' (got: ${FINAL_DUAL_CAPTURE})" >&2
  exit 1
fi

ts() {
  date -u +%FT%T.%3NZ
}

log() {
  printf '[%s] %s\n' "$(ts)" "$*" | tee -a "$RUN_LOG" >/dev/null
}

log_action() {
  printf '[%s] %s\n' "$(ts)" "$*" >>"$ACTION_LOG"
}

log_event() {
  local level="$1"
  shift
  printf '[%s] %s %s\n' "$(ts)" "$level" "$*" >>"$EVENT_LOG"
}

cleanup() {
  log "cleanup: stopping background processes"
  [[ -n "${APP_PID:-}" ]] && kill "$APP_PID" 2>/dev/null || true
  [[ -n "${OB_PID:-}" ]] && kill "$OB_PID" 2>/dev/null || true
  [[ -n "${XVFB_PID:-}" ]] && kill "$XVFB_PID" 2>/dev/null || true
  [[ -n "${APP_PID:-}" ]] && wait "$APP_PID" 2>/dev/null || true
  [[ -n "${OB_PID:-}" ]] && wait "$OB_PID" 2>/dev/null || true
  [[ -n "${XVFB_PID:-}" ]] && wait "$XVFB_PID" 2>/dev/null || true
}
trap cleanup EXIT

if [[ ! -x "$APP_BIN" ]]; then
  echo "ERROR: app binary not found: $APP_BIN" >&2
  echo "Build first: cmake --fresh --preset qt-linux-release && cmake --build --preset qt-linux-release -j\"$(scripts/build_jobs.sh)\"" >&2
  exit 1
fi

DISPLAY_NUM=""
for d in $(seq 99 130); do
  if ! xdpyinfo -display ":${d}" >/dev/null 2>&1; then
    DISPLAY_NUM="$d"
    break
  fi
done
if [[ -z "$DISPLAY_NUM" ]]; then
  echo "ERROR: failed to find free X display" >&2
  exit 1
fi
export DISPLAY=":${DISPLAY_NUM}"

log "run_dir=$RUN_DIR"
log "channels primary=${PRIMARY_CHANNEL} pip=${PIP_CHANNEL} grid3=${GRID_CHANNEL_3} grid4=${GRID_CHANNEL_4} settle=${SETTLE_SEC}s hold=${HOLD_SEC}s hold_shot_interval=${HOLD_SHOT_INTERVAL_SEC}s overlay_mode=${CAPTURE_OVERLAY_MODE} final_dual_capture=${FINAL_DUAL_CAPTURE}"
echo "RUN_DIR=$RUN_DIR"

log_action "start_xvfb display=$DISPLAY"
Xvfb "$DISPLAY" -screen 0 1920x1080x24 -ac +extension GLX +render -noreset >"${RUN_DIR}/xvfb.log" 2>&1 &
XVFB_PID=$!
sleep 1

log_action "start_openbox"
openbox >"${RUN_DIR}/openbox.log" 2>&1 &
OB_PID=$!
sleep 1

log_action "start_app binary=$APP_BIN"
export OKILTV_DEBUG_STDERR=1
export OKILTV_HEADLESS_TEST=1
export OKILTV_UI_TEST_CAPTURE_REQUEST_FILE="$CAPTURE_REQUEST_FILE"
"$APP_BIN" >"$APP_LOG" 2>&1 &
APP_PID=$!

wid=""
best=0
for _ in $(seq 1 120); do
  while read -r id; do
    [[ -z "$id" ]] && continue
    eval "$(xdotool getwindowgeometry --shell "$id" 2>/dev/null || true)"
    area=$(( ${WIDTH:-0} * ${HEIGHT:-0} ))
    if (( area > best )); then
      best=$area
      wid=$id
    fi
  done < <(xdotool search --name OKILTV 2>/dev/null || true)
  [[ -n "$wid" && $best -ge 1000000 ]] && break
  sleep 0.5
done

if [[ -z "$wid" ]]; then
  log_event ERROR "main window not found"
  log "ERROR: unable to find main OKILTV window"
  exit 1
fi

log "main_window_id=$wid area=$best"
log_action "window_found id=$wid area=$best"
xdotool windowactivate "$wid"
xdotool mousemove --window "$wid" 20 20
sleep 0.2
xdotool mousemove --window "$wid" 340 240
sleep 0.8

send_keys() {
  local sequence="$1"
  local pause="${2:-0.5}"
  log_action "key $sequence"
  xdotool key --window "$wid" "$sequence"
  sleep "$pause"
}

tune_numeric() {
  local channel_number="$1"
  log_action "tune_numeric channel=$channel_number"
  for (( i=0; i<${#channel_number}; i++ )); do
    digit="${channel_number:$i:1}"
    xdotool key --window "$wid" "$digit"
    sleep 0.2
  done
  sleep "$SETTLE_SEC"
}

capture_snapshot() {
  local label="$1"
  local shot="${SHOT_DIR}/${label}.png"
  rm -f "$shot" "$CAPTURE_REQUEST_FILE" "${CAPTURE_REQUEST_FILE}.tmp"
  printf '%s\n' "$shot" > "${CAPTURE_REQUEST_FILE}.tmp"
  mv "${CAPTURE_REQUEST_FILE}.tmp" "$CAPTURE_REQUEST_FILE"

  png_complete() {
    local png_path="$1"
    python3 - "$png_path" <<'PY'
import struct
import sys

path = sys.argv[1]
try:
    with open(path, "rb") as handle:
        data = handle.read()
except OSError:
    raise SystemExit(1)

if len(data) < 8 or data[:8] != b"\x89PNG\r\n\x1a\n":
    raise SystemExit(1)

offset = 8
found_iend = False
while offset + 8 <= len(data):
    chunk_len = int.from_bytes(data[offset:offset + 4], "big")
    chunk_type = data[offset + 4:offset + 8]
    chunk_end = offset + 8 + chunk_len + 4
    if chunk_end > len(data):
        raise SystemExit(1)
    offset = chunk_end
    if chunk_type == b"IEND":
        found_iend = True
        break

if not found_iend or offset != len(data):
    raise SystemExit(1)
PY
  }

  local saved=0
  for _ in $(seq 1 120); do
    if [[ -s "$shot" ]] && png_complete "$shot"; then
      saved=1
      break
    fi
    sleep 0.1
  done

  if (( saved == 0 )); then
    log_event WARN "app_capture_timeout_or_incomplete_png label=${label}; falling back to scrot"
    scrot "$shot" >/dev/null 2>&1 || true
  fi

  if [[ -s "$shot" ]]; then
    log_event SNAPSHOT "$shot"
  else
    log_event WARN "screenshot_failed label=${label}"
  fi
}

prepare_clean_capture() {
  log_action "prepare_clean_capture"
  xdotool mousemove --window "$wid" 960 540
  sleep 0.2
  xdotool key --window "$wid" Escape
  sleep 0.8
}

prepare_overlay_visible_capture() {
  log_action "prepare_overlay_visible_capture"
  xdotool mousemove --window "$wid" 20 20
  sleep 0.15
  xdotool mousemove --window "$wid" 340 240
  sleep 0.15
  # Two successive keybinds keep overlays active right before capture.
  xdotool key --window "$wid" Tab
  sleep 0.15
  xdotool key --window "$wid" Tab
  sleep 0.35
}

prepare_capture() {
  if [[ "$CAPTURE_OVERLAY_MODE" == "visible" ]]; then
    prepare_overlay_visible_capture
  else
    prepare_clean_capture
  fi
}

record_multiview_events() {
  rg -n '\[multiview\.' "$APP_LOG" \
    | while IFS= read -r line; do
        log_event APP "$line"
      done || true
}

tune_numeric "$PRIMARY_CHANNEL"
prepare_capture
capture_snapshot "01_primary_channel_${PRIMARY_CHANNEL}"

# Provide a distinct selected channel so PiP opens with an assigned secondary,
# rather than entering the pending empty-PiP channel picker.
prepare_clean_capture
send_keys Left 0.7
send_keys Down 0.7
send_keys ctrl+p 1.0
# PiP selection uses the mouse; Ctrl+arrows is reserved for grids.
eval "$(xdotool getwindowgeometry --shell "$wid")"
xdotool mousemove --window "$wid" "$((WIDTH * 85 / 100))" "$((HEIGHT * 85 / 100))"
sleep 0.3
xdotool key --window "$wid" Escape
sleep 0.5
xdotool click 1
sleep 0.7
tune_numeric "$PIP_CHANNEL"
prepare_capture
capture_snapshot "02_pip_primary_${PRIMARY_CHANNEL}_secondary_${PIP_CHANNEL}"

send_keys ctrl+o 1.0
xdotool keydown --window "$wid" Control_L
send_keys Down 0.4
xdotool keyup --window "$wid" Control_L
sleep 0.7
tune_numeric "$GRID_CHANNEL_3"
prepare_capture
capture_snapshot "03_grid_three_tiles_${PRIMARY_CHANNEL}_${PIP_CHANNEL}_${GRID_CHANNEL_3}"

xdotool keydown --window "$wid" Control_L
send_keys Left 0.4
xdotool keyup --window "$wid" Control_L
sleep 0.7
tune_numeric "$GRID_CHANNEL_4"
prepare_capture
capture_snapshot "04_grid_four_tiles_${PRIMARY_CHANNEL}_${PIP_CHANNEL}_${GRID_CHANNEL_3}_${GRID_CHANNEL_4}"

xdotool keydown --window "$wid" Control_L
send_keys Right 0.4
xdotool keyup --window "$wid" Control_L
sleep 0.7
prepare_capture
capture_snapshot "05_grid_focus_rotated"

if (( HOLD_SEC > 0 )); then
  log_action "hold_after_setup seconds=${HOLD_SEC}"
  if (( HOLD_SHOT_INTERVAL_SEC > 0 )); then
    elapsed_hold=0
    while (( elapsed_hold < HOLD_SEC )); do
      remaining=$((HOLD_SEC - elapsed_hold))
      step=$HOLD_SHOT_INTERVAL_SEC
      if (( step > remaining )); then
        step=$remaining
      fi
      sleep "$step"
      elapsed_hold=$((elapsed_hold + step))
      if kill -0 "$APP_PID" 2>/dev/null; then
        prepare_capture
        capture_snapshot "$(printf '06_hold_%04ds' "$elapsed_hold")"
      else
        log_event ERROR "app_process_exited_during_hold seconds=${elapsed_hold}"
        break
      fi
    done
  else
    sleep "$HOLD_SEC"
    if kill -0 "$APP_PID" 2>/dev/null; then
      prepare_capture
      capture_snapshot "06_after_hold_${HOLD_SEC}s"
    else
      log_event ERROR "app_process_exited_during_hold seconds=${HOLD_SEC}"
    fi
  fi
fi

if [[ "$FINAL_DUAL_CAPTURE" == "1" ]]; then
  if kill -0 "$APP_PID" 2>/dev/null; then
    prepare_clean_capture
    capture_snapshot "07_final_clean"
    prepare_overlay_visible_capture
    capture_snapshot "08_final_overlay"
  else
    log_event ERROR "app_process_exited_before_final_dual_capture"
  fi
fi

record_multiview_events

snapshot_count=$(find "$SHOT_DIR" -maxdepth 1 -type f -name '*.png' | wc -l | awk '{print $1}')
multiview_events=$(grep -ci '\[multiview\.' "$EVENT_LOG" 2>/dev/null || true)

{
  echo "run_dir=$RUN_DIR"
  echo "primary_channel=$PRIMARY_CHANNEL"
  echo "pip_channel=$PIP_CHANNEL"
  echo "grid_channel_3=$GRID_CHANNEL_3"
  echo "grid_channel_4=$GRID_CHANNEL_4"
  echo "settle_seconds=$SETTLE_SEC"
  echo "hold_seconds=$HOLD_SEC"
  echo "hold_shot_interval_seconds=$HOLD_SHOT_INTERVAL_SEC"
  echo "capture_overlay_mode=$CAPTURE_OVERLAY_MODE"
  echo "final_dual_capture=$FINAL_DUAL_CAPTURE"
  echo "snapshot_count=$snapshot_count"
  echo "multiview_event_count=$multiview_events"
  echo "artifact_files="
  echo "$ACTION_LOG"
  echo "$APP_LOG"
  echo "$EVENT_LOG"
  echo "$SUMMARY_TXT"
  echo "screenshot_paths="
  find "$SHOT_DIR" -maxdepth 1 -type f -name '*.png' | sort
} >"$SUMMARY_TXT"

log "completed snapshot_count=${snapshot_count}"
log "summary=$SUMMARY_TXT"

echo "$RUN_DIR"
