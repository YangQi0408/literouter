#!/usr/bin/env python3
"""Make the portable PNG-in-ICO icon without external imaging dependencies."""
from pathlib import Path
import struct


def make_icon(source: Path, output: Path):
    png = source.read_bytes()
    if not png.startswith(b'\x89PNG\r\n\x1a\n'):
        raise ValueError('tray icon is not PNG')
    width, height = struct.unpack_from('>II', png, 16)
    if not (1 <= width <= 256 and 1 <= height <= 256):
        raise ValueError('ICO source must be no larger than 256 pixels')
    directory = struct.pack('<HHH', 0, 1, 1)
    entry = struct.pack('<BBBBHHII', width % 256, height % 256, 0, 0, 1, 32, len(png), 22)
    output.write_bytes(directory + entry + png)


if __name__ == '__main__':
    root = Path(__file__).resolve().parents[1]
    make_icon(root / 'tray/assets/literouter-tray.png', root / 'tray/assets/literouter-tray.ico')
