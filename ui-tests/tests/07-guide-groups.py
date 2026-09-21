#!/usr/bin/env python3
"""Guide group filtering with local media/EPG and real keyboard navigation.

Run: python3 ui-tests/tests/07-guide-groups.py --app-bin <OKILTV>
Uses the same Xvfb/Openbox/xdotool/ffmpeg fixture as channel-selection.
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


class GuideRunner(fixture.LocalRunner):
    def seed_settings(self):
        super().seed_settings()
        playlist = self.run_dir / "channels.m3u"
        text = playlist.read_text()
        for number in (29, 30):
            text = text.replace(
                f'tvg-id="fixture{number}" group-title="Fixture"',
                f'tvg-id="fixture{number}" group-title="Other"')
        playlist.write_text(text)


def main():
    module.ensure_dbus_session()
    args = module.parse_args()
    args.test_name = "guide-groups"
    runner = GuideRunner(args)
    runner.prepare_dirs()
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(runner.run_dir))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    runner.http_port = server.server_port
    threading.Thread(target=server.serve_forever, daemon=True).start()
    checks = []

    def guide_channel(state):
        selection = next(e for e in state["elements"] if e["id"] == "guide.selection")
        return selection["value"]["selectedChannel"].get("id")

    def wait(label, predicate, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = runner.read_state(3)
            if predicate(state):
                checks.append(label)
                runner.log("PASS: " + label)
                return
            time.sleep(.1)
        runner.save_state_snapshot("failure", state)
        raise AssertionError(label)

    def key(value):
        runner.xdotool("key", "--window", runner.window_id, value)
        time.sleep(.35)

    def close_guide():
        key("Escape")
        wait("Guide close animation completes", lambda s: s["window"]["visibleOverlay"] == "none")

    def open_picker():
        key("ctrl+g")
        wait("Group search is visible", lambda s: any(
            i["text"] == "Search groups" and i.get("bounds", {}).get("x", -1) >= 0
            for i in s["inventory"]))

    def open_group(name):
        open_picker()
        runner.xdotool("type", "--window", runner.window_id, name)
        key("Down")
        # No helper delay between confirmation and Guide opening.
        runner.xdotool("key", "--window", runner.window_id, "Return", "ctrl+Up")

    def selected(channel):
        return lambda state: (state["window"]["visibleOverlay"] == "guide"
                              and guide_channel(state) == channel
                              and state["playback"]["currentChannel"].get("id") == 0)

    try:
        runner.seed_settings()
        runner.launch_stack()
        runner.wait_for_bridge(60)
        runner.window_id = runner.find_largest_window(60)
        runner.start_sse()
        runner.tune_channel(1)
        wait("initial playback", lambda s: s["playback"]["currentChannel"].get("id") == 0, 30)
        key("Escape")
        open_group("Other")
        wait("Guide opens in selected group while playback stays outside it", selected(28), 30)
        key("Down")
        wait("Guide reaches second channel in group", selected(29))
        key("Down")
        wait("Guide stops at group's last channel", selected(29))
        key("Up")
        key("Up")
        wait("Guide stops at group's first channel", selected(28))
        close_guide()
        open_group("Favourites")
        wait("Empty group clears Guide selection without retuning", selected(None))
        key("Return")
        wait("Enter on empty Guide does not tune", selected(None))
        close_guide()
        open_picker()
        # All Groups is first and is offered only with an empty search.
        key("Down")
        for _ in range(4):
            key("Up")
        runner.xdotool("key", "--window", runner.window_id, "Return", "ctrl+Up")
        wait("All Groups restores playback channel in Guide", selected(0))
        key("Down")
        wait("All Groups includes channels outside previous group", selected(1))
        close_guide()
        open_group("Other")
        wait("Reopening Guide reapplies selected group", selected(28))
    finally:
        (runner.run_dir / "guide-result.json").write_text(json.dumps({"passed": checks}, indent=2))
        runner.cleanup()
        server.shutdown()


if __name__ == "__main__":
    main()
