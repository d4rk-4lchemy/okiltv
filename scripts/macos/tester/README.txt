OKILTV Apple Silicon beta build

Requires macOS 15.0+ on an Apple Silicon Mac. This is an ad-hoc signed,
unnotarized beta bundle; full runtime acceptance is pending. Qt/Homebrew/Python
are not required on the Mac. Keep the app and writable tester files together.

1. Verify the .tar.gz SHA-256 against its .sha256 file, then extract the archive.
2. Run Verify bundle.command and retain any failure output.
3. Open OKILTV.app from Finder. If macOS blocks an intentionally supplied test
   build, record the message and use the per-app Open Anyway flow. Return
   damaged-bundle or malformed-signature errors to the sender.
4. Run Prepare fixtures.command after extraction or moving the test directory.
   Add fixtures/local.m3u as a local M3U source and fixtures/local.xml as XMLTV.
5. Play Local video fixture: moving video and audible tone are both required.
   Import fixtures/sample subtitles.srt to test subtitle uploads.
6. Follow TESTER-CHECKLIST.md. Return build-manifest.json, per-step results and
   redacted diagnostics/crash reports. Never return saved source files/credentials.
   If Run platform tests.command is present, run it and return its result too.

Launch diagnostics.command launches the same app with normal account storage.
Logs and fixtures stay outside the signed app. The app uses macOS Keychain for
protected data; it never stores an encryption key beside application files.
If storage reports Keychain access denied, unlock the login keychain and allow
OKILTV in the item's access settings, then retry. Do not delete its key.

Public Developer ID signing, notarization and full feature acceptance are pending.
