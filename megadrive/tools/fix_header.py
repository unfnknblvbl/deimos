#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""Pads the ROM to a power of two (at least 128 KB, at most 4 MB), writes the
ROM end address and the checksum into the header.
Licence: GPL-2.0-or-later."""
import struct
import sys

# Usage: fix_header.py <in.bin> <out.md> (build.sh's last step). No functions: it runs at import.
data = bytearray(open(sys.argv[1], "rb").read())
size = 0x20000  # start at 128 KB and double until the ROM fits
while size < len(data):
    size *= 2
if size > 0x400000:
    raise SystemExit("the ROM is bigger than the Mega Drive's 4 MB")
data += b"\xff" * (size - len(data))  # pad with $FF (erased flash)
struct.pack_into(">I", data, 0x1A4, size - 1)  # header $1A4: ROM end address (the start, $1A0, is 0)
csum = sum(struct.unpack(">%dH" % ((size - 0x200) // 2), data[0x200:])) & 0xFFFF  # the standard checksum: sum of the big-endian words after the header
struct.pack_into(">H", data, 0x18E, csum)  # header $18E: checksum
open(sys.argv[2], "wb").write(data)
print("%s: %d KB, checksum %04X" % (sys.argv[2], size // 1024, csum))
