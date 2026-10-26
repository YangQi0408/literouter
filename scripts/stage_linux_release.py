#!/usr/bin/env python3
"""Stage relocatable Linux programs and the tray's complete runtime closure.

Only copies/mutates newly staged files. Never patches a build or installed binary.
The CLI uses system glibc; the tray's launcher selects one matching private
loader/runtime pair and its shared closure. Missing dependencies fail packaging.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from patch_elf_interp import patch_file


def run(*arguments):
    return subprocess.check_output(arguments, text=True, stderr=subprocess.STDOUT)


def closure(binary: Path):
    text = run('ldd', str(binary))
    if 'not found' in text:
        raise RuntimeError('unresolved tray dependency:\n' + text)
    result = {}
    for line in text.splitlines():
        match = re.match(r'\s*(\S+) => (/\S+) \(', line)
        if match:
            name, source = match.groups()
            if name.startswith('/') and Path(name).name == 'ld-linux-x86-64.so.2':
                name = 'ld-linux-x86-64.so.2'
            result[name] = Path(source).resolve(strict=True)
    # ldd is a system-loader trace, and can report its OWN loader even for a
    # program built against another glibc. The ELF PT_INTERP is authoritative.
    interpreter = Path(run('patchelf', '--print-interpreter', str(binary)).strip()).resolve(strict=True)
    result[interpreter.name] = interpreter
    libc = result.get('libc.so.6')
    if libc is None or not interpreter.is_relative_to(libc.parent.parent):
        raise RuntimeError('tray loader and libc are not from the same runtime payload')
    return result


def relocate(binary: Path, rpath: str):
    if not patch_file(str(binary)):
        raise RuntimeError(f'could not relocate interpreter: {binary}')
    run('patchelf', '--set-rpath', rpath, str(binary))


def reject_private_paths(checked: str, output: Path):
    # A runner's own staging directory is the package under test, not an old
    # registry dependency. Validate resolved paths rather than matching its home.
    for source in re.findall(r'=>\s+(/\S+)', checked):
        path = Path(source).resolve()
        if path.is_relative_to(output.resolve()):
            continue
        if '/.mcpp/' in source or '/home/runner/' in source:
            raise RuntimeError('staged program still reaches build-private paths:\n' + checked)


def stage(cli: Path, tray: Path, output: Path):
    output.mkdir(parents=True, exist_ok=True)
    if (output / 'literouter').exists() or (output / 'literouter-tray').exists():
        raise RuntimeError('refusing to replace an already staged program')
    libraries = closure(tray)
    runtime = output / 'lib/tray'
    runtime.mkdir(parents=True)
    provenance = []
    # The tray's GLib closure can require newer glibc than a user's system.
    # Stage the matching loader/libc pair, not just a loader paired with another
    # libc. The CLI deliberately remains a plain system-glibc executable.
    for name, source in libraries.items():
        if '/' in name:
            raise RuntimeError(f'absolute dependency is not portable: {name}')
        target = runtime / name
        shutil.copy2(source, target)
        provenance.append({'name': name, 'source_name': source.name,
                           'payload': next(('/'.join(source.relative_to(parent).parts[:2])
                                            for parent in source.parents if parent.name == 'xpkgs'),
                                           'workspace-built'),
                           'sha256_before_relocation': hashlib.sha256(source.read_bytes()).hexdigest()})
        if name != 'ld-linux-x86-64.so.2':
            run('patchelf', '--set-rpath', '$ORIGIN', str(target))
        needed = run('patchelf', '--print-needed', str(target)).splitlines()
        for dependency in needed:
            if '/' in dependency:
                raise RuntimeError(f'{name} has an absolute DT_NEEDED: {dependency}')
    for source, name, rpath in [(cli, 'literouter', '$ORIGIN'),
                                (tray, 'lib/tray/literouter-tray.bin', '$ORIGIN')]:
        target = output / name
        shutil.copy2(source, target)
        target.chmod(0o755)
        relocate(target, rpath)
        # Resolve using ONLY the relocatable search paths, not old registry
        # rpaths or LD_LIBRARY_PATH inherited from a developer shell.
        checker = ['ldd', str(target)] if name == 'literouter' else [
            str(runtime / 'ld-linux-x86-64.so.2'), '--library-path', str(runtime), '--list', str(target)]
        checked = subprocess.check_output(checker, text=True,
            env=os.environ | {'LD_LIBRARY_PATH': ''}, stderr=subprocess.STDOUT)
        if 'not found' in checked:
            raise RuntimeError('staged program is missing libraries:\n' + checked)
        reject_private_paths(checked, output)
    launcher = output / 'literouter-tray'
    launcher.write_text('''#!/bin/sh
set -eu
app_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
runtime="$app_dir/lib/tray"
exec "$runtime/ld-linux-x86-64.so.2" --library-path "$runtime" \\
  --argv0 "$app_dir/literouter-tray" "$runtime/literouter-tray.bin" "$@"
''')
    launcher.chmod(0o755)
    # --list alone does not perform every relocation. Execute the actual entry
    # point so mismatched GLIBC_PRIVATE loader symbols cannot pass packaging.
    subprocess.run([str(launcher), '--help'], check=True, timeout=15,
                   env=os.environ | {'LD_LIBRARY_PATH': ''}, capture_output=True)
    root = Path(__file__).resolve().parents[1]
    assets = output / 'assets'
    assets.mkdir(exist_ok=True)
    for name in ['literouter-tray.png', 'literouter-tray.ico']:
        shutil.copy2(root / 'tray/assets' / name, assets / name)
    (output / 'RUNTIME-LIBRARIES.json').write_text(json.dumps(provenance, indent=2) + '\n')
    print(f'Staged Linux CLI + tray and {len(libraries)} shared runtime libraries at {output}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--tray', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    stage(args.cli.resolve(), args.tray.resolve(), args.output.resolve())
