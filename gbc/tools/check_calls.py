#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
After the link: a plain call (not through the banked-call trampoline) to a
function in another ROM bank jumps into whatever that bank's address holds
in the caller's bank (a prototype without __banked: view/view.h's, say).
Every such call fails the build.  check_calls.py obj/code.noi obj/*.asm

How: the linker's .noi file gives each symbol's address, with its ROM bank
in bits 16 and up. A .asm file's bank is the bank most of the functions it
defines (labels "_name::") were placed in; any plain "call _name" to a
symbol in another switchable bank (address 0x10000 and up) is reported.
Calls into bank 0 (always mapped) are fine.
Licence: GPL-2.0-or-later.
"""
import re
import sys

# the link's symbols: "DEF name address" lines
addr = {}
for line in open(sys.argv[1]):
    p = line.split()
    if len(p) == 3 and p[0] == "DEF":
        addr[p[1]] = int(p[2], 16)
bad = 0
for f in sys.argv[2:]:
    text = open(f).read()
    # the banks of the functions this file defines (bank 0 ones left out)
    mine = [addr[n] >> 16 for n in re.findall(r"^(_\w+)::", text, re.M) if n in addr and addr[n] >= 0x4000]
    if not mine:
        continue
    bank = max(set(mine), key=mine.count)   # (the file's bank: the most common)
    for n in sorted(set(re.findall(r"\bcall\s+(_\w+)", text))):
        a = addr.get(n)
        if a is not None and a >= 0x10000 and (a >> 16) != bank:
            print(f"{f}: a plain call to {n} in bank {a >> 16} from bank {bank}")
            bad += 1
sys.exit(1 if bad else 0)
