#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The Game Boy Advance cartridge: the code (src/gba.ld: from $08000000, at
most 512 KB), then the SNES game's cartridge image as it is (its trailing
$FF bytes left off) from $08080000, read at its SNES addresses
(src/plat.c); the header's logo and checksum. A test build's script
(32x/tools/make_script.py's) goes after the SNES image's 4 MB
($08480000).

  gba_rom.py code.bin data.sfc OUT.gba [script.bin]

The cartridge is padded with $FF to a multiple of 64 KB.
Licence: GPL-2.0-or-later.
"""
import sys
from pathlib import Path

DATA_AT = 0x80000                   # the SNES image's offset in the cartridge
SNES_SIZE = 0x400000                # the most it may be (the script goes after this)
# the boot logo the GBA's BIOS checks (as every GBA cartridge has it)
LOGO = bytes.fromhex(
    "24ffae51699aa2213d84820a84e409ad11248b98c0817f21a352be199309ce2010464a4af82731ec58c7e83382e3ceb"
    "f85f4df94ce4b09c194568ac01372a7fc9f844d73a3ca9a615897a327fc039876231dc7610304ae56bf38840040a70e"
    "fdff52fe036f9530f197fbc08560d68025a963be03014e38e2f9a234ffbb3e0344780090cb88113a9465c07c6387f03"
    "cafd625e48b380aac7221d4f807")


def header(rom):
    """Put the logo at $04 and the header checksum at $BD: minus the sum of bytes $A0..$BC, minus $19."""
    rom[4:4 + len(LOGO)] = LOGO
    c = 0
    for i in range(0xA0, 0xBD):
        c = (c - rom[i]) & 0xFF
    rom[0xBD] = (c - 0x19) & 0xFF


def main():
    """Build the cartridge from the command-line arguments (see the module docstring)."""
    code, data, out = sys.argv[1:4]
    c = Path(code).read_bytes()
    d = Path(data).read_bytes().rstrip(b"\xFF")
    if len(c) > DATA_AT:
        raise SystemExit(f"the code does not fit: {len(c)} bytes")
    if len(d) > SNES_SIZE:
        raise SystemExit("the SNES image is more than 4 MB")
    rom = bytearray(b"\xFF" * (DATA_AT + len(d)))
    rom[:len(c)] = c
    rom[DATA_AT:DATA_AT + len(d)] = d
    if len(sys.argv) > 4 and sys.argv[4]:
        rom += b"\xFF" * (DATA_AT + SNES_SIZE - len(rom))
        rom += Path(sys.argv[4]).read_bytes()
    rom += b"\xFF" * (-len(rom) % 0x10000)     # to a multiple of 64 KB
    assert len(LOGO) == 156
    header(rom)
    Path(out).write_bytes(rom)
    print(f"{out}: {len(rom) // 1024} KB (code {len(c) // 1024} KB, data {len(d) // 1024} KB)")


if __name__ == "__main__":
    main()
