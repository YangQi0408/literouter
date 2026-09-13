#!/usr/bin/env python3
"""
patch_elf_interp.py

Inspects 64-bit ELF binaries and patches PT_INTERP in-place to use standard
/lib64/ld-linux-x86-64.so.2 when the binary was linked against an isolated
toolchain dynamic linker (such as ~/.mcpp/.../ld-linux-x86-64.so.2).
"""

import os
import struct
import sys

DEFAULT_INTERP = b"/lib64/ld-linux-x86-64.so.2"

def patch_file(file_path: str, new_interp: bytes = DEFAULT_INTERP) -> bool:
    if not os.path.isfile(file_path):
        return False

    with open(file_path, "r+b") as f:
        data = bytearray(f.read())
        # Check ELF magic and 64-bit
        if len(data) < 64 or data[:4] != b"\x7fELF" or data[4] != 2:
            return False

        e_phoff = struct.unpack_from("<Q", data, 32)[0]
        e_phentsize = struct.unpack_from("<H", data, 54)[0]
        e_phnum = struct.unpack_from("<H", data, 56)[0]

        target = new_interp + b"\0"
        patched = False

        for i in range(e_phnum):
            offset = e_phoff + i * e_phentsize
            p_type = struct.unpack_from("<I", data, offset)[0]
            if p_type == 3:  # PT_INTERP
                p_offset = struct.unpack_from("<Q", data, offset + 8)[0]
                p_filesz = struct.unpack_from("<Q", data, offset + 32)[0]
                old_interp = data[p_offset : p_offset + p_filesz].split(b"\0")[0]

                if old_interp == new_interp:
                    # Already standard
                    return True

                if len(target) <= p_filesz:
                    # Overwrite in-place and pad remaining bytes with zeroes
                    padded = target + b"\0" * (p_filesz - len(target))
                    data[p_offset : p_offset + p_filesz] = padded
                    f.seek(0)
                    f.write(data)
                    print(f"Patched {file_path}: PT_INTERP '{old_interp.decode('utf-8', 'replace')}' -> '{new_interp.decode()}'")
                    patched = True
                else:
                    print(f"Warning: new interp longer than {p_filesz} bytes for {file_path}", file=sys.stderr)
                break

        return patched

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: patch_elf_interp.py <elf-binary> [<elf-binary>...]", file=sys.stderr)
        sys.exit(1)

    for path in sys.argv[1:]:
        patch_file(path)
