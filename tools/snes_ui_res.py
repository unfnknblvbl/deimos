#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The SNES ROM's resources made from the original game (DoomRPG.jar): the
Mega Drive's own (megadrive/tools/build_md_ui.py, run here with the
title's word SUPER and the SNES version), as they are - the SNES converts
them as it loads them (snes/src/gfx.s md_tiles, ui.c). The editor makes
the same bytes from the user's jar.

res.bin (from a bank boundary; the header's $FFB7 says which bank):
  "DEIMRES1", u16 number of entries, u16 0
  per entry: u32 offset (from the start), u32 size   (little-endian)
  then the entries, in NAMES order (Mega Drive formats: tiles 4 bits a
  pixel, two pixels a byte; colours and name tables big-endian words)

spc_streams: the music and the sound effects for the SPC (tools/
snes_sound.py's spc_streams.bin, from the Mega Drive game's streams).

Usage: snes_ui_res.py <deimos dir> <out res.bin> <spc_streams.bin> <version> [title word]
(the title word defaults to SUPER; it is passed to build_md_ui.py)
Licence: GPL-2.0-or-later.
"""
import struct
import sys
from pathlib import Path

# the entries, in order (gb_res.py's E_* and the SNES code use these indices)
NAMES = ["ui_font", "ui_faces", "ui_icons", "ui_hud_pal", "ui_dlg_pal", "title_tiles", "title_map", "title_pal",
         "spc_streams"]


def build(arrays):
    """res.bin from {name: bytes}: the header, the offset / size table, the
    entries (each on an even offset)"""
    head = bytearray(b"DEIMRES1" + struct.pack("<HH", len(NAMES), 0))
    head += bytes(8 * len(NAMES))
    body = bytearray()
    for i, n in enumerate(NAMES):
        b = arrays[n]
        while (len(head) + len(body)) % 2:
            body.append(0)
        struct.pack_into("<II", head, 12 + 8 * i, len(head) + len(body), len(b))
        body += b
    return bytes(head + body)


def main():
    """Run megadrive/tools/build_md_ui.py into a scratch folder, take its arrays
    (ui_data.c), add the SPC streams, write res.bin."""
    md = Path(sys.argv[1])
    sys.path.insert(0, str(md / "tools"))
    from make_engine import c_arrays
    import subprocess
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        subprocess.run([sys.executable, str(md / "tools/build_md_ui.py"), str(md / "assets/jar"), d,
                        sys.argv[4], sys.argv[5] if len(sys.argv) > 5 else "SUPER"], check=True, capture_output=True)
        arrays = c_arrays(Path(d) / "ui_data.c")
    arrays["spc_streams"] = Path(sys.argv[3]).read_bytes()
    out = build(arrays)
    Path(sys.argv[2]).write_bytes(out)
    print(f"{sys.argv[2]}: {len(out) // 1024} KB")


if __name__ == "__main__":
    main()
