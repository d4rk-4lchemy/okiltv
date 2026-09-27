#!/usr/bin/env python3
"""VOD catalog and real window shortcut regression, using only local fixtures."""
import http.server
import importlib.util
import json
import pathlib
import subprocess
import threading
import time
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ui_runner", ROOT / "ui-tests/linux-ui-test-runner.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class LocalRunner(module.Runner):
    def seed_settings(self):
        media = self.run_dir / "movie.mp4"
        subtitles = self.run_dir / "subtitles.srt"
        subtitles.write_text("1\n00:00:00,000 --> 00:01:59,000\nLocal subtitle\n")
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
                        "-i", "color=c=darkred:size=320x180:rate=10",
                        "-f", "lavfi", "-i", "sine=frequency=440:duration=120", "-i", str(subtitles),
                        "-map", "0:v", "-map", "1:a", "-map", "1:a", "-map", "2:s", "-map", "2:s",
                        "-c:a", "aac", "-c:s", "mov_text", "-t", "120", "-c:v", "mpeg4",
                        "-movflags", "+faststart", str(media)], check=True)
        playlist = self.run_dir / "channels.m3u"
        playlist.write_text(f"#EXTM3U\n#EXTINF:-1,Local Live\n{media.as_uri()}\n")
        live = "11111111-1111-1111-1111-111111111111"
        vod = "22222222-2222-2222-2222-222222222222"
        settings = {"activeProfileId": live, "profiles": [
            {"id": live, "name": "Local Live", "type": 2, "m3UFilePath": str(playlist), "isActive": True},
            {"id": vod, "name": "Local Cinema", "type": 0, "xtreamBaseUrl": f"http://127.0.0.1:{self.http_port}",
             "xtreamUsername": "synthetic-user", "xtreamPassword": "synthetic-password", "vodEnabled": False}],
            "vodEnabled": False, "autoRefreshEpg": False, "minimizeToTrayOnMinimize": False,
            "mpvOptions": {"ao": "null", "hwdec": "no"}}
        for path in self.settings_path_candidates:
            path.write_text(json.dumps(settings))


def main():
    module.ensure_dbus_session()
    args = module.parse_args()
    args.test_name = "vod-movies"
    runner = LocalRunner(args)
    runner.prepare_dirs()

    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(runner.run_dir), **kwargs)

        def log_message(self, *args):
            pass

        def do_GET(self):
            target = urllib.parse.urlparse(self.path)
            if target.path.startswith("/movie/"):
                data = (runner.run_dir / "movie.mp4").read_bytes()
                byte_range = self.headers.get("Range", "")
                start, end = 0, len(data) - 1
                if byte_range.startswith("bytes="):
                    first, last = byte_range[6:].split("-", 1)
                    start = int(first or 0)
                    end = min(int(last) if last else end, end)
                self.send_response(206 if byte_range else 200)
                self.send_header("Content-Type", "video/mp4")
                self.send_header("Accept-Ranges", "bytes")
                self.send_header("Content-Length", str(end - start + 1))
                if byte_range:
                    self.send_header("Content-Range", f"bytes {start}-{end}/{len(data)}")
                self.end_headers()
                self.wfile.write(data[start:end + 1])
                return
            if target.path != "/player_api.php":
                return super().do_GET()
            query = urllib.parse.parse_qs(target.query)
            action = query.get("action", [""])[0]
            if action == "get_vod_categories":
                result = [{"category_id": "1", "category_name": "Local films"}]
            elif action == "get_vod_streams":
                result = [{"stream_id": i, "name": f"Local Movie {i:02}", "year": 2024, "category_id": "1"} for i in range(1, 41)]
            elif action == "get_vod_info":
                result = {"info": {"plot": "A local movie for automated testing.", "duration_secs": 120},
                          "movie_data": {"stream_id": query.get("vod_id", ["1"])[0], "container_extension": "mp4"}}
            else:
                result = []
            body = json.dumps(result).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    runner.http_port = server.server_port
    threading.Thread(target=server.serve_forever, daemon=True).start()
    checks = []

    def named(state, name):
        return next((i for i in state["inventory"] if i.get("objectName") == name), None)

    def wait(label, predicate, timeout=15):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            state = runner.read_state(3)
            if predicate(state):
                checks.append(label)
                runner.log("PASS: " + label)
                return state
            time.sleep(.1)
        runner.save_state_snapshot("failure", state)
        raise AssertionError(label)

    def key(value):
        runner.xdotool("key", "--window", runner.window_id, value)
        time.sleep(.25)

    def reveal_button(name):
        def reveal(state):
            if (named(state, name) or {}).get("enabled", False):
                return True
            runner.xdotool("mousemove", "--window", runner.window_id, "800", "450")
            runner.xdotool("mousemove", "--window", runner.window_id, "801", "450")
            return False
        return wait("Button enabled: " + name, reveal)

    def click(name):
        state = reveal_button(name)
        item = named(state, name)
        assert item, name
        bounds = item["bounds"]
        runner.xdotool("mousemove", "--window", runner.window_id,
                       str(round(bounds["x"] + bounds["width"] / 2)), str(round(bounds["y"] + bounds["height"] / 2)))
        wait("Button ready: " + name, lambda s: (named(s, name) or {}).get("enabled", False))
        runner.xdotool("click", "1")

    try:
        runner.seed_settings()
        runner.launch_stack()
        runner.wait_for_bridge(60)
        runner.window_id = runner.find_largest_window(60)
        runner.xdotool("windowactivate", runner.window_id)
        runner.tune_channel(1)
        initial = runner.wait_for_playback_ready()
        channel = initial["playback"]["currentChannel"]
        key("Escape")
        key("v")
        wait("V opens real catalog", lambda s: any(i.get("text") == "Local Movie 01" for i in s["inventory"]))
        runner.request_capture_wait("vod-catalog")
        key("ctrl+f")
        key("v")
        wait("V types in search", lambda s: (named(s, "ui.vod.search") or {}).get("text") == "v")
        key("ctrl+a")
        key("BackSpace")
        wait("Search restores catalog", lambda s: any(i.get("text") == "Local Movie 01" for i in s["inventory"]))
        key("Down")
        key("Return")
        wait("Enter opens details", lambda s: bool(named(s, "ui.vod.play")))
        runner.request_capture_wait("vod-details")
        key("Escape")
        wait("Escape returns to library", lambda s: bool(named(s, "ui.vod.search")))
        key("v")
        wait("V closes library without retuning", lambda s: not named(s, "ui.vod.close") and s["playback"]["currentChannel"] == channel)
        key("v")
        wait("V reopens library", lambda s: bool(named(s, "ui.vod.close")))
        key("Return")
        wait("Details can reopen", lambda s: bool(named(s, "ui.vod.play")))
        click("ui.vod.play")
        wait("Play closes catalog", lambda s: not named(s, "ui.vod.close"))
        runner.xdotool("mousemove", "--window", runner.window_id, "600", "450")
        state = wait("VOD timeline shows title and duration", lambda s:
                     (named(s, "ui.transport.title") or {}).get("text", "").startswith("Local Movie")
                     and (named(s, "ui.transport.duration") or {}).get("text") == "02:00")
        title = named(state, "ui.transport.title")["text"]
        assert not named(state, "ui.live.guideButton")
        assert all(not r["visible"] for r in state["regions"] if r["name"] in ("left_pane", "right_pane"))
        checks.append("VOD hides channel/EPG rails and Guide button")
        key("F3")
        wait("F3 displays VOD track metadata", lambda s: any(
            i.get("text") == "v(1) a(2) s(2)" for i in s["inventory"]))
        wait("F3 displays VOD resolution", lambda s: any(
            " / 320x180" in i.get("text", "") for i in s["inventory"]))
        key("F3")
        click("transportAudioTracksButton")
        wait("VOD audio picker is visible", lambda s: any(i.get("text") == "Audio #2" for i in s["inventory"]))
        key("Down")
        key("Return")
        click("transportSubtitleTracksButton")
        wait("VOD subtitle picker includes None and tracks", lambda s:
             any(i.get("text") == "None" for i in s["inventory"])
             and any(i.get("text") == "Subtitle #2" for i in s["inventory"]))
        key("Escape")
        wait("Movie advances before pause", lambda s:
             (named(s, "ui.transport.position") or {}).get("text", "00:00") not in ("00:00", "--:--"))
        reveal_button("ui.live.stopPlayback")
        key("space")
        timeline = next(r for r in runner.read_state()["regions"] if r["name"] == "timeshift_timeline")
        def seek(seconds, drag=False):
            x = round(timeline["x"] + timeline["width"] * seconds / 120)
            y = round(timeline["cy"])
            if drag:
                runner.xdotool("mousedown", "1")
            runner.xdotool("mousemove", "--window", runner.window_id, str(x), str(y))
            time.sleep(.2)
            runner.xdotool("mouseup", "1") if drag else runner.xdotool("click", "1")
            wait(f"VOD seeks to {seconds} seconds", lambda s:
                 (named(s, "ui.transport.position") or {}).get("text") in
                 (f"00:{seconds - 1:02}", f"00:{seconds:02}", f"00:{seconds + 1:02}"))
        seek(37)
        seek(44, drag=True)
        runner.request_capture_wait("vod-playback")
        key("v")
        wait("Catalog opens over movie", lambda s: bool(named(s, "ui.vod.close")))
        key("Escape")
        wait("Browsing returns to grid", lambda s: not named(s, "ui.vod.play"))
        key("Down")
        key("Return")
        wait("Another movie details open", lambda s: bool(named(s, "ui.vod.play")))
        key("v")
        wait("Movie title survives browsing", lambda s: not named(s, "ui.vod.close")
             and (named(s, "ui.transport.title") or {}).get("text") == title)
        click("ui.live.stopPlayback")
        wait("Stopping VOD restores live rails", lambda s: any(r["visible"] for r in s["regions"] if r["name"] == "left_pane"))
        key("v")
        wait("Library reopens after stop", lambda s: bool(named(s, "ui.vod.close")))
        key("Escape")
        wait("Stopped movie returns to grid", lambda s: bool(named(s, "ui.vod.search")))
        runner.request_capture_wait("vod-poster-progress")
        for path in runner.settings_path_candidates:
            if path.exists():
                assert not json.loads(path.read_text()).get("vodEnabled", False)
        checks.append("Global activation remains session-only")
    finally:
        (runner.run_dir / "vod-result.json").write_text(json.dumps({"passed": checks}, indent=2))
        runner.cleanup()
        server.shutdown()


if __name__ == "__main__":
    main()
