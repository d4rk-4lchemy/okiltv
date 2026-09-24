#!/usr/bin/env python3
"""Linux UI test runner for OKILTV semantic bridge tests.

This runner uses real playback on Xvfb/Openbox with the in-app UI test bridge.
It avoids OKILTV_HEADLESS_TEST and drives input via xdotool + semantic regions.
"""

from __future__ import annotations

import argparse
import datetime as dt
import http.client
import json
import os
import pathlib
import random
import re
import shutil
import sqlite3
import socket
import string
import subprocess
import sys
import threading
import time
import urllib.parse
from typing import Any


REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_APP = REPO_ROOT / "qt/out/build/qt-linux-release/app/OKILTV"


def utc_stamp() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def now_iso() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def parse_iso_timestamp(value: str) -> dt.datetime | None:
    text = str(value).strip()
    if not text:
        return None
    normalized = text.replace("Z", "+00:00")
    try:
        return dt.datetime.fromisoformat(normalized)
    except ValueError:
        pass
    for fmt in ("%Y%m%dT%H%M%S%fZ", "%Y%m%dT%H%M%SZ"):
        try:
            parsed = dt.datetime.strptime(text, fmt)
            return parsed.replace(tzinfo=dt.timezone.utc)
        except ValueError:
            continue
    return None


def redact_url(text: str) -> str:
    if not text:
        return text
    text = re.sub(r"(?i)\b(username|password|user|pass)=([^&\s]+)", r"\1=***", text)
    text = re.sub(r"/(live|movie|series)/[^/\s]+/[^/\s]+/", r"/\1/***/***/", text)
    return text


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def run_cmd(args: list[str], check: bool = True, capture: bool = True, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        args,
        check=check,
        text=True,
        capture_output=capture,
        env=env,
    )


class BridgeClient:
    def __init__(self, port: int, token: str) -> None:
        self.port = port
        self.token = token
        self.request_timeout_sec = 15
        self.max_retries = 4

    def _request(self, method: str, path: str, body: dict[str, Any] | None = None) -> tuple[int, str]:
        payload = None
        headers = {"Authorization": f"Bearer {self.token}"}
        if body is not None:
            payload = json.dumps(body)
            headers["Content-Type"] = "application/json"
        last_error: Exception | None = None
        for attempt in range(1, self.max_retries + 1):
            conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=self.request_timeout_sec)
            try:
                conn.request(method, path, body=payload, headers=headers)
                resp = conn.getresponse()
                text = resp.read().decode("utf-8", errors="replace")
                return resp.status, text
            except Exception as exc:  # noqa: BLE001
                last_error = exc
                if attempt >= self.max_retries:
                    break
                time.sleep(0.2 * attempt)
            finally:
                conn.close()
        raise RuntimeError(f"{method} {path} failed after {self.max_retries} retries: {last_error}")

    def get_json(self, path: str) -> dict[str, Any]:
        status, text = self._request("GET", path)
        if status < 200 or status >= 300:
            raise RuntimeError(f"GET {path} failed: {status} {text[:200]}")
        return json.loads(text) if text else {}

    def post_json(self, path: str, body: dict[str, Any]) -> dict[str, Any]:
        status, text = self._request("POST", path, body=body)
        if status < 200 or status >= 300:
            raise RuntimeError(f"POST {path} failed: {status} {text[:200]}")
        return json.loads(text) if text else {}


class SsePump(threading.Thread):
    def __init__(self, port: int, token: str, output_path: pathlib.Path, stop_event: threading.Event) -> None:
        super().__init__(daemon=True)
        self.port = port
        self.token = token
        self.output_path = output_path
        self.stop_event = stop_event
        self.last_event_id = 0
        self.errors: list[str] = []

    def run(self) -> None:
        while not self.stop_event.is_set():
            try:
                self._run_once()
            except Exception as exc:  # noqa: BLE001
                self.errors.append(str(exc))
                time.sleep(0.5)

    def _run_once(self) -> None:
        query = urllib.parse.urlencode({"token": self.token, "cursor": self.last_event_id})
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        conn.request("GET", f"/events?{query}")
        resp = conn.getresponse()
        if resp.status != 200:
            raise RuntimeError(f"SSE status {resp.status}")
        event_type = ""
        event_id = 0
        data_chunks: list[str] = []
        with self.output_path.open("a", encoding="utf-8") as out:
            while not self.stop_event.is_set():
                raw = resp.fp.readline()
                if not raw:
                    break
                line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                if not line:
                    if data_chunks:
                        payload_text = "\n".join(data_chunks)
                        payload = {}
                        if payload_text.strip():
                            try:
                                payload = json.loads(payload_text)
                            except json.JSONDecodeError:
                                payload = {"raw": payload_text}
                        record = {
                            "id": event_id,
                            "type": event_type or "message",
                            "timestamp": now_iso(),
                            "payload": payload,
                        }
                        out.write(json.dumps(record, ensure_ascii=True) + "\n")
                        out.flush()
                        if event_id > 0:
                            self.last_event_id = event_id
                    event_type = ""
                    event_id = 0
                    data_chunks = []
                    continue
                if line.startswith("id:"):
                    try:
                        event_id = int(line[3:].strip())
                    except ValueError:
                        event_id = 0
                elif line.startswith("event:"):
                    event_type = line[6:].strip()
                elif line.startswith("data:"):
                    data_chunks.append(line[5:].lstrip())
        conn.close()


class Runner:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.run_dir = pathlib.Path(args.run_dir) if args.run_dir else pathlib.Path(f"/tmp/okiltv-ui-{args.test_name}-{utc_stamp()}")
        self.appdata_dir = self.run_dir / "appdata"
        self.settings_dirs = [
            self.appdata_dir / "OKILTV",
            self.appdata_dir / "OKILTV" / "OKILTV",
            self.appdata_dir / "OKILTV" / "OKILTV" / "OKILTV",
        ]
        self.settings_path_candidates = [path / "settings.json" for path in self.settings_dirs]
        self.screenshots_dir = self.run_dir / "screenshots"
        self.state_dir = self.run_dir / "state-snapshots"
        self.summary_path = self.run_dir / "summary.json"
        self.runner_log = self.run_dir / "runner.log"
        self.bridge_events_path = self.run_dir / "runner-events.ndjson"
        self.display = ""
        self.window_id = ""
        self.token = "".join(random.choice(string.ascii_letters + string.digits) for _ in range(32))
        self.bridge_port = find_free_port()
        self.client = BridgeClient(self.bridge_port, self.token)
        self.procs: list[subprocess.Popen[str]] = []
        self.stop_event = threading.Event()
        self.sse_pump: SsePump | None = None

    def log(self, message: str) -> None:
        line = f"[{now_iso()}] {message}"
        with self.runner_log.open("a", encoding="utf-8") as out:
            out.write(line + "\n")
        print(line)

    def read_state(self, timeout_sec: float = 30.0, retry_interval_sec: float = 0.35) -> dict[str, Any]:
        deadline = time.time() + max(0.5, timeout_sec)
        last_error: Exception | None = None
        while time.time() < deadline:
            try:
                return self.client.get_json("/state")
            except Exception as exc:  # noqa: BLE001
                last_error = exc
                time.sleep(max(0.05, retry_interval_sec))
        raise RuntimeError(f"Timed out reading /state: {last_error}")

    def prepare_dirs(self) -> None:
        self.run_dir.mkdir(parents=True, exist_ok=True)
        for settings_dir in self.settings_dirs:
            settings_dir.mkdir(parents=True, exist_ok=True)
        self.screenshots_dir.mkdir(parents=True, exist_ok=True)
        self.state_dir.mkdir(parents=True, exist_ok=True)

    def seed_settings(self) -> None:
        raise NotImplementedError("Use a local fixture scenario from ui-tests/tests/*.py")

    def scrub_settings_credentials(self) -> None:
        for settings_path in self.settings_path_candidates:
            if not settings_path.exists():
                continue
            try:
                settings = json.loads(settings_path.read_text(encoding="utf-8"))
            except Exception:  # noqa: BLE001
                continue
            changed = False
            for profile in settings.get("profiles", []):
                if "xtreamUsername" in profile:
                    profile["xtreamUsername"] = "***"
                    changed = True
                if "xtreamPassword" in profile:
                    profile["xtreamPassword"] = "***"
                    changed = True
            if changed:
                settings_path.write_text(json.dumps(settings, indent=2), encoding="utf-8")

    def reserve_display(self) -> str:
        for display_num in range(99, 131):
            probe = run_cmd(["xdpyinfo", "-display", f":{display_num}"], check=False)
            if probe.returncode != 0:
                return f":{display_num}"
        raise RuntimeError("No free X display found in :99-:130")

    def start_process(self, args: list[str], env: dict[str, str], stdout_path: pathlib.Path) -> subprocess.Popen[str]:
        out_handle = stdout_path.open("w", encoding="utf-8")
        proc = subprocess.Popen(
            args,
            stdout=out_handle,
            stderr=subprocess.STDOUT,
            text=True,
            env=env,
        )
        self.procs.append(proc)
        return proc

    def launch_stack(self) -> None:
        self.display = self.reserve_display()
        base_env = os.environ.copy()
        base_env["DISPLAY"] = self.display
        self.start_process(
            ["Xvfb", self.display, "-screen", "0", "1920x1080x24", "-ac", "+extension", "GLX", "+render", "-noreset"],
            base_env,
            self.run_dir / "xvfb.log",
        )
        time.sleep(0.8)
        self.start_process(["openbox"], base_env, self.run_dir / "openbox.log")
        time.sleep(0.8)

        app_env = base_env.copy()
        app_env["XDG_DATA_HOME"] = str(self.appdata_dir)
        app_env["XDG_CONFIG_HOME"] = str(self.appdata_dir)
        app_env["XDG_CACHE_HOME"] = str(self.appdata_dir / ".cache")
        app_env["OKILTV_UI_TEST"] = "1"
        app_env["OKILTV_UI_TEST_TOKEN"] = self.token
        app_env["OKILTV_UI_TEST_PORT"] = str(self.bridge_port)
        app_env["OKILTV_UI_TEST_RUN_DIR"] = str(self.run_dir)
        app_env["OKILTV_DEBUG_FILE"] = str(self.run_dir / "app.log")
        if self.args.qt_debug_logs:
            app_env["QT_LOGGING_RULES"] = "*.debug=true"
        self.start_process([str(self.args.app_bin)], app_env, self.run_dir / "app.stdout.log")

    def display_env(self) -> dict[str, str]:
        env = os.environ.copy()
        if self.display:
            env["DISPLAY"] = self.display
        return env

    def wait_for_bridge(self, timeout_sec: float = 60.0) -> None:
        deadline = time.time() + timeout_sec
        while time.time() < deadline:
            try:
                health = self.client.get_json("/health")
                if health.get("ok"):
                    self.log(f"Bridge healthy on 127.0.0.1:{self.bridge_port}")
                    return
            except Exception:
                pass
            time.sleep(0.3)
        raise RuntimeError("Timed out waiting for UI test bridge /health")

    def start_sse(self) -> None:
        self.sse_pump = SsePump(self.bridge_port, self.token, self.bridge_events_path, self.stop_event)
        self.sse_pump.start()

    def find_largest_window(self, timeout_sec: float = 60.0) -> str:
        deadline = time.time() + timeout_sec
        while time.time() < deadline:
            search = run_cmd(["xdotool", "search", "--name", "OKILTV"], check=False, env=self.display_env())
            ids = [line.strip() for line in search.stdout.splitlines() if line.strip()]
            best_id = ""
            best_area = 0
            for wid in ids:
                info = run_cmd(["xwininfo", "-id", wid], check=False, env=self.display_env())
                width_match = re.search(r"Width:\s+(\d+)", info.stdout)
                height_match = re.search(r"Height:\s+(\d+)", info.stdout)
                if not width_match or not height_match:
                    continue
                area = int(width_match.group(1)) * int(height_match.group(1))
                if area > best_area:
                    best_area = area
                    best_id = wid
            if best_id:
                self.log(f"Selected largest window id={best_id} area={best_area}")
                return best_id
            time.sleep(0.3)
        raise RuntimeError("Timed out waiting for OKILTV window")

    def xdotool(self, *args: str) -> None:
        cmd = ["xdotool", *args]
        run_cmd(cmd, check=True, capture=False, env=self.display_env())

    def tune_channel(self, channel: int) -> None:
        self.log(f"Tuning channel {channel}")
        self.xdotool("windowactivate", self.window_id)
        time.sleep(0.2)
        for digit in str(channel):
            self.xdotool("key", "--window", self.window_id, digit)
            time.sleep(0.15)
        self.xdotool("key", "--window", self.window_id, "Return")

    def wait_for_playback_ready(self, timeout_sec: float = 60.0) -> dict[str, Any]:
        deadline = time.time() + timeout_sec
        last_state: dict[str, Any] = {}
        while time.time() < deadline:
            try:
                state = self.read_state(timeout_sec=6.0, retry_interval_sec=0.25)
            except Exception:  # noqa: BLE001
                time.sleep(0.5)
                continue
            last_state = state
            playback = state.get("playback", {})
            channel = playback.get("currentChannel", {})
            is_playing = False
            for element in state.get("elements", []):
                if element.get("id") == "bottom.transport":
                    is_playing = bool((element.get("value") or {}).get("isPlaying"))
                    break
            if channel.get("id") is not None and is_playing:
                return state
            time.sleep(0.5)
        raise RuntimeError(
            "Timed out waiting for playback ready"
            f" after {timeout_sec:.1f}s; last channel id={last_state.get('playback', {}).get('currentChannel', {}).get('id')!r},"
            f" transport={self.transport_value(last_state) if last_state else {}}"
        )

    def save_state_snapshot(self, label: str, state: dict[str, Any]) -> pathlib.Path:
        path = self.state_dir / f"{label}.json"
        path.write_text(json.dumps(state, indent=2), encoding="utf-8")
        return path

    def request_capture(self, label: str, subdir: str = "") -> dict[str, Any]:
        return self.client.post_json("/capture", {"label": label, "subdir": subdir})

    def request_capture_wait(self, label: str, subdir: str = "", timeout_sec: float = 12.0) -> pathlib.Path | None:
        response = self.request_capture(label, subdir)
        output_path = str(response.get("outputPath", "")).strip()
        self.log(f"Capture queued: {output_path}")
        if not output_path:
            return None
        target = pathlib.Path(output_path)
        deadline = time.time() + timeout_sec
        while time.time() < deadline:
            try:
                if target.exists() and target.stat().st_size > 0:
                    return target
            except OSError:
                pass
            time.sleep(0.2)
        return target

    def compare_capture_ssim(self, left: pathlib.Path | None, right: pathlib.Path | None) -> dict[str, Any]:
        result: dict[str, Any] = {
            "left": str(left) if left else "",
            "right": str(right) if right else "",
            "crop": "center-55%x55%",
            "changed": False,
        }
        if left is None or right is None:
            result["error"] = "missing capture path"
            return result
        if not left.exists() or not right.exists():
            result["error"] = "capture file missing"
            return result

        crop_expr = "crop=iw*0.55:ih*0.55:iw*0.225:ih*0.18"
        cmd = [
            "ffmpeg",
            "-hide_banner",
            "-i",
            str(left),
            "-i",
            str(right),
            "-lavfi",
            f"[0:v]{crop_expr}[va];[1:v]{crop_expr}[vb];[va][vb]ssim",
            "-f",
            "null",
            "-",
        ]
        proc = run_cmd(cmd, check=False)
        output = "\n".join(part for part in (proc.stdout, proc.stderr) if part).strip()
        match = re.search(r"All:([0-9]*\.?[0-9]+)", output)
        if proc.returncode != 0:
            result["error"] = f"ffmpeg exited with code {proc.returncode}"
            if output:
                result["ffmpegTail"] = "\n".join(output.splitlines()[-5:])
            return result
        if not match:
            result["error"] = "missing SSIM output"
            if output:
                result["ffmpegTail"] = "\n".join(output.splitlines()[-5:])
            return result

        score = float(match.group(1))
        result["ssim"] = score
        result["changed"] = score <= float(self.args.timeshift_screenshot_change_max_ssim)
        return result

    @staticmethod
    def transport_value(state: dict[str, Any]) -> dict[str, Any]:
        for element in state.get("elements", []):
            if element.get("id") == "bottom.transport":
                return dict(element.get("value") or {})
        return {}

    @staticmethod
    def timeshift_value(state: dict[str, Any]) -> dict[str, Any]:
        playback = state.get("playback", {})
        return dict(playback.get("timeshift") or {})

    @staticmethod
    def playback_url(state: dict[str, Any]) -> str:
        playback = state.get("playback", {})
        return str(playback.get("playbackUrl") or "")

    @staticmethod
    def is_live_local_master_url(url: str) -> bool:
        normalized = str(url or "").strip()
        return (
            normalized.startswith("http://127.0.0.1:")
            and "/master.m3u8" in normalized
            and "pdt=" not in normalized
            and "target_pdt=" not in normalized
        )

    def timeline_note(self, state: dict[str, Any]) -> dict[str, Any]:
        ts = self.timeshift_value(state)
        return {
            "active": bool(ts.get("active")),
            "atLiveEdge": bool(ts.get("atLiveEdge")),
            "attachedWindowEndEpochMs": ts.get("attachedWindowEndEpochMs"),
            "attachedWindowStartEpochMs": ts.get("attachedWindowStartEpochMs"),
            "behindLiveSeconds": ts.get("behindLiveSeconds"),
            "liveEdgeEpochMs": ts.get("liveEdgeEpochMs"),
            "positionSeconds": ts.get("positionSeconds"),
            "availableSeconds": ts.get("availableSeconds"),
            "noticeText": ts.get("noticeText", ""),
            "windowStartEpochMs": ts.get("windowStartEpochMs"),
            "playbackUrl": self.playback_url(state),
        }

    def app_log_matches(self, patterns: list[str], max_lines: int = 80) -> list[str]:
        log_path = self.run_dir / "app.log"
        if not log_path.exists():
            return []
        regexes = [re.compile(pattern) for pattern in patterns]
        matches: list[str] = []
        try:
            with log_path.open("r", encoding="utf-8", errors="replace") as handle:
                for raw_line in handle:
                    line = raw_line.rstrip("\n")
                    if any(regex.search(line) for regex in regexes):
                        matches.append(line)
        except OSError:
            return []
        if len(matches) <= max_lines:
            return matches
        return matches[-max_lines:]

    def mpv_segment_requests(self) -> list[dict[str, Any]]:
        log_path = self.run_dir / "app.log"
        if not log_path.exists():
            return []
        pattern = re.compile(
            r"^\[(?P<timestamp>[^\]]+)\].*HLS request for url '(?P<url>[^']*?/index(?P<index>\d+)\.ts)'",
        )
        requests: list[dict[str, Any]] = []
        try:
            with log_path.open("r", encoding="utf-8", errors="replace") as handle:
                for raw_line in handle:
                    line = raw_line.rstrip("\n")
                    match = pattern.search(line)
                    if not match:
                        continue
                    timestamp = parse_iso_timestamp(match.group("timestamp"))
                    if timestamp is None:
                        continue
                    requests.append(
                        {
                            "timestamp": timestamp,
                            "timestampIso": timestamp.isoformat().replace("+00:00", "Z"),
                            "segmentIndex": int(match.group("index")),
                            "url": match.group("url"),
                            "line": line,
                        }
                    )
        except OSError:
            return []
        return requests

    def playback_server_segment_requests(self) -> list[dict[str, Any]]:
        path = self.run_dir / "network.ndjson"
        if not path.exists():
            return []
        pattern = re.compile(r"/index(?P<index>\d+)\.ts$")
        requests: list[dict[str, Any]] = []
        try:
            with path.open("r", encoding="utf-8", errors="replace") as handle:
                for raw_line in handle:
                    line = raw_line.strip()
                    if not line:
                        continue
                    try:
                        record = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    payload = record.get("payload") or {}
                    if payload.get("category") != "timeshift.local-http":
                        continue
                    target = str(payload.get("target", ""))
                    match = pattern.search(target)
                    if not match:
                        continue
                    timestamp = parse_iso_timestamp(str(record.get("timestamp", "")))
                    if timestamp is None:
                        continue
                    requests.append(
                        {
                            "timestamp": timestamp,
                            "timestampIso": timestamp.isoformat().replace("+00:00", "Z"),
                            "segmentIndex": int(match.group("index")),
                            "url": target,
                            "line": line,
                            "source": "network.ndjson",
                        }
                    )
        except OSError:
            return []
        return requests

    def segment_context_for_capture(
        self,
        capture_path: pathlib.Path | None,
        requests: list[dict[str, Any]],
        window_sec: float = 6.0,
    ) -> dict[str, Any]:
        result: dict[str, Any] = {
            "capturePath": str(capture_path) if capture_path else "",
            "captureTimestamp": "",
            "latestSegmentIndex": None,
            "recentSegmentIndices": [],
            "recentSegmentCount": 0,
            "segmentAdvancedInWindow": False,
        }
        if capture_path is None:
            result["error"] = "missing capture path"
            return result
        stamp = capture_path.name.split("-", 1)[0]
        capture_time = parse_iso_timestamp(stamp)
        if capture_time is None:
            result["error"] = f"unable to parse capture timestamp: {stamp}"
            return result
        result["captureTimestamp"] = capture_time.isoformat().replace("+00:00", "Z")

        latest_request: dict[str, Any] | None = None
        window_start = capture_time - dt.timedelta(seconds=max(0.0, window_sec))
        recent_indices: list[int] = []
        recent_lines: list[str] = []
        for entry in requests:
            timestamp = entry["timestamp"]
            if timestamp <= capture_time:
                latest_request = entry
            if window_start <= timestamp <= capture_time:
                recent_indices.append(int(entry["segmentIndex"]))
                recent_lines.append(entry["line"])
        if latest_request is not None:
            result["latestSegmentIndex"] = int(latest_request["segmentIndex"])
            result["latestSegmentUrl"] = latest_request["url"]
            result["latestSegmentTimestamp"] = latest_request["timestampIso"]
        if recent_indices:
            unique_recent = sorted(set(recent_indices))
            result["recentSegmentIndices"] = unique_recent
            result["recentSegmentCount"] = len(recent_indices)
            result["segmentAdvancedInWindow"] = len(unique_recent) >= 2 and unique_recent[-1] > unique_recent[0]
            result["recentRequestLines"] = recent_lines[-8:]
        return result

    @staticmethod
    def serializable_segment_request(entry: dict[str, Any]) -> dict[str, Any]:
        if not entry:
            return {}
        return {
            "timestampIso": entry.get("timestampIso", ""),
            "segmentIndex": entry.get("segmentIndex"),
            "url": entry.get("url", ""),
            "line": entry.get("line", ""),
        }

    def wait_for_transport_settle(
        self,
        timeout_sec: float = 40.0,
        min_wait_sec: float = 1.0,
        require_playing: bool = False,
    ) -> dict[str, Any]:
        start = time.time()
        deadline = start + timeout_sec
        last_state: dict[str, Any] = {}
        while time.time() < deadline:
            try:
                state = self.read_state(timeout_sec=6.0, retry_interval_sec=0.25)
            except Exception:  # noqa: BLE001
                time.sleep(0.4)
                continue
            last_state = state
            transport = self.transport_value(state)
            settled = not bool(transport.get("isLoading")) and not bool(transport.get("isBuffering"))
            playing_ok = bool(transport.get("isPlaying")) or not require_playing
            if (time.time() - start) >= min_wait_sec and settled and playing_ok:
                return state
            time.sleep(0.4)
        return last_state

    @staticmethod
    def _safe_float(value: Any) -> float | None:
        try:
            if value is None:
                return None
            return float(value)
        except (TypeError, ValueError):
            return None

    def _analyze_timeshift_cycle(self, notes: dict[str, dict[str, Any]], states: dict[str, dict[str, Any]]) -> dict[str, Any]:
        start_60 = notes.get("after_60s", {})
        after_rewind = notes.get("after_rewind_load", {})
        after_20 = notes.get("after_rewind_20s", {})
        after_home = notes.get("after_home_load", {})

        behind_60 = self._safe_float(start_60.get("behindLiveSeconds"))
        behind_rewind = self._safe_float(after_rewind.get("behindLiveSeconds"))
        behind_home = self._safe_float(after_home.get("behindLiveSeconds"))

        pos_rewind = self._safe_float(after_rewind.get("positionSeconds"))
        pos_20 = self._safe_float(after_20.get("positionSeconds"))

        transport_rewind = self.transport_value(states.get("after_rewind_load", {}))
        transport_home = self.transport_value(states.get("after_home_load", {}))

        moved_back = False
        if behind_60 is not None and behind_rewind is not None:
            moved_back = (behind_rewind - behind_60) >= max(8.0, float(self.args.timeshift_rewind_sec) * 0.5)

        progressed_after_rewind = False
        if pos_rewind is not None and pos_20 is not None:
            progressed_after_rewind = (pos_20 - pos_rewind) >= max(8.0, float(self.args.timeshift_post_rewind_sec) * 0.35)

        returned_live = bool(after_home.get("atLiveEdge"))
        if not returned_live and behind_home is not None:
            returned_live = behind_home <= float(self.args.timeshift_live_edge_max_behind_sec)

        stable_after_rewind = not bool(transport_rewind.get("isLoading")) and not bool(transport_rewind.get("isBuffering"))
        stable_after_home = not bool(transport_home.get("isLoading")) and not bool(transport_home.get("isBuffering"))

        checks = {
            "movedBackByRequestedAmount": moved_back,
            "playedForwardAfterRewind": progressed_after_rewind,
            "returnedToLiveEdge": returned_live,
            "stableAfterRewindLoad": stable_after_rewind,
            "stableAfterHomeLoad": stable_after_home,
        }
        return {
            "checks": checks,
            "passed": all(checks.values()),
            "metrics": {
                "behind_60s": behind_60,
                "behind_after_rewind_load": behind_rewind,
                "behind_after_home_load": behind_home,
                "position_after_rewind_load": pos_rewind,
                "position_after_rewind_20s": pos_20,
            },
            "transport": {
                "after_rewind_load": transport_rewind,
                "after_home_load": transport_home,
            },
            "notes": notes,
        }

    def run_timeshift_cycle_sequence(self, state_after_tune: dict[str, Any]) -> dict[str, Any]:
        if not bool(self.args.timeshift_enabled):
            raise RuntimeError("--timeshift-cycle-sequence requires --timeshift-enabled")

        self.log("Running timeshift cycle sequence")

        self.request_capture_wait("01-playback-start", "timeshift-cycle")
        self.save_state_snapshot("02-playback-start", state_after_tune)

        time.sleep(max(0.0, float(self.args.timeshift_observe_sec)))
        state_after_60 = self.read_state(timeout_sec=60.0)
        self.save_state_snapshot("03-after-observe", state_after_60)
        self.request_capture_wait("02-after-observe", "timeshift-cycle")

        rewind_steps = max(1, int(round(float(self.args.timeshift_rewind_sec) / 10.0)))
        for _ in range(rewind_steps):
            self.xdotool("key", "--window", self.window_id, "j")
            time.sleep(0.35)

        state_after_rewind_load = self.wait_for_transport_settle(timeout_sec=float(self.args.timeshift_load_timeout_sec), min_wait_sec=1.5)
        self.save_state_snapshot("04-after-rewind-load", state_after_rewind_load)
        self.request_capture_wait("03-after-rewind-load", "timeshift-cycle")

        time.sleep(max(0.0, float(self.args.timeshift_post_rewind_sec)))
        state_after_rewind_20 = self.read_state(timeout_sec=60.0)
        self.save_state_snapshot("05-after-rewind-wait", state_after_rewind_20)

        self.xdotool("key", "--window", self.window_id, "Home")
        state_after_home_load = self.wait_for_transport_settle(timeout_sec=float(self.args.timeshift_load_timeout_sec), min_wait_sec=1.5)
        self.save_state_snapshot("06-after-home-load", state_after_home_load)
        self.request_capture_wait("04-after-home-load", "timeshift-cycle")

        notes = {
            "after_60s": self.timeline_note(state_after_60),
            "after_rewind_load": self.timeline_note(state_after_rewind_load),
            "after_rewind_20s": self.timeline_note(state_after_rewind_20),
            "after_home_load": self.timeline_note(state_after_home_load),
        }
        states = {
            "after_rewind_load": state_after_rewind_load,
            "after_home_load": state_after_home_load,
        }

        analysis = self._analyze_timeshift_cycle(notes, states)
        analysis_path = self.run_dir / "timeshift-cycle-analysis.json"
        analysis_path.write_text(json.dumps(analysis, indent=2), encoding="utf-8")
        self.log(f"Timeshift analysis saved: {analysis_path}")
        if not bool(analysis.get("passed")):
            raise RuntimeError(f"Timeshift cycle analysis failed: {analysis_path}")
        return analysis

    def click_timeshift_timeline_fraction(self, fraction: float) -> None:
        state = self.read_state(timeout_sec=60.0)
        timeline = self.region_by_name(state, "timeshift_timeline")
        if not timeline or not bool(timeline.get("visible")):
            raise RuntimeError("Timeshift timeline region is not visible")

        width = int(timeline.get("width", 0) or 0)
        height = int(timeline.get("height", 0) or 0)
        if width < 2 or height < 2:
            raise RuntimeError(f"Timeshift timeline bounds invalid: {timeline}")

        clamped = max(0.0, min(1.0, float(fraction)))
        x = int(timeline.get("x", 0) + round(clamped * float(width - 1)))
        y = int(timeline.get("y", 0) + max(1, height // 2))
        self.log(f"Clicking timeshift timeline at fraction={clamped:.3f} x={x} y={y}")
        self.xdotool("mousemove", "--window", self.window_id, str(x), str(y))
        time.sleep(0.08)
        self.xdotool("mousedown", "--window", self.window_id, "1")
        time.sleep(0.06)
        self.xdotool("mouseup", "--window", self.window_id, "1")

    def wait_after_timeshift_load(self) -> None:
        delay = max(0.0, float(self.args.timeshift_post_load_capture_delay_sec))
        if delay > 0.0:
            time.sleep(delay)

    def _analyze_timeshift_timeline(
        self,
        notes: dict[str, dict[str, Any]],
        states: dict[str, dict[str, Any]],
        captures: dict[str, pathlib.Path | None],
    ) -> dict[str, Any]:
        before_click = notes.get("before_click", {})
        after_click = notes.get("after_click_load", {})
        after_wait = notes.get("after_click_wait", {})
        after_home = notes.get("after_home_load", {})
        after_home_wait = notes.get("after_home_wait", {})

        behind_before = self._safe_float(before_click.get("behindLiveSeconds"))
        behind_click = self._safe_float(after_click.get("behindLiveSeconds"))
        behind_home = self._safe_float(after_home.get("behindLiveSeconds"))

        pos_click = self._safe_float(after_click.get("positionSeconds"))
        pos_wait = self._safe_float(after_wait.get("positionSeconds"))
        pos_home = self._safe_float(after_home.get("positionSeconds"))
        pos_home_wait = self._safe_float(after_home_wait.get("positionSeconds"))

        transport_click = self.transport_value(states.get("after_click_load", {}))
        transport_home = self.transport_value(states.get("after_home_load", {}))
        transport_home_wait = self.transport_value(states.get("after_home_wait", {}))
        moved_back = False
        if behind_before is not None and behind_click is not None:
            moved_back = (behind_click - behind_before) >= max(8.0, float(self.args.timeshift_timeline_min_rewind_sec))

        progressed_after_click = False
        if pos_click is not None and pos_wait is not None:
            progressed_after_click = (pos_wait - pos_click) >= max(8.0, float(self.args.timeshift_timeline_post_sec) * 0.35)

        returned_live = bool(after_home.get("atLiveEdge"))
        if not returned_live and behind_home is not None:
            returned_live = behind_home <= float(self.args.timeshift_live_edge_max_behind_sec)

        stable_after_click = not bool(transport_click.get("isLoading"))
        stable_after_home = not bool(transport_home.get("isLoading"))
        stable_after_home_wait = not bool(transport_home_wait.get("isLoading"))

        screenshot_checks = {
            "beforeToAfterClickLoad": self.compare_capture_ssim(captures.get("before_click"), captures.get("after_click_load")),
            "afterClickLoadToAfterClickWait": self.compare_capture_ssim(captures.get("after_click_load"), captures.get("after_click_wait")),
            "afterClickWaitToAfterHomeLoad": self.compare_capture_ssim(captures.get("after_click_wait"), captures.get("after_home_load")),
            "afterHomeLoadToAfterHomeWait": self.compare_capture_ssim(captures.get("after_home_load"), captures.get("after_home_wait")),
        }

        checks = {
            "movedBackFromTimelineClick": moved_back,
            "playedForwardAfterTimelineClick": progressed_after_click,
            "returnedToLiveEdge": returned_live,
            "stableAfterTimelineLoad": stable_after_click,
            "stableAfterHomeLoad": stable_after_home,
            "stableAfterHomeWait": stable_after_home_wait,
        }
        return {
            "checks": checks,
            "passed": all(checks.values()),
            "metrics": {
                "behind_before_click": behind_before,
                "behind_after_click_load": behind_click,
                "behind_after_home_load": behind_home,
                "position_after_click_load": pos_click,
                "position_after_click_wait": pos_wait,
                "position_after_home_load": pos_home,
                "position_after_home_wait": pos_home_wait,
                "playback_url_after_home_load": after_home.get("playbackUrl", ""),
                "playback_url_after_home_wait": after_home_wait.get("playbackUrl", ""),
            },
            "transport": {
                "after_click_load": transport_click,
                "after_home_load": transport_home,
                "after_home_wait": transport_home_wait,
            },
            "screenshots": screenshot_checks,
            "notes": notes,
        }

    def run_timeshift_timeline_sequence(self, state_after_tune: dict[str, Any]) -> dict[str, Any]:
        if not bool(self.args.timeshift_enabled):
            raise RuntimeError("--timeshift-timeline-sequence requires --timeshift-enabled")

        self.log("Running timeshift timeline click sequence")

        self.request_capture_wait("01-playback-start", "timeshift-timeline")
        self.save_state_snapshot("02-playback-start", state_after_tune)

        time.sleep(max(0.0, float(self.args.timeshift_timeline_observe_sec)))
        state_before_click = self.read_state(timeout_sec=60.0)
        self.save_state_snapshot("03-before-timeline-click", state_before_click)
        capture_before_click = self.request_capture_wait("02-before-timeline-click", "timeshift-timeline")

        self.click_timeshift_timeline_fraction(float(self.args.timeshift_timeline_target_fraction))
        self.wait_for_transport_settle(
            timeout_sec=float(self.args.timeshift_load_timeout_sec),
            min_wait_sec=1.5,
        )
        self.wait_after_timeshift_load()
        state_after_click_load = self.read_state(timeout_sec=60.0)
        self.save_state_snapshot("04-after-timeline-click-load", state_after_click_load)
        capture_after_click_load = self.request_capture_wait("03-after-timeline-click-load", "timeshift-timeline")

        time.sleep(max(0.0, float(self.args.timeshift_timeline_post_sec)))
        state_after_click_wait = self.read_state(timeout_sec=60.0)
        self.save_state_snapshot("05-after-timeline-click-wait", state_after_click_wait)
        capture_after_click_wait = self.request_capture_wait("04-after-timeline-click-wait", "timeshift-timeline")

        self.xdotool("key", "--window", self.window_id, "Home")
        self.wait_for_transport_settle(
            timeout_sec=float(self.args.timeshift_load_timeout_sec),
            min_wait_sec=1.5,
        )
        self.wait_after_timeshift_load()
        state_after_home_load = self.read_state(timeout_sec=60.0)
        self.save_state_snapshot("06-after-home-load", state_after_home_load)
        capture_after_home_load = self.request_capture_wait("05-after-home-load", "timeshift-timeline")

        time.sleep(max(0.0, float(self.args.timeshift_timeline_home_post_sec)))
        state_after_home_wait = self.read_state(timeout_sec=60.0)
        self.save_state_snapshot("07-after-home-wait", state_after_home_wait)
        capture_after_home_wait = self.request_capture_wait("06-after-home-wait", "timeshift-timeline")

        notes = {
            "before_click": self.timeline_note(state_before_click),
            "after_click_load": self.timeline_note(state_after_click_load),
            "after_click_wait": self.timeline_note(state_after_click_wait),
            "after_home_load": self.timeline_note(state_after_home_load),
            "after_home_wait": self.timeline_note(state_after_home_wait),
        }
        states = {
            "after_click_load": state_after_click_load,
            "after_home_load": state_after_home_load,
            "after_home_wait": state_after_home_wait,
        }
        captures = {
            "before_click": capture_before_click,
            "after_click_load": capture_after_click_load,
            "after_click_wait": capture_after_click_wait,
            "after_home_load": capture_after_home_load,
            "after_home_wait": capture_after_home_wait,
        }

        analysis = self._analyze_timeshift_timeline(notes, states, captures)
        analysis_path = self.run_dir / "timeshift-timeline-analysis.json"
        analysis_path.write_text(json.dumps(analysis, indent=2), encoding="utf-8")
        self.log(f"Timeshift timeline analysis saved: {analysis_path}")
        if not bool(analysis.get("passed")):
            raise RuntimeError(f"Timeshift timeline analysis failed: {analysis_path}")
        return analysis

    def _analyze_timeshift_key_steps(
        self,
        notes: dict[str, dict[str, Any]],
        states: dict[str, dict[str, Any]],
        captures: dict[str, pathlib.Path | None],
    ) -> dict[str, Any]:
        anomaly_jump_sec = float(self.args.timeshift_step_anomaly_jump_sec)

        def note_value(name: str, key: str) -> float | None:
            return self._safe_float((notes.get(name) or {}).get(key))

        def moved_back(before_name: str, after_name: str) -> tuple[bool, float | None]:
            before = note_value(before_name, "behindLiveSeconds")
            after = note_value(after_name, "behindLiveSeconds")
            if before is None or after is None:
                return False, None
            delta = after - before
            return delta >= float(self.args.timeshift_step_min_seek_delta_sec), delta

        def moved_forward(before_name: str, after_name: str) -> tuple[bool, float | None, float | None]:
            before = note_value(before_name, "behindLiveSeconds")
            after = note_value(after_name, "behindLiveSeconds")
            before_pos = note_value(before_name, "positionSeconds")
            after_pos = note_value(after_name, "positionSeconds")
            behind_delta = None if before is None or after is None else before - after
            pos_delta = None if before_pos is None or after_pos is None else after_pos - before_pos
            min_delta = float(self.args.timeshift_step_min_seek_delta_sec)
            ok = (
                (behind_delta is not None and behind_delta >= min_delta)
                or (pos_delta is not None and pos_delta >= min_delta)
            )
            return ok, behind_delta, pos_delta

        def progressed(before_name: str, after_name: str, wait_sec: float) -> tuple[bool, float | None]:
            before = note_value(before_name, "positionSeconds")
            after = note_value(after_name, "positionSeconds")
            if before is None or after is None:
                return False, None
            delta = after - before
            return delta >= max(3.0, wait_sec * 0.35), delta

        def step_diagnostic(
            *,
            step: str,
            phase: str,
            before_name: str,
            after_name: str,
            screenshot_key: str,
            expected: str,
        ) -> dict[str, Any]:
            before_note = notes.get(before_name, {})
            after_note = notes.get(after_name, {})
            behind_before = note_value(before_name, "behindLiveSeconds")
            behind_after = note_value(after_name, "behindLiveSeconds")
            position_before = note_value(before_name, "positionSeconds")
            position_after = note_value(after_name, "positionSeconds")
            available_before = note_value(before_name, "availableSeconds")
            available_after = note_value(after_name, "availableSeconds")

            behind_delta = None if behind_before is None or behind_after is None else behind_after - behind_before
            position_delta = None if position_before is None or position_after is None else position_after - position_before
            available_delta = None if available_before is None or available_after is None else available_after - available_before
            screenshot = screenshot_checks[screenshot_key]
            screenshot_changed = bool(screenshot.get("changed"))
            screenshot_ssim = self._safe_float(screenshot.get("ssim"))

            suspicions: list[str] = []
            if expected == "rewind" and behind_delta is not None and behind_delta < float(self.args.timeshift_step_min_seek_delta_sec):
                suspicions.append("rewind_delta_too_small")
            if expected == "forward":
                if behind_delta is not None and behind_delta > 0.0:
                    suspicions.append("forward_key_increased_behind_live")
                if position_delta is not None and position_delta < float(self.args.timeshift_step_min_seek_delta_sec):
                    suspicions.append("forward_position_jump_too_small")
            if available_delta is not None and available_delta >= anomaly_jump_sec:
                suspicions.append("timeshift_window_grew_abnormally")
            if behind_delta is not None and abs(behind_delta) >= anomaly_jump_sec:
                suspicions.append("seek_delta_abnormally_large")
            if not screenshot_changed:
                significant_metadata_change = (
                    (behind_delta is not None and abs(behind_delta) >= 5.0)
                    or (position_delta is not None and abs(position_delta) >= 5.0)
                    or (available_delta is not None and abs(available_delta) >= 5.0)
                )
                if significant_metadata_change:
                    suspicions.append("metadata_changed_without_picture_change")
                else:
                    suspicions.append("picture_static")

            return {
                "step": step,
                "phase": phase,
                "expected": expected,
                "before": before_note,
                "after": after_note,
                "behindDeltaSeconds": behind_delta,
                "positionDeltaSeconds": position_delta,
                "availableDeltaSeconds": available_delta,
                "transportAfter": transport_values.get(after_name, {}),
                "screenshot": screenshot,
                "suspicions": suspicions,
            }

        def wait_diagnostic(
            *,
            step: str,
            before_name: str,
            after_name: str,
            screenshot_key: str,
            wait_sec: float,
        ) -> dict[str, Any]:
            before_note = notes.get(before_name, {})
            after_note = notes.get(after_name, {})
            behind_before = note_value(before_name, "behindLiveSeconds")
            behind_after = note_value(after_name, "behindLiveSeconds")
            position_before = note_value(before_name, "positionSeconds")
            position_after = note_value(after_name, "positionSeconds")
            available_before = note_value(before_name, "availableSeconds")
            available_after = note_value(after_name, "availableSeconds")

            behind_delta = None if behind_before is None or behind_after is None else behind_after - behind_before
            position_delta = None if position_before is None or position_after is None else position_after - position_before
            available_delta = None if available_before is None or available_after is None else available_after - available_before
            screenshot = screenshot_checks[screenshot_key]
            screenshot_changed = bool(screenshot.get("changed"))

            suspicions: list[str] = []
            min_progress = max(3.0, wait_sec * 0.35)
            if position_delta is not None and position_delta < min_progress:
                suspicions.append("playback_progress_too_small")
            if available_delta is not None and available_delta >= anomaly_jump_sec:
                suspicions.append("timeshift_window_grew_abnormally")
            if not screenshot_changed:
                if position_delta is not None and position_delta >= min_progress:
                    suspicions.append("playback_clock_advanced_without_picture_change")
                else:
                    suspicions.append("picture_static")

            return {
                "step": step,
                "phase": "wait",
                "expected": "play-forward",
                "before": before_note,
                "after": after_note,
                "waitSeconds": wait_sec,
                "behindDeltaSeconds": behind_delta,
                "positionDeltaSeconds": position_delta,
                "availableDeltaSeconds": available_delta,
                "transportAfter": self.transport_value(states.get(after_name, {})),
                "screenshot": screenshot,
                "suspicions": suspicions,
            }

        rewind1_ok, rewind1_delta = moved_back("before_j1", "after_j1_load")
        rewind2_ok, rewind2_delta = moved_back("before_j2", "after_j2_load")
        rewind3_ok, rewind3_delta = moved_back("before_j3", "after_j3_load")
        forward_ok, forward_behind_delta, forward_position_delta = moved_forward("before_l", "after_l_load")

        wait1_ok, wait1_delta = progressed("after_j1_load", "before_j2", float(self.args.timeshift_step_wait_sec))
        wait2_ok, wait2_delta = progressed("after_j2_load", "before_j3", float(self.args.timeshift_step_wait_sec))
        wait3_ok, wait3_delta = progressed("after_j3_load", "before_l", float(self.args.timeshift_step_wait_sec))
        wait4_ok, wait4_delta = progressed("after_l_load", "before_home", float(self.args.timeshift_step_wait_sec))
        home_wait_ok, home_wait_delta = progressed("after_home_load", "after_home_wait", float(self.args.timeshift_step_home_wait_sec))

        behind_home = note_value("after_home_load", "behindLiveSeconds")
        returned_live = bool((notes.get("after_home_load") or {}).get("atLiveEdge"))
        if not returned_live and behind_home is not None:
            returned_live = behind_home <= float(self.args.timeshift_live_edge_max_behind_sec)
        transport_checks: dict[str, bool] = {}
        transport_values: dict[str, dict[str, Any]] = {}
        for name in ("after_j1_load", "after_j2_load", "after_j3_load", "after_l_load", "after_home_load", "after_home_wait"):
            transport = self.transport_value(states.get(name, {}))
            transport_values[name] = transport
            transport_checks[name] = not bool(transport.get("isLoading"))

        screenshot_checks = {
            "beforeJ1ToAfterJ1Load": self.compare_capture_ssim(captures.get("before_j1"), captures.get("after_j1_load")),
            "afterJ1LoadToBeforeJ2": self.compare_capture_ssim(captures.get("after_j1_load"), captures.get("before_j2")),
            "beforeJ2ToAfterJ2Load": self.compare_capture_ssim(captures.get("before_j2"), captures.get("after_j2_load")),
            "afterJ2LoadToBeforeJ3": self.compare_capture_ssim(captures.get("after_j2_load"), captures.get("before_j3")),
            "beforeJ3ToAfterJ3Load": self.compare_capture_ssim(captures.get("before_j3"), captures.get("after_j3_load")),
            "afterJ3LoadToBeforeL": self.compare_capture_ssim(captures.get("after_j3_load"), captures.get("before_l")),
            "beforeLToAfterLLoad": self.compare_capture_ssim(captures.get("before_l"), captures.get("after_l_load")),
            "afterLLoadToBeforeHome": self.compare_capture_ssim(captures.get("after_l_load"), captures.get("before_home")),
            "beforeHomeToAfterHomeLoad": self.compare_capture_ssim(captures.get("before_home"), captures.get("after_home_load")),
            "afterHomeLoadToAfterHomeWait": self.compare_capture_ssim(captures.get("after_home_load"), captures.get("after_home_wait")),
        }
        segment_requests = self.mpv_segment_requests()
        segment_request_source = "app.log"
        if not segment_requests:
            segment_requests = self.playback_server_segment_requests()
            segment_request_source = "network.ndjson"
        segment_contexts = {
            name: self.segment_context_for_capture(path, segment_requests)
            for name, path in captures.items()
        }

        timeline: list[dict[str, Any]] = [
            step_diagnostic(
                step="j1",
                phase="action",
                before_name="before_j1",
                after_name="after_j1_load",
                screenshot_key="beforeJ1ToAfterJ1Load",
                expected="rewind",
            ),
            wait_diagnostic(
                step="between_j1_j2",
                before_name="after_j1_load",
                after_name="before_j2",
                screenshot_key="afterJ1LoadToBeforeJ2",
                wait_sec=float(self.args.timeshift_step_wait_sec),
            ),
            step_diagnostic(
                step="j2",
                phase="action",
                before_name="before_j2",
                after_name="after_j2_load",
                screenshot_key="beforeJ2ToAfterJ2Load",
                expected="rewind",
            ),
            wait_diagnostic(
                step="between_j2_j3",
                before_name="after_j2_load",
                after_name="before_j3",
                screenshot_key="afterJ2LoadToBeforeJ3",
                wait_sec=float(self.args.timeshift_step_wait_sec),
            ),
            step_diagnostic(
                step="j3",
                phase="action",
                before_name="before_j3",
                after_name="after_j3_load",
                screenshot_key="beforeJ3ToAfterJ3Load",
                expected="rewind",
            ),
            wait_diagnostic(
                step="between_j3_l",
                before_name="after_j3_load",
                after_name="before_l",
                screenshot_key="afterJ3LoadToBeforeL",
                wait_sec=float(self.args.timeshift_step_wait_sec),
            ),
            step_diagnostic(
                step="l",
                phase="action",
                before_name="before_l",
                after_name="after_l_load",
                screenshot_key="beforeLToAfterLLoad",
                expected="forward",
            ),
            wait_diagnostic(
                step="between_l_home",
                before_name="after_l_load",
                after_name="before_home",
                screenshot_key="afterLLoadToBeforeHome",
                wait_sec=float(self.args.timeshift_step_wait_sec),
            ),
            step_diagnostic(
                step="home",
                phase="action",
                before_name="before_home",
                after_name="after_home_load",
                screenshot_key="beforeHomeToAfterHomeLoad",
                expected="jump-live",
            ),
            wait_diagnostic(
                step="after_home",
                before_name="after_home_load",
                after_name="after_home_wait",
                screenshot_key="afterHomeLoadToAfterHomeWait",
                wait_sec=float(self.args.timeshift_step_home_wait_sec),
            ),
        ]
        for entry in timeline:
            before_key = {
                "j1": "before_j1",
                "between_j1_j2": "after_j1_load",
                "j2": "before_j2",
                "between_j2_j3": "after_j2_load",
                "j3": "before_j3",
                "between_j3_l": "after_j3_load",
                "l": "before_l",
                "between_l_home": "after_l_load",
                "home": "before_home",
                "after_home": "after_home_load",
            }.get(entry["step"], "")
            after_key = {
                "j1": "after_j1_load",
                "between_j1_j2": "before_j2",
                "j2": "after_j2_load",
                "between_j2_j3": "before_j3",
                "j3": "after_j3_load",
                "between_j3_l": "before_l",
                "l": "after_l_load",
                "between_l_home": "before_home",
                "home": "after_home_load",
                "after_home": "after_home_wait",
            }.get(entry["step"], "")
            entry["mpvSegments"] = {
                "beforeCapture": segment_contexts.get(before_key, {}),
                "afterCapture": segment_contexts.get(after_key, {}),
            }
            before_segment = segment_contexts.get(before_key, {}).get("latestSegmentIndex")
            after_segment = segment_contexts.get(after_key, {}).get("latestSegmentIndex")
            if before_segment is not None and after_segment is not None:
                entry["mpvSegmentDelta"] = int(after_segment) - int(before_segment)
            else:
                entry["mpvSegmentDelta"] = None

        findings: list[dict[str, Any]] = []
        for entry in timeline:
            for suspicion in entry.get("suspicions", []):
                findings.append({
                    "step": entry["step"],
                    "phase": entry["phase"],
                    "issue": suspicion,
                    "behindDeltaSeconds": entry.get("behindDeltaSeconds"),
                    "positionDeltaSeconds": entry.get("positionDeltaSeconds"),
                    "availableDeltaSeconds": entry.get("availableDeltaSeconds"),
                    "ssim": self._safe_float((entry.get("screenshot") or {}).get("ssim")),
                    "mpvSegmentDelta": entry.get("mpvSegmentDelta"),
                })

        app_log_excerpt = self.app_log_matches(
            patterns=[
                r"\[timeshift\.seek\]",
                r"\[timeshift\.jump_live\]",
                r"Relative seek",
                r"Jump live",
                r"playback restart complete",
                r"Seek failed",
            ],
            max_lines=80,
        )

        checks = {
            "firstJMovedBack": rewind1_ok,
            "secondJMovedBack": rewind2_ok,
            "thirdJMovedBack": rewind3_ok,
            "lMovedForward": forward_ok,
            "playedForwardBetweenJ1AndJ2": wait1_ok,
            "playedForwardBetweenJ2AndJ3": wait2_ok,
            "playedForwardBetweenJ3AndL": wait3_ok,
            "playedForwardBetweenLAndHome": wait4_ok,
            "playedForwardAfterHome": home_wait_ok,
            "returnedToLiveEdge": returned_live,
            "stableAfterJ1Load": transport_checks["after_j1_load"],
            "stableAfterJ2Load": transport_checks["after_j2_load"],
            "stableAfterJ3Load": transport_checks["after_j3_load"],
            "stableAfterLLoad": transport_checks["after_l_load"],
            "stableAfterHomeLoad": transport_checks["after_home_load"],
            "stableAfterHomeWait": transport_checks["after_home_wait"],
        }
        return {
            "checks": checks,
            "passed": all(checks.values()),
            "metrics": {
                "rewind1BehindDelta": rewind1_delta,
                "rewind2BehindDelta": rewind2_delta,
                "rewind3BehindDelta": rewind3_delta,
                "forwardBehindDelta": forward_behind_delta,
                "forwardPositionDelta": forward_position_delta,
                "wait1PositionDelta": wait1_delta,
                "wait2PositionDelta": wait2_delta,
                "wait3PositionDelta": wait3_delta,
                "wait4PositionDelta": wait4_delta,
                "homeWaitPositionDelta": home_wait_delta,
                "behindAfterHomeLoad": behind_home,
                "playback_url_after_home_load": (notes.get("after_home_load") or {}).get("playbackUrl", ""),
                "playback_url_after_home_wait": (notes.get("after_home_wait") or {}).get("playbackUrl", ""),
            },
            "transport": transport_values,
            "screenshots": screenshot_checks,
            "mpvSegmentRequests": {
                "source": segment_request_source,
                "totalRequests": len(segment_requests),
                "firstRequest": self.serializable_segment_request(segment_requests[0]) if segment_requests else {},
                "lastRequest": self.serializable_segment_request(segment_requests[-1]) if segment_requests else {},
                "captures": segment_contexts,
            },
            "timeline": timeline,
            "findings": findings,
            "appLogExcerpt": app_log_excerpt,
            "notes": notes,
        }

    def run_timeshift_key_step_sequence(self, state_after_tune: dict[str, Any]) -> dict[str, Any]:
        if not bool(self.args.timeshift_enabled):
            raise RuntimeError("--timeshift-key-step-sequence requires --timeshift-enabled")

        self.log("Running timeshift key-step sequence")

        self.request_capture_wait("01-playback-start", "timeshift-key-step")
        self.save_state_snapshot("02-playback-start", state_after_tune)

        time.sleep(max(0.0, float(self.args.timeshift_step_observe_sec)))

        state_before_j1 = self.read_state(timeout_sec=60.0)
        self.save_state_snapshot("03-before-j1", state_before_j1)
        capture_before_j1 = self.request_capture_wait("02-before-j1", "timeshift-key-step")

        self.log("Pressing key j (step 1)")
        self.xdotool("key", "--window", self.window_id, "j")
        self.wait_for_transport_settle(
            timeout_sec=float(self.args.timeshift_load_timeout_sec),
            min_wait_sec=1.5,
            require_playing=True,
        )
        self.wait_after_timeshift_load()
        state_after_j1_load = self.read_state(timeout_sec=60.0)
        self.save_state_snapshot("04-after-j1-load", state_after_j1_load)
        capture_after_j1_load = self.request_capture_wait("03-after-j1-load", "timeshift-key-step")

        time.sleep(max(0.0, float(self.args.timeshift_step_wait_sec)))
        state_before_j2 = self.read_state(timeout_sec=60.0)
        self.save_state_snapshot("05-before-j2", state_before_j2)
        capture_before_j2 = self.request_capture_wait("04-before-j2", "timeshift-key-step")

        self.log("Pressing key j (step 2)")
        self.xdotool("key", "--window", self.window_id, "j")
        self.wait_for_transport_settle(
            timeout_sec=float(self.args.timeshift_load_timeout_sec),
            min_wait_sec=1.5,
            require_playing=True,
        )
        self.wait_after_timeshift_load()
        state_after_j2_load = self.read_state(timeout_sec=60.0)
        self.save_state_snapshot("06-after-j2-load", state_after_j2_load)
        capture_after_j2_load = self.request_capture_wait("05-after-j2-load", "timeshift-key-step")

        time.sleep(max(0.0, float(self.args.timeshift_step_wait_sec)))
        state_before_j3 = self.read_state(timeout_sec=60.0)
        self.save_state_snapshot("07-before-j3", state_before_j3)
        capture_before_j3 = self.request_capture_wait("06-before-j3", "timeshift-key-step")

        self.log("Pressing key j (step 3)")
        self.xdotool("key", "--window", self.window_id, "j")
        self.wait_for_transport_settle(
            timeout_sec=float(self.args.timeshift_load_timeout_sec),
            min_wait_sec=1.5,
            require_playing=True,
        )
        self.wait_after_timeshift_load()
        state_after_j3_load = self.read_state(timeout_sec=60.0)
        self.save_state_snapshot("08-after-j3-load", state_after_j3_load)
        capture_after_j3_load = self.request_capture_wait("07-after-j3-load", "timeshift-key-step")

        time.sleep(max(0.0, float(self.args.timeshift_step_wait_sec)))
        state_before_l = self.read_state(timeout_sec=60.0)
        self.save_state_snapshot("09-before-l", state_before_l)
        capture_before_l = self.request_capture_wait("08-before-l", "timeshift-key-step")

        self.log("Pressing key l")
        self.xdotool("key", "--window", self.window_id, "l")
        self.wait_for_transport_settle(
            timeout_sec=float(self.args.timeshift_load_timeout_sec),
            min_wait_sec=1.5,
            require_playing=True,
        )
        self.wait_after_timeshift_load()
        state_after_l_load = self.read_state(timeout_sec=60.0)
        self.save_state_snapshot("10-after-l-load", state_after_l_load)
        capture_after_l_load = self.request_capture_wait("09-after-l-load", "timeshift-key-step")

        time.sleep(max(0.0, float(self.args.timeshift_step_wait_sec)))
        state_before_home = self.read_state(timeout_sec=60.0)
        self.save_state_snapshot("11-before-home", state_before_home)
        capture_before_home = self.request_capture_wait("10-before-home", "timeshift-key-step")

        self.log("Pressing key Home")
        self.xdotool("key", "--window", self.window_id, "Home")
        self.wait_for_transport_settle(
            timeout_sec=float(self.args.timeshift_load_timeout_sec),
            min_wait_sec=1.5,
            require_playing=True,
        )
        self.wait_after_timeshift_load()
        state_after_home_load = self.read_state(timeout_sec=60.0)
        self.save_state_snapshot("12-after-home-load", state_after_home_load)
        capture_after_home_load = self.request_capture_wait("11-after-home-load", "timeshift-key-step")

        time.sleep(max(0.0, float(self.args.timeshift_step_home_wait_sec)))
        state_after_home_wait = self.read_state(timeout_sec=60.0)
        self.save_state_snapshot("13-after-home-wait", state_after_home_wait)
        capture_after_home_wait = self.request_capture_wait("12-after-home-wait", "timeshift-key-step")

        notes = {
            "before_j1": self.timeline_note(state_before_j1),
            "after_j1_load": self.timeline_note(state_after_j1_load),
            "before_j2": self.timeline_note(state_before_j2),
            "after_j2_load": self.timeline_note(state_after_j2_load),
            "before_j3": self.timeline_note(state_before_j3),
            "after_j3_load": self.timeline_note(state_after_j3_load),
            "before_l": self.timeline_note(state_before_l),
            "after_l_load": self.timeline_note(state_after_l_load),
            "before_home": self.timeline_note(state_before_home),
            "after_home_load": self.timeline_note(state_after_home_load),
            "after_home_wait": self.timeline_note(state_after_home_wait),
        }
        states = {
            "after_j1_load": state_after_j1_load,
            "after_j2_load": state_after_j2_load,
            "after_j3_load": state_after_j3_load,
            "after_l_load": state_after_l_load,
            "after_home_load": state_after_home_load,
            "after_home_wait": state_after_home_wait,
        }
        captures = {
            "before_j1": capture_before_j1,
            "after_j1_load": capture_after_j1_load,
            "before_j2": capture_before_j2,
            "after_j2_load": capture_after_j2_load,
            "before_j3": capture_before_j3,
            "after_j3_load": capture_after_j3_load,
            "before_l": capture_before_l,
            "after_l_load": capture_after_l_load,
            "before_home": capture_before_home,
            "after_home_load": capture_after_home_load,
            "after_home_wait": capture_after_home_wait,
        }

        analysis = self._analyze_timeshift_key_steps(notes, states, captures)
        analysis_path = self.run_dir / "timeshift-key-step-analysis.json"
        analysis_path.write_text(json.dumps(analysis, indent=2), encoding="utf-8")
        self.log(f"Timeshift key-step analysis saved: {analysis_path}")
        if not bool(analysis.get("passed")):
            raise RuntimeError(f"Timeshift key-step analysis failed: {analysis_path}")
        return analysis

    def database_path_candidates(self) -> list[pathlib.Path]:
        return [settings_path.parent / "iptv.db" for settings_path in self.settings_path_candidates]

    @staticmethod
    def epg_entry_count(db_path: pathlib.Path) -> int:
        if not db_path.exists():
            return 0
        try:
            conn = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True, timeout=1.0)
            cur = conn.cursor()
            count = int(cur.execute("SELECT COUNT(*) FROM epg_entries").fetchone()[0])
            conn.close()
            return count
        except Exception:  # noqa: BLE001
            return 0

    def epg_cache_file_candidates(self) -> list[pathlib.Path]:
        epg_files: list[pathlib.Path] = []
        for settings_path in self.settings_path_candidates:
            epg_dir = settings_path.parent / "epg"
            if not epg_dir.exists():
                continue
            epg_files.extend(sorted(epg_dir.glob("*.cache")))
            # Staging databases must not count as a ready EPG.
            for manifest in sorted(epg_dir.glob("*.cache.json")):
                try:
                    name = json.loads(manifest.read_text()).get("file", "")
                    if name and pathlib.Path(name).name == name:
                        epg_files.append(epg_dir / name)
                except (OSError, ValueError):
                    pass
        return epg_files

    def wait_for_epg_loaded(self, min_entries: int, timeout_sec: float) -> pathlib.Path:
        deadline = time.time() + timeout_sec
        seen_db: pathlib.Path | None = None
        seen_cache: pathlib.Path | None = None
        while time.time() < deadline:
            for db_path in self.database_path_candidates():
                if not db_path.exists():
                    continue
                seen_db = db_path
                count = self.epg_entry_count(db_path)
                if count >= min_entries:
                    self.log(f"EPG loaded: {count} entries in {db_path}")
                    return db_path
            for cache_file in self.epg_cache_file_candidates():
                seen_cache = cache_file
                try:
                    size = cache_file.stat().st_size
                except OSError:
                    size = 0
                if size >= int(self.args.epg_min_cache_bytes):
                    self.log(f"EPG loaded: cache file {cache_file} size={size} bytes")
                    return cache_file
            time.sleep(0.5)
        db_info = str(seen_db) if seen_db else "<missing iptv.db>"
        cache_info = str(seen_cache) if seen_cache else "<missing published EPG cache>"
        raise RuntimeError(
            "Timed out waiting for EPG load "
            f"(need >= {min_entries} entries OR cache >= {int(self.args.epg_min_cache_bytes)} bytes): "
            f"db={db_info} cache={cache_info}")

    @staticmethod
    def region_by_name(state: dict[str, Any], name: str) -> dict[str, Any]:
        for region in state.get("regions", []):
            if isinstance(region, dict) and region.get("name") == name:
                return region
        return {}

    def wait_for_region_visibility(self, region_name: str, visible: bool, timeout_sec: float = 6.0) -> dict[str, Any]:
        deadline = time.time() + timeout_sec
        last_state: dict[str, Any] = {}
        while time.time() < deadline:
            try:
                state = self.read_state(timeout_sec=6.0, retry_interval_sec=0.2)
                last_state = state
            except Exception:  # noqa: BLE001
                time.sleep(0.2)
                continue
            region = self.region_by_name(state, region_name)
            if bool(region.get("visible")) == visible:
                return state
            time.sleep(0.2)
        return last_state

    def run_guide_settings_sequence(self) -> None:
        self.log("Running guide/settings navigation sequence")
        self.xdotool("key", "--window", self.window_id, "ctrl+Up")
        guide_open_state = self.wait_for_region_visibility("guide_overlay", True)
        self.save_state_snapshot("02-guide-open", guide_open_state)

        self.xdotool("key", "--window", self.window_id, "Down")
        time.sleep(0.2)
        self.xdotool("key", "--window", self.window_id, "Down")
        time.sleep(0.2)
        self.xdotool("key", "--window", self.window_id, "Return")
        guide_nav_state = self.wait_for_region_visibility("guide_overlay", True)
        self.save_state_snapshot("03-guide-nav", guide_nav_state)

        self.xdotool("key", "--window", self.window_id, "ctrl+Down")
        self.wait_for_region_visibility("guide_overlay", False)
        post_guide_state = self.read_state(timeout_sec=60.0)
        right_pane = self.region_by_name(post_guide_state, "right_pane")
        click_x = int(right_pane.get("x", 1260) + max(1, right_pane.get("width", 340)) - 22)
        click_y = int(right_pane.get("y", 0) + 24)
        self.xdotool("mousemove", "--window", self.window_id, str(click_x), str(click_y))
        time.sleep(0.1)
        self.xdotool("click", "--window", self.window_id, "1")
        settings_open_state = self.wait_for_region_visibility("settings_overlay", True)
        self.save_state_snapshot("04-settings-open", settings_open_state)

    def run_guide_probe(self) -> None:
        self.log("Running guide probe sequence")
        self.xdotool("key", "--window", self.window_id, "ctrl+Up")
        guide_open_state = self.wait_for_region_visibility("guide_overlay", True)
        self.save_state_snapshot("02-guide-open", guide_open_state)
        time.sleep(max(0.0, float(self.args.guide_capture_delay_sec)))
        capture_response = self.request_capture("guide-open-after-delay", "guide")
        self.log(f"Guide capture queued: {capture_response.get('outputPath', '')}")

    def run(self) -> int:
        start_time = time.time()
        epg_checked = False
        self.prepare_dirs()
        self.seed_settings()
        self.launch_stack()
        self.wait_for_bridge()
        self.start_sse()
        self.window_id = self.find_largest_window()

        enabled_timeshift_sequences = [
            name
            for name, enabled in (
                ("--timeshift-cycle-sequence", self.args.timeshift_cycle_sequence),
                ("--timeshift-timeline-sequence", self.args.timeshift_timeline_sequence),
                ("--timeshift-key-step-sequence", self.args.timeshift_key_step_sequence),
            )
            if enabled
        ]
        if len(enabled_timeshift_sequences) > 1:
            raise RuntimeError(f"Timeshift sequence flags are mutually exclusive: {', '.join(enabled_timeshift_sequences)}")

        self.xdotool("windowactivate", self.window_id)
        if (
            self.args.timeshift_cycle_sequence
            or self.args.timeshift_timeline_sequence
            or self.args.timeshift_key_step_sequence
        ) and self.args.wait_for_epg:
            self.wait_for_epg_loaded(
                min_entries=max(1, int(self.args.epg_min_entries)),
                timeout_sec=float(self.args.epg_timeout_sec),
            )
            epg_checked = True

        self.tune_channel(self.args.channel)
        state_after_tune = self.wait_for_playback_ready(timeout_sec=self.args.playback_timeout_sec)
        self.save_state_snapshot("01-after-tune", state_after_tune)

        if self.args.wait_for_epg and not epg_checked:
            self.wait_for_epg_loaded(
                min_entries=max(1, int(self.args.epg_min_entries)),
                timeout_sec=float(self.args.epg_timeout_sec),
            )

        if not self.args.timeshift_cycle_sequence:
            capture_response = self.request_capture("live-playback", "smoke")
            self.log(f"Capture queued: {capture_response.get('outputPath', '')}")

        if self.args.guide_probe:
            self.run_guide_probe()

        if self.args.guide_settings_sequence:
            self.run_guide_settings_sequence()

        if self.args.timeshift_enabled:
            if self.args.timeshift_cycle_sequence:
                self.run_timeshift_cycle_sequence(state_after_tune)
            elif self.args.timeshift_timeline_sequence:
                self.run_timeshift_timeline_sequence(state_after_tune)
            elif self.args.timeshift_key_step_sequence:
                self.run_timeshift_key_step_sequence(state_after_tune)
            else:
                self.xdotool("key", "--window", self.window_id, "j")
                time.sleep(1.2)
                state_after_j = self.read_state(timeout_sec=60.0)
                self.save_state_snapshot("02-after-j", state_after_j)
                self.xdotool("key", "--window", self.window_id, "Home")
                time.sleep(1.2)
                state_after_home = self.read_state(timeout_sec=60.0)
                self.save_state_snapshot("03-after-home", state_after_home)

        time.sleep(max(0.0, float(self.args.duration_sec)))
        final_state = self.read_state(timeout_sec=60.0)
        self.save_state_snapshot("99-final", final_state)

        # Scrub credentials from seeded settings after startup artifacts are collected.
        self.scrub_settings_credentials()

        summary = {
            "testName": self.args.test_name,
            "startedUtc": dt.datetime.fromtimestamp(start_time, tz=dt.timezone.utc).isoformat(),
            "finishedUtc": now_iso(),
            "durationSec": round(time.time() - start_time, 3),
            "result": "PASS",
            "runDir": str(self.run_dir),
            "windowId": self.window_id,
            "bridgePort": self.bridge_port,
            "channel": self.args.channel,
            "timeshiftEnabled": bool(self.args.timeshift_enabled),
            "artifacts": {
                "appLog": str(self.run_dir / "app.log"),
                "eventsNdjson": str(self.run_dir / "events.ndjson"),
                "networkNdjson": str(self.run_dir / "network.ndjson"),
                "runnerEvents": str(self.bridge_events_path),
                "screenshotsDir": str(self.screenshots_dir),
                "stateSnapshotsDir": str(self.state_dir),
                "xvfbLog": str(self.run_dir / "xvfb.log"),
                "openboxLog": str(self.run_dir / "openbox.log"),
            },
        }
        self.summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
        self.log(f"Run completed: {self.summary_path}")
        return 0

    def cleanup(self) -> None:
        self.stop_event.set()
        if self.sse_pump and self.sse_pump.is_alive():
            self.sse_pump.join(timeout=2.0)
        for proc in reversed(self.procs):
            if proc.poll() is None:
                proc.terminate()
        deadline = time.time() + 3.0
        for proc in reversed(self.procs):
            if proc.poll() is None:
                timeout = max(0.0, deadline - time.time())
                try:
                    proc.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    proc.kill()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run semantic Linux UI smoke tests for OKILTV.")
    parser.add_argument("--test-name", default="live-playback-smoke", help="Test name for run dir naming.")
    parser.add_argument("--channel", type=int, default=1, help="Channel number for initial tune.")
    parser.add_argument("--duration-sec", type=int, default=10, help="Post-setup observation duration.")
    parser.add_argument("--playback-timeout-sec", type=int, default=60, help="Seconds to wait for playback readiness.")
    parser.add_argument("--timeshift-enabled", action="store_true", help="Enable timeshift in seeded settings and perform J/Home checks.")
    parser.add_argument("--timeshift-window-minutes", type=int, default=90)
    parser.add_argument("--timeshift-segment-seconds", type=int, default=5)
    parser.add_argument("--timeshift-max-disk-gb", type=int, default=8)
    parser.add_argument("--timeshift-cycle-sequence", action="store_true", help="Run extended timeshift rewind/live cycle with captures and analysis.")
    parser.add_argument("--timeshift-timeline-sequence", action="store_true", help="Run timeline-click timeshift cycle (mouse click on timeline -> wait -> return live).")
    parser.add_argument("--timeshift-key-step-sequence", action="store_true", help="Run keyboard step validation (J/J/J/L/Home with screenshots before and after each action).")
    parser.add_argument("--timeshift-observe-sec", type=int, default=60, help="Observe playback duration before rewind in timeshift cycle mode.")
    parser.add_argument("--timeshift-rewind-sec", type=int, default=60, help="Requested rewind depth for timeshift cycle mode.")
    parser.add_argument("--timeshift-post-rewind-sec", type=int, default=20, help="Wait duration after rewind before jump-to-live in timeshift cycle mode.")
    parser.add_argument("--timeshift-timeline-target-fraction", type=float, default=0.35, help="Target fraction [0..1] used for timeline-click sequence.")
    parser.add_argument("--timeshift-timeline-observe-sec", type=int, default=60, help="Observe playback duration before timeline click in timeline sequence mode.")
    parser.add_argument("--timeshift-timeline-post-sec", type=int, default=10, help="Wait duration after timeline click before rewind-motion capture and jump-to-live.")
    parser.add_argument("--timeshift-timeline-home-post-sec", type=int, default=5, help="Wait duration after Home/live jump before final live-motion capture.")
    parser.add_argument("--timeshift-timeline-min-rewind-sec", type=float, default=20.0, help="Minimum behind-live increase required after timeline click.")
    parser.add_argument("--timeshift-step-observe-sec", type=int, default=60, help="Observe playback duration before the first J press in key-step mode.")
    parser.add_argument("--timeshift-step-wait-sec", type=int, default=5, help="Wait duration after each J/L press before the next pre-action capture.")
    parser.add_argument("--timeshift-step-home-wait-sec", type=int, default=5, help="Wait duration after Home before final playback-motion capture.")
    parser.add_argument("--timeshift-step-min-seek-delta-sec", type=float, default=8.0, help="Minimum behind-live delta required for each J/L action in key-step mode.")
    parser.add_argument("--timeshift-step-anomaly-jump-sec", type=float, default=120.0, help="Threshold used to flag abnormal single-step jumps in key-step diagnostics.")
    parser.add_argument("--timeshift-load-timeout-sec", type=int, default=40, help="Timeout for post-seek/post-home load settle in timeshift cycle mode.")
    parser.add_argument("--timeshift-post-load-capture-delay-sec", type=float, default=2.5, help="Extra delay after load settle before capturing timeshift post-action state.")
    parser.add_argument("--timeshift-live-edge-max-behind-sec", type=float, default=10.0, help="Max behind-live seconds considered as live-edge return when atLiveEdge is false.")
    parser.add_argument("--timeshift-screenshot-change-max-ssim", type=float, default=0.995, help="Maximum SSIM score treated as a visible picture change in screenshot comparisons.")
    parser.add_argument("--wait-for-epg", action="store_true", help="Wait for EPG readiness before scenario steps (DB rows and/or cache size gate).")
    parser.add_argument("--epg-timeout-sec", type=int, default=180, help="Seconds to wait for EPG readiness when --wait-for-epg is set.")
    parser.add_argument("--epg-min-entries", type=int, default=1, help="Minimum epg_entries row count accepted for EPG readiness.")
    parser.add_argument("--epg-min-cache-bytes", type=int, default=1_000_000, help="Minimum EPG cache file size accepted for EPG readiness.")
    parser.add_argument("--guide-probe", action="store_true", help="Open guide, wait, and capture evidence for Guide rendering.")
    parser.add_argument("--guide-capture-delay-sec", type=float, default=2.0, help="Delay after opening guide before requesting guide capture.")
    parser.add_argument("--guide-settings-sequence", action="store_true", help="Inject guide/settings navigation keys and save overlay snapshots.")
    parser.add_argument("--run-dir", default="", help="Optional explicit run dir. Defaults to /tmp/okiltv-ui-<name>-<utc>.")
    parser.add_argument("--app-bin", type=pathlib.Path, default=DEFAULT_APP, help="Path to OKILTV binary.")
    parser.add_argument("--qt-debug-logs", action="store_true", help="Enable verbose Qt debug logging (very large artifacts).")
    return parser.parse_args()


def ensure_dbus_session() -> None:
    if os.environ.get("DBUS_SESSION_BUS_ADDRESS"):
        return
    os.execvp(
        "dbus-run-session",
        ["dbus-run-session", "--", sys.executable, *sys.argv],
    )


def main() -> int:
    args = parse_args()
    if not args.app_bin.exists():
        raise SystemExit(f"App binary not found: {args.app_bin}")
    ensure_dbus_session()

    runner = Runner(args)
    try:
        return runner.run()
    except Exception as exc:  # noqa: BLE001
        runner.log(f"FAIL: {redact_url(str(exc))}")
        summary = {
            "testName": args.test_name,
            "finishedUtc": now_iso(),
            "result": "FAIL",
            "error": redact_url(str(exc)),
            "runDir": str(runner.run_dir),
        }
        runner.summary_path.parent.mkdir(parents=True, exist_ok=True)
        runner.summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
        return 1
    finally:
        runner.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
