#!/usr/bin/env python3
"""Archive download via real Guide/right-pane shortcuts and the save dialog."""
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
        media = self.run_dir / "archive.ts"
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                        "-f", "lavfi", "-i", "testsrc2=size=160x90:rate=25",
                        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
                        "-t", "12", "-c:v", "mpeg2video", "-g", "25", "-c:a", "mp2",
                        "-f", "mpegts", str(media)], check=True)
        playlist = self.run_dir / "channels.m3u"
        playlist.write_text('#EXTM3U\n#EXTINF:-1 tvg-id="fixture" group-title="Fixture" '
                            'catchup="default" catchup-days="1" '
                            f'catchup-source="http://127.0.0.1:{self.http_port}/archive.ts?utc={{utc}}",Fixture\n'
                            f'{media.as_uri()}\n')
        now = dt.datetime.now(dt.timezone.utc)
        stamp = lambda value: value.strftime("%Y%m%d%H%M%S +0000")
        start = now - dt.timedelta(minutes=10)
        self.archive_start = start
        (self.run_dir / "epg.xml").write_text(
            '<tv><channel id="fixture"><display-name>Fixture</display-name></channel>'
            f'<programme channel="fixture" start="{stamp(start - dt.timedelta(minutes=10))}" '
            f'stop="{stamp(start - dt.timedelta(minutes=10) + dt.timedelta(seconds=40))}">'
            '<title>Partial fixture</title></programme>'
            f'<programme channel="fixture" start="{stamp(start)}" stop="{stamp(start + dt.timedelta(seconds=12))}">'
            '<title>Archived fixture</title></programme>'
            f'<programme channel="fixture" start="{stamp(now - dt.timedelta(minutes=5))}" '
            f'stop="{stamp(now + dt.timedelta(hours=1))}"><title>Current fixture</title></programme></tv>')
        profile = "11111111-1111-1111-1111-111111111111"
        settings = {"activeProfileId": profile,
                    "dateOrder": "dmy", "timeFormat": "24h",
                    "profiles": [{"id": profile, "name": "Archive fixture", "type": 2,
                                  "m3UFilePath": str(playlist), "isActive": True,
                                  "xmltvUrl": f"http://127.0.0.1:{self.http_port}/epg.xml"}],
                    "overlayAutoHide": False, "overlayInactivitySeconds": 600,
                    "autoRefreshEpg": True, "minimizeToTrayOnMinimize": False,
                    "recordingsDirectory": str(self.run_dir), "mpvOptions": {"ao": "null"}}
        for path in self.settings_path_candidates:
            path.write_text(json.dumps(settings))


def main():
    module.ensure_dbus_session()
    args = module.parse_args()
    args.test_name = "catchup-download"
    runner = LocalRunner(args)
    runner.prepare_dirs()
    transfer_waiting = threading.Event()
    release_transfer = threading.Event()

    class ArchiveHandler(http.server.SimpleHTTPRequestHandler):
        gated = False

        def copyfile(self, source, outputfile):
            try:
                if self.path.startswith("/archive.ts") and not ArchiveHandler.gated:
                    ArchiveHandler.gated = True
                    outputfile.write(source.read(32768))
                    outputfile.flush()
                    transfer_waiting.set()
                    if not release_transfer.wait(30):
                        return
                super().copyfile(source, outputfile)
            except (BrokenPipeError, ConnectionResetError):
                pass  # Pausing intentionally closes this connection.

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0),
        functools.partial(ArchiveHandler, directory=str(runner.run_dir)))
    runner.http_port = server.server_port
    threading.Thread(target=server.serve_forever, daemon=True).start()
    checks = []

    def destination_for(title, start):
        # The suggested name includes channel and local programme time. Fix the
        # saved format above so this assertion is independent of the CI locale.
        stamp = start.astimezone().strftime("%d-%m-%Y %H_%M")
        return runner.run_dir / f"Fixture - {stamp} - {title}.mkv"

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

    def key(value, targeted=True):
        if targeted:
            runner.xdotool("key", "--window", runner.window_id, value)
        else:
            runner.xdotool("key", value)
        time.sleep(.25)

    def selected_title(state):
        return next(e for e in state["elements"] if e["id"] == "guide.selection")["value"]["selectedProgram"].get("title")

    def control(state, name):
        return next((i for i in state["inventory"] if i["objectName"] == name and "bounds" in i), None)

    def click_control(name):
        state = wait("control available: " + name, lambda s: control(s, name) is not None
                     and control(s, name).get("enabled", False)
                     and 0 <= control(s, name)["bounds"]["y"] < s["window"]["height"] - 28)
        bounds = control(state, name)["bounds"]
        runner.xdotool("mousemove", "--window", runner.window_id,
                       str(round(bounds["x"] + bounds["width"] / 2)),
                       str(round(bounds["y"] + bounds["height"] / 2)))
        runner.xdotool("click", "1")
        time.sleep(.15)

    def dialog():
        # Native/fallback file pickers are a separate window; don't send keys to
        # the main application while accepting or cancelling the save dialog.
        output = subprocess.run(["xdotool", "search", "--onlyvisible", "--name", "Download programme"],
                                env=runner.display_env(), capture_output=True, text=True)
        return output.stdout.strip().splitlines()

    def await_dialog():
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            found = dialog()
            if found:
                runner.xdotool("windowactivate", "--sync", found[-1])
                return
            time.sleep(.1)
        raise AssertionError("Ctrl+D did not open the save dialog")

    try:
        runner.seed_settings()
        runner.launch_stack()
        runner.wait_for_bridge(60)
        runner.window_id = runner.find_largest_window(60)
        runner.start_sse()
        runner.tune_channel(1)
        wait("channel loaded", lambda s: s["playback"]["currentChannel"].get("id") == 0, 30)
        key("ctrl+Up")
        wait("Guide EPG ready", lambda s: selected_title(s) == "Current fixture", 30)
        key("Left")
        wait("Guide historical programme selected", lambda s: selected_title(s) == "Archived fixture")
        key("ctrl+d")
        await_dialog()
        key("Escape", False)
        assert not list(runner.run_dir.glob("*.mkv"))
        checks.append("cancelling dialog creates no file")
        runner.xdotool("windowactivate", "--sync", runner.window_id)
        key("ctrl+d")
        await_dialog()
        key("Return", False)
        destination = destination_for("Archived fixture", runner.archive_start)
        wait("archive transfer has started", lambda s: transfer_waiting.is_set())
        runner.xdotool("windowactivate", "--sync", runner.window_id)
        key("Escape")
        wait("Guide closes before download controls", lambda s:
             s["window"]["visibleOverlay"] == "none" and not next(
                 r for r in s["regions"] if r["name"] == "guide_overlay")["visible"])
        runner.xdotool("mousemove", "--window", runner.window_id, "700", "450")
        key("Right")
        click_control("ui.downloads.indicator")
        click_control("ui.downloads.pause")
        wait("pause offers Resume", lambda s: control(s, "ui.downloads.pause") is not None
             and control(s, "ui.downloads.pause")["text"] == "Resume downloads")
        source = next(runner.run_dir.glob(".okiltv-download-*.source"))
        saved = source.read_bytes()
        assert saved and not destination.exists()
        release_transfer.set()
        time.sleep(.3)
        assert source.read_bytes() == saved
        click_control("ui.downloads.close")
        click_control("ui.downloads.indicator")
        wait("panel reopening preserves pause", lambda s: control(s, "ui.downloads.pause") is not None
             and control(s, "ui.downloads.pause")["text"] == "Resume downloads")
        click_control("ui.downloads.pause")
        click_control("ui.downloads.close")
        checks.append("pause retains downloaded data and Resume continues the job")
        wait("Guide download finishes as MKV", lambda s: destination.exists(), 25)
        probe = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration",
                                "-of", "json", str(destination)], capture_output=True, text=True, check=True)
        assert abs(float(json.loads(probe.stdout)["format"]["duration"]) - 12) < 1
        runner.xdotool("windowactivate", "--sync", runner.window_id)
        key("Escape")
        wait("Guide fully closed", lambda s: not next(r for r in s["regions"] if r["name"] == "guide_overlay")["visible"])
        key("Right")
        wait("right EPG panel visible", lambda s: next(r for r in s["regions"] if r["name"] == "right_pane")["x"] < s["window"]["width"] - 300)
        key("Up")
        key("ctrl+d")
        await_dialog()
        key("Return", False)
        duplicate = destination.with_stem(destination.stem + " (2)")
        wait("right EPG download uses a unique filename", lambda s: duplicate.exists(), 25)
        wait("completion notification visible", lambda s: any("Download complete" in i.get("text", "") for i in s["inventory"]))
        runner.xdotool("windowactivate", "--sync", runner.window_id)
        key("Up")
        key("ctrl+d")
        await_dialog()
        key("Return", False)
        incomplete = destination_for("Partial fixture", runner.archive_start - dt.timedelta(minutes=10))
        partial = incomplete.with_suffix(".partial.mkv")
        wait("incomplete archive is preserved", lambda s: partial.exists() and partial.stat().st_size > 0, 25)
        assert not incomplete.exists()
        wait("failure notification identifies retained data", lambda s: any("Incomplete data retained" in i.get("text", "") for i in s["inventory"]))
        assert not list(runner.run_dir.glob(".okiltv-download-*"))
        checks.append("no unlabelled temporary files left")
        runner.save_state_snapshot("completed", runner.read_state())
        (runner.run_dir / "regression-results.json").write_text(json.dumps({"passed": checks}, indent=2))
        runner.log("PASS: archive download regression")
    finally:
        release_transfer.set()
        runner.cleanup()
        server.shutdown()


if __name__ == "__main__":
    main()
