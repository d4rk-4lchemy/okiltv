#!/usr/bin/env python3
"""Pointer selection regression using local media/EPG and the existing UI runner.

Run: python3 ui-tests/tests/05-channel-selection.py --app-bin <OKILTV>
No provider credentials are used. Requires the runner's Xvfb/Openbox/xdotool stack
and ffmpeg. Artifacts and isolated settings are kept in --run-dir.
"""
import datetime as dt
import functools
import http.server
import importlib.util
import json
import pathlib
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
        subprocess.run([
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
            "-f", "lavfi", "-i", "testsrc2=size=160x90:rate=10",
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
            "-t", "240", "-c:v", "mpeg2video", "-b:v", "100k",
            "-c:a", "mp2", "-b:a", "64k", "-f", "mpegts", str(media),
        ], check=True)
        playlist = self.run_dir / "channels.m3u"
        playlist.write_text("#EXTM3U\n" + "".join(
            f'#EXTINF:-1 tvg-id="fixture{i}" group-title="Fixture",Fixture {i:02d}\n{media.as_uri()}\n'
            for i in range(1, 31)
        ))
        now = dt.datetime.now(dt.timezone.utc)
        stamp = lambda t: t.strftime("%Y%m%d%H%M%S +0000")
        (self.run_dir / "epg.xml").write_text('<tv>' + ''.join(
            f'<channel id="fixture{i}"><display-name>Fixture {i:02d}</display-name></channel>'
            f'<programme channel="fixture{i}" start="{stamp(now-dt.timedelta(hours=1))}" '
            f'stop="{stamp(now+dt.timedelta(hours=1))}"><title>Programme {i:02d}</title></programme>'
            for i in range(1, 31)
        ) + '</tv>')
        profile = "11111111-1111-1111-1111-111111111111"
        settings = {
            "activeProfileId": profile,
            "profiles": [{"id": profile, "name": "Local fixture", "type": 2,
                          "m3UFilePath": str(playlist), "isActive": True,
                          "xmltvUrl": f"http://127.0.0.1:{self.http_port}/epg.xml"}],
            "overlayAutoHide": True, "overlayAutoHideSeconds": 2,
            "autoRefreshEpg": True, "minimizeToTrayOnMinimize": False,
            "playerDeinterlaceEnabled": False, "multiviewEnabled": True,
            "mpvOptions": {"ao": "null"},
        }
        settings["profiles"].append({
            **settings["profiles"][0],
            "id": "22222222-2222-2222-2222-222222222222",
            "name": "Second local fixture", "isActive": False,
        })
        for path in self.settings_path_candidates:
            path.write_text(json.dumps(settings))


def main():
    module.ensure_dbus_session()
    args = module.parse_args()
    args.test_name = "channel-selection"
    runner = LocalRunner(args)
    runner.prepare_dirs()
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(runner.run_dir))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    runner.http_port = server.server_port
    threading.Thread(target=server.serve_forever, daemon=True).start()
    checks = []

    def element(state, name):
        return next(e for e in state["elements"] if e["id"] == name)

    def selected(state):
        return element(state, "left.selection")["value"].get("id")

    def playing(state):
        return state["playback"]["currentChannel"].get("id")

    def wait(label, predicate, timeout=8):
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
        time.sleep(.35)

    def move(x, y):
        runner.xdotool("mousemove", "--window", runner.window_id, str(round(x)), str(round(y)))
        time.sleep(.3)

    def row(number):
        # A keyboard tune hides chrome. Reveal it explicitly and wait for the
        # slide to finish before sending the hover/click to a row: pointer
        # activity during the transition is deliberately ignored by the app.
        state = runner.read_state()
        if not visible(state):
            move(700, 450)
            runner.xdotool("click", "1")
            state = wait("channel panel ready for pointer input", visible)
        bounds = next(r for r in state["regions"] if r["name"] == "left_pane")
        move(100, bounds["y"] + 16 + 42 + 8 + 31 + (number - 1) * 64)

    def outside():
        move(710, 450)
        move(700, 450)

    def click(count=1):
        runner.xdotool("click", "--repeat", str(count), "--delay", "80", "1")
        time.sleep(.35)

    def visible(state):
        return 0 <= element(state, "left.selection")["bounds"]["x"] < 10

    def epg(state, number):
        # The bridge's right.now describes playback; inspect the actual rendered
        # right rail text to verify the browse model instead.
        return any(i["text"] == f"Programme {number:02d}" and i.get("bounds", {}).get("x", 0) > 700
                   for i in state["inventory"])

    try:
        runner.seed_settings()
        runner.launch_stack()
        runner.wait_for_bridge(60)
        runner.window_id = runner.find_largest_window(60)
        runner.start_sse()
        runner.tune_channel(1)
        wait("initial playback", lambda s: playing(s) == 0, 30)
        key("Escape")
        outside()
        row(2)
        wait("hover previews B without tuning", lambda s: selected(s) == 1 and playing(s) == 0 and epg(s, 2), 30)
        outside()
        wait("unpin exit restores A", lambda s: selected(s) == 0 and epg(s, 1))
        row(2)
        click()
        row(3)
        wait("pinned B permits temporary C", lambda s: selected(s) == 2 and playing(s) == 0)
        pane = next(r for r in runner.read_state()["regions"] if r["name"] == "left_pane")
        move(100, pane["y"] + 30)
        wait("moving to search stays inside panel", lambda s: selected(s) == 2)
        outside()
        wait("exit restores pinned B", lambda s: selected(s) == 1 and epg(s, 2))
        time.sleep(3)
        wait("click locks auto-hide", lambda s: visible(s) and playing(s) == 0)
        row(2)
        click()
        wait("separated second click does not tune", lambda s: playing(s) == 0)
        activation_count = (runner.run_dir / "app.log").read_text().count("Activating channel Fixture 02")
        click(2)
        wait("double click tunes B and closes", lambda s: playing(s) == 1 and not visible(s))
        time.sleep(.5)
        assert (runner.run_dir / "app.log").read_text().count("Activating channel Fixture 02") == activation_count + 1
        checks.append("double click activates exactly once")
        outside()
        row(1)
        click()
        row(3)
        key("Return")
        wait("Enter tunes hovered C and closes", lambda s: playing(s) == 2 and not visible(s))
        outside()
        row(2)
        click()
        row(1)
        key("ctrl+Up")
        wait("Guide receives pinned B", lambda s: element(s, "guide.selection")["value"]["selectedChannel"].get("id") == 1)
        state = runner.read_state()
        guide_channel_bounds = next(
            item["bounds"] for item in state["inventory"]
            if item["text"] == "Fixture 02" and item.get("bounds", {}).get("x", 9999) < 400)
        # The Guide channel column is visually sticky while its text remains a
        # child of horizontally scrolled content, so inventory X can be negative.
        move(100, guide_channel_bounds["y"] + guide_channel_bounds["height"] / 2)
        runner.xdotool("click", "3")
        wait("Guide right click opens PiP and closes overlay", lambda s:
             s["window"]["visibleOverlay"] == "none" and playing(s) == 2)
        key("ctrl+p")
        outside()
        wait("Guide PiP exit restores playback C", lambda s: selected(s) == 2)
        time.sleep(3)
        wait("Guide cleared auto-hide lock", lambda s: not visible(s))
        outside()
        row(2)
        click()
        state = runner.read_state()
        settings_bounds = next(i["bounds"] for i in state["inventory"] if i["text"] == "Settings" and i["property"] == "caption")
        move(settings_bounds["x"] + settings_bounds["width"] / 2,
             settings_bounds["y"] + settings_bounds["height"] / 2)
        click()
        wait("Settings opens from pinned chrome", lambda s: s["window"]["visibleOverlay"] == "settings")
        key("Escape")
        outside()
        time.sleep(3)
        wait("Settings clears mouse lock", lambda s: not visible(s))
        key("Left")
        key("Up")
        key("Return")
        wait("keyboard-only activation", lambda s: playing(s) == 1 and not visible(s))
        outside()
        row(3)
        click()
        runner.xdotool("click", "--repeat", "5", "--delay", "10", "5")
        runner.xdotool("mousemove", "--window", runner.window_id, "700", "450")
        time.sleep(.6)
        wait("exit during scroll restores pin", lambda s: selected(s) == 2 and playing(s) == 1)
        key("Escape")
        wait("Escape closes pinned chrome", lambda s: not visible(s) and playing(s) == 1)
        outside()
        time.sleep(3)
        wait("Escape cleared auto-hide lock", lambda s: not visible(s))
        outside()
        row(3)
        runner.xdotool("click", "3")
        wait("right click opens PiP and closes chrome", lambda s: playing(s) == 1 and not visible(s))
        key("Left")
        key("Down")
        key("ctrl+p")
        time.sleep(1)
        key("ctrl+shift+p")
        wait("PiP selection and swap", lambda s: playing(s) == 2)
        key("Escape")
        key("ctrl+p")
        wait("PiP close preserves primary", lambda s: playing(s) == 2)
        key("Escape")
        outside()
        row(2)
        click()
        key("ctrl+s")
        row(2)  # Source picker uses the same search/row geometry.
        click()
        wait("source switch with overlapping channel IDs", lambda s:
             "22222222" in s["playback"]["currentChannel"].get("profileId", ""), 20)
        outside()
        time.sleep(3)
        wait("source switch clears old mouse lock", lambda s: not visible(s))
        key("ctrl+f")
        wait("search opens from video-only mode", visible)
        runner.xdotool("type", "--window", runner.window_id, "Fixture 02")
        wait("search receives typed text", lambda s: element(s, "left.search")["value"] == "Fixture 02")
        key("Down")
        key("Return")
        wait("search and keyboard confirmation", lambda s: playing(s) == 1 and not visible(s))
        key("ctrl+g")
        runner.xdotool("type", "--window", runner.window_id, "Fixture")
        time.sleep(.3)
        row(1)
        click()
        wait("group picker still confirms with one click", lambda s: playing(s) == 1 and any(
            i["text"] == "Search channels" and 0 <= i.get("bounds", {}).get("x", -1) < 340
            for i in s["inventory"]))
    finally:
        (runner.run_dir / "selection-result.json").write_text(json.dumps({"passed": checks}, indent=2))
        runner.cleanup()
        server.shutdown()


if __name__ == "__main__":
    main()
