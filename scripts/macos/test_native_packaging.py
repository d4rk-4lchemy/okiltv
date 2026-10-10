#!/usr/bin/env python3
"""Native beta packaging contracts that can also run on the Linux development host."""
import hashlib
import json
from pathlib import Path
import plistlib
import tempfile
import unittest
from unittest.mock import patch

import package as packaging


class NativePackagingTests(unittest.TestCase):
    def test_inventory_keeps_installed_versions_and_all_media_dependencies(self):
        with tempfile.TemporaryDirectory() as directory:
            deps = Path(directory)
            names = ['mpv', 'ffmpeg', 'openssl@3', 'x264', 'libplacebo']
            for name in names:
                keg = deps / 'Cellar' / name / '1.0_2'
                keg.mkdir(parents=True)
                (keg / 'INSTALL_RECEIPT.json').write_text(json.dumps({'source': {'tap_git_head': 'installed-commit'}}))
                (deps / 'opt').mkdir(exist_ok=True)
                (deps / 'opt' / name).symlink_to(keg)

            def brew(*args):
                if args[1] == 'deps':
                    self.assertIn('--union', args)
                    return 'x264\nlibplacebo\nx264\n'
                return str(deps / 'opt' / args[-1]) + '\n'

            with patch.object(packaging, 'run', side_effect=brew):
                inventory = packaging.native_packages(deps)
            self.assertEqual(set(inventory), set(names))
            self.assertEqual(inventory['mpv']['version'], '1.0_2')
            self.assertEqual(inventory['mpv']['formula_commit'], 'installed-commit')
            self.assertEqual(inventory['mpv']['receipt_sha256'], hashlib.sha256(
                (deps / 'opt/mpv/INSTALL_RECEIPT.json').read_bytes()).hexdigest())

    def test_inventory_rejects_packages_outside_homebrew_cellar(self):
        with tempfile.TemporaryDirectory() as directory:
            deps = Path(directory)
            (deps / 'Cellar').mkdir()
            with patch.object(packaging, 'run', side_effect=['', str(deps)]):
                with self.assertRaisesRegex(ValueError, 'outside the selected Cellar'):
                    packaging.native_packages(deps)

    def test_inventory_accepts_symlinked_prefix_and_rejects_escaped_keg(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            deps = root / 'homebrew'
            (deps / 'opt').mkdir(parents=True)
            alias = root / 'homebrew-alias'
            alias.symlink_to(deps, target_is_directory=True)
            names = ['mpv', 'ffmpeg', 'openssl@3']
            for name in names:
                keg = deps / 'Cellar' / name / '1.0_2'
                keg.mkdir(parents=True)
                (keg / 'INSTALL_RECEIPT.json').write_text('{}')
                (deps / 'opt' / name).symlink_to(keg, target_is_directory=True)

            def brew(*args):
                if args[1] == 'deps':
                    return ''
                return str(alias / 'opt' / args[-1]) + '\n'

            with patch.object(packaging, 'run', side_effect=brew):
                inventory = packaging.native_packages(alias)
                self.assertEqual(set(inventory), set(names))
                self.assertEqual(inventory['mpv']['prefix'], str(deps / 'Cellar/mpv/1.0_2'))

                outside = root / 'outside/ffmpeg/1.0_2'
                outside.mkdir(parents=True)
                (outside / 'INSTALL_RECEIPT.json').write_text('{}')
                (deps / 'opt/ffmpeg').unlink()
                (deps / 'opt/ffmpeg').symlink_to(outside, target_is_directory=True)
                with self.assertRaisesRegex(ValueError, 'outside the selected Cellar: ffmpeg'):
                    packaging.native_packages(alias)

    def test_release_version_mismatch_is_rejected_before_bundle_assembly(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / 'qt/out/build/qt-macos-arm64-native-release'
            info = build / 'app/OKILTV.app/Contents/Info.plist'
            info.parent.mkdir(parents=True)
            info.write_bytes(plistlib.dumps({'CFBundleShortVersionString': '1.0.0'}))
            stage = root / 'qt/out/package/qt-macos-arm64-native-release'
            with patch.object(packaging, 'REPO', root), patch.object(packaging, 'Packager') as assembler:
                with self.assertRaisesRegex(ValueError, 'validated release version'):
                    packaging.package(build, stage, root / 'publish', native=True, expected_version='2.0.0')
                assembler.assert_not_called()

    def test_native_signing_seals_outer_app_after_nested_code_and_frameworks(self):
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / 'OKILTV.app'
            framework = app / 'Contents/Frameworks/Test.framework'
            framework.mkdir(parents=True)
            binary = framework / 'Test'
            binary.touch()
            with patch.object(packaging, 'is_macho', side_effect=lambda path: path == binary), \
                    patch.object(packaging, 'run', return_value='') as run:
                packaging.sign_native(app)
            signed = [call.args[-1] for call in run.call_args_list if '--sign' in call.args]
            self.assertEqual(signed, [binary, framework, app])
            self.assertIn('--verify', run.call_args_list[-1].args)


if __name__ == '__main__':
    unittest.main(verbosity=2)
