# Pull request tests

`.github/workflows/pr-tests.yml` runs on every pull request targeting `main`
and supports manual dispatch. It tests the PR merge commit with read-only
permissions and no repository secrets. New commits cancel older runs of the
same PR; a failed group does not cancel other groups.

| Job group | Coverage |
| --- | --- |
| `core` | Services, settings, database and database startup |
| `app` | Controllers, models and real libmpv integration |
| `playback` | Playback decisions and synthetic catch-up transport |
| `qml` | Four component suites using qmltestrunner |
| `ui` | Five local-media scenarios on Xvfb/Openbox |
| `windows` | Native Windows process ownership, cleanup and DVR reconciliation |

Linux jobs use CTest labels and build only their required targets. The UI job
uses FFmpeg-generated media and a separate unlocked disposable Secret Service
keyring for each scenario. No IPTV account or private recording is needed.
Windows runs process ownership tests and the two Windows-only DVR controller
cases using Qt/MinGW on `windows-2022`. JUnit reports and diagnostic logs are
saved as separate Actions artifacts for seven days. The final **Tests passed**
check succeeds only when all groups pass; it can be selected as a required
check in branch protection.

Run all Linux suites locally (with the dependencies listed in the workflow):

```bash
cmake --preset qt-linux-debug -DOKILTV_BUILD_UI_TESTS=ON
cmake --build --preset qt-linux-debug -j"$(scripts/build_jobs.sh)"
ctest --preset qt-linux-debug --output-on-failure
# One group:
ctest --preset qt-linux-debug -L '^ui$' --output-on-failure
```

`copy_clean_repo.sh` copies both workflows, test sources and the UI harness,
and verifies the required CI files before staging the clean checkout.

# Release builds

`.github/workflows/release.yml` builds Windows x64 (NSIS installer and portable
ZIP) and Linux x86_64 (AppImage) on Ubuntu 24.04. Windows uses the existing
MinGW/Wine scripts; both SDKs use the Qt version in `dependencies.env`.

## One-time setup

1. In the same GitHub repository, publish a dependency release with tag
   `deps-mpv-win64-1` and an asset named exactly `mpv-2.dll`.
   Use the DLL whose SHA-256 is recorded as `MPV_DLL_SHA256` in
   `scripts/ci/dependencies.env`. If uploading a different tested DLL, update
   that digest as well. Dependency releases can be marked as pre-releases;
   all `deps-*` tags are ignored by the application workflow.
2. Run `copy_clean_repo.sh /path/to/clean-checkout` (or its existing `--force`
   mode), review the staged changes, commit and push. The script includes
   `.github/workflows/`, all CI inputs and the packaging scripts. It verifies
   that the required CI files were copied intact. The DLL stays outside Git.
3. Include the workflow on the repository's default branch to expose the
   **Actions → Release packages → Run workflow** button. Run it against the
   desired branch for a trial build. This produces downloadable Actions
   artifacts for 14 days and does not publish or modify a Release.

No personal access token or custom repository secret is required for the
dependency asset in the same repository. Downloads use the built-in
`GITHUB_TOKEN`; only the final publishing job receives `contents: write`.

## Publish an application release

1. Set the application version in `qt/CMakeLists.txt` and synchronize the source.
2. Create a tag `v<version>` on the synchronized commit, for example `v0.5.3`.
3. Publish a GitHub Release for that tag. Publishing a pre-release also works;
   saving a draft or pushing a tag alone does not start this workflow.

The workflow checks that the tag matches CMake, builds both platforms, runs
the Linux test suite and checks AppDir libraries/plugins, including the dynamic
libmpv and libsecret ABI. Only after both jobs succeed does it attach:

- `OKILTV-qt-win-x64-<version>.zip`
- `OKILTV-qt-win-x64-setup-<version>.exe`
- `OKILTV-qt-linux-x86_64-<version>.AppImage`
- `SHA256SUMS-windows.txt` and `SHA256SUMS-linux.txt`

Re-running the workflow replaces assets with the same names. The release must
allow asset uploads/updates: this publish-after-release flow is not compatible
with GitHub's immutable releases setting. For immutable releases, a separate
build-draft-then-publish flow would be needed.

## Dependency updates and platform scope

- Change the dependency tag and SHA-256 together when upgrading Windows mpv.
  A failed checksum stops the build before packaging.
- AppImage packaging uses versioned, checksum-verified linuxdeploy tools. The
  Linux mpv comes from Ubuntu's `libmpv2` package, independently of the Windows
  DLL. Ubuntu packages and hosted runner images still receive updates; these
  builds are not guaranteed to be byte-for-byte reproducible.
- The AppImage bundles Qt/QML, libmpv, libsecret and their collected libraries.
  Explicit library inputs are necessary because mpv and libsecret are loaded
  dynamically. It targets Ubuntu 24.04 or a compatible/newer x86_64 system;
  compatibility with older distributions is not promised.
- A running desktop Secret Service/keyring, graphics drivers and an audio
  service remain host requirements. FFmpeg/ffprobe command-line tools used for
  recording and timeshift remain external dependencies, as in existing builds.
- Packages are unsigned. Playback, graphics, audio and installer behavior still
  need a desktop smoke test on each target platform.

Local AppImage packaging after a Linux Release build:

```bash
export QT_LINUX_SDK_ROOT=/opt/Qt/6.10.3/gcc_64
bash scripts/package_linux_appimage.sh
```

The script needs curl, 7-Zip-compatible build tooling, ImageMagick (`convert`),
desktop-file-utils, libmpv2 and libsecret-1-0; see the workflow for build packages.
