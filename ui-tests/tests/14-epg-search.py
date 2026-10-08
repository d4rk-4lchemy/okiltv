#!/usr/bin/env python3
"""Exclusive Live EPG search using generated media and local XMLTV only."""
import datetime as dt
import functools
import http.server
import importlib.util
import json
import pathlib
import os
import subprocess
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ui_runner", ROOT / "ui-tests/linux-ui-test-runner.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class LocalRunner(module.Runner):
    def seed_settings(self):
        media = self.run_dir / "sample.ts"
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                        "-f", "lavfi", "-i", "testsrc2=size=160x90:rate=10",
                        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
                        "-t", "240", "-c:v", "mpeg2video", "-b:v", "100k",
                        "-c:a", "mp2", "-b:a", "64k", "-f", "mpegts", str(media)], check=True)
        playlist = self.run_dir / "channels.m3u"
        for i, colour in [(1, "#18a7dd"), (2, "#ee586e"), (3, "#689c60"), (4, "#b58ae5")]:
            (self.run_dir / f"logo-{i}.svg").write_text(
                '<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">'
                f'<rect x="2" y="2" width="60" height="60" rx="12" fill="{colour}"/>'
                '<path d="M14 19h24v5H20v22h-6z" fill="white"/>'
                f'<text x="40" y="47" fill="white" text-anchor="middle" '
                f'font-family="sans-serif" font-weight="bold" font-size="32">{i}</text></svg>')
        playlist.write_text("#EXTM3U\n" + "".join(
            f'#EXTINF:-1 tvg-id="{epg}" tvg-logo="http://127.0.0.1:{self.http_port}/logo-{i}.svg" '
            f'group-title="{group}",Fixture {i:02d}\n{media.as_uri()}\n'
            for i, epg, group in [(1, "shared", "First"), (2, "shared", "Second"), (3, "hidden", "Hidden")])
            + f'#EXTINF:-1 tvg-id="archive" tvg-logo="http://127.0.0.1:{self.http_port}/logo-4.svg" '
              f'group-title="Second" catchup="default" catchup-days="1" '
              f'catchup-source="http://127.0.0.1:{self.http_port}/sample.ts?utc={{utc}}",Archive fixture\n{media.as_uri()}\n')
        now = dt.datetime.now(dt.timezone.utc)
        stamp = lambda value: value.strftime("%Y%m%d%H%M%S +0000")
        def programme(channel, start, stop, title):
            return (f'<programme channel="{channel}" start="{stamp(start)}" stop="{stamp(stop)}">'
                    f'<title>{title}</title><sub-title>Oceany</sub-title>'
                    '<desc>Local description with &lt; and &amp; characters. '
                    + ('A long description. ' * 50) + '</desc></programme>')
        entries = [programme("shared", now-dt.timedelta(hours=1), now+dt.timedelta(hours=1), "Planeta Ziemia"),
                   programme("shared", now-dt.timedelta(hours=3), now-dt.timedelta(hours=2), "Łódź nocą"),
                   programme("hidden", now-dt.timedelta(hours=1), now+dt.timedelta(hours=1), "Planeta Hidden")]
        entries += [programme("archive", now-dt.timedelta(minutes=20), now-dt.timedelta(minutes=10), "Archive Search")]
        if os.environ.get("OKILTV_EPG_SEARCH_SMOKE") == "punctuation":
            entries += [programme("shared", now+dt.timedelta(minutes=30), now+dt.timedelta(minutes=90), "Spider-Man")]
        entries += [programme("shared", now+dt.timedelta(hours=i+2), now+dt.timedelta(hours=i+3),
                              f"Planeta Ziemia {i:02d}") for i in range(60)]
        (self.run_dir / "epg.xml").write_text('<tv>' + ''.join(entries) + '</tv>')
        profile = "11111111-1111-1111-1111-111111111111"
        settings = {"activeProfileId": profile,
                    "profiles": [{"id": profile, "name": "Search fixture", "type": 2,
                                  "m3UFilePath": str(playlist), "isActive": True,
                                  "xmltvUrl": f"http://127.0.0.1:{self.http_port}/epg.xml"},
                                 {"id": "22222222-2222-2222-2222-222222222222", "name": "Other source",
                                  "type": 2, "m3UFilePath": str(playlist), "isActive": False}],
                    "hiddenGroupsByProfile": {profile: ["Hidden"]},
                    "overlayAutoHide": True, "overlayAutoHideSeconds": 2,
                    "autoRefreshEpg": True, "minimizeToTrayOnMinimize": False,
                    "playerDeinterlaceEnabled": False, "multiviewEnabled": True,
                    "mpvOptions": {"ao": "null", "hwdec": "no"}}
        for path in self.settings_path_candidates:
            path.write_text(json.dumps(settings))


def main():
    module.ensure_dbus_session()
    args = module.parse_args()
    args.test_name = "epg-search"
    runner = LocalRunner(args)
    runner.prepare_dirs()
    smoke = os.environ.get("OKILTV_EPG_SEARCH_SMOKE", "")
    epg_release = threading.Event()

    class FixtureHandler(http.server.SimpleHTTPRequestHandler):
        def do_GET(self):
            if smoke == "preparing" and self.path.split("?", 1)[0] == "/epg.xml":
                epg_release.wait(60)
            super().do_GET()

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0),
        functools.partial(FixtureHandler, directory=str(runner.run_dir)))
    runner.http_port = server.server_port
    threading.Thread(target=server.serve_forever, daemon=True).start()
    checks = []

    def wait(label, predicate, timeout=15):
        deadline = time.monotonic() + timeout
        state = {}
        while time.monotonic() < deadline:
            state = runner.read_state(3)
            if predicate(state):
                runner.log("PASS: " + label)
                checks.append(label)
                return state
            time.sleep(.1)
        runner.save_state_snapshot("failure", state)
        raise AssertionError(label)

    def key(value):
        runner.xdotool("key", "--window", runner.window_id, value)
        time.sleep(.15)

    def search(state):
        return state.get("epgSearch", {})

    def opened(state):
        return state["window"]["visibleOverlay"] == "epg-search" and search(state).get("active")

    def playing(state):
        return state["playback"]["currentChannel"].get("id")

    def query(text):
        key("ctrl+f")
        runner.xdotool("type", "--window", runner.window_id, text)
        return wait("current results for " + text,
                    lambda s: search(s).get("resultsCurrent") and search(s).get("query") == text)

    def capture_query_limit_error():
        before = runner.read_state(3)
        full_query = "p" * 257
        key("ctrl+f")
        runner.xdotool("type", "--window", runner.window_id, full_query)
        wait("257-character query retained with explicit limit error", lambda s:
             search(s).get("query") == full_query and search(s).get("status") == "error"
             and not search(s).get("resultsCurrent") and "256" in search(s).get("errorText", ""))
        runner.request_capture_wait("08-query-limit-error")
        key("Return"); key("ctrl+r"); key("ctrl+d")
        wait("limit error fences playback and recording actions", lambda s: opened(s)
             and search(s).get("query") == full_query and playing(s) == playing(before)
             and s["playback"]["isRecording"] == before["playback"]["isRecording"])

    def selected_channel(state):
        return next((e["value"].get("id") for e in state["elements"] if e["id"] == "left.selection"), None)

    def tune_named(name, channel_id):
        key("Escape"); key("Tab")
        wait("channel editor for " + name, lambda s: s["multiview"]["searchFocused"])
        key("ctrl+a")
        runner.xdotool("type", "--window", runner.window_id, name)
        key("Down")
        wait("channel selected: " + name, lambda s: selected_channel(s) == channel_id)
        key("Return")

    def control(state, name):
        return next((item for item in state["inventory"] if item["objectName"] == name
                     and item.get("enabled", False) and "bounds" in item), None)

    def click_control(name):
        state = wait("control available: " + name, lambda s: control(s, name) is not None
                     and 0 <= control(s, name)["bounds"]["y"] < s["window"]["height"])
        bounds = control(state, name)["bounds"]
        runner.xdotool("mousemove", "--window", runner.window_id,
                       str(round(bounds["x"] + bounds["width"] / 2)),
                       str(round(bounds["y"] + bounds["height"] / 2)))
        runner.xdotool("click", "1")

    def capture_results_with_logos():
        state = wait("logo result geometry ready", lambda s: search(s).get("resultsCurrent")
                     and not search(s).get("detailsBusy")
                     and len(search(s).get("rows", [])) >= 2
                     and all(control(s, "ui.epgSearch.result." + row["resultKey"]) is not None
                             for row in search(s)["rows"][:2]))
        capture = runner.request_capture_wait("02-results")
        assert capture is not None and capture.exists()
        # Check pixels in the actual rendered channel logos, rather than only
        # checking that the model has a logo URL. Both fixture logos are opaque.
        for row in search(state)["rows"][:2]:
            bounds = control(state, "ui.epgSearch.result." + row["resultKey"])["bounds"]
            x, y = round(bounds["x"] + 18), round(bounds["y"] + 20)
            pixel = subprocess.check_output(["convert", str(capture), "-crop", f"1x1+{x}+{y}",
                                             "-depth", "8", "rgb:-"])
            expected = bytes((24, 167, 221) if row["channelName"] == "Fixture 01" else (238, 88, 110))
            assert pixel == expected, f"Channel logo missing for {row['channelName']}: {pixel!r}"
        runner.log("PASS: both station logos rendered beside result text")
        checks.append("both station logos rendered beside result text")

    def click_result(index, double=False):
        state = wait("pointer result details ready", lambda s: opened(s)
                     and search(s).get("resultsCurrent") and not search(s).get("detailsBusy")
                     and len(search(s).get("rows", [])) > index)
        target_key = search(state)["rows"][index]["resultKey"]
        state = wait("pointer result geometry ready", lambda s:
                     control(s, "ui.epgSearch.result." + target_key) is not None
                     and not control(s, "ui.epgSearch.result." + target_key).get("scrollMoving"))
        bounds = control(state, "ui.epgSearch.result." + target_key)["bounds"]
        runner.xdotool("mousemove", "--window", runner.window_id,
                       str(round(bounds["x"] + 20)), str(round(bounds["y"] + 20)))
        if double:
            runner.xdotool("click", "--repeat", "2", "--delay", "100", "1")
        else:
            runner.xdotool("click", "1")

    def double_click_result(index):
        click_result(index, double=True)

    try:
        runner.seed_settings()
        runner.launch_stack()
        runner.wait_for_bridge(60)
        runner.window_id = runner.find_largest_window(60)
        runner.start_sse()
        runner.tune_channel(1)
        wait("initial channel", lambda s: playing(s) == 0, 30)
        if smoke == "preparing":
            key("Escape"); key("ctrl+f")
            wait("search opens during first local XMLTV import", opened)
            runner.xdotool("type", "--window", runner.window_id, "planet")
            wait("first XMLTV import exposes preparing", lambda s: search(s).get("status") == "preparing"
                 and search(s).get("query") == "planet" and not search(s).get("resultsCurrent"))
            runner.request_capture_wait("08-preparing-first-import")
            epg_release.set()
            wait("first XMLTV import publishes current results", lambda s: search(s).get("resultsCurrent")
                 and search(s).get("query") == "planet" and len(search(s).get("rows", [])) > 0, 30)
            runner.request_capture_wait("09-preparing-ready")
            capture_query_limit_error()
            query("planet")
            key("Escape")
            wait("first import search leaves playback unchanged", lambda s:
                 s["window"]["visibleOverlay"] == "none" and playing(s) == 0)
            return
        if smoke == "punctuation":
            key("Escape"); key("ctrl+f")
            wait("search opens for punctuation prefixes", opened)
            # xdotool drops Unicode dash keysyms in this Xvfb keyboard map;
            # the store suite covers Unicode dashes and accent normalization.
            for text in ["Spider-M", "Spider.M", "Spider (M)", '"Spider-M"']:
                state = query(text)
                assert len(search(state).get("rows", [])) == 2
                assert all(row["title"] == "Spider-Man" for row in search(state)["rows"])
                checks.append("punctuation prefix matches Spider-Man: " + text)
            runner.request_capture_wait("10-punctuation-prefix")
            state = query("Spider-X")
            assert len(search(state).get("rows", [])) == 0
            key("Escape")
            wait("punctuation search preserves playback", lambda s:
                 s["window"]["visibleOverlay"] == "none" and playing(s) == 0)
            return
        if smoke == "actions":
            key("Escape"); key("ctrl+f")
            wait("search opens for action shortcuts", opened)
            query("archive search")
            wait("unwatched archive hides redundant restart", lambda s: not search(s).get("detailsBusy")
                 and search(s).get("actions", {}).get("actionKind") == "catchup"
                 and not search(s).get("actions", {}).get("fromBeginningEnabled")
                 and control(s, "ui.epgSearch.beginning") is None
                 and control(s, "ui.epgSearch.recording") is None)
            for shortcut in ["ctrl+Return", "ctrl+KP_Enter"]:
                key("ctrl+f")
                query("planet ziem")
                wait("default Live action ready", lambda s: not search(s).get("detailsBusy")
                     and search(s).get("actions", {}).get("primaryEnabled"))
                key(shortcut)
                wait(shortcut + " falls back to primary action", lambda s: playing(s) == 0
                     and s["window"]["visibleOverlay"] == "none")
            return
        if smoke:
            # The initial render surface is created after the first Live load.
            # Retune so bridge checks include measured video metadata. Xvfb
            # captures may still show black video; metadata does not prove
            # visible frame delivery.
            tune_named("Fixture 02", 1)
            wait("fixture render surface primed", lambda s: playing(s) == 1)
            tune_named("Fixture 01", 0)
            wait("fixture video metadata available", lambda s: playing(s) == 0
                 and s["playback"]["debugOverlay"].get("videoCodec") not in [None, "N/A"], 30)
            key("Escape")
            runner.request_capture_wait("00-video-metadata-live")
            if smoke == "pointer":
                key("ctrl+f")
                wait("search opens for pointer smoke", opened)
                query("planet ziem")
                capture_results_with_logos()
                click_result(0)
                wait("selected row collapses without tuning", lambda s: opened(s) and playing(s) == 0
                     and control(s, "ui.epgSearch.primary") is None)
                click_result(0)
                wait("selected row click reopens details", lambda s: opened(s) and playing(s) == 0
                     and control(s, "ui.epgSearch.primary") is not None)
                double_click_result(0)
                wait("expanded selected row double click activates", lambda s: playing(s) == 0
                     and s["window"]["visibleOverlay"] == "none")
                key("ctrl+f")
                query("planet ziem")
                click_result(0)
                wait("selected row collapses again", lambda s: opened(s)
                     and control(s, "ui.epgSearch.primary") is None)
                double_click_result(0)
                wait("collapsed selected row double click activates", lambda s: playing(s) == 0
                     and s["window"]["visibleOverlay"] == "none")
                key("ctrl+f")
                query("planet ziem")
                double_click_result(1)
                wait("double click survives inline layout change", lambda s: playing(s) == 1
                     and s["window"]["visibleOverlay"] == "none")
                return
            if smoke == "scale":
                factor = float(os.environ.get("QT_SCALE_FACTOR", "1"))
                key("ctrl+f")
                wait("scaled search query focus", lambda s: opened(s)
                     and s["window"]["activeFocusObject"] == "ui.epgSearch.query")
                query("planet ziem")
                for width, height in [(800, 500), (426, 240)]:
                    runner.xdotool("windowsize", runner.window_id, str(round(width * factor)), str(round(height * factor)))
                    state = wait(f"scaled layout {width}x{height}", lambda s: s["window"]["width"] == width
                                 and s["window"]["height"] == height and control(s, "ui.epgSearch.close") is not None)
                    for name in ["ui.epgSearch.close", "ui.epgSearch.query"]:
                        bounds = control(state, name)["bounds"]
                        assert 0 <= bounds["x"] and bounds["x"] + bounds["width"] <= width + 1
                        assert 0 <= bounds["y"] and bounds["y"] + bounds["height"] <= height + 1
                    runner.request_capture_wait(f"05-scale-{factor}-{width}x{height}")
                key("Escape")
                wait("scaled Escape keeps channel", lambda s: s["window"]["visibleOverlay"] == "none" and playing(s) == 0)
            else:
                def retained(s):
                    return (s["multiview"]["mode"], s["multiview"]["focused"],
                            tuple((t["index"], t["channelId"], t["paused"], t["renderMatchesBackend"])
                                  for t in s["multiview"]["tiles"]))
                key("Left"); key("Down"); key("ctrl+p")
                wait("PiP has two assigned streams", lambda s: s["multiview"]["mode"] == "pip"
                     and {t["channelId"] for t in s["multiview"]["tiles"]} == {0, 1})
                key("Escape")
                state = runner.read_state(3)
                runner.xdotool("mousemove", "--window", runner.window_id, str(round(state["window"]["width"] * .85)), str(round(state["window"]["height"] * .85)))
                time.sleep(.5)
                key("Escape")
                runner.xdotool("click", "1")
                wait("PiP secondary focused", lambda s: s["multiview"]["focused"] == 1)
                key("space")
                state = wait("PiP secondary paused", lambda s: s["multiview"]["tiles"][1]["paused"])
                expected = retained(state)
                key("ctrl+f")
                wait("PiP preserved while search open", lambda s: opened(s) and retained(s) == expected)
                query("planet ziem")
                runner.request_capture_wait("06-pip-search")
                key("Escape")
                wait("PiP preserved after search close", lambda s: s["window"]["visibleOverlay"] == "none" and retained(s) == expected)
                key("ctrl+p")
                wait("PiP closed explicitly", lambda s: s["multiview"]["mode"] == "off")
                key("ctrl+o")
                wait("grid opened", lambda s: s["multiview"]["available"])
                key("ctrl+Right")
                wait("grid secondary focused", lambda s: s["multiview"]["focused"] == 1)
                tune_named("Fixture 01", 0)
                wait("grid secondary assigned", lambda s: s["multiview"]["tiles"][1]["channelId"] == 0)
                key("Escape"); key("space")
                state = wait("grid secondary paused", lambda s: s["multiview"]["tiles"][1]["paused"])
                expected = retained(state)
                runner.xdotool("keydown", "--window", runner.window_id, "Control_L")
                key("Left")
                wait("grid candidate previews other tile", lambda s: s["multiview"]["selecting"] and s["multiview"]["candidate"] == 0)
                key("f")
                runner.xdotool("keyup", "--window", runner.window_id, "Control_L")
                wait("search cancels grid candidate without committing", lambda s: opened(s)
                     and not s["multiview"]["selecting"] and retained(s) == expected)
                key("Escape")
                wait("grid preserved after search close", lambda s: s["window"]["visibleOverlay"] == "none" and retained(s) == expected)
                key("ctrl+o")
                wait("grid closed explicitly", lambda s: s["multiview"]["mode"] == "off")
                key("ctrl+s")
                wait("source picker open", lambda s: s["multiview"]["pickerOpen"])
                key("Down"); key("Down"); key("Return")
                wait("second source picker confirmed", lambda s: not s["multiview"]["pickerOpen"], 30)
                key("Escape"); key("ctrl+f")
                wait("search opens for second source", opened)
                key("ctrl+f")
                runner.xdotool("type", "--window", runner.window_id, "planet")
                wait("source without local EPG is explicit", lambda s: search(s).get("status") == "no-epg" and not search(s)["rows"])
                runner.request_capture_wait("07-no-epg")
                capture_query_limit_error()
                key("ctrl+f")
                runner.xdotool("type", "--window", runner.window_id, "planet")
                wait("valid query recovers from limit error", lambda s: search(s).get("status") == "no-epg"
                     and search(s).get("query") == "planet" and not search(s).get("errorText"))
            return
        key("Escape")
        key("Tab")
        wait("Tab keeps channel search", lambda s: s["multiview"]["searchFocused"])
        runner.xdotool("type", "--window", runner.window_id, "Fixture 01")
        wait("channel filter entered", lambda s: any(e["id"] == "left.search" and e["value"] == "Fixture 01"
                                                    for e in s["elements"]))
        key("ctrl+f")
        wait("Ctrl+F opens from channel editor", lambda s: opened(s)
             and s["window"]["activeFocusObject"] == "ui.epgSearch.query")
        initial = runner.read_state(3)
        query_bounds = control(initial, "ui.epgSearch.query")["bounds"]
        close_bounds = control(initial, "ui.epgSearch.close")["bounds"]
        assert control(initial, "ui.epgSearch.filter.all") is None
        assert control(initial, "ui.epgSearch.primary") is None
        assert abs(close_bounds["x"] - query_bounds["x"] - query_bounds["width"] - 12) <= 1
        assert abs(close_bounds["y"] - query_bounds["y"]) <= 1, (close_bounds, query_bounds)
        runner.request_capture_wait("01-empty")
        state = query("planet ziem")
        assert control(state, "ui.epgSearch.query")["bounds"] == query_bounds
        wait("selected result expands inline", lambda s: control(s, "ui.epgSearch.primary") is not None)
        key("ctrl+f"); key("BackSpace")
        state = wait("clearing restores only the query", lambda s: opened(s) and search(s).get("query") == ""
                     and control(s, "ui.epgSearch.filter.all") is None
                     and control(s, "ui.epgSearch.primary") is None)
        assert control(state, "ui.epgSearch.query")["bounds"] == query_bounds
        state = query("planet ziem")
        assert len(search(state)["rows"]) == 50 and search(state)["hasMore"]
        assert {r["channelName"] for r in search(state)["rows"]} == {"Fixture 01", "Fixture 02"}
        wait("selection starts at current airing", lambda s: search(s)["rows"][0]["sectionKey"] == "now")
        capture_results_with_logos()
        key("Down")
        state = wait("Down selects without tuning", lambda s: search(s)["selectedIndex"] == 1 and playing(s) == 0)
        selected = search(state)["selectedKey"]
        key("ctrl+f")
        wait("Ctrl+F refocuses and preserves selection", lambda s: search(s)["selectedKey"] == selected
             and s["window"]["activeFocusObject"] == "ui.epgSearch.query")
        key("shift+Tab")
        wait("Shift+Tab leaves editor inside search", lambda s:
             s["window"]["activeFocusObject"].startswith("ui.epgSearch.")
             and s["window"]["activeFocusObject"] != "ui.epgSearch.query")
        for shortcut in ["f", "m", "v", "b", "F1", "F2", "F3", "F6", "ctrl+p", "ctrl+o", "ctrl+g", "ctrl+s", "ctrl+Right"]:
            key(shortcut)
        wait("global playback shortcuts blocked outside editor", lambda s: opened(s) and playing(s) == 0
             and s["multiview"]["mode"] == "off")
        key("ctrl+f")
        for expected in ["now", "upcoming", "past", "all"] * 2:
            key("Tab")
            wait("Tab cycles time filter to " + expected, lambda s:
                 search(s).get("resultsCurrent") and search(s)["timeFilter"] == expected
                 and search(s)["query"] == "planet ziem"
                 and s["window"]["activeFocusObject"] == "ui.epgSearch.query" and playing(s) == 0)
        key("ctrl+Tab")
        wait("Ctrl+Tab does not cycle time filters", lambda s:
             search(s)["timeFilter"] == "all" and s["window"]["activeFocusObject"] == "ui.epgSearch.query")
        key("ctrl+f")
        runner.xdotool("type", "--window", runner.window_id, "x")
        runner.xdotool("key", "--window", runner.window_id, "Return", "ctrl+r", "ctrl+d")
        wait("edit invalidates old actions", lambda s: opened(s) and playing(s) == 0
             and search(s).get("query") == "x" and not s["playback"]["isRecording"])
        state = query("lodz")
        assert len(search(state)["rows"]) == 2 and all(r["sectionKey"] == "past" for r in search(state)["rows"])
        key("Return")
        wait("unavailable past airing keeps playback", lambda s: opened(s) and playing(s) == 0)
        state = query("planet hidden")
        assert not search(state)["rows"], "Hidden group leaked into search"
        runner.request_capture_wait("03-no-results")
        query("archive search")
        wait("archive download enabled", lambda s: search(s).get("actions", {}).get("downloadEnabled"))
        wait("unwatched archive has only the default Play action", lambda s: not search(s).get("detailsBusy")
             and control(s, "ui.epgSearch.primary") is not None
             and not search(s).get("actions", {}).get("fromBeginningEnabled")
             and control(s, "ui.epgSearch.beginning") is None
             and control(s, "ui.epgSearch.recording") is None)
        key("ctrl+d")
        deadline = time.monotonic() + 10
        dialog_id = None
        while time.monotonic() < deadline:
            output = subprocess.run(["xdotool", "search", "--onlyvisible", "--name", "Download programme"],
                                    env=runner.display_env(), capture_output=True, text=True)
            if output.stdout.strip():
                dialog_id = output.stdout.strip().splitlines()[-1]
                break
            time.sleep(.1)
        assert dialog_id, "Search Ctrl+D did not open existing save dialog"
        runner.xdotool("windowactivate", "--sync", dialog_id)
        runner.xdotool("key", "Escape")
        wait("download cancellation restores same search result", lambda s: opened(s)
             and search(s)["query"] == "archive search"
             and s["window"]["activeFocusObject"] == "ui.epgSearch.query")
        query("planet ziem")
        click_control("ui.epgSearch.filter.upcoming")
        state = wait("upcoming filter", lambda s: search(s).get("resultsCurrent") and search(s)["timeFilter"] == "upcoming")
        assert all(r["sectionKey"] == "upcoming" for r in search(state)["rows"])
        key("ctrl+f")
        key("Return")
        wait("future primary action keeps current channel", lambda s: opened(s) and playing(s) == 0)
        if control(runner.read_state(3), "ui.epgSearch.back"):
            click_control("ui.epgSearch.back")
        click_control("ui.epgSearch.filter.all")
        wait("all filter restored", lambda s: search(s).get("resultsCurrent") and search(s)["timeFilter"] == "all")
        # Real pointer/wheel input must not reveal chrome or close the search.
        runner.xdotool("mousemove", "--window", runner.window_id, "700", "450")
        runner.xdotool("click", "5")
        time.sleep(3)
        wait("exclusive overlay survives activity and auto-hide", opened)
        for width, height in [(1920, 1080), (1280, 720), (800, 600), (426, 240)]:
            runner.xdotool("windowsize", runner.window_id, str(width), str(height))
            state = wait(f"layout {width}x{height}", lambda s: s["window"]["width"] == width
                         and s["window"]["height"] == height and control(s, "ui.epgSearch.close") is not None)
            for name in ["ui.epgSearch.close", "ui.epgSearch.query"]:
                bounds = control(state, name)["bounds"]
                assert 0 <= bounds["x"] < width and 0 <= bounds["y"] < height
                assert bounds["x"] + bounds["width"] <= width + 1
                assert bounds["y"] + bounds["height"] <= height + 1
            runner.request_capture_wait(f"04-layout-{width}x{height}")
        click_control("ui.epgSearch.close")
        wait("X closes without changing playback", lambda s: s["window"]["visibleOverlay"] == "none" and playing(s) == 0)
        key("ctrl+f")
        wait("reopening restores empty query", lambda s: opened(s) and search(s).get("query") == "")
        key("Escape")
        wait("one Escape closes", lambda s: s["window"]["visibleOverlay"] == "none")
        runner.xdotool("windowsize", runner.window_id, "1280", "720")
        key("f")
        wait("fullscreen enabled", lambda s: s["window"]["fullscreen"])
        key("ctrl+f")
        wait("search over fullscreen", opened)
        key("Escape")
        wait("Escape preserves fullscreen", lambda s: s["window"]["fullscreen"]
             and s["window"]["visibleOverlay"] == "none")
        key("f")
        key("Tab")
        wait("channel filter preserved after close", lambda s: s["multiview"]["searchFocused"]
             and any(e["id"] == "left.search" and e["value"] == "Fixture 01" for e in s["elements"]))
        key("ctrl+f")
        query("planet ziem")
        key("Down")
        wait("outside-filter target details ready", lambda s: search(s)["selectedIndex"] == 1
             and not search(s)["detailsBusy"] and search(s).get("actions", {}).get("primaryEnabled"))
        key("ctrl+Return")
        wait("explicit channel outside filter activates", lambda s: playing(s) == 1
             and s["window"]["visibleOverlay"] == "none")
        key("ctrl+f")
        state = query("planet ziem")
        double_click_result(1)
        wait("double click survives inline layout change", lambda s: playing(s) == 1
             and s["window"]["visibleOverlay"] == "none")
        key("ctrl+Up")
        wait("Guide open", lambda s: s["window"]["visibleOverlay"] == "guide")
        key("ctrl+f")
        wait("Guide retains Ctrl+F context", lambda s: s["window"]["visibleOverlay"] == "guide")
        key("Escape")
    finally:
        epg_release.set()
        (runner.run_dir / "epg-search-result.json").write_text(json.dumps({"passed": checks, "smokeMode": os.environ.get("OKILTV_EPG_SEARCH_SMOKE", ""),
            "qtScaleFactor": os.environ.get("QT_SCALE_FACTOR", "1")}, indent=2))
        runner.cleanup()
        server.shutdown()


if __name__ == "__main__":
    main()
