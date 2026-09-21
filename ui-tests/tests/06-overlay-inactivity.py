#!/usr/bin/env python3
"""Live overlay inactivity regression; reuses the local media fixture from test 05."""
import functools
import http.server
import importlib.util
import json
import pathlib
import threading
import time

spec = importlib.util.spec_from_file_location("selection_fixture", pathlib.Path(__file__).with_name("05-channel-selection.py"))
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
module = fixture.module


def main():
    module.ensure_dbus_session()
    args = module.parse_args()
    args.test_name = "overlay-inactivity"
    runner = fixture.LocalRunner(args)
    runner.prepare_dirs()
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(runner.run_dir))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    runner.http_port = server.server_port
    threading.Thread(target=server.serve_forever, daemon=True).start()
    checks = []

    def wait(label, predicate, timeout=12):
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
        runner.xdotool("mousemove", "--window", runner.window_id, str(x), str(y))
        time.sleep(.3)

    def visible(state):
        return any(r["name"] == "left_pane" and 0 <= r["x"] < 10 for r in state["regions"])

    try:
        runner.seed_settings()
        for path in runner.settings_path_candidates:
            settings = json.loads(path.read_text())
            settings["overlayInactivitySeconds"] = 10
            path.write_text(json.dumps(settings))
        runner.launch_stack()
        runner.wait_for_bridge(60)
        runner.window_id = runner.find_largest_window(60)
        runner.start_sse()
        runner.tune_channel(1)
        wait("initial playback", lambda s: s["playback"]["currentChannel"].get("id") == 0, 30)
        key("Escape")
        move(700, 450)
        key("Left")
        key("Down")
        wait("keyboard navigation locks chrome", visible)
        time.sleep(6)
        key("Shift_L")
        time.sleep(6)
        wait("non-navigation keyboard input resets timeout", visible)
        move(710, 450)
        time.sleep(6)
        wait("pointer activity resets timeout", visible)
        wait("idle keyboard-locked chrome collapses", lambda s: not visible(s))
        key("Left")
        key("Down")
        move(100, 250)
        runner.xdotool("click", "1")
        wait("mouse selection pins chrome", visible)
        wait("stationary hover and mouse pin expire", lambda s: not visible(s))
        key("ctrl+Up")
        wait("Guide opens", lambda s: s["window"]["visibleOverlay"] == "guide")
        time.sleep(11)
        wait("inactivity preserves Guide", lambda s: s["window"]["visibleOverlay"] == "guide")
        key("Escape")
        wait("Guide close animation completes", lambda s:
             s["window"]["visibleOverlay"] == "none" and not any(
                 r["name"] == "guide_overlay" and r["visible"] for r in s["regions"]))
        # Do not read the Settings button's off-window animation coordinates.
        move(700, 450)
        runner.xdotool("click", "1")
        key("Right")
        state = wait("Settings button is inside the window", lambda s: any(
            i["text"] == "Settings" and i["property"] == "caption"
            and 0 <= i.get("bounds", {}).get("x", -1) < s["window"]["width"] - 48
            for i in s["inventory"]))
        bounds = next(i["bounds"] for i in state["inventory"] if i["text"] == "Settings" and i["property"] == "caption")
        move(round(bounds["x"] + bounds["width"] / 2), round(bounds["y"] + bounds["height"] / 2))
        runner.xdotool("click", "1")
        wait("Settings opens", lambda s: s["window"]["visibleOverlay"] == "settings")
        time.sleep(11)
        wait("inactivity preserves Settings", lambda s: s["window"]["visibleOverlay"] == "settings")
    finally:
        (runner.run_dir / "inactivity-result.json").write_text(json.dumps({"passed": checks}, indent=2))
        runner.cleanup()
        server.shutdown()


if __name__ == "__main__":
    main()
