#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Reading an engine file (editor/engines/*.eng; the format is in
megadrive/tools/make_engine.py): its blanked regions, the parts the editor
makes from the user's jar.

The builds without DoomRPG.jar (DEIMOS_NOJAR=1, the consoles' nojar.py)
put zeros where those parts go, each the size its region has. The sizes
are those the jar's pictures and sounds give; they place the code that
follows them, so they must be the same with or without the jar.

  engine_file.py <engine.eng>     prints the regions: name, offset, size
Licence: GPL-2.0-or-later.
"""
import struct
import sys
from pathlib import Path


def regions(path):
    """{name: (offset, size)} of the engine file's blanked regions."""
    b = Path(path).read_bytes()
    if b[:8] != b"DEIMENG1":
        raise SystemExit(f"{path} is not an engine file")
    n = struct.unpack_from("<I", b, 16)[0]
    o, out = 20, {}
    for _ in range(n):
        k = b[o]
        name = b[o + 1:o + 1 + k].decode()
        out[name] = struct.unpack_from("<II", b, o + 1 + k)
        o += 9 + k
    return out


if __name__ == "__main__":
    for name, (off, size) in regions(sys.argv[1]).items():
        print(f"{name:20} ${off:06X} {size}")
