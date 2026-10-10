#!/usr/bin/env python3
"""Exercise bundle rejection against mutations of a real packaged application."""
import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

from check_bundle import check_bundle
from macho import inspect, system_reference


class BundleMutationTests(unittest.TestCase):
    source = None

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.app = Path(self.temporary.name) / 'OKILTV.app'
        shutil.copytree(self.source, self.app, symlinks=True)

    def test_complete_bundle_passes(self):
        self.assertEqual(check_bundle(self.app)['status'], 'PASS')

    def test_missing_packaged_helper_fails(self):
        (self.app / 'Contents/MacOS/ffprobe').unlink()
        with self.assertRaisesRegex(ValueError, 'Missing runtime input'):
            check_bundle(self.app)

    def test_external_symlink_fails(self):
        (self.app / 'external-link').symlink_to('/')
        with self.assertRaisesRegex(ValueError, 'External symlink'):
            check_bundle(self.app)

    def test_wrong_binary_architecture_fails(self):
        binary = self.app / 'Contents/MacOS/OKILTV'
        with binary.open('r+b') as stream:
            stream.seek(4)
            stream.write(struct.pack('<I', 0x01000007))
        with self.assertRaisesRegex(ValueError, 'Architecture/deployment floor'):
            check_bundle(self.app)

    def test_changed_code_fails_signature(self):
        binary = self.app / 'Contents/MacOS/OKILTV'
        with binary.open('r+b') as stream:
            stream.seek(16384)
            value = stream.read(1)[0]
            stream.seek(16384)
            stream.write(bytes([value ^ 1]))
        with self.assertRaisesRegex(ValueError, 'Invalid signature code digest'):
            check_bundle(self.app)

    def test_changed_resource_fails_seal(self):
        with (self.app / 'Contents/Resources/OKILTV.icns').open('ab') as stream:
            stream.write(b'changed resource')
        with self.assertRaisesRegex(ValueError, 'Invalid signed resource digest'):
            check_bundle(self.app)

    def test_homebrew_dependency_fails(self):
        binary = self.app / 'Contents/MacOS/OKILTV'
        dependency = next(ref for ref in inspect(binary)['dependencies'] if not system_reference(ref))
        tool = 'install_name_tool'
        if sys.platform != 'darwin':
            wrappers = list((Path(os.environ['OSXCROSS_ROOT']) / 'bin').glob('arm64-apple-darwin*-install_name_tool'))
            tool = str(wrappers[0])
        subprocess.run([tool, '-change', dependency, '/opt/homebrew/lib/not-shipped.dylib', str(binary)],
                       check=True, stdout=subprocess.DEVNULL)
        with self.assertRaisesRegex(ValueError, 'Non-relocatable load command'):
            check_bundle(self.app)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('app', type=Path)
    args = parser.parse_args()
    BundleMutationTests.source = args.app.resolve()
    unittest.main(argv=[sys.argv[0]], verbosity=2)
