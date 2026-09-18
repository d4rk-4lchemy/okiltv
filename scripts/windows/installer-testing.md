# Windows installer verification

Build with `scripts/package_qt_win64.sh`. Test in a disposable Windows VM
(or Wine with 32-bit support: NSIS produces a 32-bit installer for the 64-bit app).

1. With no installation, install to a path containing spaces. Check that
   `OKILTV.exe` and `Uninstall.exe` exist and Apps & Features points to this folder.
   The finish-page **Uruchom aplikację** checkbox must be checked. Finish should
   launch OKILTV; repeating with the box unchecked should not launch it.
2. Install an older release in a non-default folder, then run the new installer.
   No uninstall prompt should appear at startup. After the license page, the
   maintenance page should display the old folder and default to updating.
   Update must skip the directory page and replace files in that folder.
   Check sources/settings and shortcuts still work. Older installers may name
   their uninstaller `.exe`; update must replace this with `Uninstall.exe`.
3. Choose clean installation. Check the directory page permits both the old
   folder and a different folder. Go Back and switch between both modes.
   Cancel before Install: the old application must still be installed.
4. Complete clean installation, once to the same folder and once to another.
   Verify the previous registered files and shortcuts are removed, only one
   Apps & Features entry remains, and the finish checkbox launches the new copy.
   Settings in AppData and an unrelated file placed in the old folder must survive.
5. Remove the old uninstaller before choosing clean installation. Installation
   must stop with an error, without copying the new application into place.
   Also test with the old application running and check failures are reported.
6. Run the installer with `/S /D=C:\Another folder` when an installation already
   exists. It must update the registered folder, without dialogs or launching
   OKILTV. On a fresh system, `/D` should select the installation folder.
