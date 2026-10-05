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
             "xtreamUsername": "synthetic-user", "xtreamPassword": "synthetic-password", "vodEnabled": True,
             "autoRefreshIntervalHours": 0}],
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

    movie_requests = []

    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(runner.run_dir), **kwargs)

        def log_message(self, *args):
            pass

        def do_GET(self):
            target = urllib.parse.urlparse(self.path)
            if target.path.startswith("/movie/"):
                movie_requests.append(1)
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
                result = [{"category_id": "1", "category_name": "Local films"},
                          {"category_id": "2", "category_name": "Other films"},
                          {"category_id": "3", "category_name": "Empty group"}]
            elif action == "get_series_categories":
                result = [{"category_id": "s1", "category_name": "Local series"}]
            elif action == "get_vod_streams":
                result = [{"stream_id": i, "name": f"Local Movie {i:02}" + (" with a deliberately long title for continuous scrolling" if i == 40 else ""), "year": 2024, "category_id": "1" if i % 2 else "2"} for i in range(1, 41)]
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

    def sidebar_right(state):
        # The text inventory omits MouseAreas/Rectangles. All movies fills the
        # sidebar inside its 10 px margins, so its right edge locates the handle.
        bounds = named(state, "ui.vod.all")["bounds"]
        return bounds["x"] + bounds["width"] + 10

    def groups_ready(state):
        return (named(state, "ui.player.groupSearch") or {}).get("enabled", False)

    def library_ready(state):
        return (named(state, "ui.vod.close") or {}).get("enabled", False)

    def reveal_movies(state):
        if (named(state, "ui.vod.playback.search") or {}).get("enabled", False):
            return True
        key("Left")
        return False

    def movie_row(state, index, title):
        row = named(state, f"ui.vod.playback.row.{index}") or {}
        return row.get("enabled", False) and row.get("text") == title

    def back_outside_panel(state, switch_name, search_name):
        button = named(state, "ui.vod.backToCatalog") or {}
        switch = named(state, switch_name) or {}
        search = named(state, search_name) or {}
        if not button.get("enabled") or not switch.get("enabled"):
            return False
        bounds = button["bounds"]
        panel_right = switch["bounds"]["x"] + switch["bounds"]["width"] + 8
        return (abs(bounds["x"] - panel_right - 24) < 1
                and abs(bounds["y"] - search["bounds"]["y"] - 7) < 1
                and bounds["width"] == 40 and bounds["height"] == 40)

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
        if value in ("Left", "Right", "Up", "Down", "Return", "Escape", "Tab", "f"):
            wait("Chrome settles before " + value, lambda s: not s["window"]["chromeAnimationsRunning"])
        runner.xdotool("key", "--window", runner.window_id, value)
        time.sleep(.25)

    def click_text(label, prop="text"):
        state = wait("Control available: " + label, lambda s: any(
            i.get("text") == label and i.get("property") == prop and i.get("enabled")
            and i.get("bounds", {}).get("y", -1) >= 0 for i in s["inventory"]))
        item = next(i for i in state["inventory"] if i.get("text") == label
                    and i.get("property") == prop and i.get("enabled") and i.get("bounds", {}).get("y", -1) >= 0)
        bounds = item["bounds"]
        runner.xdotool("mousemove", "--window", runner.window_id,
                       str(round(bounds["x"] + bounds["width"] / 2)), str(round(bounds["y"] + bounds["height"] / 2)))
        runner.xdotool("click", "1")
        time.sleep(.3)

    def select_source(label):
        click_text(label)
        last_click = time.monotonic()
        def selected(state):
            nonlocal last_click
            if any(i.get("text") == label and i.get("className", "").startswith("FormTextField")
                   and i.get("enabled") for i in state["inventory"]):
                return True
            # Selecting Sources can still be sliding when its labels appear.
            # Confirm the editable form rather than trusting the first click.
            if time.monotonic() - last_click > 1:
                row = next((i for i in state["inventory"] if i.get("text") == label
                            and i.get("enabled") and i.get("className", "").startswith("QQuickText")
                            and 0 < i.get("bounds", {}).get("x", -1) < state["window"]["width"] / 2
                            and 70 < i.get("bounds", {}).get("y", -1) < state["window"]["height"] - 60), None)
                if row:
                    bounds = row["bounds"]
                    runner.xdotool("mousemove", "--window", runner.window_id,
                                   str(round(bounds["x"] + bounds["width"] / 2)),
                                   str(round(bounds["y"] + bounds["height"] / 2)))
                    runner.xdotool("click", "1")
                    last_click = time.monotonic()
            return False
        return wait("Source form selects " + label, selected)

    def scroll_to(name):
        for _ in range(20):
            state = runner.read_state()
            item = named(state, name)
            if item:
                bounds = item["bounds"]
                if 70 < bounds["y"] < state["window"]["height"] - 100:
                    return state
                direction = "5" if bounds["y"] > state["window"]["height"] - 100 else "4"
            else:
                direction = "5"
            runner.xdotool("mousemove", "--window", runner.window_id,
                           str(state["window"]["width"] - 160), str(round(state["window"]["height"] / 2)))
            runner.xdotool("click", "--repeat", "2", "--delay", "70", direction)
            time.sleep(.15)
        raise AssertionError("Source control could not be scrolled into view: " + name)

    def reveal_button(name):
        def reveal(state):
            if (named(state, name) or {}).get("enabled", False):
                return True
            runner.xdotool("mousemove", "--window", runner.window_id, "800", "450")
            runner.xdotool("mousemove", "--window", runner.window_id, "801", "450")
            return False
        return wait("Button enabled: " + name, reveal)

    def click(name, instant=False):
        reveal_button(name)
        last_bounds = None
        def ready(state):
            nonlocal last_bounds
            item = named(state, name) or {}
            if not item.get("enabled", False):
                return False
            if item.get("hovered", False):
                return True
            bounds = item["bounds"]
            if "hovered" not in item and bounds == last_bounds:
                return True
            # Probe completion/layout changes can move details controls between
            # reading their bounds and clicking; follow the current hit target.
            runner.xdotool("mousemove", "--window", runner.window_id,
                           str(round(bounds["x"] + bounds["width"] / 2)),
                           str(round(bounds["y"] + bounds["height"] / 2)))
            last_bounds = bounds
            return False
        wait("Button ready: " + name, ready)
        if instant:
            # xdotool click waits 100 ms after release, obscuring short animations.
            runner.xdotool("mousedown", "1", "mouseup", "1")
        else:
            runner.xdotool("click", "1")

    def click_mode_and_check_transition(name, closing=False):
        before = wait("Media navigation is ready", lambda s:
                      (named(s, name) or {}).get("enabled", False))
        expected = named(before, name)["bounds"]
        live_channel = before["multiview"]["tiles"][0]["channelId"]
        live_position = before["multiview"]["tiles"][0]["position"]
        click(name, instant=True)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            state = runner.read_state(3)
            assert state["multiview"]["isPlaying"], "Background Live paused during library navigation"
            assert state["multiview"]["tiles"][0]["channelId"] == live_channel, \
                "Background Live changed channel during library navigation"
            position = state["multiview"]["tiles"][0]["position"]
            assert position >= live_position - .15, "Background Live rewound during library navigation"
            live_position = position
            item = named(state, name)
            assert item, "Media switch disappeared during the mode transition"
            assert all(abs(item["bounds"][axis] - expected[axis]) < 1 for axis in ("x", "y")), \
                "Media switch moved with the library"
            if state["window"]["vodTransitioning"]:
                assert not item["enabled"], "Navigation accepted input during the library animation"
                if closing:
                    for region in state["regions"]:
                        if region["name"] == "left_pane":
                            assert region["x"] + region["width"] <= 1, "Live list opened before the library closed"
                        elif region["name"] == "right_pane":
                            assert region["x"] >= state["window"]["width"] - 1, "Live EPG opened before the library closed"
            # HTTP reads and process scheduling can outlast the 240 ms slide.
            # Validate every sampled transition, but require the settled target
            # rather than requiring the external runner to observe each phase.
            # Enabled navigation alone could also mean the click was ignored.
            elif item.get("enabled", False) and not state["window"]["chromeAnimationsRunning"] and (
                    (closing and state["window"]["visibleOverlay"] == "none"
                     and not named(state, "ui.vod.close")
                     and (named(state, "ui.live.guideButton") or {}).get("enabled", False)
                     and (named(state, "ui.live.settingsButton") or {}).get("enabled", False))
                    or (not closing and state["window"]["visibleOverlay"] == "vod"
                        and library_ready(state))):
                checks.append("Fixed media navigation and completed library/Live transition: " + name)
                return state
            time.sleep(.02)
        runner.save_state_snapshot("media-transition-failure", state)
        raise AssertionError("Media transition did not finish: " + name)

    try:
        runner.seed_settings()
        runner.launch_stack()
        runner.wait_for_bridge(60)
        runner.window_id = runner.find_largest_window(60)
        runner.xdotool("windowactivate", runner.window_id)
        runner.tune_channel(1)
        initial = runner.wait_for_playback_ready()
        channel = initial["playback"]["currentChannel"]
        key("f")
        reveal_button("ui.live.guideButton")
        state = wait("Fullscreen Live panels reach the top edge", lambda s:
                     (named(s, "ui.live.guideButton") or {}).get("enabled", False)
                     and all(abs(r["y"]) < 1 for r in s["regions"]
                             if r["name"] in ("left_pane", "right_pane")))
        navigation = named(state, "ui.navigation.live")["bounds"]
        assert named(state, "ui.live.guideButton")["bounds"]["y"] > navigation["y"] + navigation["height"]
        runner.request_capture_wait("fullscreen-live-panels")
        click("ui.live.guideButton")
        wait("Fullscreen Guide frame reaches the top without covering navigation", lambda s:
             any(r["name"] == "guide_overlay" and r["visible"] and abs(r["y"]) < 1 for r in s["regions"])
             and any(i.get("text") == "Collapse Guide" and i.get("enabled")
                     and i["bounds"]["y"] > navigation["y"] + navigation["height"] for i in s["inventory"]))
        click_text("Collapse Guide", "caption")
        wait("Fullscreen Guide finishes closing", lambda s:
             not any(r["name"] == "guide_overlay" and r["visible"] for r in s["regions"]))
        click("ui.live.settingsButton")
        wait("Fullscreen Settings frame reaches the top edge", lambda s:
             s["window"]["visibleOverlay"] == "settings"
             and any(r["name"] == "settings_overlay" and abs(r["y"]) < 1 for r in s["regions"]))
        key("Escape")
        wait("Fullscreen Settings closes", lambda s: s["window"]["visibleOverlay"] == "none")
        click_mode_and_check_transition("ui.navigation.movies")
        state = wait("Fullscreen library contains navigation above its toolbar", lambda s:
                     library_ready(s) and named(s, "ui.vod.all")["bounds"]["y"] < 100
                     and named(s, "ui.vod.search")["bounds"]["y"] > navigation["y"] + navigation["height"])
        runner.request_capture_wait("fullscreen-library-navigation")
        click("ui.navigation.series")
        wait("Fullscreen navigation follows the Series library", lambda s: library_ready(s)
             and named(s, "ui.vod.all")["text"] == "All series")
        click_mode_and_check_transition("ui.navigation.live", closing=True)
        wait("Fullscreen navigation returns to Live", lambda s: s["window"]["visibleOverlay"] == "none")
        key("f")
        key("ctrl+f")
        key("Tab")
        wait("Tab from Live search focuses media navigation", lambda s: s["window"]["navigationFocused"])
        key("Right")
        assert runner.read_state()["window"]["visibleOverlay"] == "none"
        checks.append("Navigation arrows preview without opening a library")
        key("Return")
        wait("Enter confirms Series without tuning TV", lambda s: library_ready(s)
             and (named(s, "ui.vod.all") or {}).get("text") == "All series"
             and s["playback"]["currentChannel"] == channel)
        click("ui.navigation.live")
        wait("Keyboard-selected Series returns to Live", lambda s: s["window"]["visibleOverlay"] == "none")
        key("Escape")
        click("ui.navigation.movies")
        wait("Title switch opens Movies", library_ready)
        click("ui.navigation.movies")
        wait("Repeated Movies click retains the library", library_ready)
        click("ui.navigation.series")
        wait("Title switch opens Series", lambda s: library_ready(s)
             and (named(s, "ui.vod.all") or {}).get("text") == "All series")
        click("ui.navigation.movies")
        wait("Title switch restores Movies", lambda s: library_ready(s)
             and (named(s, "ui.vod.all") or {}).get("text") == "All movies")
        click("ui.navigation.live")
        wait("Live TV closes the library and retains the current channel", lambda s:
             not named(s, "ui.vod.close") and s["playback"]["currentChannel"] == channel)
        key("v")
        state = wait("V opens real catalog after sliding in", lambda s:
                     library_ready(s) and any(i.get("text") == "Local Movie 01" for i in s["inventory"]))
        drag_y = round(named(state, "ui.vod.all")["bounds"]["y"] + 24)
        runner.xdotool("mousemove", "--window", runner.window_id,
                       str(round(sidebar_right(state))), str(drag_y))
        runner.xdotool("mousedown", "1")
        runner.xdotool("mousemove", "--window", runner.window_id,
                       str(round(state["window"]["width"] * .6)), str(drag_y))
        runner.xdotool("mouseup", "1")
        state = wait("Library sidebar stops at one third of the window", lambda s:
                     abs(sidebar_right(s)
                         - int(s["window"]["width"] / 3)) < 1)
        for control in ("ui.vod.search", "ui.vod.close"):
            bounds = named(state, control)["bounds"]
            assert bounds["x"] + bounds["width"] <= state["window"]["width"] + 1, control
        runner.xdotool("mousemove", "--window", runner.window_id,
                       str(round(sidebar_right(state))), str(drag_y))
        runner.xdotool("mousedown", "1")
        runner.xdotool("mousemove", "--window", runner.window_id, "50", str(drag_y))
        runner.xdotool("mouseup", "1")
        wait("Library sidebar stops at 208 pixels", lambda s:
             sidebar_right(s) == 208)
        runner.request_capture_wait("vod-catalog")
        # Long provider titles use the packaged QML marquee without touching playback.
        key("ctrl+f"); key("ctrl+a"); runner.xdotool("type", "Local Movie 40")
        state = wait("Long title is available in the grid", lambda s:
                     bool(named(s, "ui.vod.gridTitle.0"))
                     and "Local Movie 40" in named(s, "ui.vod.gridTitle.0")["text"])
        key("Down")
        wait("Keyboard indication scrolls the long grid title", lambda s:
             bool(named(s, "ui.vod.gridTitle.0.marqueeText"))
             and named(s, "ui.vod.gridTitle.0.marqueeText")["bounds"]["x"]
                 < named(s, "ui.vod.gridTitle.0")["bounds"]["x"] - 5)
        key("ctrl+f")
        state = wait("Search focus stops the grid title", lambda s:
                     not named(s, "ui.vod.gridTitle.0.marqueeText"))
        bounds = named(state, "ui.vod.gridTitle.0")["bounds"]
        runner.xdotool("mousemove", "--window", runner.window_id,
                       str(round(bounds["x"] + 30)), str(round(bounds["y"] + 10)))
        wait("Pointer indication scrolls the long grid title", lambda s:
             bool(named(s, "ui.vod.gridTitle.0.marqueeText"))
             and named(s, "ui.vod.gridTitle.0.marqueeText")["bounds"]["x"]
                 < named(s, "ui.vod.gridTitle.0")["bounds"]["x"] - 5)
        runner.request_capture_wait("vod-title-marquee")
        key("ctrl+f"); key("ctrl+a"); key("BackSpace")
        wait("Clearing marquee search restores the grid", lambda s:
             any(item.get("text") == "Local Movie 01" for item in s["inventory"]))
        key("ctrl+f")
        key("v")
        wait("V types in search", lambda s: (named(s, "ui.vod.search") or {}).get("text") == "v")
        key("ctrl+a")
        key("BackSpace")
        wait("Search restores catalog", lambda s: any(i.get("text") == "Local Movie 01" for i in s["inventory"]))
        key("Down")
        key("Return")
        wait("Enter opens details", lambda s: bool(named(s, "ui.vod.play")))
        click("ui.vod.detailsToWatch")
        wait("To Watch saved in details", lambda s: (named(s, "ui.vod.detailsToWatch") or {}).get("text") == "Remove from plan to watch")
        click("ui.vod.detailsFavourite")
        wait("Favourites saved in details", lambda s: (named(s, "ui.vod.detailsFavourite") or {}).get("text") == "Remove from favourites")
        runner.request_capture_wait("vod-details-lists")
        key("Escape")
        wait("Escape returns to marked library", lambda s: bool(named(s, "ui.vod.search")) and not named(s, "ui.vod.play"))
        assert any(i.get("text") == "To Watch" for i in runner.read_state()["inventory"])
        assert any(i.get("text") == "Favourites" for i in runner.read_state()["inventory"])
        key("Return")
        wait("Marked film details reopen", lambda s: (named(s, "ui.vod.detailsToWatch") or {}).get("text") == "Remove from plan to watch")
        click("ui.vod.watched")
        wait("Mark watched clears To Watch and retains Favourites", lambda s:
             (named(s, "ui.vod.detailsToWatch") or {}).get("text") == "Add to plan to watch"
             and (named(s, "ui.vod.detailsFavourite") or {}).get("text") == "Remove from favourites")
        click("ui.vod.detailsToWatch")
        wait("Watched film can return to To Watch", lambda s: (named(s, "ui.vod.detailsToWatch") or {}).get("text") == "Remove from plan to watch")
        click("ui.vod.watched")
        wait("Film can be marked unwatched again", lambda s: (named(s, "ui.vod.watched") or {}).get("text") == "Mark as watched")
        wait("Mark unwatched retains To Watch", lambda s: (named(s, "ui.vod.detailsToWatch") or {}).get("text") == "Remove from plan to watch")
        runner.request_capture_wait("vod-details")
        key("Escape")
        wait("Escape returns to library", lambda s: bool(named(s, "ui.vod.search")))
        # Send repeats within the animation; reading the full state can itself
        # take longer than 240 ms on a software-rendered desktop.
        runner.xdotool("key", "--delay", "1", "--window", runner.window_id, "v", "v", "Escape", "ctrl+f", "space", "1")
        wait("V closes library without retuning", lambda s: not named(s, "ui.vod.close") and s["playback"]["currentChannel"] == channel)
        runner.xdotool("key", "--delay", "1", "--window", runner.window_id, "v", "v", "Escape", "ctrl+f", "Return")
        wait("Repeated keys during opening preserve the library", lambda s:
             library_ready(s) and not named(s, "ui.vod.play"))
        click("ui.vod.close")
        wait("Close button finishes hiding the library", lambda s: not named(s, "ui.vod.close"))
        key("v")
        wait("V reopens library with browse focus restored", library_ready)
        key("Escape")
        wait("Escape finishes hiding the library", lambda s: not named(s, "ui.vod.close"))
        key("v")
        wait("V reopens library", library_ready)
        key("Return")
        wait("Details can reopen", lambda s: bool(named(s, "ui.vod.play")))
        click("ui.vod.play")
        wait("Play closes catalog", lambda s: not named(s, "ui.vod.close"))
        runner.xdotool("mousemove", "--window", runner.window_id, "600", "450")
        state = wait("VOD timeline shows title and duration", lambda s:
                     (named(s, "ui.transport.title") or {}).get("text", "").startswith("Local Movie")
                     and (named(s, "ui.transport.duration") or {}).get("text") == "02:00")
        title = named(state, "ui.transport.title")["text"]
        click("ui.navigation.movies")
        wait("Movie library opens during VOD", library_ready)
        time.sleep(1)
        click("ui.vod.close")
        wait("Closing the library resumes its VOD pause", lambda s:
             not named(s, "ui.vod.close") and (named(s, "ui.live.playPause") or {}).get("text") == "Pause")
        runner.xdotool("mousemove", "--window", runner.window_id, "600", "450")
        state = wait("Movie remains active after browsing", lambda s:
                     (named(s, "ui.transport.title") or {}).get("text") == title)
        click("ui.navigation.live")
        wait("Live TV switches from VOD to the remembered TV channel", lambda s:
             s["playback"]["currentChannel"] == channel and not named(s, "ui.vod.backToCatalog"))
        click("ui.navigation.movies")
        state = wait("Movies can reopen after returning to TV", library_ready)
        if not named(state, "ui.vod.play"):
            key("Return")
        wait("Remembered movie details are available", lambda s: bool(named(s, "ui.vod.play")))
        click("ui.vod.play")
        state = wait("VOD can start again after the Live handoff", lambda s:
                     (named(s, "ui.transport.title") or {}).get("text") == title
                     and not named(s, "ui.vod.close"))
        assert not named(state, "ui.live.guideButton")
        assert all(not r["visible"] for r in state["regions"] if r["name"] in ("left_pane", "right_pane"))
        checks.append("VOD hides channel/EPG rails and Guide button")
        reveal_button("ui.vod.backToCatalog")
        runner.xdotool("mousemove", "--window", runner.window_id, "180", "120")
        wait("VOD panels show the playing movie", lambda s:
             bool(named(s, "ui.vod.playback.search"))
             and (named(s, "ui.vod.playback.title") or {}).get("text") == title)
        wait("Movie advances before browsing groups", lambda s:
             (named(s, "ui.transport.position") or {}).get("text", "00:00") not in ("00:00", "--:--"))
        key("space")
        wait("Pause is confirmed before group browsing", lambda s:
             (named(s, "ui.live.playPause") or {}).get("text") == "Play")
        # mpv acknowledges pause asynchronously; let its final position telemetry
        # reach the label before pinning the long group-browsing assertion.
        time.sleep(0.6)
        paused_position = named(runner.read_state(), "ui.transport.position")["text"]
        click("ui.navigation.series")
        wait("Series can be browsed while a movie is manually paused", library_ready)
        click("ui.vod.close")
        wait("Closing Series preserves the manual movie pause", lambda s:
             not named(s, "ui.vod.close") and (named(s, "ui.live.playPause") or {}).get("text") == "Play")
        reveal_button("ui.vod.backToCatalog")
        requests_before_groups = len(movie_requests)
        state = wait("Back arrow sits outside the movie panel with shared margins", lambda s:
                     back_outside_panel(s, "ui.vod.playback.openGroups", "ui.vod.playback.search"))
        click("ui.vod.playback.openGroups")
        wait("Groups button opens the existing VOD picker", lambda s:
             groups_ready(s) and (named(s, "ui.player.groupReturn") or {}).get("text") == "Movies →"
             and bool(named(s, "ui.player.group.__to_watch__")) and bool(named(s, "ui.player.group.__favourites__")))
        wait("Back arrow remains outside the group picker", lambda s:
             back_outside_panel(s, "ui.player.groupReturnAction", "ui.player.groupSearch"))
        click("ui.player.groupReturn")
        wait("Movies button returns without changing the filter", lambda s:
             movie_row(s, 1, "Local Movie 02"))
        key("Escape")
        state = reveal_button("ui.vod.backToCatalog")
        bounds = named(state, "ui.vod.backToCatalog")["bounds"]
        runner.xdotool("mousemove", "--window", runner.window_id,
                       str(round(bounds["x"] + 20)), str(round(bounds["y"] + 20)))
        wait("Back hover is acknowledged", lambda s:
             (named(s, "ui.vod.backToCatalog") or {}).get("hovered", False))
        time.sleep(3.5)
        state = runner.read_state()
        assert (named(state, "ui.vod.backToCatalog") or {}).get("enabled", False)
        checks.append("Back hover keeps playback chrome visible beyond auto-hide")
        key("Escape")
        wait("Escape hides chrome under the stationary Back cursor", lambda s:
             not named(s, "ui.vod.backToCatalog") and not named(s, "ui.vod.playback.search")
             and not s["window"]["chromeAnimationsRunning"])
        key("Left")
        wait("First Left focuses movies", lambda s:
             bool(named(s, "ui.vod.playback.search")) and not named(s, "ui.player.groupSearch"))
        key("Left")
        wait("Second Left opens VOD groups", lambda s:
             groups_ready(s) and (named(s, "ui.player.groupReturn") or {}).get("text") == "Movies →"
             and bool(named(s, "ui.player.group.__continue_watching__"))
             and not named(s, "ui.vod.playback.search"))
        key("Down"); key("Down"); key("Down"); key("Down"); key("Down"); key("Right")
        wait("Right confirms the highlighted movie group", lambda s:
             movie_row(s, 0, "Local Movie 02")
             and not named(s, "ui.player.groupSearch"))
        key("Left")
        wait("Selected group can reopen", lambda s: groups_ready(s))
        key("Return")
        wait("Reopening keeps the selected movie group", lambda s:
             movie_row(s, 0, "Local Movie 02"))
        key("Left")
        wait("Picker reopens for pointer search", lambda s: groups_ready(s))
        click("ui.player.groupSearch")
        runner.xdotool("type", "--window", runner.window_id, "missing")
        wait("Group search handles empty results", lambda s: any(
            i.get("text") == "No groups match this search." for i in s["inventory"]))
        key("ctrl+a")
        runner.xdotool("type", "--window", runner.window_id, "local")
        wait("Group search filters category names", lambda s:
             bool(named(s, "ui.player.group.1")) and not named(s, "ui.player.group.2"))
        click("ui.player.group.1")
        wait("A single group click returns to filtered movies", lambda s:
             movie_row(s, 0, "Local Movie 01"))
        key("Left")
        wait("Picker opens for empty category", lambda s: groups_ready(s) and bool(named(s, "ui.player.group.3")))
        click("ui.player.group.3")
        wait("Empty movie category leaves playback available", lambda s:
             (named(s, "ui.vod.playback.search") or {}).get("enabled", False)
             and any(i.get("text") == "No movies found" for i in s["inventory"]))
        key("Left")
        wait("Picker opens from an empty movie list", lambda s: groups_ready(s) and bool(named(s, "ui.player.group.")))
        click("ui.player.group.")
        wait("All movies restores the complete list", lambda s:
             movie_row(s, 1, "Local Movie 02"))
        key("Left")
        wait("Picker opens for cancel", lambda s: groups_ready(s))
        key("Escape")
        wait("Escape hides the movie group picker", lambda s:
             not named(s, "ui.player.groupSearch") and not named(s, "ui.vod.playback.search")
             and not named(s, "ui.vod.backToCatalog"))
        key("Left")
        wait("Left restores movies after Escape", lambda s:
             (named(s, "ui.vod.playback.search") or {}).get("enabled", False))
        key("Left")
        wait("Groups reopen after Escape", lambda s: groups_ready(s))
        click("ui.player.groupReturn")
        state = wait("Movies return cancels without filtering", lambda s:
                     movie_row(s, 1, "Local Movie 02"))
        assert named(state, "ui.transport.title")["text"] == title
        assert named(state, "ui.transport.position")["text"] == paused_position
        assert len(movie_requests) == requests_before_groups
        checks.append("Group browsing preserves the paused session and media connection")
        key("space")
        key("ctrl+f")
        key("ctrl+a")
        key("0"); key("2")
        wait("Sidebar search filters independently", lambda s:
             (named(s, "ui.vod.playback.search") or {}).get("text") == "02"
             and (named(s, "ui.vod.playback.title") or {}).get("text") == title)
        key("Down"); key("Return")
        state = wait("Sidebar Enter switches the movie", lambda s:
                     (named(s, "ui.transport.title") or {}).get("text") == "Local Movie 02")
        title = "Local Movie 02"
        runner.xdotool("mousemove", "--window", runner.window_id, "650", "460")
        reveal_button("ui.vod.backToCatalog")
        runner.xdotool("mousemove", "--window", runner.window_id, "180", "120")
        wait("Playing movie summary follows handoff", lambda s:
             (named(s, "ui.vod.playback.title") or {}).get("text") == title)
        runner.request_capture_wait("vod-side-panels")
        runner.xdotool("windowsize", runner.window_id, "800", "600")
        runner.xdotool("mousemove", "--window", runner.window_id, "400", "300")
        def narrow_list_visible(state):
            if named(state, "ui.vod.playback.search") and not named(state, "ui.vod.playback.title"):
                return True
            runner.xdotool("mousemove", "--window", runner.window_id,
                           str(140 + int(time.monotonic() * 10) % 20), "70")
            return False
        wait("Narrow window shows only the movie list", narrow_list_visible)
        click("ui.vod.playback.infoButton")
        wait("Information button switches the narrow panel", lambda s:
             bool(named(s, "ui.vod.playback.title")) and not named(s, "ui.vod.playback.search")
             and not named(s, "ui.vod.backToCatalog"))
        runner.request_capture_wait("vod-narrow-summary")
        key("ctrl+f")
        wait("Search shortcut returns to the narrow list", lambda s:
             bool(named(s, "ui.vod.playback.search")) and not named(s, "ui.vod.playback.title"))
        key("Down")
        key("Left")
        wait("Narrow movie list opens groups", lambda s: groups_ready(s))
        click("ui.player.group.1")
        wait("Narrow category keeps the movie search", lambda s:
             (named(s, "ui.vod.playback.search") or {}).get("enabled", False)
             and (named(s, "ui.vod.playback.search") or {}).get("text") == "02"
             and any(i.get("text") == "No movies found" for i in s["inventory"]))
        key("Left")
        wait("Narrow groups reopen from empty search results", lambda s: groups_ready(s))
        key("F1")
        wait("Audio picker replaces movie groups", lambda s:
             not named(s, "ui.player.groupSearch")
             and not named(s, "ui.vod.backToCatalog")
             and any(i.get("text") == "Audio #2" and i.get("enabled") for i in s["inventory"]))
        key("Escape")
        wait("Movies return after dismissing audio picker", reveal_movies)
        key("Left")
        wait("Groups return after dismissing audio picker", lambda s: groups_ready(s))
        click("ui.player.group.")
        wait("All movies restores searched film at narrow width", lambda s:
             movie_row(s, 0, "Local Movie 02"))
        runner.xdotool("windowsize", runner.window_id, "426", "240")
        wait("Back arrow fits the minimum window", lambda s:
             back_outside_panel(s, "ui.vod.playback.openGroups", "ui.vod.playback.search")
             and named(s, "ui.vod.backToCatalog")["bounds"]["x"] + 40 <= 426)
        state = runner.read_state()
        mode = named(state, "ui.navigation.series")["bounds"]
        close_bounds = named(state, "ui.window.minimize")["bounds"]
        assert mode["y"] > close_bounds["y"] + close_bounds["height"]
        assert close_bounds["height"] == 30
        assert mode["x"] + mode["width"] <= 426
        assert mode["height"] == 28
        checks.append("Standalone navigation fits below the restored title bar at minimum size")
        key("f")
        wait("Fullscreen movie panels reclaim the title-bar space", lambda s:
             back_outside_panel(s, "ui.vod.playback.openGroups", "ui.vod.playback.search")
             and abs(named(s, "ui.vod.backToCatalog")["bounds"]["y"] - 24) < 1
             and abs(named(s, "ui.vod.playback.settingsButton")["bounds"]["y"] - 14) < 1
             and bool(named(s, "ui.navigation.movies")) and not named(s, "ui.window.close"))
        key("f")
        runner.xdotool("windowsize", runner.window_id, "800", "600")
        key("Escape")
        wait("Escape dismisses VOD panels without stopping", lambda s:
             not named(s, "ui.vod.playback.search") and not named(s, "ui.vod.playback.title")
             and not named(s, "ui.vod.backToCatalog"))
        runner.xdotool("windowsize", runner.window_id, "1600", "900")
        runner.xdotool("mousemove", "--window", runner.window_id, "650", "460")
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
        wait("Catalog opens over movie", library_ready)
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
        wait("Library reopens after stop", library_ready)
        key("Escape")
        wait("Stopped movie returns to grid", lambda s: bool(named(s, "ui.vod.search")))
        runner.request_capture_wait("vod-poster-progress")
        key("ctrl+f"); key("ctrl+a"); key("0"); key("3")
        wait("Library search finds the third movie", lambda s:
             any(i.get("text") == "Local Movie 03" for i in s["inventory"])
             and not any(i.get("text") == "Local Movie 01" for i in s["inventory"]))
        key("Down")
        key("Return")
        wait("Movie details reopen for back action", lambda s: bool(named(s, "ui.vod.play")))
        click("ui.vod.play")
        wait("Movie restarts for back action", lambda s: not named(s, "ui.vod.close"))
        wait("Third movie starts for shelf regression", lambda s:
             (named(s, "ui.transport.title") or {}).get("text") == "Local Movie 03"
             and (named(s, "ui.transport.duration") or {}).get("text") == "02:00"
             and (named(s, "ui.transport.position") or {}).get("text", "00:00") not in ("00:00", "--:--"))
        reveal_button("ui.live.stopPlayback")
        key("space")
        timeline = next(r for r in runner.read_state()["regions"] if r["name"] == "timeshift_timeline")
        seek(30)
        click("ui.vod.playback.openGroups")
        wait("Groups open before returning to the library", lambda s: groups_ready(s))
        click("ui.vod.backToCatalog")
        wait("Back arrow stops movie and opens library", lambda s:
             library_ready(s)
             and not named(s, "ui.transport.duration"))
        key("Escape")
        # Start a fourth film so the responsive posters ensure the
        # horizontal-wheel regression exercises actual shelf overflow.
        key("ctrl+f"); key("ctrl+a"); runner.xdotool("type", "Local Movie 04")
        wait("Fourth movie is available for shelf overflow", lambda s: any(
            i.get("text") == "Local Movie 04" for i in s["inventory"]))
        key("Down"); key("Return")
        wait("Fourth movie details open", lambda s: bool(named(s, "ui.vod.play")))
        click("ui.vod.play")
        wait("Fourth movie starts", lambda s:
             (named(s, "ui.transport.title") or {}).get("text") == "Local Movie 04"
             and (named(s, "ui.transport.duration") or {}).get("text") == "02:00")
        reveal_button("ui.live.stopPlayback");key("space")
        timeline = next(r for r in runner.read_state()["regions"] if r["name"] == "timeshift_timeline")
        seek(30);key("Left");key("Escape")
        wait("Fourth movie panels hide before keyboard Back", lambda s:
             not named(s, "ui.vod.backToCatalog") and not (named(s, "ui.live.stopPlayback") or {}).get("enabled"))
        time.sleep(.3) # Off-screen transport objects remain in the bridge inventory.
        key("BackSpace")
        wait("Fourth movie progress returns to the library", lambda s: library_ready(s) and not named(s, "ui.transport.duration"))
        key("Escape")
        key("ctrl+f"); key("ctrl+a"); key("BackSpace")
        click("ui.vod.all")
        runner.xdotool("windowsize", runner.window_id, "800", "600")
        key("ctrl+f"); key("Down"); key("Up")
        wait("Continue watching has four films in progress", lambda s:
                     bool(named(s, "ui.vod.continueTitle.3")))
        key("Right"); key("Down"); key("Up")
        wait("Shelf keyboard browsing does not open details", lambda s:
             bool(named(s, "ui.vod.continueHeading")) and not named(s, "ui.vod.play"))
        for _ in range(4): key("Left")
        state = wait("Keyboard returns to the first shelf poster", lambda s:
                     abs((named(s, "ui.vod.continueTitle.0") or {}).get("bounds", {}).get("x", -1000)
                         - (named(s, "ui.vod.allHeading") or {}).get("bounds", {}).get("x", 0)) < 1)
        first = named(state, "ui.vod.continueTitle.0")["bounds"]
        runner.xdotool("mousemove", "--window", runner.window_id,
                       str(round(first["x"] + 50)), str(round(first["y"] - 120)))
        before_wheel = named(runner.read_state(3), "ui.vod.continueTitle.0")["bounds"]["x"]
        runner.xdotool("click", "5")
        wait("Mouse wheel scrolls Continue watching right", lambda s:
             (named(s, "ui.vod.continueTitle.0") or {}).get("bounds", {}).get("x", before_wheel) < before_wheel - 10)
        runner.xdotool("click", "4")
        wait("Mouse wheel scrolls Continue watching left", lambda s:
             abs((named(s, "ui.vod.continueTitle.0") or {}).get("bounds", {}).get("x", -1000) - before_wheel) < 1)
        runner.request_capture_wait("vod-continue-wheel")
        click("ui.vod.close")
        wait("Library closes at compact size", lambda s: not named(s, "ui.vod.close"))
        runner.xdotool("key", "--window", runner.window_id, "v")
        runner.xdotool("windowsize", runner.window_id, "1600", "900")
        state = wait("Resize during opening settles the library below the title bar", lambda s:
                     library_ready(s) and s["window"]["width"] == 1600
                     and named(s, "ui.vod.close")["bounds"]["y"] < 120)
        click("ui.vod.close")
        wait("Resized library finishes closing", lambda s: not named(s, "ui.vod.close"))
        key("f")
        fullscreen_height = runner.read_state()["window"]["height"]
        runner.xdotool("key", "--delay", "1", "--window", runner.window_id, "v", "Escape")
        wait("Escape during opening preserves fullscreen and the library", lambda s:
             library_ready(s) and s["window"]["height"] == fullscreen_height)
        runner.xdotool("key", "--delay", "1", "--window", runner.window_id, "v", "Escape")
        wait("Escape during closing preserves fullscreen", lambda s:
             not named(s, "ui.vod.close") and s["window"]["height"] == fullscreen_height)
        key("f")

        # The bridge saves background snapshots during animation. Inspect them
        # after settling instead of racing slow HTTP reads against a 240 ms slide.
        snapshot_paths = sorted(
            (runner.run_dir / "state-snapshots").glob("state-*.json"),
            key=lambda p: int(p.stem.split("-")[1]))
        saw_opening = saw_closing = False
        previous = None
        for path in snapshot_paths:
            following = json.loads(path.read_text())
            current, previous = previous, following
            if current is None:
                continue
            button = named(current, "ui.vod.close")
            if not button or button.get("enabled") or current["window"]["visibleOverlay"] != "vod":
                continue
            next_button = named(following, "ui.vod.close")
            if next_button and next_button.get("enabled"):
                saw_opening |= button["bounds"]["y"] > next_button["bounds"]["y"]
            elif not next_button:
                saw_closing = True
        assert saw_opening and saw_closing, "Both slides must retain disabled library controls and VOD ownership"
        checks.append("Opening and closing retain disabled controls and VOD ownership during the slide")
        for path in runner.settings_path_candidates:
            if path.exists():
                assert not json.loads(path.read_text()).get("vodEnabled", False)
        checks.append("The legacy global technical flag stays unchanged")
        assert any(json.loads(path.read_text()).get("vodLibrarySidebarWidth") == 208
                   for path in runner.settings_path_candidates if path.exists())
        checks.append("Library sidebar preference remains persisted after browsing and playback")
        key("Left")
        click_text("Settings", "caption")
        wait("Settings opens over VOD", lambda s: s["window"]["visibleOverlay"] == "settings")
        state = wait("Title navigation is disabled in Settings", lambda s:
             named(s, "ui.navigation.live") and not named(s, "ui.navigation.live").get("enabled"))
        bounds = named(state, "ui.navigation.live")["bounds"]
        settings_size = (state["window"]["width"], state["window"]["height"])
        runner.xdotool("mousemove", "--window", runner.window_id,
                       str(round(bounds["x"] + bounds["width"] / 2)), str(round(bounds["y"] + bounds["height"] / 2)))
        runner.xdotool("click", "--repeat", "2", "--delay", "80", "1")
        state = runner.read_state()
        assert state["window"]["visibleOverlay"] == "settings"
        assert (state["window"]["width"], state["window"]["height"]) == settings_size
        checks.append("Disabled navigation does not maximize or leave Settings")
        click_text("Sources", "caption")
        select_source("Local Cinema")
        state = scroll_to("ui.sources.enableVod")
        switch = named(state, "ui.sources.enableVod")
        margin = next(i for i in state["inventory"] if i.get("text") == "Archive safety margin (minutes)")
        assert switch["enabled"] and switch["bounds"]["y"] > margin["bounds"]["y"]
        checks.append("Enable VOD follows the archive margin for a saved Xtream source")
        scroll_to("ui.sources.media.1")
        click("ui.sources.media.1")
        wait("Movie settings contain provider categories without cached titles", lambda s: any(
            i.get("text") == "Empty group" for i in s["inventory"]))
        click("ui.sources.media.2")
        wait("Series categories configure independently", lambda s: any(
            i.get("text") == "Local series" for i in s["inventory"]))
        runner.request_capture_wait("vod-source-media-groups")
        scroll_to("ui.sources.enableVod")
        click("ui.sources.enableVod")
        select_source("Local Live")
        state = scroll_to("ui.sources.enableVod")
        assert not named(state, "ui.sources.enableVod")["enabled"]
        select_source("Local Cinema")
        scroll_to("ui.sources.enableVod")
        assert not named(runner.read_state(), "ui.sources.media.1")
        checks.append("Switching sources retains the disabled Xtream draft and disables M3U VOD")
        key("Escape")
        click_text("Discard")
        wait("Discard closes source settings", lambda s: s["window"]["visibleOverlay"] == "none")
        key("Left")
        click_text("Settings", "caption")
        click_text("Sources", "caption")
        select_source("Local Cinema")
        scroll_to("ui.sources.media.1")
        assert named(runner.read_state(), "ui.sources.media.1")
        checks.append("Discard restores the saved enabled flag and all media segments")
        scroll_to("ui.sources.enableVod")
        click("ui.sources.enableVod")
        click_text("Save source changes", "caption")
        key("Escape")
        key("v")
        wait("Saved VOD opt-out removes the source from the library", lambda s: any(
            i.get("text") == "Enable VOD for an Xtream source in Settings to browse movies." for i in s["inventory"]))
        runner.request_capture_wait("vod-source-disabled")
    finally:
        (runner.run_dir / "vod-result.json").write_text(json.dumps({"passed": checks}, indent=2))
        runner.cleanup()
        server.shutdown()


if __name__ == "__main__":
    main()
