#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""Trims the linked ROM to the smallest power-of-two size that holds every
used bank (wlalink writes all 256 banks of the memory map) and reports it.
Usage: rom_size.py rom/deimos.sms [--gg]   (--gg: a Game Gear ROM, region 7)

Run by build.sh (step 4 and 4b) straight after wlalink. It rewrites the ROM
file in place:
  1. drops the trailing all-zero 16 KB banks, rounding up to a power of two
     of at least 128 KB (the smallest size the header's size code allows here);
  2. fills in the Sega header at $7FF0..$7FFF (the "TMR SEGA" header in
     bank 1): the 16-bit checksum at $7FFA (little-endian) and the
     region/size byte at $7FFF (high nibble region, low nibble size code).
The BIOS of an export Master System checks this header before it boots the
cartridge.
Licence: GPL-2.0-or-later."""
import sys
from pathlib import Path

p = Path(sys.argv[1])
b = p.read_bytes()
BANK = 0x4000                   # one Sega-mapper bank: 16 KB
# the highest bank that holds any non-zero byte
last = max(i for i in range(len(b) // BANK) if any(b[i * BANK:(i + 1) * BANK]))
size = BANK
# round up to a power of two (ROM sizes the header can describe)
while size < (last + 1) * BANK:
    size *= 2
size = max(size, 0x20000)      # never below 128 KB (size code $F)
p.write_bytes(b[:size])
print(f"ROM: {last + 1} banks used -> {size // 1024} KB")

# header (bank 1, $7FF0): checksum of the whole ROM except the header, and
# the size code, region 4 (export). Size codes: $F 128 KB, $0 256 KB,
# $1 512 KB, $2 1 MB. The checksum skips the 16 header bytes $7FF0..$7FFF.
code = {0x20000: 0xF, 0x40000: 0x0, 0x80000: 0x1, 0x100000: 0x2}[size]
b = bytearray(p.read_bytes())
chk = (sum(b[:0x7FF0]) + sum(b[0x8000:size])) & 0xFFFF
b[0x7FFA] = chk & 0xFF
b[0x7FFB] = chk >> 8
region = 0x70 if "--gg" in sys.argv else 0x40     # 7: Game Gear international, 4: SMS export
b[0x7FFF] = region | code
p.write_bytes(bytes(b))
print(f"header: checksum ${chk:04X}, size code {code:X}")
