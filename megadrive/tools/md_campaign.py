#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The campaign's data block: every map of analysis/campaign.json, at the end
of the ROM. The code finds it through the ROM directory in the cartridge
header's notes field ($1C8): "DEIMOSMD", u32 block address, u32 the end of
the space the block may use. Big-endian; every offset below is from the
start of the block, every table starts on a 4-byte boundary.

Block:
  0   "MDCP", u16 version (VERSION, now 2), u16 number of maps
  8   u32 texture bank, u32 sprite bank, u32 weapons, u32 block size
  24  two u32 zeros (reserved)
  32  per map, 32 bytes: u32 map pack, u8 original map id, u8 0,
      title (26 bytes, zero-padded)
  then the weapons (4 x u32 offset, u32 size: frames, sprites, palettes,
  tiles), the banks and the map packs. Every console but the Master System
  carries this same block (the SNES-layout ones in their cartridge image);
  the Deimos Editor's builder (editor/src/md_build.cpp) makes the same bytes.

Texture bank: 64x64 wall textures (4096 bytes, column-major colour
indices), each once however many maps use it. Sprite bank: each sprite's
column data, once. A map's colours, histograms and column tables are its
own (they are small).

Map pack:
  0   "MDPK", u16 version (2), u16 number of entries (ENTRIES)
  8   u16 walls, textures, sprites, scenery, floor colour, ceiling colour
  20  u8 number of texture swaps (the Reactor's portal), then (from, to)
      byte pairs
  32  per entry: u32 offset, u32 size
  tex_off: u32 per texture (its place in the texture bank); spr_off: u32
  per sprite (in the sprite bank); vis: per (cell, facing) a list of the
  cells whose things can show (u16 each, $FFFF ends it), vis_index: the
  word offset of each list ($FFFF: every cell).

Usage: md_campaign.py <analysis dir> <jar dir> build/gen
Writes build/gen/campaign.bin and build/gen/campaign.s.
Licence: GPL-2.0-or-later.
"""
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from build_render_data import tables, write_common  # noqa: E402

VERSION = 2  # the block's format version (header offset 4)
PACK_VERSION = 2  # each map pack's format version
# a map pack's tables, in order (the ROM looks them up by index; see
# build_render_data.py for what each holds, gen_cdata.py for level and strings)
ENTRIES = ["walls", "tex_off", "tex_pal", "tex_hist", "spr_cols", "spr_off", "spr_coff", "pvs_index", "pvs",
           "spr_pal", "spr_hist", "scenery", "def_spr", "vis_index", "vis", "level", "strings"]
WEAPONS = ["wp_frames", "wp_sprites", "wp_pal", "wp_tiles"]  # the weapon picture tables (build_weapons.py), shared by every map
MAP_REC = 32  # bytes per map record
HEAD = 32  # bytes of block header before the map records


def pack_vals(fmt, vals):
    """Packs the numbers `vals` as big-endian struct type `fmt` (such as "H" or "I")."""
    return struct.pack(f">{len(vals)}{fmt}", *[int(v) for v in vals])


def vis_lists(flat):
    """128-byte cell maps -> (index words, list words): a map as the list of
    its cells; map 0 (every cell) becomes $FFFF.

    flat: build_render_data's "vis" table, cell bitmaps of 1024 bits (bit
    c&7 of byte c>>3). Returns, per bitmap, the word offset of its list in
    the list words (0xFFFF for bitmap 0), and the lists themselves, each a
    run of cell numbers ended by 0xFFFF. Raises SystemExit if the lists do
    not fit in 16-bit offsets.
    """
    maps = [flat[i:i + 128] for i in range(0, len(flat), 128)]
    out, starts = [], []
    for k, m in enumerate(maps):
        if k == 0:
            starts.append(0xFFFF)
            continue
        starts.append(len(out))
        for c in range(1024):
            if m[c >> 3] >> (c & 7) & 1:
                out.append(c)
        out.append(0xFFFF)
    if len(out) >= 0xFFFF:
        raise SystemExit("visibility lists too long")
    return starts, out


class Block:
    """The campaign block being built: a growing bytearray."""
    def __init__(self):
        """Starts empty."""
        self.data = bytearray()

    def add(self, b):
        """Appends b on a 4-byte boundary; returns its offset in the block."""
        self.data += bytes(-len(self.data) % 4)
        off = len(self.data)
        self.data += b
        return off


def main():
    """md_campaign.py <analysis dir> <jar dir> <gen dir>: builds the campaign
    block (layout in the module docstring) and writes <gen>/campaign.bin
    and campaign.s (which puts the block, then the per-area colours that
    build.sh makes with tools/md_areas.c, into the ROM's .pack section);
    also writes build_render_data's shared files (write_common). Prints
    the sizes.
    """
    adir, jar, gen = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    camp = json.loads((adir / "campaign.json").read_text())["maps"]
    n = len(camp)
    blk = Block()
    blk.data += bytes(HEAD + MAP_REC * n)  # the header and map records, filled in at the end
    blk.data[0:8] = b"MDCP" + struct.pack(">HH", VERSION, n)

    # weapons: one copy for every map
    wpk = gen / "pack"
    wp = blk.add(bytes(8 * len(WEAPONS)))  # the weapons' directory: (offset, size) per table
    for i, name in enumerate(WEAPONS):
        b = (wpk / f"{name}.bin").read_bytes()
        struct.pack_into(">II", blk.data, wp + 8 * i, blk.add(b), len(b))

    # every map's tables; textures and sprites go to the banks once each
    per_map = []
    tex_bank, tex_at = bytearray(), {}
    spr_bank, spr_at = bytearray(), {}
    for m in camp:
        lvl = json.loads((adir / "maps" / f"{m['name']}.bsp.json").read_text())
        g = json.loads((adir / "maps" / f"{m['name']}.json").read_text())
        t = tables(lvl, g, jar)
        c = t["counts"]
        px = bytes(t["tex"][1])  # this map's textures, 4096 bytes each
        tex_off = []
        for k in range(c["n_tex"]):
            key = px[k * 4096:(k + 1) * 4096]
            if key not in tex_at:  # a texture already in the bank is shared
                tex_at[key] = len(tex_bank)
                tex_bank += key
            tex_off.append(tex_at[key])
        sd = bytes(t["spr_data"][1])
        so = list(t["spr_off"][1]) + [len(sd)]  # each sprite's start in spr_data, plus the end
        spr_off = []
        for k in range(c["n_spr"]):
            key = sd[so[k]:so[k + 1]]
            if key not in spr_at:
                spr_bank += bytes(-len(spr_bank) % 2)  # sprites start on even offsets (word reads)
                spr_at[key] = len(spr_bank)
                spr_bank += key
            spr_off.append(spr_at[key])
        vi, vl = vis_lists(bytes(t["vis"][1]))
        tabs = {name: pack_vals(*t[name]) for name in ("walls", "tex_pal", "tex_hist", "spr_cols", "spr_coff",
                                                       "pvs_index", "pvs", "spr_pal", "spr_hist", "scenery",
                                                       "def_spr")}
        tabs["tex_off"] = pack_vals("I", tex_off)
        tabs["spr_off"] = pack_vals("I", spr_off)
        tabs["vis_index"] = pack_vals("H", [vi[k] for k in t["vis_index"][1]])  # per (cell, facing): its list's word offset
        tabs["vis"] = pack_vals("H", vl)
        tabs["level"] = (gen / "maps" / m["name"] / "level.bin").read_bytes()
        tabs["strings"] = (gen / "maps" / m["name"] / "strings.bin").read_bytes()
        per_map.append((c, tabs))
    tb = blk.add(bytes(tex_bank))
    sb = blk.add(bytes(spr_bank))
    struct.pack_into(">III", blk.data, 8, tb, sb, wp)  # header: texture bank, sprite bank, weapons

    for i, (m, (c, tabs)) in enumerate(zip(camp, per_map)):
        head = bytearray(32 + 8 * len(ENTRIES))
        head[0:8] = b"MDPK" + struct.pack(">HH", PACK_VERSION, len(ENTRIES))
        head[8:20] = struct.pack(">6H", c["n_walls"], c["n_tex"], c["n_spr"], c["n_scenery"], c["floor"],
                                 c["ceiling"])
        sw = c.get("swaps", [])           # the portal's texture swaps (n, then slot pairs)
        head[20] = len(sw)
        for k, (a, b) in enumerate(sw):
            head[21 + 2 * k], head[22 + 2 * k] = a, b
        po = blk.add(bytes(head))
        for k, name in enumerate(ENTRIES):
            struct.pack_into(">II", blk.data, po + 32 + 8 * k, blk.add(tabs[name]), len(tabs[name]))
        title = m["title"].upper().encode("latin1")[:25]  # at most 25 characters, so at least one zero follows
        rec = struct.pack(">IBB", po, m["id"], 0) + title + bytes(26 - len(title))
        blk.data[HEAD + MAP_REC * i:HEAD + MAP_REC * (i + 1)] = rec
    blk.data += bytes(-len(blk.data) % 4)
    struct.pack_into(">I", blk.data, 20, len(blk.data))  # header: the block's size
    (gen / "campaign.bin").write_bytes(blk.data)
    (gen / "campaign_locked.bin").unlink(missing_ok=True)  # (an older build's second block)
    (gen / "campaign.s").write_text("| generated by tools/md_campaign.py: the campaign's data block, then\n"
                                    "| the Mega Drive's colours per area (tools/md_areas.c, build.sh)\n"
                                    "    .section .pack, \"a\"\n    .global md_campaign\nmd_campaign:\n"
                                    "    .incbin \"build/gen/campaign.bin\"\n"
                                    "    .balign 4\n    .global md_areas\nmd_areas:\n"
                                    "    .incbin \"build/gen/areas.bin\"\n")
    write_common(gen)
    sizes = [sum(len(v) for v in tabs.values()) for _c, tabs in per_map]
    print(f"campaign: {n} maps, {len(blk.data) // 1024} KB: textures {len(tex_bank) // 1024} KB "
          f"({len(tex_at)}), sprites {len(spr_bank) // 1024} KB ({len(spr_at)}), maps "
          f"{min(sizes) // 1024}..{max(sizes) // 1024} KB")


if __name__ == "__main__":
    main()
