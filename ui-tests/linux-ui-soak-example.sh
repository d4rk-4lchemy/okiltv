#!/usr/bin/env bash
set -u
set -o pipefail

# Log-centric headless Linux UI soak test.
# Artifacts are always written to /tmp.
#
# Usage:
#   ui-tests/linux-ui-soak-example.sh [channel_number] [duration_sec] [screenshot_interval_sec] [sample_interval_sec]
# Example:
#   ui-tests/linux-ui-soak-example.sh 1 300 60 10

CHANNEL_NUM="${1:-1}"
DURATION_SEC="${2:-300}"
SNAP_INTERVAL="${3:-60}"
SAMPLE_INTERVAL="${4:-10}"

# Detection thresholds (log-centric anomaly detector)
BLACK_YAVG_THRESHOLD="22.0"
FREEZE_CONSEC_THRESHOLD=2
BLACK_CONSEC_THRESHOLD=2
NOCONN_CONSEC_THRESHOLD=2

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
APP_BIN="${REPO_ROOT}/qt/out/build/qt-linux-release/app/OKILTV"

RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)"
RUN_DIR="/tmp/okiltv_ch${CHANNEL_NUM}_test_${RUN_ID}"
SHOT_DIR="${RUN_DIR}/screenshots"
PROBE_DIR="${RUN_DIR}/probe-frames"
mkdir -p "$SHOT_DIR" "$PROBE_DIR"

RUN_LOG="${RUN_DIR}/runner.log"
ACTION_LOG="${RUN_DIR}/actions.log"
APP_LOG="${RUN_DIR}/app.log"
EVENT_LOG="${RUN_DIR}/events.log"
PROC_CSV="${RUN_DIR}/proc_metrics.csv"
PROBE_CSV="${RUN_DIR}/probe_metrics.csv"
SOCKET_LOG="${RUN_DIR}/socket_samples.log"
VMSTAT_LOG="${RUN_DIR}/vmstat.log"
SUMMARY_TXT="${RUN_DIR}/summary.txt"

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
  [[ -n "${VMSTAT_PID:-}" ]] && kill "$VMSTAT_PID" 2>/dev/null || true
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
log "channel=$CHANNEL_NUM duration=${DURATION_SEC}s screenshot_interval=${SNAP_INTERVAL}s sample_interval=${SAMPLE_INTERVAL}s"
log "thresholds: black_yavg<=${BLACK_YAVG_THRESHOLD}, freeze_consec>=${FREEZE_CONSEC_THRESHOLD}, black_consec>=${BLACK_CONSEC_THRESHOLD}, no_conn_consec>=${NOCONN_CONSEC_THRESHOLD}"
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
"$APP_BIN" >"$APP_LOG" 2>&1 &
APP_PID=$!

log_action "start_vmstat"
vmstat 1 >"$VMSTAT_LOG" 2>&1 &
VMSTAT_PID=$!

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

# Capture geometry for x11grab probe region.
eval "$(xdotool getwindowgeometry --shell "$wid" 2>/dev/null || true)"
WIN_X=${X:-0}
WIN_Y=${Y:-0}
WIN_W=${WIDTH:-1600}
WIN_H=${HEIGHT:-900}

# Probe center area (video-dominant region, avoids left/right panes and top bar).
PROBE_W=$(( WIN_W * 60 / 100 ))
PROBE_H=$(( WIN_H * 60 / 100 ))
PROBE_X=$(( WIN_X + WIN_W * 20 / 100 ))
PROBE_Y=$(( WIN_Y + WIN_H * 20 / 100 ))

if (( PROBE_W < 320 )); then PROBE_W=320; fi
if (( PROBE_H < 180 )); then PROBE_H=180; fi

log_action "probe_region x=$PROBE_X y=$PROBE_Y w=$PROBE_W h=$PROBE_H"

# Bring overlays and tune channel.
log_action "ui_prepare mouse_move"
xdotool mousemove --window "$wid" 20 20
sleep 0.2
xdotool mousemove --window "$wid" 340 240
sleep 0.5

log_action "tune_numeric channel=$CHANNEL_NUM"
for (( i=0; i<${#CHANNEL_NUM}; i++ )); do
  digit="${CHANNEL_NUM:$i:1}"
  xdotool key --window "$wid" "$digit"
  sleep 0.2
done
sleep 2.6

log_action "tune_confirm Return"
xdotool key --window "$wid" Return
sleep 0.8

log_action "toggle_debug_bubble F3"
xdotool key --window "$wid" F3
sleep 0.4

printf 'timestamp,elapsed_s,pid,cpu_percent,mem_percent,rss_kb,vsz_kb,state,etime,tcp_conn_count,tcp_rxq_sum,tcp_txq_sum\n' >"$PROC_CSV"
printf 'timestamp,elapsed_s,frame_hash,yavg,same_hash_count,low_luma_count,no_conn_count,probe_frame\n' >"$PROBE_CSV"

last_app_line=0
start_epoch=$(date +%s)
next_metric=0
next_snap=0

hard_fail=0
fail_reason=""
app_error_flag=0

last_frame_hash=""
same_hash_count=0
low_luma_count=0
no_conn_count=0

freeze_active=0
black_active=0
noconn_active=0

append_new_app_events() {
  local total
  total=$(wc -l <"$APP_LOG" 2>/dev/null || echo 0)
  (( total <= last_app_line )) && return

  local from=$((last_app_line + 1))
  local tmp_new
  tmp_new=$(mktemp)
  sed -n "${from},${total}p" "$APP_LOG" >"$tmp_new" 2>/dev/null || true

  grep -Ei 'Stream error|couldn.t be loaded|playback ended|buffering|reconnect|startup buffer fallback|stalled|loadfile|file-loaded|PLAYBACK_RESTART' "$tmp_new" \
    | while IFS= read -r line; do
        log_event APP "$line"
      done || true

  if grep -Eqi 'Stream error|couldn.t be loaded|mpv loadfile failed|mpv_initialize failed|mpv_render_context_create failed' "$tmp_new"; then
    app_error_flag=1
    log_event ERROR "app_error_detected"
  fi

  rm -f "$tmp_new"
  last_app_line=$total
}

sample_runtime() {
  local elapsed="$1"

  local ps_line conn_count rxq_sum txq_sum
  ps_line=$(ps -p "$APP_PID" -o %cpu=,%mem=,rss=,vsz=,stat=,etime= 2>/dev/null | awk '{$1=$1; print}')

  local sock_match
  sock_match=$(ss -Htnp 2>/dev/null | awk -v pid="$APP_PID" '$0 ~ ("pid=" pid ",") {print}')
  if [[ -n "$sock_match" ]]; then
    conn_count=$(printf '%s\n' "$sock_match" | awk 'NF {n++} END{print n+0}')
    rxq_sum=$(printf '%s\n' "$sock_match" | awk 'NF {rx += $2} END {print rx+0}')
    txq_sum=$(printf '%s\n' "$sock_match" | awk 'NF {tx += $3} END {print tx+0}')
  else
    conn_count=0
    rxq_sum=0
    txq_sum=0
  fi

  {
    echo "[$(ts)] elapsed=${elapsed}s pid=${APP_PID} conn_count=${conn_count} rxq_sum=${rxq_sum} txq_sum=${txq_sum}"
    if [[ -n "$sock_match" ]]; then
      echo "$sock_match"
    else
      echo "<no sockets for pid>"
    fi
    echo
  } >>"$SOCKET_LOG"

  if [[ -n "$ps_line" ]]; then
    local cpu mem rss vsz stat etime
    read -r cpu mem rss vsz stat etime <<<"$ps_line"
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
      "$(ts)" "$elapsed" "$APP_PID" "$cpu" "$mem" "$rss" "$vsz" "$stat" "$etime" "$conn_count" "$rxq_sum" "$txq_sum" >>"$PROC_CSV"
  fi

  local probe_frame frame_hash yavg
  probe_frame=$(printf '%s/probe_s%04d.png' "$PROBE_DIR" "$elapsed")
  if ffmpeg -v error -y -f x11grab -video_size "${PROBE_W}x${PROBE_H}" -i "${DISPLAY}+${PROBE_X},${PROBE_Y}" -frames:v 1 "$probe_frame"; then
    frame_hash=$(ffmpeg -v error -i "$probe_frame" -vf "format=rgb24" -f framemd5 - \
      | awk -F',' '$1 ~ /^[0-9]+$/ && NF >= 6 {gsub(/^ +| +$/, "", $6); print $6; exit}')
    yavg=$(ffmpeg -v error -i "$probe_frame" -vf "signalstats,metadata=print:file=-" -f null - 2>/dev/null \
      | rg -o 'YAVG=[0-9.]+' | head -n1 | cut -d= -f2)
  else
    frame_hash=""
    yavg=""
    log_event WARN "probe_capture_failed elapsed=${elapsed}s"
  fi

  if [[ -n "$frame_hash" && "$frame_hash" == "$last_frame_hash" ]]; then
    same_hash_count=$((same_hash_count + 1))
  else
    same_hash_count=0
  fi
  last_frame_hash="$frame_hash"

  if [[ -n "$yavg" ]] && awk "BEGIN {exit !($yavg <= $BLACK_YAVG_THRESHOLD)}"; then
    low_luma_count=$((low_luma_count + 1))
  else
    low_luma_count=0
  fi

  if (( conn_count == 0 )); then
    no_conn_count=$((no_conn_count + 1))
  else
    no_conn_count=0
  fi

  printf '%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$(ts)" "$elapsed" "${frame_hash:-}" "${yavg:-}" "$same_hash_count" "$low_luma_count" "$no_conn_count" "$probe_frame" >>"$PROBE_CSV"

  if (( same_hash_count >= FREEZE_CONSEC_THRESHOLD )); then
    if (( freeze_active == 0 )); then
      freeze_active=1
      log_event WARN "freeze-suspect same_hash_count=${same_hash_count} hash=${frame_hash:-none}"
    fi
  else
    if (( freeze_active == 1 )); then
      freeze_active=0
      log_event INFO "freeze-recovered"
    fi
  fi

  if (( low_luma_count >= BLACK_CONSEC_THRESHOLD )); then
    if (( black_active == 0 )); then
      black_active=1
      log_event WARN "black-video-suspect low_luma_count=${low_luma_count} yavg=${yavg:-n/a}"
    fi
  else
    if (( black_active == 1 )); then
      black_active=0
      log_event INFO "black-video-recovered"
    fi
  fi

  if (( no_conn_count >= NOCONN_CONSEC_THRESHOLD )); then
    if (( noconn_active == 0 )); then
      noconn_active=1
      log_event WARN "connection-drop-suspect no_conn_count=${no_conn_count}"
    fi
  else
    if (( noconn_active == 1 )); then
      noconn_active=0
      log_event INFO "connection-recovered"
    fi
  fi
}

capture_snapshot() {
  local elapsed="$1"
  local minute=$((elapsed / 60))
  local shot
  shot=$(printf '%s/okiltv_ch%s_ui_m%02d_s%04d.png' "$SHOT_DIR" "$CHANNEL_NUM" "$minute" "$elapsed")
  scrot "$shot" >/dev/null 2>&1 || true
  if [[ -f "$shot" ]]; then
    log_event SNAPSHOT "$shot"
  else
    log_event WARN "screenshot_failed elapsed=${elapsed}s"
  fi
}

while true; do
  now_epoch=$(date +%s)
  elapsed=$((now_epoch - start_epoch))

  if ! kill -0 "$APP_PID" 2>/dev/null; then
    hard_fail=1
    fail_reason="app process exited before test end"
    log_event ERROR "app_process_exited elapsed=${elapsed}s"
    break
  fi

  append_new_app_events

  if (( elapsed >= next_metric )); then
    sample_runtime "$elapsed"
    next_metric=$((next_metric + SAMPLE_INTERVAL))
  fi

  if (( elapsed >= next_snap )); then
    capture_snapshot "$elapsed"
    next_snap=$((next_snap + SNAP_INTERVAL))
  fi

  (( elapsed >= DURATION_SEC )) && break
  sleep 1
done

append_new_app_events
end_epoch=$(date +%s)
actual_elapsed=$((end_epoch - start_epoch))

event_warnings=$(grep -ci ' WARN ' "$EVENT_LOG" 2>/dev/null || true)
event_errors=$(grep -ci ' ERROR ' "$EVENT_LOG" 2>/dev/null || true)
freeze_suspects=$(grep -ci 'freeze-suspect' "$EVENT_LOG" 2>/dev/null || true)
black_suspects=$(grep -ci 'black-video-suspect' "$EVENT_LOG" 2>/dev/null || true)
conn_suspects=$(grep -ci 'connection-drop-suspect' "$EVENT_LOG" 2>/dev/null || true)
app_errors=$(grep -ci 'app_error_detected' "$EVENT_LOG" 2>/dev/null || true)
snap_count=$(find "$SHOT_DIR" -maxdepth 1 -type f -name "okiltv_ch${CHANNEL_NUM}_ui_m*.png" | wc -l | awk '{print $1}')

result="PASS"
if (( hard_fail == 1 || app_errors > 0 || freeze_suspects > 0 || black_suspects > 0 || conn_suspects > 0 )); then
  result="FAIL"
fi

if [[ -z "$fail_reason" && "$result" == "FAIL" ]]; then
  fail_reason="anomaly detected (see events.log)"
fi
if [[ -z "$fail_reason" ]]; then
  fail_reason="none"
fi

{
  echo "run_dir=$RUN_DIR"
  echo "channel=$CHANNEL_NUM"
  echo "duration_sec=$actual_elapsed"
  echo "sample_interval_sec=$SAMPLE_INTERVAL"
  echo "screenshot_interval_sec=$SNAP_INTERVAL"
  echo "result=$result"
  echo "fail_reason=$fail_reason"
  echo "event_warnings=$event_warnings"
  echo "event_errors=$event_errors"
  echo "freeze_suspects=$freeze_suspects"
  echo "black_video_suspects=$black_suspects"
  echo "connection_drop_suspects=$conn_suspects"
  echo "app_error_events=$app_errors"
  echo "snapshot_count=$snap_count"
  echo "artifact_files="
  echo "$ACTION_LOG"
  echo "$APP_LOG"
  echo "$EVENT_LOG"
  echo "$PROC_CSV"
  echo "$PROBE_CSV"
  echo "$SOCKET_LOG"
  echo "$VMSTAT_LOG"
  echo "$SUMMARY_TXT"
  echo "screenshot_paths="
  find "$SHOT_DIR" -maxdepth 1 -type f -name "okiltv_ch${CHANNEL_NUM}_ui_m*.png" | sort
} >"$SUMMARY_TXT"

log "completed duration=${actual_elapsed}s result=$result"
log "summary=$SUMMARY_TXT"

echo "$RUN_DIR"
