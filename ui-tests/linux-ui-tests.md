# Linux UI tests

The automated suite uses the semantic UI bridge (`OKILTV_UI_TEST=1`) with
local fixtures and real playback under Xvfb/Openbox. It does not enable
`OKILTV_HEADLESS_TEST`: the production storage path runs against a disposable
Secret Service keyring created by `scripts/ci/run_ui_test.sh`.

See [the scenario list and commands](tests/README.md) and
[the bridge contract](ui-tests-model.md). These tests are registered in CTest
with label `ui` when `OKILTV_BUILD_UI_TESTS=ON`, and also receive exactly one CI
shard label: `ui-live`, `ui-movies` or `ui-playback`. CI runs shards on separate runners and keeps scenarios serial
within each runner; the shared desktop resource lock still serializes local
`ctest -L '^ui$'` runs. They run in
`.github/workflows/pr-tests.yml` for PRs targeting `main`.

`linux-ui-test-runner.py` supplies the shared bridge, process and capture helpers;
its `Runner.seed_settings()` must be implemented by a local fixture scenario.
The shared movie/series fixture sets mpv `gpu-dumb-mode=yes` under
`LIBGL_ALWAYS_SOFTWARE=1`, using the native VOD render test's mpv workaround
while retaining the normal Mesa driver for QML performance. It retains real
video, exact seek/resume and track assertions; hardware shader quality remains a separate acceptance check.

Live-provider shell wrappers and private-capture-only tests are not part of the
automated suite. Recording/timeshift logic also has synthetic C++ coverage in
`OKILTVQtAppTests`.
