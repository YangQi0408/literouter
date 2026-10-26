#!/usr/bin/env python3
"""Pure packaging contracts plus isolated helper smoke tests; no installation."""
import importlib.util
import hashlib
import io
import json
import os
from pathlib import Path
import plistlib
import struct
import subprocess
import tempfile
import tarfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / (name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


class ReleaseTools(unittest.TestCase):
    def test_license_manifest_covers_runtime_and_vendored_notices(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            vendor = root / 'home/registry/data/xpkgs/compat-x-eui-neo/0.6.0/eui-neo-0.6.0'
            vendor.mkdir(parents=True)
            (vendor / 'LICENSE').write_text('fixture upstream notice')
            (vendor / 'private-config.json').write_text('not a license; never distribute')
            (vendor / '3rd').mkdir()
            (vendor / '3rd/stb_image.h').write_text('fixture header with its own license')
            sources = root / 'runtime.tar'
            with tarfile.open(sources, 'w') as archive:
                text = b'fixture runtime license'
                entry = tarfile.TarInfo('runtime/licenses/glib/COPYING')
                entry.size = len(text)
                archive.addfile(entry, io.BytesIO(text))
            output = root / 'notices'
            with patch.dict(os.environ, {'MCPP_HOME': str(root / 'home')}):
                module('stage_licenses').stage(output, sources)
            manifest = json.loads((output / 'LICENSE-MANIFEST.json').read_text())
            files = {path.relative_to(output).as_posix() for path in output.rglob('*')
                     if path.is_file() and path.name != 'LICENSE-MANIFEST.json'}
            self.assertEqual({entry['path'] for entry in manifest}, files)
            self.assertEqual(len(manifest), 4)
            for entry in manifest:
                self.assertEqual(entry['sha256'], hashlib.sha256((output / entry['path']).read_bytes()).hexdigest())
            self.assertFalse(any('private-config' in name for name in files))

    def test_program_selection_does_not_pick_tray_or_tests(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for relative in ['platform/hash/bin/cli/literouter', 'platform/hash/bin/tray/literouter-tray',
                             'platform/hash/bin/test_literouter']:
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('fixture')
            selected = module('find_program').find_program(root, 'cli', 'literouter')
            self.assertEqual(selected, root / 'platform/hash/bin/cli/literouter')

    def test_icon_is_deterministic(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'icon.ico'
            module('make_tray_icon').make_icon(ROOT / 'tray/assets/literouter-tray.png', output)
            self.assertEqual(output.read_bytes(), (ROOT / 'tray/assets/literouter-tray.ico').read_bytes())
            self.assertEqual(struct.unpack_from('<HHH', output.read_bytes()), (0, 1, 1))

    def test_macos_bundle_has_numeric_version_resources_and_notices(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'program'
            source.write_text('fixture executable')
            # Native Python passes Windows paths verbatim; Git Bash needs
            # slash-form drive paths, and System32's WSL bash is not this shell.
            bash = str(Path(os.environ.get('PROGRAMFILES', 'C:/Program Files')) / 'Git/bin/bash.exe') if os.name == 'nt' else 'bash'
            reply = subprocess.run([bash, (ROOT / 'scripts/make_macos_app.sh').as_posix(), '--binary', source.as_posix(),
                '--version', 'v0.5.0', '--output', (root / 'output').as_posix(), '--no-sign'], capture_output=True,
                env=os.environ | {'MACOSX_DEPLOYMENT_TARGET': '14.0'})
            self.assertEqual(reply.returncode, 0, reply.stderr.decode('utf-8', errors='replace'))
            app = root / 'output/literouter.app/Contents'
            info = plistlib.loads((app / 'Info.plist').read_bytes())
            self.assertEqual(info['CFBundleShortVersionString'], '0.5.0')
            self.assertEqual(info['CFBundleVersion'], '0.5.0')
            self.assertEqual(info['LSMinimumSystemVersion'], '14.0')
            self.assertTrue(info['LSUIElement'])
            self.assertTrue((app / 'MacOS/assets/literouter-tray.png').is_file())
            for name in ['LICENSE', 'NOTICE', 'THIRD_PARTY_NOTICES.md']:
                self.assertTrue((app / 'Resources' / name).is_file())

    def test_windows_installer_uses_scoped_path_and_does_not_kill_other_installs(self):
        source = (ROOT / 'scripts/make_windows_installer.iss').read_text(encoding='utf-8')
        self.assertIn('HKEY_CURRENT_USER', source)
        self.assertIn('EnvironmentRoot', source)
        self.assertNotIn("'/F /IM", source)
        self.assertNotIn('http://127.0.0.1:8787/__literouter/shutdown', source)
        self.assertIn('assets\\*', source)
        self.assertIn("FileExists(ExpandConstant('{app}\\.literouter-path-added'))", source)
        self.assertNotIn('Type: filesandordirs; Name: "{app}"', source)

    def test_release_requires_tests_and_passes_app_to_installer(self):
        source = (ROOT / '.github/workflows/release.yml').read_text(encoding='utf-8')
        self.assertNotIn('continue-on-error: true', source)
        self.assertIn('mcpp test -p core --profile release', source)
        self.assertIn('--app "$app"', source)
        self.assertIn('stage_linux_release.py', source)
        ci = (ROOT / '.github/workflows/ci.yml').read_text(encoding='utf-8')
        self.assertNotIn("-name 'literouter*'", ci)


if __name__ == '__main__':
    unittest.main()
