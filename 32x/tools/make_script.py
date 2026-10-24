#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The 32X's test script (sh2/plat.c, PLAT_SCRIPT) from a run tools/snes_play.py
--script wrote: "SCRP", u16 the bytes of S compared, then $1k press button
k, $40 offset value poke, $80 crc16 compare (the CRC-32's low 16 bits), 0.

  make_script.py script.txt OUT.bin

The input has one command a line: "k NAME" press a button (NAME from
KEYS), "w OFFSET VALUE" poke a byte of S, "c SIZE CRC" compare the
first SIZE bytes of S with CRC (hex). Other lines are ignored. The script
is played by sh2/plat.c and by the GBA port (gba/tools/gba_lockstep.py).
"S" is the game state struct of core/rules.h.
Licence: GPL-2.0-or-later.
"""
import struct
import sys

# button k of a $1k command (the same order as KEYS in sh2/plat.c)
KEYS = ["a", "b", "x", "y", "l", "r", "start", "select", "up", "down", "left", "right"]


def main():
    """Read the text script and write the binary one (see the module docstring)."""
    out = bytearray(b"SCRP")
    n = None                        # the size of S compared (from the last "c" line)
    body = bytearray()
    for line in open(sys.argv[1]):
        p = line.split()
        if not p:
            continue
        if p[0] == "k":
            body.append(0x10 | KEYS.index(p[1]))
        elif p[0] == "w":
            body += bytes([0x40, int(p[1]), int(p[2]) & 255])
        elif p[0] == "c":
            n = int(p[1])
            body += bytes([0x80]) + struct.pack(">H", int(p[2], 16) & 0xFFFF)
    out += struct.pack(">H", n) + body + b"\0"   # (n is None if there is no "c" line: pack fails)
    open(sys.argv[2], "wb").write(out)
    print(f"{sys.argv[2]}: {len(out)} bytes")


if __name__ == "__main__":
    main()
