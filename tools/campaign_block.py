#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Reads the Mega Drive game's campaign block (tools/md_campaign.py format: big-endian,
offsets from the block's start), for the ports' tools and tests.

The "campaign block" holds every map of the campaign:
  0    "MDCP", 6 u16 the number of maps
  8    u32 tex_bank, spr_bank, weapons, size (the textures' and sprites'
       pixel data, the weapons' tables, the block's size); 24 two u32
       zeros (reserved)
  32 + 32 m   map m: u32 its pack's offset, u8 the original game's id,
              then its title (bytes 6..31, NUL-padded)
A map's "pack": 8 six u16 (walls, textures, sprites, scenery, floor and
ceiling colours), 20 u8 number of texture swaps, 21 the swaps (pairs);
32 + 8 k: u32 offset, u32 size of entry k (ENTRIES order). The "level"
entry is the level blob the game rules read; "strings" the map's text.
Used by snes_fx_block.py, snes_view_test.py and the other ports' tools.
Licence: GPL-2.0-or-later.
"""
import struct
from pathlib import Path

# a pack's entries, in order (src/campaign.h's P_* on each port)
ENTRIES = ["walls", "tex_off", "tex_pal", "tex_hist", "spr_cols", "spr_off", "spr_coff", "pvs_index", "pvs",
           "spr_pal", "spr_hist", "scenery", "def_spr", "vis_index", "vis", "level", "strings"]
# each table's element (struct format: big-endian h/H/I/B); level and strings are raw
FMT = {"walls": "h", "tex_off": "I", "tex_pal": "H", "tex_hist": "H", "spr_cols": "B", "spr_off": "I",
       "spr_coff": "H", "pvs_index": "H", "pvs": "H", "spr_pal": "H", "spr_hist": "H", "scenery": "h",
       "def_spr": "B", "vis_index": "H", "vis": "H"}


class Block:
    """A campaign block, from a file path or bytes: b the bytes, n_maps,
    tex_bank / spr_bank / weapons / size from the header."""

    def __init__(self, path_or_bytes):
        self.b = Path(path_or_bytes).read_bytes() if not isinstance(path_or_bytes, (bytes, bytearray)) \
            else bytes(path_or_bytes)
        assert self.b[:4] == b"MDCP"
        self.n_maps = struct.unpack_from(">H", self.b, 6)[0]
        self.tex_bank, self.spr_bank, self.weapons, self.size = struct.unpack_from(">4I", self.b, 8)

    def map(self, i):
        """map i (a Map)"""
        return Map(self, i)


class Map:
    """One map: its record (pack, orig_id, title), its pack's counts and
    swaps, raw[name] each entry's bytes and t[name] the entries decoded as
    lists (FMT)."""

    def __init__(self, blk, i):
        b = blk.b
        self.blk = blk
        rec = 32 + 32 * i
        self.pack, self.orig_id = struct.unpack_from(">IB", b, rec)
        self.title = b[rec + 6:rec + 32].rstrip(b"\0").decode("latin1")
        p = self.pack
        (self.n_walls, self.n_tex, self.n_spr, self.n_scenery, self.floor,
         self.ceiling) = struct.unpack_from(">6H", b, p + 8)
        n = b[p + 20]
        self.swaps = [(b[p + 21 + 2 * k], b[p + 22 + 2 * k]) for k in range(n)]
        self.raw, self.t = {}, {}
        for k, name in enumerate(ENTRIES):
            off, size = struct.unpack_from(">II", b, p + 32 + 8 * k)
            self.raw[name] = b[off:off + size]
            if name in FMT:
                f = FMT[name]
                self.t[name] = list(struct.unpack(f">{size // struct.calcsize(f)}{f}", self.raw[name]))

    def texture(self, k):
        """texture k's 64 x 64 bytes (a byte a texel, a colour index)"""
        o = self.blk.tex_bank + self.t["tex_off"][k]
        return self.blk.b[o:o + 4096]                     # u*64 + v

    def sprite_data(self, k):
        """the block from sprite k's data on (its length is not stored here)"""
        return self.blk.b[self.blk.spr_bank + self.t["spr_off"][k]:]
