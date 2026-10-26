#!/usr/bin/env python3
"""Collect exact installed upstream license notices into each binary package."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import tarfile


def stage(output: Path, runtime_sources: Path | None = None):
    root = Path(__file__).resolve().parents[1]
    home = Path(os.environ.get('MCPP_HOME', Path.home() / '.mcpp'))
    stores = [home / 'registry/data/xpkgs', home / 'xpkgs', home / 'registry/xpkgs']
    output.mkdir(parents=True, exist_ok=True)
    # Scan only license/notice files in the pinned dependencies, never copy
    # arbitrary cached application data or secrets into a release archive.
    packages = ['compat-x-eui-neo/0.6.0', 'compat-x-freetype/2.13.3',
        'compat-x-libpng/1.6.43', 'compat-x-yyjson/0.12.0', 'compat-x-tray/0.0.0-8dd1358',
        'compat-x-glfw/3.4', 'compat-x-glad/0.0.0-651a425', 'compat-x-khrplatform/2026.05.31',
        'compat-x-opengl/2026.05.31', 'compat-x-x11/1.8.13', 'compat-x-xcb/1.17.0',
        'compat-x-xorgproto/2025.1', 'compat-x-zlib/1.3.2', 'xim-x-zlib/1.3.1',
        'xim-x-libffi/3.4.4', 'xim-x-pcre2/10.42']
    manifest = []
    def record(target):
        manifest.append({'path': target.relative_to(output).as_posix(),
                         'sha256': hashlib.sha256(target.read_bytes()).hexdigest()})

    shutil.copy2(root / 'LICENSE', output / 'Apache-2.0.txt')
    record(output / 'Apache-2.0.txt')
    upstream_count = 0
    for package in packages:
        directory = next((store / package for store in stores if (store / package).exists()), None)
        if directory is None:
            continue # platform-inapplicable X11 components, for example
        for source in directory.rglob('*'):
            if not source.is_file() or source.is_symlink() or source.stat().st_size > 2_000_000:
                continue
            name = source.name.upper()
            if not (name.startswith(('COPYING', 'LICENSE', 'COPYRIGHT', 'NOTICE')) or
                    name in ['FTL.TXT', 'GPLV2.TXT'] or '/LICENSES/' in str(source).upper()):
                continue
            relative = Path(package) / source.relative_to(directory)
            target = output / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            record(target)
            upstream_count += 1
    if runtime_sources:
        with tarfile.open(runtime_sources) as archive:
            for entry in archive.getmembers():
                if entry.isfile() and '/licenses/' in entry.name:
                    relative = entry.name.split('/licenses/', 1)[1]
                    if '..' in Path(relative).parts or Path(relative).is_absolute():
                        raise RuntimeError('unsafe source license member')
                    target = output / 'linux-runtime' / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with archive.extractfile(entry) as source, target.open('wb') as file:
                        shutil.copyfileobj(source, file)
                    record(target)
    # Header-only codecs carry their notices inside the header, rather than a
    # standalone LICENSE file. Include complete pinned headers as source/notice
    # material so no contributor attribution is synthesized or accidentally lost.
    for name in ['stb_image.h', 'nanosvg.h', 'nanosvgrast.h', 'miniaudio.h']:
        source = next((store / 'compat-x-eui-neo/0.6.0/eui-neo-0.6.0/3rd' / name
                       for store in stores if (store / 'compat-x-eui-neo/0.6.0/eui-neo-0.6.0/3rd' / name).is_file()), None)
        if source is not None:
            target = output / 'eui-vendored-headers' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            record(target)
    if not upstream_count:
        raise RuntimeError('no installed third-party license notices were found')
    manifest.sort(key=lambda entry: entry['path'])
    (output / 'LICENSE-MANIFEST.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Staged {len(manifest)} upstream license notices at {output}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--runtime-sources', type=Path)
    args = parser.parse_args()
    stage(args.output.resolve(), args.runtime_sources)
