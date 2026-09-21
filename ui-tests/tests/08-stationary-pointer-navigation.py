#!/usr/bin/env python3
"""Channel/group keyboard scrolling beneath a stationary pointer.

Uses the local fixture and Xvfb stack from 05-channel-selection.py.
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


class Runner(fixture.LocalRunner):
    def seed_settings(self):
        super().seed_settings()
        playlist = self.run_dir / "channels.m3u"
        media = (self.run_dir / "sample.ts").as_uri()
        playlist.write_text(playlist.read_text() + "".join(
            f'#EXTINF:-1 group-title="Group {i:02d}",Group channel {i:02d}\n{media}\n'
            for i in range(1, 31)))


def main():
    module.ensure_dbus_session()
    args = module.parse_args()
    args.test_name = "stationary-pointer-navigation"
    runner = Runner(args)
    runner.prepare_dirs()
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(runner.run_dir))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    runner.http_port = server.server_port
    threading.Thread(target=server.serve_forever, daemon=True).start()
    checks = []

    def selection(state):
        return next(e["value"] for e in state["elements"] if e["id"] == "left.selection")

    def wait(label, predicate):
        end = time.monotonic() + 10
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
        time.sleep(.15)

    def move(x, y):
        runner.xdotool("mousemove", "--window", runner.window_id, str(round(x)), str(round(y)))
        time.sleep(.3)

    try:
        runner.seed_settings()
        runner.launch_stack()
        runner.wait_for_bridge(60)
        runner.window_id = runner.find_largest_window(60)
        runner.start_sse()
        runner.tune_channel(1)
        wait("initial playback", lambda s: s["playback"]["currentChannel"].get("id") == 0)
        move(700, 450)
        key("Left")
        state = runner.read_state()
        pane = next(r for r in state["regions"] if r["name"] == "left_pane")
        pointer_y = pane["y"] + 16 + 42 + 8 + 31 + 64
        move(100, pointer_y)
        # Re-enter from video-only mode to start at the playback channel.
        key("Escape")
        key("Left")
        for channel_id in range(1, 21):
            key("Down")
            wait(f"stationary pointer: channel {channel_id}",
                 lambda s, expected=channel_id: selection(s).get("id") == expected)
        time.sleep(.5)
        wait("no delayed hover steals keyboard selection", lambda s: selection(s).get("id") == 20)
        move(102, pointer_y)
        wait("real pointer movement resumes channel hover", lambda s: selection(s).get("id") != 20)
        key("Escape")
        key("ctrl+g")
        key("Down")
        # All Groups, Favourites, Fixture, then Group 01 ... Group 30.
        for _ in range(35):
            key("Up")
        for _ in range(20):
            key("Down")
        key("Return")
        wait("stationary pointer: scrolled group 18 confirmed",
             lambda s: selection(s).get("name") == "Group channel 18")
        key("ctrl+g")
        key("Down")
        for _ in range(10):
            key("Down")
        move(104, pointer_y)
        key("Return")
        wait("real pointer movement resumes group hover",
             lambda s: selection(s).get("name", "").startswith("Group channel ")
             and selection(s).get("name") != "Group channel 28")
        wait("browsing never retunes", lambda s: s["playback"]["currentChannel"].get("id") == 0)
    finally:
        (runner.run_dir / "navigation-result.json").write_text(json.dumps({"passed": checks}, indent=2))
        runner.cleanup()
        server.shutdown()


if __name__ == "__main__":
    main()
