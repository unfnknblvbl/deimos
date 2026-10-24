#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Reads the renderer's level data back out of a ROM (through the level
header in bank 3, rom/live_layout.inc) into the form tools/live_data.py's
Model takes. So the render test can check any ROM, including ones the
editor made, against the bit-exact model: the model draws from exactly the
data the console has.

Usage (as a module): D = rom_level.load(path)
Usage (as a script): rom_level.py ROM   (prints counts of what it found)

The "level header" is a 64-byte record at $8000 in bank 3 (fields in
live_data.H_FIELDS) that gives the bank and address of every renderer table.
A "view" here is one (cell x, cell z, facing) position the player can stand
in; its visibility list ("pvs") names the walls and props seen from there.
Licence: GPL-2.0-or-later.
"""
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import live_data as L

BANK = 0x4000                   # one Sega-mapper bank: 16 KB


def load(path):
    """Decode the level data of the ROM at path into live_data's Model form.

    Returns a dict: walls (tuples ax, az, bx, bz, tex, line, ua, ub, two),
    pvs {(x, z, facing): (wall ids, scenery ids)}, scenery (x, z, sprite),
    texcols (per texture, 64 columns of 64 colour-pair bytes), sprites
    (per prop sprite, (top row, length, bytes) per canvas column), key (the
    transparent pair byte), ceil_pair and floor_pair (fixed 0x00 and 0x11),
    tables (screen size tables per view size), palette (16 RGB entries) and
    cram (the 16 raw SMS colour bytes).
    """
    rom = Path(path).read_bytes()

    def at(bank, addr, n):
        """n ROM bytes at CPU address addr ($8000-$BFFF) with bank in slot 2."""
        o = bank * BANK + (addr - 0x8000)
        return rom[o:o + n]
    hdr = at(3, 0x8000, L.HEADER_SIZE)
    assert hdr[:2] == b"LV", "no level header in bank 3"
    H, off = {}, 0
    # unpack the header fields (little-endian; the magic is kept as bytes)
    for name, size in L.H_FIELDS:
        H[name] = int.from_bytes(hdr[off:off + size], "little") if name != "magic" else hdr[off:off + size]
        off += size
    n_walls = H["n_walls"]
    wb = at(H["walls_bank"], H["walls_addr"], 16 * n_walls)
    # 16 bytes per wall: two end points (s16 x, z), texture, line, texture
    # u at each end, two-sided flag, 3 pad bytes
    walls = [struct.unpack_from("<hhhhBBBBB", wb, 16 * i) for i in range(n_walls)]
    # visibility: index u16 per (cell*4 + f), 0 = none, else 1 + offset into the lists
    # (32x32 cells x 4 facings = 4096 entries; cell = z * 32 + x)
    index = struct.unpack("<4096H", at(H["pvs_index_bank"], H["pvs_index_addr"], 8192))
    pvs = {}
    for k, v in enumerate(index):
        if not v:
            continue
        o = v - 1
        bank, addr = H["pvs_bank"] + o // BANK, 0x8000 + o % BANK
        rec = at(bank, addr, 96)           # a list is at most 96 bytes, never across a bank
        # record: wall count, wall ids (u16), prop count, scenery ids (u8)
        nw = rec[0]
        wl = list(struct.unpack_from(f"<{nw}H", rec, 1))
        ns = rec[1 + 2 * nw]
        sl = list(rec[2 + 2 * nw:2 + 2 * nw + ns])
        cell, f = k // 4, k % 4
        pvs[(cell % 32, cell // 32, f)] = (wl, sl)
    n_tex = 1 + max(w[4] for w in walls)   # textures in use: up to the highest wall texture
    # texcols: per texture, 64 u16 column ids into the shared column pool
    ids = struct.unpack(f"<{64 * n_tex}H", at(H["texcols_bank"], H["texcols_addr"], 128 * n_tex))

    def column(c):
        """Column c of the pool: 64 bytes, 256 columns per bank from tex_bank."""
        return at(H["tex_bank"] + c // 256, 0x8000 + (c % 256) * 64, 64)
    texcols = [[column(ids[t * 64 + u]) for u in range(64)] for t in range(n_tex)]
    n_scen = 0                              # (overwritten below)
    # scenery records: x, z (s16), sprite (u8), pad; read the most there can be
    scen_raw = at(H["scenery_bank"], H["scenery_addr"], 6 * 256)
    scen_pvs = {s for _, sl in pvs.values() for s in sl}
    n_scen = 1 + max(scen_pvs) if scen_pvs else 0   # the count is not stored: the highest id seen
    scenery = [struct.unpack_from("<hhB", scen_raw, 6 * i) for i in range(n_scen)]
    n_spr = 1 + max((s[2] for s in scenery), default=-1)
    # billboard sprite directory: 8 bytes each, u32 offset into the sprite
    # blob, first and end canvas row used, pad
    dirb = at(H["bsprite_dir_bank"], H["bsprite_dir_addr"], 8 * max(1, n_spr))
    key = H["bill_key"]
    sprites = []
    for i in range(n_spr):
        o, a, b = struct.unpack_from("<IBB", dirb, 8 * i)
        bank, addr = H["bspr_bank"] + o // BANK, 0x8000 + o % BANK
        rec = at(bank, addr, BANK - (addr - 0x8000))
        top, ln, data = [], [], []
        # each sprite starts with 64 u16 column offsets (0: empty column);
        # a column is 64 pair bytes, the key byte marking transparent rows
        for c in range(64):
            co = struct.unpack_from("<H", rec, 2 * c)[0]
            if co == 0:
                top.append(0)
                ln.append(0)
                data.append(b"")
                continue
            col = rec[co:co + 64]
            rows = [r for r in range(64) if col[r] != key]
            r0, r1 = (rows[0], rows[-1] + 1) if rows else (0, 0)
            top.append(r0)
            ln.append(r1 - r0)
            data.append(bytes(col[r0:r1]))
        sprites.append((top, ln, data))
    cram = at(H["palette_bank"], H["palette_addr"], 16)
    # SMS CRAM byte %00BBGGRR: 2 bits per channel, scaled 0..3 -> 0..255
    palette = np.array([[(c & 3) * 85, ((c >> 2) & 3) * 85, ((c >> 4) & 3) * 85] for c in cram], np.uint8)
    # the screen tables are not read from the ROM: they depend only on the size
    tables = {name: L.size_table(*L.SIZES[name])[1] for name in L.SIZES}
    return {"walls": walls, "pvs": pvs, "scenery": scenery, "texcols": texcols, "sprites": sprites, "key": key,
            "ceil_pair": 0x00, "floor_pair": 0x11, "tables": tables, "palette": palette, "cram": cram}


if __name__ == "__main__":
    D = load(sys.argv[1])
    print(f"{len(D['walls'])} walls, {len(D['pvs'])} views, {len(D['texcols'])} textures, "
          f"{len(D['scenery'])} props, {len(D['sprites'])} prop sprites")
