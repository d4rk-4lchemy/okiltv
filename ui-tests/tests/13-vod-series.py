#!/usr/bin/env python3
"""Series shortcuts, selection and both panels through a local Xtream fixture."""
import http.server
import importlib.util
import json
import pathlib
import threading
import time
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("vod_movies_fixture", ROOT / "ui-tests/tests/12-vod-movies.py")
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
module = fixture.module


def main():
    module.ensure_dbus_session()
    args = module.parse_args()
    args.test_name = "vod-series"
    runner = fixture.LocalRunner(args)
    runner.prepare_dirs()
    checks = []
    media_requests = []

    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(runner.run_dir), **kwargs)

        def log_message(self, *args):
            pass

        def do_GET(self):
            target = urllib.parse.urlparse(self.path)
            if target.path.startswith("/series/"):
                media_requests.append(target.path.rsplit("/", 1)[-1])
                # Native VOD needs byte ranges for seeking and durable resume.
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
                try:
                    self.wfile.write(data[start:end + 1])
                except (BrokenPipeError, ConnectionResetError):
                    pass # Stop/replacement may close a pending media response.
                return
            action = urllib.parse.parse_qs(target.query).get("action", [""])[0]
            result = []
            if action == "get_series_categories":
                result = [{"category_id": "series", "category_name": "Local series"}]
            elif action == "get_series":
                result = [{"series_id": "7", "name": "Local Series", "category_id": "series"}]
            elif action == "get_series_info":
                result = {"info": {"name": "Local Series", "plot": "Local episode regression."},
                          "seasons": [{"season_number": 0}, {"season_number": 1}, {"season_number": 3}],
                          "episodes": {"0": [{"id": "100", "title": "Special", "episode_num": 1, "season": 0, "container_extension": "mp4", "info": {"duration_secs": 120}}],
                                       "1": [{"id": "101", "title": "First", "episode_num": 1, "season": 1, "container_extension": "mp4", "info": {"duration_secs": 120}}],
                                       "3": [{"id": "303", "title": "Next with a deliberately long episode title for continuous scrolling", "episode_num": 3, "season": 3, "container_extension": "mp4", "info": {"duration_secs": 120}},
                                             {"id": "304", "title": "Last with a deliberately long episode title for continuous scrolling", "episode_num": 4, "season": 3, "container_extension": "mp4", "info": {"duration_secs": 120}}]}}
            body = json.dumps(result).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    runner.http_port = server.server_port
    threading.Thread(target=server.serve_forever, daemon=True).start()

    def named(state, name):
        return next((item for item in state["inventory"] if item.get("objectName") == name), None)

    def wait(label, predicate, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
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

    def playback_ready(state, name):
        if name in ("ui.live.playPause", "ui.live.stopPlayback"):
            if (named(state, name) or {}).get("enabled", False):
                return True
            x = state["window"]["width"] // 2
            y = state["window"]["height"] // 2
            runner.xdotool("mousemove", "--window", runner.window_id, str(x), str(y))
            runner.xdotool("mousemove", "--window", runner.window_id, str(x + 1), str(y))
            return False
        if (named(state, name) or {}).get("enabled", False) and (named(state, "ui.live.previousChannel") or {}).get("enabled", False):
            return True
        right = name.startswith("ui.vod.episode") or name == "ui.vod.playback.settingsButton"
        key("Right" if right else "Left")
        return False

    def click(name):
        playback = (name.startswith("ui.vod.episode") or name in ("ui.vod.playback.settingsButton", "ui.vod.backToCatalog", "ui.live.playPause", "ui.live.stopPlayback")) and runner.read_state(3)["window"]["visibleOverlay"] == "none"
        state = wait("Enabled: " + name, lambda s: playback_ready(s, name) if playback else (named(s, name) or {}).get("enabled", False))
        bounds = named(state, name)["bounds"]
        runner.xdotool("mousemove", "--window", runner.window_id, str(round(bounds["x"] + bounds["width"] / 2)), str(round(bounds["y"] + bounds["height"] / 2)))
        if name in ("ui.live.playPause", "ui.live.stopPlayback"):
            wait("Transport button ready: " + name, lambda s: (named(s, name) or {}).get("enabled", False) and (named(s, name) or {}).get("hovered", False))
        runner.xdotool("click", "1")
        time.sleep(.25)

    def click_text(text):
        state = wait("Enabled text: " + text, lambda s: any(item.get("text") == text and item.get("enabled") for item in s["inventory"]))
        item = next(item for item in state["inventory"] if item.get("text") == text and item.get("enabled"))
        bounds = item["bounds"]
        runner.xdotool("mousemove", "--window", runner.window_id, str(round(bounds["x"] + bounds["width"] / 2)), str(round(bounds["y"] + bounds["height"] / 2)))
        runner.xdotool("click", "1")
        time.sleep(.25)

    def text_present(state, text):
        return any(text in item.get("text", "") for item in state["inventory"])

    def preplay_tracks_ready(state):
        # ComboBox exposes its caption through child Labels in the text bridge.
        # Their effective enabled state follows the audio/subtitle selectors.
        return sum(item.get("text") == "Default" and bool(item.get("enabled"))
                   for item in state["inventory"]) >= 2

    try:
        runner.seed_settings()
        runner.launch_stack()
        runner.wait_for_bridge(60)
        runner.window_id = runner.find_largest_window(60)
        runner.xdotool("windowactivate", runner.window_id)
        runner.tune_channel(1)
        runner.wait_for_playback_ready()
        live_url = runner.read_state(3)["playback"]["playbackUrl"]
        key("Escape"); key("b")
        wait("B opens series without changing playback", lambda s: (named(s, "ui.vod.close") or {}).get("enabled") and text_present(s, "Local Series"))
        assert not media_requests
        key("v")
        wait("V switches the open library to movies", lambda s: text_present(s, "All movies"))
        key("b")
        wait("B switches back to independently browsed series", lambda s: text_present(s, "All series") and text_present(s, "Local Series"))
        key("Return")
        state = wait("First episode probe enables audio/subtitles before playback", lambda s: text_present(s, "S01E01 · First") and preplay_tracks_ready(s))
        assert media_requests and all(item == "101.mp4" for item in media_requests)
        assert state["playback"]["playbackUrl"] == live_url, "Episode probing must preserve Live playback"
        probed_requests = len(media_requests)
        key("Right")
        wait("Right selects the next season without playback", lambda s: text_present(s, "S03E03 · Next") and (named(s, "ui.vod.play") or {}).get("enabled"))
        key("Right")
        state = wait("Right clamps at the final season", lambda s: text_present(s, "S03E03 · Next"))
        assert not preplay_tracks_ready(state), "Metadata must not cross seasons"
        key("Left")
        wait("Left selects the previous season and restores its tracks", lambda s: text_present(s, "S01E01 · First") and preplay_tracks_ready(s))
        assert len(media_requests) == probed_requests, "Season keys must not probe or play media"
        # Whole-series marking updates every season; an unwatched Special does not
        # change the aggregate. Row status clicks never select/start playback.
        click("ui.vod.watched")
        wait("Bulk Watched marks the series", lambda s: text_present(s, "Mark series as unwatched"))
        assert len(media_requests) == probed_requests, "Status changes must not probe/play media"
        click_text("Season 1"); key("Home"); key("Return")
        state = wait("Specials are marked by the bulk action", lambda s: text_present(s, "S00E01 · Special") and text_present(s, "Mark episode as unwatched"))
        state = wait("Selected Special is playable without probing", lambda s: (named(s, "ui.vod.play") or {}).get("enabled"))
        assert len(media_requests) == probed_requests, "Only the first ordinary episode may be probed"
        special_button = next(item["objectName"] for item in state["inventory"] if item.get("objectName", "").startswith("ui.vod.episodeWatched."))
        click(special_button)
        wait("Unwatched Specials preserve the series status", lambda s: text_present(s, "Mark episode as watched") and text_present(s, "Mark series as unwatched"))
        assert len(media_requests) == probed_requests, "Status changes must not start episode media"
        click_text("Specials"); key("Home"); key("Down"); key("Return")
        state = wait("Return to the marked ordinary season", lambda s: text_present(s, "S01E01 · First") and text_present(s, "Mark episode as unwatched"))
        state = wait("First episode reuses cached pre-play tracks", lambda s: preplay_tracks_ready(s) and (named(s, "ui.vod.play") or {}).get("enabled"))
        assert len(media_requests) == probed_requests, "Fresh episode metadata must avoid another probe"
        first_button = next(item["objectName"] for item in state["inventory"] if item.get("objectName", "").startswith("ui.vod.episodeWatched."))
        click(first_button)
        wait("Unwatched ordinary episode clears series status", lambda s: text_present(s, "Mark series as watched"))
        click("ui.vod.watched")
        wait("Bulk action restores all episodes to Watched", lambda s: text_present(s, "Mark series as unwatched"))
        click("ui.vod.watched")
        wait("Bulk Unwatched resets the series", lambda s: text_present(s, "Mark series as watched") and text_present(s, "Mark episode as watched"))
        assert len(media_requests) == probed_requests
        assert runner.read_state(3)["playback"]["playbackUrl"] == live_url
        # Explicit Play hands the existing video surface from Live to the episode.
        click("ui.vod.play")
        wait("Series starts on the existing VOD surface", lambda s: s["window"]["visibleOverlay"] == "none" and any(item == "101.mp4" for item in media_requests))
        key("Left")
        wait("Left series panel and right episode panel appear", lambda s: (named(s, "ui.vod.playback.search") or {}).get("enabled") and text_present(s, "S01E01 · First"))
        click("ui.live.nextChannel")
        wait("Next crosses the season gap", lambda s: media_requests[-1] == "303.mp4")
        key("Left")
        wait("Playing season is selected on the right", lambda s: playback_ready(s, "ui.vod.episode.0") and text_present(s, "S03E04 · Last"))
        runner.request_capture_wait("series-playback-wide")
        state = runner.read_state(3)
        next_title = next(item for item in state["inventory"]
                          if item.get("objectName", "").startswith("ui.vod.episodeTitle.")
                          and not item["objectName"].endswith(".marqueeText")
                          and "S03E03" in item.get("text", ""))
        last_title = next(item for item in state["inventory"]
                          if item.get("objectName", "").startswith("ui.vod.episodeTitle.")
                          and not item["objectName"].endswith(".marqueeText")
                          and "S03E04" in item.get("text", ""))
        next_name, last_name = next_title["objectName"], last_title["objectName"]
        bounds = next_title["bounds"]
        runner.xdotool("mousemove", "--window", runner.window_id,
                       str(round(bounds["x"] + 20)), str(round(bounds["y"] + bounds["height"] / 2)))
        wait("Hovered episode title scrolls in the right panel", lambda s:
             bool(named(s, next_name + ".marqueeText"))
             and named(s, next_name + ".marqueeText")["bounds"]["x"] < named(s, next_name)["bounds"]["x"] - 5)
        key("Right"); key("Down")
        wait("Keyboard indication overrides the stationary episode pointer", lambda s:
             not named(s, next_name + ".marqueeText")
             and bool(named(s, last_name + ".marqueeText"))
             and named(s, last_name + ".marqueeText")["bounds"]["x"] < named(s, last_name)["bounds"]["x"] - 5)
        assert media_requests[-1] == "303.mp4", "Marquee indication must not start an episode"
        runner.request_capture_wait("series-episode-marquee")
        key("Up")
        key("Right");key("Down")
        assert media_requests[-1] == "303.mp4", "Episode selection must not change playback"
        key("Return")
        wait("Enter in the right panel starts the selected episode", lambda s: media_requests[-1] == "304.mp4")
        key("Left");click("ui.vod.episode.0")
        wait("Right panel can resume the previous available episode", lambda s: media_requests[-1] == "303.mp4")
        key("Left");click("ui.vod.episode.1")
        wait("A single right panel click starts the selected episode", lambda s: media_requests[-1] == "304.mp4")
        runner.xdotool("windowsize", runner.window_id, "800", "700")
        key("Left")
        wait("Narrow series playback shows the left list", lambda s: playback_ready(s, "ui.vod.playback.search"))
        key("Right")
        wait("Right reveals narrow episodes after the slide", lambda s: playback_ready(s, "ui.vod.episode.0"))
        runner.request_capture_wait("series-playback-narrow")
        key("Up")
        assert media_requests[-1] == "304.mp4", "Narrow episode selection must preserve playback"
        key("Return")
        wait("Narrow right panel retains keyboard focus", lambda s: media_requests[-1] == "303.mp4")
        key("Left");key("Right")
        wait("Narrow episodes reopen after switching", lambda s: playback_ready(s, "ui.vod.episode.1"))
        key("Down");key("Return")
        wait("Narrow Enter starts the last episode", lambda s: media_requests[-1] == "304.mp4")
        runner.xdotool("windowsize", runner.window_id, "1600", "900")
        key("Left")
        click("ui.vod.playback.settingsButton")
        wait("Settings opens while the episode plays", lambda s: s["window"]["visibleOverlay"] == "settings")
        key("Escape"); key("Left")
        click("ui.vod.backToCatalog")
        wait("Back stops VOD and opens the correct series details", lambda s: s["window"]["visibleOverlay"] == "vod" and (named(s, "ui.vod.close") or {}).get("enabled") and text_present(s, "Local Series") and text_present(s, "S03E04 · Last"))
        click("ui.vod.play")
        wait("Last episode restarts for Stop regression", lambda s: s["window"]["visibleOverlay"] == "none" and (named(s, "ui.transport.duration") or {}).get("text") == "02:00" and media_requests[-1] == "304.mp4")
        key("Left")
        wait("Episode is playing before pause", lambda s: playback_ready(s, "ui.live.playPause") and (named(s, "ui.live.playPause") or {}).get("text") == "Pause")
        click("ui.live.playPause")
        state = wait("Episode pauses before seeking", lambda s: (named(s, "ui.live.playPause") or {}).get("text") == "Play")
        timeline = next(region for region in state["regions"] if region["name"] == "timeshift_timeline")
        runner.xdotool("mousemove", "--window", runner.window_id, str(round(timeline["x"] + timeline["width"] * 70 / 120)), str(round(timeline["cy"])))
        runner.xdotool("click", "1")
        wait("Episode seeks to a resumable position", lambda s: (named(s, "ui.transport.position") or {}).get("text") in ("01:09", "01:10", "01:11"))
        click("ui.live.stopPlayback")
        state = wait("Stop opens the playing series and retains the selected episode and progress", lambda s: s["window"]["visibleOverlay"] == "vod" and (named(s, "ui.vod.play") or {}).get("enabled") and text_present(s, "Local Series") and text_present(s, "S03E04 · Last") and "Resume" in (named(s, "ui.vod.play") or {}).get("text", ""))
        assert "00:01" in named(state, "ui.vod.play")["text"], "Stop must persist the episode's resume position"
        stopped_requests = len(media_requests)
        time.sleep(.5)
        assert len(media_requests) == stopped_requests, "Stop must not start another episode"
        click("ui.vod.play")
        wait("Resume reloads the stopped episode", lambda s: s["window"]["visibleOverlay"] == "none" and len(media_requests) > stopped_requests and media_requests[-1] == "304.mp4")
        key("Left")
        wait("Resume restores the saved episode position", lambda s: playback_ready(s, "ui.live.stopPlayback") and (named(s, "ui.transport.position") or {}).get("text") in tuple(f"01:{second:02}" for second in range(4, 14)))
        key("Escape")
        wait("Escape hides playback panels before returning", lambda s: not named(s, "ui.vod.backToCatalog") and not (named(s, "ui.live.stopPlayback") or {}).get("enabled"))
        time.sleep(.3)
        key("BackSpace")
        wait("Backspace returns to details after resumed playback", lambda s: s["window"]["visibleOverlay"] == "vod" and (named(s, "ui.vod.close") or {}).get("enabled") and text_present(s, "S03E04 · Last"))
        key("Return")
        wait("Enter in returned details restarts the selected episode", lambda s: s["window"]["visibleOverlay"] == "none" and media_requests[-1] == "304.mp4")
        key("Left"); key("Escape")
        wait("Playback panels hide again", lambda s: not named(s, "ui.vod.backToCatalog") and not (named(s, "ui.live.stopPlayback") or {}).get("enabled"))
        time.sleep(.3)
        key("Escape")
        wait("Escape on video returns to owning series details", lambda s: s["window"]["visibleOverlay"] == "vod" and (named(s, "ui.vod.close") or {}).get("enabled") and text_present(s, "S03E04 · Last"))
        runner.request_capture_wait("series-details-wide")
        key("Escape")
        wait("Escape returns from series details to browsing", lambda s: not named(s, "ui.vod.play") and (named(s, "ui.vod.close") or {}).get("enabled"))
        key("b")
        wait("Repeated B closes the series library", lambda s: s["window"]["visibleOverlay"] == "none")
        key("b")
        runner.xdotool("windowsize", runner.window_id, "700", "800")
        wait("Compact series library uses the shared layout", lambda s: (named(s, "ui.vod.close") or {}).get("enabled") and text_present(s, "All series"))
        runner.request_capture_wait("series-library-narrow")
    finally:
        (runner.run_dir / "series-result.json").write_text(json.dumps({"passed": checks}, indent=2))
        runner.cleanup()
        server.shutdown()


if __name__ == "__main__":
    main()
