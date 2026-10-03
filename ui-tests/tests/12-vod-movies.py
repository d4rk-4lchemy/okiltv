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
            elif action == "get_vod_streams":
                result = [{"stream_id": i, "name": f"Local Movie {i:02}", "year": 2024, "category_id": "1" if i % 2 else "2"} for i in range(1, 41)]
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
        runner.xdotool("key", "--window", runner.window_id, "v", "v", "Escape", "ctrl+f", "space", "1")
        wait("V closes library without retuning", lambda s: not named(s, "ui.vod.close") and s["playback"]["currentChannel"] == channel)
        runner.xdotool("key", "--window", runner.window_id, "v", "v", "Escape", "ctrl+f", "Return")
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
        paused_position = named(runner.read_state(), "ui.transport.position")["text"]
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
             not named(s, "ui.vod.backToCatalog") and not named(s, "ui.vod.playback.search"))
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
        key("f")
        wait("Back arrow reserves the correct fullscreen top margin", lambda s:
             back_outside_panel(s, "ui.vod.playback.openGroups", "ui.vod.playback.search")
             and abs(named(s, "ui.vod.backToCatalog")["bounds"]["y"] - 24) < 1)
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
        key("ctrl+f"); key("ctrl+a"); key("BackSpace")
        click("ui.vod.all")
        runner.xdotool("windowsize", runner.window_id, "800", "600")
        key("ctrl+f"); key("Down"); key("Up")
        wait("Continue watching has three films in progress", lambda s:
                     bool(named(s, "ui.vod.continueTitle.2")))
        key("Right"); key("Down"); key("Up")
        wait("Shelf keyboard browsing does not open details", lambda s:
             bool(named(s, "ui.vod.continueHeading")) and not named(s, "ui.vod.play"))
        key("Left"); key("Left")
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
                     and named(s, "ui.vod.close")["bounds"]["y"] < 100)
        click("ui.vod.close")
        wait("Resized library finishes closing", lambda s: not named(s, "ui.vod.close"))
        key("f")
        fullscreen_height = runner.read_state()["window"]["height"]
        runner.xdotool("key", "--window", runner.window_id, "v", "Escape")
        wait("Escape during opening preserves fullscreen and the library", lambda s:
             library_ready(s) and s["window"]["height"] == fullscreen_height)
        runner.xdotool("key", "--window", runner.window_id, "v", "Escape")
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
        checks.append("Global activation remains session-only")
        assert any(json.loads(path.read_text()).get("vodLibrarySidebarWidth") == 208
                   for path in runner.settings_path_candidates if path.exists())
        checks.append("Library sidebar preference remains persisted after browsing and playback")
    finally:
        (runner.run_dir / "vod-result.json").write_text(json.dumps({"passed": checks}, indent=2))
        runner.cleanup()
        server.shutdown()


if __name__ == "__main__":
    main()
