#!/usr/bin/env python3
"""Ship corresponding source and build/relocation scripts for Linux runtime libs.

The desktop bundle uses dynamically replaceable GNU/GLib libraries. Their
source archive accompanies the binary release; this is not a future source
offer or a claim that LGPL components are Apache-licensed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import tarfile
import tempfile
import urllib.request


SOURCES = {
    'glibc': ('2.44', 'https://ftp.gnu.org/gnu/glibc/glibc-2.44.tar.xz'),
    'glib': ('2.88.3', 'https://download.gnome.org/sources/glib/2.88/glib-2.88.3.tar.xz'),
    'util-linux': ('2.40.2', 'https://mirrors.edge.kernel.org/pub/linux/utils/util-linux/v2.40/util-linux-2.40.2.tar.xz'),
    'libselinux': ('3.11', 'https://github.com/SELinuxProject/selinux/releases/download/3.11/libselinux-3.11.tar.gz'),
}
SOURCE_DIGESTS = {
    'glibc': '37f600f2bef3c5e8300147059568b2a2e40a7ad6ccc65ce942556d49429cc667',
    'glib': 'ab24d24e698dfa1e408b7bcdb508f4aafc906185a8b8ce72fdf79bbbdc9b383b',
    'util-linux': 'd78b37a66f5922d70edf3bdfb01a6b33d34ed3c3cafd6628203b2a2b67c8e8b3',
    'libselinux': '73d419c6e20e874adaa4019372cbd097eecf4d276e13f27ec5e67d35c0bd203c',
}


def stage(output: Path):
    home = Path(os.environ.get('MCPP_HOME', Path.home() / '.mcpp'))
    registry = home / 'registry/data'
    recipes = registry / 'xim-pkgindex'
    if not (recipes / '.agents/tools/graphics/build-glibc.sh').is_file():
        raise RuntimeError('GNU runtime corresponding build scripts are not available in the registry')
    with tempfile.TemporaryDirectory(prefix='literouter-runtime-sources-') as temporary:
        root = Path(temporary) / 'literouter-linux-runtime-sources'
        root.mkdir()
        manifest = []
        for name, (upstream, url) in SOURCES.items():
            expected = '2.44.3' if name == 'glibc' else upstream
            payload = registry / 'xpkgs' / ('xim-x-' + name) / expected
            metadata = payload / '.xpkg-install.json'
            if not metadata.is_file():
                raise RuntimeError(f'{name} runtime version changed; update corresponding source mapping')
            installed = json.loads(metadata.read_text())
            if installed.get('version') != expected:
                raise RuntimeError(f'cannot identify installed {name} source')
            destination = root / url.rsplit('/', 1)[-1]
            with urllib.request.urlopen(url, timeout=90) as response, destination.open('wb') as file:
                shutil.copyfileobj(response, file)
            digest = hashlib.sha256(destination.read_bytes()).hexdigest()
            if digest != SOURCE_DIGESTS[name]:
                raise RuntimeError(f'{name} corresponding source checksum mismatch')
            manifest.append({'component': name, 'upstream_version': upstream,
                'payload_version': installed['version'], 'payload_revision': installed.get('revision', 0),
                'url': url, 'sha256': digest})
            shutil.copy2(payload / '.xpkg.lua', root / (name + '-payload-recipe.lua'))
            # Carry license/copyright files from the EXACT upstream source,
            # beside the archive so users need not extract all sources to read them.
            with tarfile.open(destination) as archive:
                for entry in archive.getmembers():
                    if entry.isfile() and (Path(entry.name).name.upper().startswith(('COPYING', 'LICENSE')) or
                                           '/LICENSES/' in entry.name or '/licenses/' in entry.name or
                                           Path(entry.name).name.startswith(('LGPL-', 'GPL-', 'BSD-', 'MIT'))):
                        if '..' in Path(entry.name).parts or entry.name.startswith('/'):
                            raise RuntimeError('unsafe license source path')
                        license_path = root / 'licenses' / name / entry.name
                        license_path.parent.mkdir(parents=True, exist_ok=True)
                        with archive.extractfile(entry) as source, license_path.open('wb') as file:
                            shutil.copyfileobj(source, file)
        def build_inputs(directory, names):
            return [name for name in names if not (Path(directory) / name).is_dir() and
                    Path(name).suffix not in ['.sh', '.py', '.lua', '.patch', '.md']]
        shutil.copytree(recipes / '.agents/tools/graphics', root / 'build-and-relocation',
                        ignore=build_inputs)
        shutil.copytree(Path(__file__).resolve().parent, root / 'literouter-packaging-scripts',
                        ignore=shutil.ignore_patterns('__pycache__', 'tests'))
        (root / 'SOURCES.json').write_text(json.dumps(manifest, indent=2) + '\n')
        (root / 'README.txt').write_text(
            'Corresponding upstream source, payload patches/build scripts and relocation recipes\n'
            'for the GNU/GLib runtime shipped with literouter on Linux.\n'
            'The packaged runtime remains dynamically linked and may be replaced/relinked;\n'
            'reverse engineering for debugging library modifications is not restricted.\n'
            'See SOURCES.json for versions and digests, licenses/ for upstream license texts.\n'
            'build-and-relocation/build-glibc.sh 2.44 2.44.3 2 builds the GNU payload;\n'
            'build-gtk4-stack.sh --only glib records the GLib configure options.\n')
        output.parent.mkdir(parents=True, exist_ok=True)
        if output.exists():
            raise RuntimeError('refusing to overwrite a runtime source archive')
        with tarfile.open(output, 'w:xz') as archive:
            archive.add(root, arcname=root.name)
    print('GNU/GLib runtime corresponding source:', output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    stage(args.output.resolve())
