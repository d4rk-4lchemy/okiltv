# Linux UI regression tests

All scenarios use local generated MPEG-TS, M3U and XMLTV fixtures. No IPTV
account, private capture or repository secret is needed.

| Scenario | Coverage |
| --- | --- |
| `05-channel-selection.py` | Hover, pinning, keyboard selection, Guide/Settings handoff, PiP swap/close |
| `06-overlay-inactivity.py` | Input resets timeout; idle chrome collapses; Guide/Settings remain open |
| `07-guide-groups.py` | Selected groups, empty groups and Guide navigation |
| `08-stationary-pointer-navigation.py` | Keyboard scrolling under a stationary pointer |
| `08-ui-transparency.py` | Preview, keyboard/drag changes, Save/discard and playback preservation |

Configure and run the group:

```bash
export PATH=/opt/Qt/6.10.3/gcc_64/bin:$PATH
export CMAKE_PREFIX_PATH=/opt/Qt/6.10.3/gcc_64
cmake --preset qt-linux-debug -DOKILTV_BUILD_UI_TESTS=ON
cmake --build --preset qt-linux-debug --target OKILTVQt -j"$(scripts/build_jobs.sh)"
ctest --preset qt-linux-debug -L '^ui$' --output-on-failure --no-tests=error
```

Single scenario, with an isolated D-Bus session and disposable unlocked keyring:

```bash
bash scripts/ci/run_ui_test.sh ui-tests/tests/05-channel-selection.py   qt/out/build/qt-linux-debug/app/OKILTV /tmp/okiltv-channel-selection
```

Dependencies: Python 3, ffmpeg, libmpv2, libsecret-1-0, gnome-keyring,
dbus-run-session, Xvfb, Openbox, xdotool, x11-utils and Mesa software OpenGL.
The helper does not use the user's keyring or change the application security
backend. Run scenarios sequentially; CTest enforces a shared UI resource lock.

CTest writes scenario logs, snapshots and screenshots under
`qt/out/build/qt-linux-debug/ui-artifacts/`. CI retains reports and selected
UI diagnostics for seven days, excluding generated media and application data.

Wait for overlay visibility/geometry before pointer input. The application
intentionally ignores input during slide animations; fixed sleeps alone do not
establish that a control is ready.
