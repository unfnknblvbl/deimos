#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The campaign's block for the SuperFX build: the Mega Drive game's (tools/md_campaign.py
format) without what the GSU does not need - the visibility tables (pvs,
pvs_index, vis, vis_index: the GSU looks at every wall and thing), so the
game fits the GSU's 2 MB. Same layout rules: every table 4-aligned, offsets from the block's start; the empty
entries keep their place (offset, size 0).

Usage: snes_fx_block.py <campaign.bin> <out.bin>
Licence: GPL-2.0-or-later.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from campaign_block import Block, ENTRIES  # noqa: E402

DROP = {"pvs_index", "pvs", "vis_index", "vis"}   # (the entries emptied)


def trim(src):
    """the block src (a path or bytes) rebuilt without the DROP tables: the
    header and map records first, then the weapons, the texture and sprite
    banks, each map's pack and entries, the texts table; every offset in
    the header and records rewritten"""
    b = Block(src)
    raw = b.b
    n = b.n_maps
    if n == 0:                  # (an empty block - the builds without the jar: nothing to trim)
        return bytes(raw)
    out = bytearray(raw[:32 + 32 * n])

    def add(data):
        """data appended at the next 4-aligned offset; returns that offset"""
        while len(out) % 4:
            out.append(0)
        off = len(out)
        out.extend(data)
        return off

    # weapons
    wp_old = b.weapons
    wp = add(bytes(32))
    for k in range(4):
        o, s = struct.unpack_from(">II", raw, wp_old + 8 * k)
        struct.pack_into(">II", out, wp + 8 * k, add(raw[o:o + s]), s)
    first = min(b.map(i).pack for i in range(n))
    # the textures (up to the sprites) and the sprites (up to the first pack)
    tb = add(raw[b.tex_bank:b.spr_bank])
    sb = add(raw[b.spr_bank:first])
    struct.pack_into(">III", out, 8, tb, sb, wp)          # (header: tex_bank, spr_bank, weapons)
    for i in range(n):
        m = b.map(i)
        head = bytearray(raw[m.pack:m.pack + 32 + 8 * len(ENTRIES)])
        po = add(bytes(head))
        for k, name in enumerate(ENTRIES):
            data = b"" if name in DROP else m.raw[name]
            struct.pack_into(">II", out, po + 32 + 8 * k, add(data), len(data))
        struct.pack_into(">I", out, 32 + 32 * i, po)
    # the texts table (the game's texts), last as in the Mega Drive's block
    struct.pack_into(">II", out, 24, add(raw[b.texts:b.texts + b.texts_size]), b.texts_size)
    while len(out) % 4:
        out.append(0)
    struct.pack_into(">I", out, 20, len(out))            # (header: the block's size)
    return bytes(out)


def main():
    """Command line: see the module's docstring."""
    data = trim(Path(sys.argv[1]).read_bytes())
    Path(sys.argv[2]).write_bytes(data)
    print(f"{sys.argv[2]}: {len(data) // 1024} KB")


if __name__ == "__main__":
    main()
