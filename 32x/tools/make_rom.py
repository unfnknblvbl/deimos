#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The 32X cartridge: the 68000's part (m68k: vectors, headers, code; at 0),
the SH-2s' (sh2: from $4000, its SDRAM image first), and the game's data
(the SNES build's cartridge image, which the SH-2s read at its SNES
addresses: sh2/plat.c) from $80000. The 32X header ($3D4..$3EF) gets the
SH-2s' entries from the SH-2 ELF's symbols.

  make_rom.py m68k.bin sh2.bin sh2.elf.syms data.sfc OUT.32x [script.bin]
(a test script, tools/make_script.py's, goes at $3E0000)

Cartridge layout (offsets in the file; the SH-2 sees them at $02000000 +,
the 68000 at $880000 + once the 32X is on):
  $000000  the 68000's binary (vectors, headers, code; under $4000)
  $004000  the SH-2 binary: first the .sdram image the boot ROM copies to
           SDRAM, then the code run from the cartridge (up to $80000)
  $080000  the game's data (the 32X's SNES image, tools/mars_image.py)
  $3E0000  (test builds) the script; the cartridge is then a full 4 MB
The size is rounded up to a power of two (512 KB at least), padded with
$FF. Header fields written here: the 32X header's SH-2 entries at $3D4,
the Mega Drive header's ROM end at $1A4 and its checksum at $18E (the
16-bit sum of the big-endian words from $200 to the end).
Licence: GPL-2.0-or-later.
"""
import struct
import sys
from pathlib import Path

DATA_AT = 0x80000                   # where the game's data starts in the cartridge


def main():
    """Build the cartridge from the command-line arguments (see the module docstring)."""
    m68k, sh2, syms, data, out = sys.argv[1:6]
    m = Path(m68k).read_bytes()
    s = Path(sh2).read_bytes()
    sym = {}                        # symbol name -> address, from nm's "ADDR TYPE NAME" lines
    for line in Path(syms).read_text().splitlines():
        p = line.split()
        if len(p) >= 3:
            sym[p[2]] = int(p[0], 16)
    if len(m) > 0x4000 or 0x4000 + len(s) > DATA_AT:
        raise SystemExit("the code does not fit")
    d = Path(data).read_bytes().rstrip(b"\xFF")    # (trailing padding dropped: it is re-added below)
    rom = bytearray(b"\xFF" * (DATA_AT + len(d)))
    rom[:len(m)] = m
    rom[0x4000:0x4000 + len(s)] = s
    rom[DATA_AT:DATA_AT + len(d)] = d
    size = 0x80000                  # the smallest power of two that holds it all
    while size < len(rom):
        size *= 2
    if size > 0x400000:
        raise SystemExit("more than 4 MB")
    rom += b"\xFF" * (size - len(rom))
    if len(sys.argv) > 6 and sys.argv[6]:       # a test script: the cartridge grows to 4 MB
        sc = Path(sys.argv[6]).read_bytes()
        if DATA_AT + len(d) > 0x3E0000 or 0x3E0000 + len(sc) > 0x400000:
            raise SystemExit("the script does not fit")
        rom += b"\xFF" * (0x400000 - len(rom))
        size = 0x400000
        rom[0x3E0000:0x3E0000 + len(sc)] = sc
    # the 32X header: the SDRAM image's ROM offset, its SDRAM offset (0), its
    # size, the master's and slave's start, their vector tables (sh2/sh2.ld,
    # sh2/crt0.s)
    struct.pack_into(">IIIIIII", rom, 0x3D4, sym["__sdram_lma"] - 0x02000000, 0, sym["__sdram_size"],
                     sym["_master_start"], sym["_slave_start"], sym["_master_vbr"], sym["_slave_vbr"])
    struct.pack_into(">I", rom, 0x1A4, size - 1)   # the Mega Drive header's ROM end
    csum = 0                        # the Mega Drive header's checksum
    for i in range(0x200, size, 2):
        csum = (csum + (rom[i] << 8 | rom[i + 1])) & 0xFFFF
    struct.pack_into(">H", rom, 0x18E, csum)
    Path(out).write_bytes(rom)
    print(f"{out}: {size // 1024} KB (68000 {len(m)} bytes, SH-2 {len(s) // 1024} KB, data {len(d) // 1024} KB)")


if __name__ == "__main__":
    main()
