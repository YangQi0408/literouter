#!/usr/bin/env python3
"""Select an exact member program, never a similarly named desktop/test image."""
import argparse
from pathlib import Path


def find_program(root: Path, member: str, name: str):
    candidates = [file for file in root.glob(f'*/*/bin/{member}/{name}') if file.is_file()]
    candidates += [file for file in root.glob(f'*/*/bin/{member}/{name}.exe') if file.is_file()]
    if not candidates:
        raise FileNotFoundError(f'no built {member}/{name} under {root}')
    return max(candidates, key=lambda file: file.stat().st_mtime_ns)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--member', default='cli')
    parser.add_argument('--name', default='literouter')
    parser.add_argument('--root', type=Path, default=Path('target'))
    args = parser.parse_args()
    print(find_program(args.root, args.member, args.name))
