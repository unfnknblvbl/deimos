#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The Virtual Boy cartridge (8 MB, the ROM at $07000000):

  $000000   the code (src/vb.ld: at most 512 KB)
  $080000   the SNES game's cartridge image as it is (its trailing $FF
            bytes left off), read at its SNES addresses (src/plat.c)
  $480000   a test build's script (32x/tools/make_script.py's) or view
            list (vb_view_test.py's), if any
  $7FFDE0   the header: the title (20 bytes), 5 zero bytes, the maker
            (2), the game's code (4), its version (1)
  $7FFE00   the interrupt vectors (src/crt0.s's .vectors, 512 bytes; the
            CPU starts at the last 16, $FFFFFFF0)

  vb_rom.py code.bin vectors.bin data.sfc OUT.vb [script.bin]

The rest is $FF.
Licence: GPL-2.0-or-later.
"""
import sys
from pathlib import Path

SIZE = 0x800000                     # 8 MB (a power of two: the console mirrors the ROM)
DATA_AT = 0x80000                   # the SNES image
SCRIPT_AT = 0x480000                # a script or a view list
HEADER_AT = SIZE - 0x220
TITLE = b"DEIMOS"                   # (20 bytes, padded with spaces)
MAKER, CODE = b"DM", b"VDRE"        # (the maker's and the game's codes: Deimos's own)


def main():
    """Build the cartridge from the command-line arguments (see the module docstring)."""
    code, vec, data, out = (Path(a).read_bytes() if i < 3 else a for i, a in enumerate(sys.argv[1:5]))
    d = data.rstrip(b"\xFF")
    if len(code) > DATA_AT:
        raise SystemExit(f"the code does not fit: {len(code)} bytes")
    if len(d) > SCRIPT_AT - DATA_AT:
        raise SystemExit("the SNES image is more than 4 MB")
    if len(vec) != 0x200:
        raise SystemExit(f"the vectors are {len(vec)} bytes, not 512")
    rom = bytearray(b"\xFF" * SIZE)
    rom[:len(code)] = code
    rom[DATA_AT:DATA_AT + len(d)] = d
    if len(sys.argv) > 5 and sys.argv[5]:
        s = Path(sys.argv[5]).read_bytes()
        if SCRIPT_AT + len(s) > HEADER_AT:
            raise SystemExit("the script is too big")
        rom[SCRIPT_AT:SCRIPT_AT + len(s)] = s
    rom[HEADER_AT:HEADER_AT + 20] = TITLE.ljust(20)
    rom[HEADER_AT + 20:HEADER_AT + 25] = bytes(5)
    rom[HEADER_AT + 25:HEADER_AT + 27] = MAKER
    rom[HEADER_AT + 27:HEADER_AT + 31] = CODE
    rom[HEADER_AT + 31] = 0
    rom[SIZE - 0x200:] = vec
    Path(out).write_bytes(rom)
    print(f"{out}: {len(rom) // 1024} KB (code {len(code) // 1024} KB, data {len(d) // 1024} KB)")


if __name__ == "__main__":
    main()
