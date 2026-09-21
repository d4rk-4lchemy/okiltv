#!/usr/bin/env python3
"""UI transparency preview, keyboard, Save/discard using local media only.

Run: python3 ui-tests/tests/08-ui-transparency.py --app-bin <OKILTV>
Requires the existing runner's Xvfb/Openbox/xdotool and ffmpeg stack.
"""
import functools
import http.server
import importlib.util
import json
import pathlib
import threading
import time

spec = importlib.util.spec_from_file_location(
    "selection_fixture", pathlib.Path(__file__).with_name("05-channel-selection.py"))
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
module = fixture.module


def main():
    module.ensure_dbus_session()
    args = module.parse_args()
    args.test_name = "ui-transparency"
    runner = fixture.LocalRunner(args)
    runner.prepare_dirs()
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(runner.run_dir))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    runner.http_port = server.server_port
    threading.Thread(target=server.serve_forever, daemon=True).start()
    checks = []

    def wait(label, predicate, timeout=15):
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

    def text_item(state, text):
        return next(i for i in state["inventory"] if i["text"] == text and "bounds" in i
                    and (text != "Settings" or i["objectName"] == "ui.live.settingsButton"))

    def preview(state):
        return next((i["text"] for i in state["inventory"]
                     if i["objectName"] == "ui.settings.uiTransparencyValue"), None)

    def key(value):
        runner.xdotool("key", "--window", runner.window_id, value)
        time.sleep(.25)

    def move(x, y):
        runner.xdotool("mousemove", "--window", runner.window_id, str(round(x)), str(round(y)))

    def click_text(text):
        bounds = text_item(runner.read_state(), text)["bounds"]
        move(bounds["x"] + bounds["width"] / 2, bounds["y"] + bounds["height"] / 2)
        runner.xdotool("click", "1")
        time.sleep(.3)

    def open_settings(expected):
        wait("overlay close animation completes", lambda s:
             s["window"]["visibleOverlay"] == "none" and not any(
                 r["name"] in ("guide_overlay", "settings_overlay") and r["visible"]
                 for r in s["regions"]))
        move(700, 450)
        key("Right")  # Reveal/focus the rail after the previous overlay unmounts.
        # Chrome is visible before its slide/opacity animations finish. The
        # ancestor disables the button throughout that interval; wait for the
        # effective enabled state rather than elapsed time or text geometry.
        state = wait("Settings button accepts input", lambda s: any(
            i["objectName"] == "ui.live.settingsButton" and i.get("enabled", False)
            and 0 <= i["bounds"]["x"] <= s["window"]["width"] - i["bounds"]["width"]
            for i in s["inventory"]))
        bounds = text_item(state, "Settings")["bounds"]
        move(bounds["x"] + bounds["width"] / 2, bounds["y"] + bounds["height"] / 2)
        wait("pointer reaches Settings button", lambda s: any(
            i["objectName"] == "ui.live.settingsButton"
            and i.get("enabled", False) and i.get("hovered", False)
            for i in s["inventory"]))
        runner.save_state_snapshot("before-settings-click", runner.read_state())
        runner.xdotool("click", "1")
        wait("Settings overlay opens", lambda s: s["window"]["visibleOverlay"] == "settings")
        wait("settings value " + expected, lambda s: preview(s) == expected)

    def slider_point(fraction):
        state = runner.read_state()
        label = text_item(state, "UI transparency")["bounds"]
        left = text_item(state, "Opaque")["bounds"]
        right = text_item(state, "Default")["bounds"]
        x = left["x"] + 2 + (right["x"] + right["width"] - left["x"] - 4) * fraction
        y = (label["y"] + label["height"] + left["y"]) / 2
        return x, y

    def set_slider(fraction):
        move(*slider_point(fraction))
        runner.xdotool("click", "1")

    def capture(label):
        time.sleep(.35)
        assert runner.request_capture_wait(label, "transparency") is not None

    try:
        runner.seed_settings()
        runner.launch_stack()
        runner.wait_for_bridge(60)
        runner.window_id = runner.find_largest_window(60)
        runner.xdotool("windowactivate", runner.window_id)
        runner.start_sse()
        runner.tune_channel(1)
        runner.wait_for_playback_ready(timeout_sec=60)
        open_settings("100%")
        capture("settings-default")
        set_slider(0)
        wait("mouse previews opaque", lambda s: preview(s) == "0%")
        capture("settings-opaque")
        key("Right")
        wait("keyboard changes one step", lambda s: preview(s) == "1%")
        key("Escape")
        wait("discard confirmation", lambda s: any(i["text"] == "Discard" for i in s["inventory"]))
        click_text("Keep Editing")
        wait("keep editing retains preview", lambda s: preview(s) == "1%")
        key("Escape")
        click_text("Discard")
        wait("discard closes settings", lambda s: s["window"]["visibleOverlay"] == "none")
        open_settings("100%")
        set_slider(0)
        wait("opaque before save", lambda s: preview(s) == "0%")
        click_text("Save settings")
        wait("save clears dirty", lambda s: any(
            e["id"] == "settings.section" and not e["value"]["dirty"] for e in s["elements"]))
        assert any(json.loads(p.read_text()).get("uiTransparency") == 0
                   for p in runner.settings_path_candidates)
        key("Escape")
        wait("save then close", lambda s: s["window"]["visibleOverlay"] == "none")
        move(700, 450)
        capture("live-opaque")
        key("ctrl+Up")
        wait("guide opens", lambda s: s["window"]["visibleOverlay"] == "guide")
        capture("guide-opaque")
        key("Escape")
        open_settings("0%")
        set_slider(1)
        wait("maximum restores default preview", lambda s: preview(s) == "100%")
        set_slider(0)
        runner.xdotool("mousedown", "1")
        try:
            move(*slider_point(.5))
            wait("midpoint previews during drag", lambda s: preview(s) == "50%")
        finally:
            runner.xdotool("mouseup", "1")
        capture("settings-midpoint")
        key("Escape")
        click_text("Discard")
        open_settings("0%")
        wait("playback identity preserved", lambda s: s["playback"]["currentChannel"].get("id") == 0)
    finally:
        (runner.run_dir / "transparency-result.json").write_text(json.dumps({"passed": checks}, indent=2))
        runner.cleanup()
        server.shutdown()


if __name__ == "__main__":
    main()
