#!/usr/bin/env python3
"""Grid Ctrl-hold selection regression using real key events and local media."""
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
    args.test_name = "multiview-keyboard"
    runner = fixture.LocalRunner(args)
    runner.prepare_dirs()
    server = http.server.ThreadingHTTPServer(
        ("127.0.0.1", 0),
        functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(runner.run_dir)))
    runner.http_port = server.server_port
    threading.Thread(target=server.serve_forever, daemon=True).start()
    checks = []

    def wait(label, predicate, timeout=10):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            state = runner.read_state(3)
            if predicate(state):
                checks.append(label)
                runner.log("PASS: " + label)
                return state
            time.sleep(.1)
        runner.save_state_snapshot("failure", state)
        raise AssertionError(f"{label}: {state.get('multiview')}, {state.get('window')}")

    def key(value):
        runner.xdotool("key", "--window", runner.window_id, value)
        time.sleep(.2)

    def ctrl(down, name="Control_L"):
        runner.xdotool("keydown" if down else "keyup", "--window", runner.window_id, name)
        time.sleep(.2)

    def grid(label, focused, candidate=None):
        return wait(label, lambda s: s["multiview"]["focused"] == focused
                    and s["multiview"]["selecting"] == (candidate is not None)
                    and (candidate is None or s["multiview"]["candidate"] == candidate))

    try:
        runner.seed_settings()
        runner.launch_stack()
        runner.wait_for_bridge(60)
        runner.window_id = runner.find_largest_window(60)
        runner.xdotool("windowactivate", "--sync", runner.window_id)
        runner.start_sse()
        runner.tune_channel(1)
        wait("initial playback", lambda s: s["playback"]["currentChannel"].get("id") == 0, 30)
        key("Escape")
        key("ctrl+o")
        wait("grid ready", lambda s: s["multiview"]["available"])
        key("ctrl+shift+Right")
        grid("additional modifiers do not start selection", 0)
        ctrl(True)
        grid("Ctrl alone does not start", 0)
        key("Right")
        grid("first arrow previews empty tile without committing", 0, 1)
        key("Return")
        grid("promotion of empty candidate does nothing", 0, 1)
        key("Down")
        grid("down previews bottom right", 0, 3)
        key("Left")
        grid("left previews bottom left", 0, 2)
        key("Up")
        grid("Ctrl+Up stays in grid", 0, 0)
        key("Left")
        grid("horizontal edge wraps", 0, 1)
        key("Up")
        grid("vertical edge wraps", 0, 3)
        ctrl(False)
        grid("release commits empty tile", 3)
        ctrl(True, "Control_R")
        key("Left")
        grid("right Ctrl starts another gesture", 3, 2)
        ctrl(False, "Control_R")
        grid("right Ctrl release commits", 2)
        ctrl(True)
        runner.xdotool("key", "--window", runner.window_id, "--repeat", "3", "--delay", "80", "Right")
        grid("repeated arrows move candidate", 2, 3)
        key("Escape")
        ctrl(False)
        grid("Escape cancels before release", 2)
        ctrl(True)
        key("Right")
        runner.xdotool("windowminimize", runner.window_id)
        wait("window loss cancels", lambda s: not s["multiview"]["selecting"])
        ctrl(False)
        runner.xdotool("windowactivate", "--sync", runner.window_id)
        wait("window reactivated", lambda s: s["window"]["active"] and s["multiview"]["available"])
        grid("release after window loss does not commit", 2)
        key("ctrl+shift+o")
        grid("removed shortcut does nothing", 2)
        key("ctrl+alt+o")
        grid("removed cleanup shortcut does nothing", 2)
        ctrl(True)
        key("Right")
        key("g")
        ctrl(False)
        grid("group picker cancels", 2)
        key("ctrl+Right")
        grid("picker protects input", 2)
        key("ctrl+Return")
        grid("picker protects promotion", 2)
        key("Escape")
        wait("picker fully closed", lambda s: not s["multiview"]["pickerOpen"]
             and not s["multiview"]["chromeAnimating"])
        key("ctrl+f")
        wait("search focused and enabled", lambda s: s["multiview"]["searchFocused"]
             and not s["multiview"]["chromeAnimating"])
        runner.xdotool("type", "--window", runner.window_id, "Fixture")
        wait("search receives input", lambda s: any(
            e["id"] == "left.search" and e["value"] == "Fixture" for e in s["elements"]))
        key("ctrl+Right")
        grid("search protects input", 2)
        key("ctrl+Return")
        grid("search protects promotion", 2)
        # Enter keeps the search field's existing action: move into the channel list.
        key("ctrl+f")
        wait("search refocused after its native Enter action", lambda s:
             s["multiview"]["searchFocused"] and not s["multiview"]["chromeAnimating"])
        key("ctrl+Up")
        wait("search can still open Guide", lambda s: s["window"]["visibleOverlay"] == "guide")
        key("ctrl+Right")
        grid("Guide protects input", 2)
        key("ctrl+Return")
        grid("Guide protects promotion", 2)
        key("Escape")
        wait("Guide fully closed", lambda s: s["multiview"]["available"])
        key("Right")
        state = wait("Settings button ready", lambda s: any(
            i["objectName"] == "ui.live.settingsButton" and i.get("enabled", False)
            and 0 <= i["bounds"]["x"] <= s["window"]["width"] - i["bounds"]["width"]
            for i in s["inventory"]))
        bounds = next(i["bounds"] for i in state["inventory"] if i["objectName"] == "ui.live.settingsButton")
        runner.xdotool("mousemove", "--window", runner.window_id,
                       str(round(bounds["x"] + bounds["width"] / 2)),
                       str(round(bounds["y"] + bounds["height"] / 2)))
        wait("Settings button hovered", lambda s: any(
            i["objectName"] == "ui.live.settingsButton" and i.get("hovered", False)
            for i in s["inventory"]))
        runner.xdotool("click", "1")
        wait("Settings opened", lambda s: s["window"]["visibleOverlay"] == "settings")
        key("ctrl+Right")
        grid("Settings protects input", 2)
        key("ctrl+Return")
        grid("Settings protects promotion", 2)
        key("Escape")
        wait("Settings fully closed", lambda s: s["multiview"]["available"])
        key("Left")
        wait("ordinary Live panel shown", lambda s: any(
            r["name"] == "left_pane" and r["visible"] for r in s["regions"]))
        ctrl(True)
        key("Up")
        grid("ordinary Live panel permits grid gesture", 2, 0)
        key("Escape")
        ctrl(False)
        ctrl(True)
        key("Right")
        # Click the centre of the primary tile without revealing the side rails.
        state = runner.read_state()
        runner.xdotool("mousemove", "--window", runner.window_id,
                       str(int(state["window"]["width"] * .25)),
                       str(int(state["window"]["height"] * .25)))
        runner.xdotool("click", "1")
        ctrl(False)
        grid("mouse selection overrides candidate", 0)
        key("Escape")
        ctrl(True)
        key("Right")
        key("o")
        ctrl(False)
        wait("layout change cancels", lambda s: s["multiview"]["mode"] == "off"
             and not s["multiview"]["selecting"])
        key("ctrl+p")
        wait("PiP ready", lambda s: s["multiview"]["mode"] == "pip")
        key("ctrl+Right")
        wait("PiP has no grid gesture", lambda s: not s["multiview"]["selecting"])
        # Assign a different channel before testing the normal PiP shortcuts.
        key("Down")
        key("Return")
        wait("PiP assigned", lambda s: s["multiview"]["mode"] == "pip"
             and s["multiview"]["focused"] == 0)
        key("Escape")
        state = runner.read_state()
        runner.xdotool("mousemove", "--window", runner.window_id,
                       str(round(state["window"]["width"] * .85)),
                       str(round(state["window"]["height"] * .85)))
        time.sleep(.5)
        key("Escape")
        time.sleep(.5)
        runner.xdotool("click", "1")
        wait("PiP mouse selection works", lambda s: s["multiview"]["mode"] == "pip"
             and s["multiview"]["focused"] == 1)
        key("ctrl+Up")
        wait("PiP Ctrl+Up opens Guide", lambda s: s["window"]["visibleOverlay"] == "guide")
    finally:
        runner.xdotool("keyup", "Control_L", "Control_R")
        (runner.run_dir / "multiview-result.json").write_text(json.dumps({"passed": checks}, indent=2))
        runner.cleanup()
        server.shutdown()


if __name__ == "__main__":
    main()
