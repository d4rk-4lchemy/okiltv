#!/usr/bin/env python3
"""Real keyboard/locale regression with five local M3U channels; no provider access.

Run via scripts/ci/run_ui_test.sh with LC_ALL=pl_PL.UTF-8 (default) or en_US.UTF-8.
The locale must be installed (or available through LOCPATH).
"""
import importlib.util
import json
import os
import pathlib
import subprocess
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
            "-t", "120", "-c:v", "mpeg2video", "-b:v", "100k", str(media),
        ], check=True)
        playlist = self.run_dir / "channels.m3u"
        playlist.write_text("#EXTM3U\n" + "".join(
            f'#EXTINF:-1 tvg-chno="{number}" group-title="Fixture",Channel {number}\n{media.as_uri()}\n'
            for number in ["1", "2", "2.5", "3", "4"]
        ))
        profile = "11111111-1111-1111-1111-111111111111"
        settings = {
            "activeProfileId": profile,
            "profiles": [{"id": profile, "name": "Decimal fixture", "type": 2,
                          "m3UFilePath": str(playlist), "isActive": True}],
            "autoRefreshEpg": False, "minimizeToTrayOnMinimize": False,
            "playerVolume": 50, "playerDeinterlaceEnabled": False,
            "mpvOptions": {"ao": "null"},
        }
        for path in self.settings_path_candidates:
            path.write_text(json.dumps(settings))


def main():
    module.ensure_dbus_session()
    os.environ.setdefault("LC_ALL", "pl_PL.UTF-8")
    args = module.parse_args()
    args.test_name = "decimal-channel-numbers"
    runner = LocalRunner(args)
    runner.prepare_dirs()
    passed = []
    separator = "," if os.environ["LC_ALL"].startswith("pl") else "."

    def key(value):
        runner.xdotool("key", "--window", runner.window_id, value)

    def number(state):
        return state["playback"]["currentChannel"].get("channelNumber")

    def hud(state):
        return next((i.get("text", "") for i in state["inventory"]
                     if i.get("objectName") == "ui.live.numericInput"), "")

    def volume(state):
        return state["playback"]["debugOverlay"].get("volumeText")

    def wait(label, predicate, timeout=10):
        end = time.monotonic() + timeout
        state = {}
        while time.monotonic() < end:
            state = runner.read_state(3)
            if predicate(state):
                passed.append(label)
                runner.log("PASS: " + label)
                return state
            time.sleep(.1)
        runner.save_state_snapshot("failure", state)
        raise AssertionError(label)

    try:
        runner.seed_settings()
        runner.launch_stack()
        runner.wait_for_bridge(60)
        runner.window_id = runner.find_largest_window(60)
        key("1")
        state = wait("integer tune", lambda s: number(s) == "1", 30)
        initial_volume = volume(state)
        assert initial_volume is not None, "Missing volume telemetry"
        for decimal_key in (["comma", "period"] if separator == "," else ["period"]):
            key("2")
            key(decimal_key)
            key(decimal_key)  # Duplicate separator is consumed, never changes volume.
            key("5")
            wait("localized HUD " + decimal_key, lambda s: hud(s) == "2" + separator + "5", 1.5)
            state = wait("decimal tune " + decimal_key, lambda s: number(s) == "2.5" and hud(s) == "")
            assert volume(state) == initial_volume, "Separator changed volume"
            key("1")
            wait("return to integer", lambda s: number(s) == "1")
        key("2")
        key("period")
        state = wait("incomplete decimal reports error", lambda s: hud(s) == "No channel")
        assert number(state) == "1", "Incomplete decimal tuned integer channel"
        key("Escape")
        key("KP_2")
        key("KP_Decimal")
        key("KP_5")
        wait("numeric keypad", lambda s: number(s) == "2.5")
        key("comma")
        state = wait("volume shortcut outside entry", lambda s: volume(s) != initial_volume)
        lower_volume = volume(state)
        key("period")
        wait("volume increase shortcut", lambda s: volume(s) != lower_volume)
        key("3")
        key("Escape")
        time.sleep(2.2)
        assert number(runner.read_state(3)) == "2.5", "Escape failed to cancel entry"
        passed.append("Escape cancels numeric tune")
    finally:
        (runner.run_dir / "decimal-result.json").write_text(json.dumps({"passed": passed}, indent=2))
        runner.cleanup()


if __name__ == "__main__":
    main()
