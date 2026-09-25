#!/usr/bin/env python3
"""Focused multiview transport and EPG regression, using independent local streams."""
import functools
import http.server
import importlib.util
import json
import os
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
    args.test_name = "multiview-controls"
    runner = fixture.LocalRunner(args)
    runner.prepare_dirs()
    server = http.server.ThreadingHTTPServer(
        ("127.0.0.1", 0),
        functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(runner.run_dir)))
    runner.http_port = server.server_port
    threading.Thread(target=server.serve_forever, daemon=True).start()
    checks = []
    retain = os.environ.get("OKILTV_TEST_RETAIN", "0") == "1"

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
        raise AssertionError(f"{label}: {state.get('multiview')}")

    def key(sequence):
        runner.xdotool("key", "--window", runner.window_id, sequence)
        time.sleep(.25)

    def tile(state, index):
        return state["multiview"]["tiles"][index]

    def selected(state):
        return next(e["value"].get("id") for e in state["elements"] if e["id"] == "left.selection")

    def epg(state):
        return next(e["value"].get("title", "") for e in state["elements"] if e["id"] == "right.now")

    def click_control(name):
        state = wait(name + " ready", lambda s: any(
            i["objectName"] == "ui.live." + name and i.get("enabled", False)
            and 0 <= i["bounds"]["x"] < s["window"]["width"]
            and 0 <= i["bounds"]["y"] < s["window"]["height"]
            for i in s["inventory"]))
        bounds = next(i["bounds"] for i in state["inventory"] if i["objectName"] == "ui.live." + name)
        runner.xdotool("mousemove", "--window", runner.window_id,
                       str(round(bounds["x"] + bounds["width"] / 2)),
                       str(round(bounds["y"] + bounds["height"] / 2)))
        wait(name + " hovered", lambda s: any(
            i["objectName"] == "ui.live." + name and i.get("hovered", False) for i in s["inventory"]))
        runner.xdotool("click", "1")

    def focused(index, channel):
        return lambda s: s["multiview"]["focused"] == index and s["multiview"]["channelId"] == channel

    try:
        runner.seed_settings()
        # This scenario tests transport targeting; auto-hide has its own scenario.
        for path in runner.settings_path_candidates:
            settings = json.loads(path.read_text())
            settings["overlayAutoHide"] = False
            settings["multiviewRetainSelectionOnPromotion"] = retain
            path.write_text(json.dumps(settings))
        runner.launch_stack()
        runner.wait_for_bridge(60)
        runner.window_id = runner.find_largest_window(60)
        runner.start_sse()
        runner.tune_channel(1)
        wait("primary playing", lambda s: s["playback"]["currentChannel"].get("id") == 0, 30)
        key("Escape")
        key("ctrl+o")
        wait("grid ready", lambda s: s["multiview"]["available"])
        key("ctrl+Right")
        wait("empty secondary owns controls", focused(1, -1))
        key("3")  # Numeric tuning commits after its idle timer; Enter selects the browse row.
        wait("secondary assigned independently", lambda s: focused(1, 2)(s)
             and tile(s, 0)["channelId"] == 0 and tile(s, 1)["channelId"] == 2
             and tile(s, 0)["paused"] is False and tile(s, 1)["paused"] is False, 30)
        key("space")
        wait("Space pauses only secondary", lambda s: tile(s, 1)["paused"] is True
             and tile(s, 0)["paused"] is False and not s["multiview"]["isPlaying"])
        key("ctrl+Left")
        wait("focus returns controls and EPG to primary", lambda s: focused(0, 0)(s)
             and selected(s) == 0 and epg(s) == "Programme 01")
        key("ctrl+Right")
        wait("paused secondary retains state on refocus", lambda s: focused(1, 2)(s)
             and not s["multiview"]["isPlaying"] and selected(s) == 2)
        key("Right")
        wait("EPG renders secondary programme", lambda s: epg(s) == "Programme 03" and any(
            i["text"] == "Programme 03" and i["bounds"]["x"] > s["window"]["width"] * .65
            for i in s["inventory"]))
        click_control("playPause")
        wait("Play button resumes only secondary", lambda s: tile(s, 1)["paused"] is False
             and tile(s, 0)["paused"] is False and s["multiview"]["isPlaying"])
        click_control("playPause")
        wait("Pause button pauses only secondary", lambda s: tile(s, 1)["paused"] is True
             and tile(s, 0)["paused"] is False and not s["multiview"]["isPlaying"])
        key("space")
        wait("Space resumes secondary", lambda s: tile(s, 1)["paused"] is False)
        click_control("nextChannel")
        wait("Next button retunes only secondary and updates EPG", lambda s:
             tile(s, 0)["channelId"] == 0 and tile(s, 1)["channelId"] == 3
             and epg(s) == "Programme 04")
        click_control("previousChannel")
        wait("Previous button returns secondary", lambda s: focused(1, 2)(s)
             and tile(s, 0)["channelId"] == 0)
        key("Escape")
        key("Down")
        wait("Down retunes focused stream", lambda s: focused(1, 3)(s) and tile(s, 0)["channelId"] == 0)
        key("Up")
        wait("Up retunes focused stream", lambda s: focused(1, 2)(s) and tile(s, 0)["channelId"] == 0)
        key("Left")
        key("Down")
        wait("left pane browses without retuning", lambda s: selected(s) == 3 and focused(1, 2)(s))
        click_control("nextChannel")
        wait("Next anchors to playback instead of browsed row", lambda s:
             focused(1, 3)(s) and tile(s, 0)["channelId"] == 0)
        key("ctrl+Left")
        wait("primary selection restored on focus", lambda s: focused(0, 0)(s)
             and selected(s) == 0 and epg(s) == "Programme 01")
        wait("video surfaces keep their own backends", lambda s:
             all(t["renderMatchesBackend"] for t in s["multiview"]["tiles"]))
        key("ctrl+Right")
        key("ctrl+Down")
        wait("empty tile clears playback EPG", lambda s: focused(3, -1)(s) and epg(s) == "")
        key("space")
        wait("Space on empty tile leaves other streams playing", lambda s:
             tile(s, 0)["paused"] is False and tile(s, 1)["paused"] is False)
        key("ctrl+Up")
        wait("occupied secondary restored", focused(1, 3))
        key("Right")
        click_control("stopPlayback")
        wait("Stop empties focused tile and EPG only", lambda s: focused(1, -1)(s)
             and tile(s, 0)["channelId"] == 0 and tile(s, 0)["paused"] is False and epg(s) == "")
        key("Escape")
        key("ctrl+o")
        wait("grid closed", lambda s: s["multiview"]["mode"] == "off")
        key("Left")
        key("Down")
        key("ctrl+p")
        wait("PiP contains a second stream", lambda s: s["multiview"]["mode"] == "pip"
             and tile(s, 1)["channelId"] == 1 and tile(s, 1)["paused"] is False)
        key("Escape")
        state = runner.read_state()
        runner.xdotool("mousemove", "--window", runner.window_id,
                       str(round(state["window"]["width"] * .85)),
                       str(round(state["window"]["height"] * .85)))
        time.sleep(.5)
        key("Escape")
        time.sleep(.5)
        runner.xdotool("click", "1")
        wait("PiP mouse focus retargets controls", focused(1, 1))
        key("space")
        wait("Space pauses PiP secondary only", lambda s:
             tile(s, 1)["paused"] is True and tile(s, 0)["paused"] is False)
        key("ctrl+shift+p")
        wait("PiP swap preserves paused session and video binding", lambda s: focused(0, 1)(s)
             and tile(s, 0)["paused"] is True and tile(s, 1)["paused"] is False
             and all(t["renderMatchesBackend"] for t in s["multiview"]["tiles"]))
        key("Escape")
        key("ctrl+p")
        wait("PiP cleanup retains primary control", lambda s:
             s["multiview"]["mode"] == "off" and focused(0, 1)(s))
        key("space")
        wait("promoted stream resumes after secondary controller disposal", lambda s:
             s["multiview"]["isPlaying"] and tile(s, 0)["paused"] is False)
        key("ctrl+o")
        wait("promotion grid ready", lambda s: s["multiview"]["available"])
        key("ctrl+Right")
        key("3")
        wait("promotion secondary playing", lambda s: focused(1, 2)(s)
             and tile(s, 1)["paused"] is False, 30)
        key("space")
        before = wait("promotion secondary paused", lambda s: tile(s, 1)["paused"] is True)
        position = tile(before, 1)["position"]
        key("ctrl+Left")
        runner.xdotool("keydown", "--window", runner.window_id, "Control_L")
        key("Right")
        wait("promotion candidate differs from active tile", lambda s:
             s["multiview"]["focused"] == 0 and s["multiview"]["candidate"] == 1
             and s["multiview"]["selecting"])
        key("Return")
        runner.xdotool("keyup", "--window", runner.window_id, "Control_L")
        wait("Ctrl+Enter promotes candidate with pause and retention preserved", lambda s:
             s["multiview"]["mode"] == "off" and focused(0, 2)(s)
             and tile(s, 0)["paused"] is True and abs(tile(s, 0)["position"] - position) < 1
             and s["multiview"]["retained"] == retain and not s["multiview"]["selecting"])
        if retain:
            key("ctrl+o")
            wait("Ctrl+O clears only background streams", lambda s:
                 s["multiview"]["mode"] == "off" and not s["multiview"]["retained"]
                 and focused(0, 2)(s) and tile(s, 0)["paused"] is True
                 and abs(tile(s, 0)["position"] - position) < 1)
        key("ctrl+o")
        wait("next Ctrl+O opens fresh grid", lambda s: s["multiview"]["available"]
             and tile(s, 0)["channelId"] == 2 and tile(s, 1)["channelId"] == -1)
        key("ctrl+KP_Enter")
        wait("numeric Enter promotes committed primary", lambda s:
             s["multiview"]["mode"] == "off" and focused(0, 2)(s)
             and tile(s, 0)["paused"] is True and not s["multiview"]["retained"])
    finally:
        runner.xdotool("keyup", "Control_L", "Control_R")
        (runner.run_dir / "controls-result.json").write_text(json.dumps({"passed": checks}, indent=2))
        runner.cleanup()
        server.shutdown()


if __name__ == "__main__":
    main()
