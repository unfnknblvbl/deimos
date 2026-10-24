#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Extracts wall textures (and, best-effort, sprite art) from Doom RPG's J2ME asset
files: mappings.bin, palettes.bin, wtexels.bin, bitshapes.bin, stexels.bin.

Format reverse-engineered from decompiled class "r" (asset loader). Wall textures
are flat 64x64, 4-bit paletted, column-major bitmaps. Each wall texture has its own
16-colour palette slice out of palettes.bin, selected via the mappings table.

This performs no execution of game code - it is a clean-room re-implementation of
the documented byte layout, reading only data files.
"""
import struct
import sys
from pathlib import Path
from PIL import Image


def read_u32_list(data: bytes, offset: int, count: int):
    """`count` little-endian u32s from data at offset, as a list."""
    return list(struct.unpack_from(f"<{count}I", data, offset))


def load_palettes(path: Path):
    """Reads palettes.bin: a u32 byte count, then 16-bit colours. Returns
    every colour as an (r, g, b) tuple of 0..255. Each colour has red in
    bits 0..4, green in 5..10, blue in 11..15; each channel is widened to 8
    bits by repeating its top bits. Textures and sprites index this list
    from their own palette offset.
    """
    data = path.read_bytes()
    n2 = struct.unpack_from("<I", data, 0)[0]  # byte count of colour data
    n_colors = n2 // 2
    colors = []
    off = 4
    for i in range(n_colors):
        s2 = struct.unpack_from("<h", data, off)[0] & 0xFFFF
        off += 2
        r5 = s2 & 0x1F
        r8 = (r5 << 3) | (r5 >> 2)
        g6 = (s2 >> 5) & 0x3F
        g8 = (g6 << 2) | (g6 >> 4)
        b5 = (s2 >> 11) & 0x1F
        b8 = (b5 << 3) | (b5 >> 2)
        colors.append((r8, g8, b8))
    return colors


def load_mappings(path: Path):
    """Reads mappings.bin: four u32 counts (walls, sprites, wall ids, sprite
    ids), the wall table and the sprite table (two u32 per entry), then the
    two id translation tables (s16 each). Returns them in a dict (see the
    comments on its keys).
    """
    data = path.read_bytes()
    n5, n6, n7, n8 = struct.unpack_from("<4I", data, 0)
    off = 16
    wall_table = read_u32_list(data, off, n5 * 2)
    off += n5 * 2 * 4
    sprite_table = read_u32_list(data, off, n6 * 2)
    off += n6 * 2 * 4
    wall_id_resolve = list(struct.unpack_from(f"<{n7}h", data, off))
    off += n7 * 2
    sprite_id_resolve = list(struct.unpack_from(f"<{n8}h", data, off))
    off += n8 * 2
    return {
        "wall_count": n5, "sprite_count": n6,
        "wall_table": wall_table,       # [id*2+0]=file offset in wtexels.bin, [id*2+1]=palette color-index offset
        "sprite_table": sprite_table,   # [id*2+0]=file offset in bitshapes.bin, [id*2+1]=palette color-index offset
        "wall_id_resolve": wall_id_resolve,     # raw wall "texture id" (from .bsp) -> wall_table slot
        "sprite_id_resolve": sprite_id_resolve,  # raw sprite "type id" (from .bsp) -> sprite_table slot
    }


def resolve_wall_id(mappings, raw_id: int) -> int:
    """The wall_table slot of a raw wall texture id from a .bsp (ids beyond the translation table are used as they are)."""
    table = mappings["wall_id_resolve"]
    if 0 <= raw_id < len(table):
        return table[raw_id]
    return raw_id


def resolve_sprite_id(mappings, raw_id: int) -> int:
    """The sprite_table slot of a raw sprite tile number (ids beyond the translation table are used as they are)."""
    table = mappings["sprite_id_resolve"]
    if 0 <= raw_id < len(table):
        return table[raw_id]
    return raw_id


def extract_wall_texture(wtexels: bytes, file_offset: int) -> list:
    """Returns 64x64 nibble grid [row][col] (0-15), column-major source data.

    file_offset is measured in NIBBLE units (as streamed/accumulated by the
    original loader's "n14 += 4096" per 64x64=4096-pixel texture), so the byte
    position within the data region (after the 4-byte header) is file_offset/2.
    Two texels per byte, the low nibble first. row is v (down), col is u.
    """
    base = 4 + file_offset // 2
    raw = wtexels[base:base + 2048]
    nibbles = [0] * 4096
    for i, byte in enumerate(raw):
        nibbles[i * 2] = byte & 0xF
        nibbles[i * 2 + 1] = (byte >> 4) & 0xF
    # column-major: texel index = col*64 + row
    grid = [[0] * 64 for _ in range(64)]
    for col in range(64):
        for row in range(64):
            grid[row][col] = nibbles[col * 64 + row]
    return grid


def render_wall_texture(grid, palette, pal_offset) -> Image.Image:
    """The texture `grid` as a 64x64 RGB image, its indices looked up from palette[pal_offset]."""
    img = Image.new("RGB", (64, 64))
    px = img.load()
    for row in range(64):
        for col in range(64):
            idx = grid[row][col]
            color = palette[pal_offset + idx]
            px[col, row] = color
    return img


def main():
    """extract_textures.py <jar dir> <out dir> [raw ids...]: saves wall
    textures as wall_NNN.png (every one, or the ids given), for looking at
    them. Not part of the build; the build imports the loaders above.
    """
    jar_dir = Path(sys.argv[1])
    out_dir = Path(sys.argv[2])
    tex_ids = [int(x) for x in sys.argv[3:]] if len(sys.argv) > 3 else None
    out_dir.mkdir(parents=True, exist_ok=True)

    palette = load_palettes(jar_dir / "palettes.bin")
    print(f"Loaded {len(palette)} palette colours")

    mappings = load_mappings(jar_dir / "mappings.bin")
    print(f"Wall textures: {mappings['wall_count']}, sprite shapes: {mappings['sprite_count']}")

    wtexels = (jar_dir / "wtexels.bin").read_bytes()

    ids = tex_ids if tex_ids is not None else range(mappings["wall_count"])
    for raw_tid in ids:
        tid = resolve_wall_id(mappings, raw_tid)
        if tid * 2 + 1 >= len(mappings["wall_table"]):
            print(f"  tex raw={raw_tid} resolved={tid}: out of range, skipping")
            continue
        file_off = mappings["wall_table"][tid * 2]
        pal_off = mappings["wall_table"][tid * 2 + 1]
        if pal_off + 16 > len(palette):
            print(f"  tex raw={raw_tid} resolved={tid}: palette offset {pal_off} out of range ({len(palette)} colours), skipping")
            continue
        grid = extract_wall_texture(wtexels, file_off)
        img = render_wall_texture(grid, palette, pal_off)
        img.save(out_dir / f"wall_{raw_tid:03d}.png")
        print(f"  tex raw={raw_tid} resolved={tid}: file_off={file_off} pal_off={pal_off} -> wall_{raw_tid:03d}.png")


if __name__ == "__main__":
    main()
