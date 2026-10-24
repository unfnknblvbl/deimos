#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Extracts "masked" textures (RLE, variable shape) from Doom RPG's J2ME assets:
mappings.bin (sprite table) + bitshapes.bin (shape/run descriptors) + stexels.bin
(pixel data) + palettes.bin.

This format is used both for true sprites (monsters, items, props - billboards)
and for "see-through" wall midtextures (bars, windows) whose id exceeds the flat
64x64 wall-texture table size, so the engine falls back to this richer table.

Reverse engineered from decompiled class "r", methods c() [[bitshapes/shape parse]]
and the second half of the (unnamed by CFR -> void_a) wtexels/stexels loader.

Usage: extract_sprites.py <jar_dir> <out_dir> id [id ...]  (writes spr_NNN.png)
As a module, its decoders are used by sprite_art.py (SpriteArt.canvas).

Shape record layout in bitshapes.bin (offsets from the record start):
  0..3   u32 position of the shape's pixels, in nibbles, in the texel
         address space that wtexels.bin starts and stexels.bin continues
  4..7   not used here
  8..11  top, bottom, left, right: the bounding box (inclusive)
  12..   one bit mask per strip, (right - left + 1 + 7) // 8 bytes each,
         bit c%8 of byte c//8 set where pixel c of the strip is drawn
The strips run from top to bottom (that many of them); the set pixels are
stored back to back as 4-bit palette indices, low nibble first.
"""
import struct
import sys
from pathlib import Path
from PIL import Image

from extract_textures import load_palettes, load_mappings, resolve_sprite_id


def load_bitshapes(path: Path):
    """Read bitshapes.bin: a u32 byte count, then the shape records; return those bytes."""
    data = path.read_bytes()
    count = struct.unpack_from("<I", data, 0)[0]
    raw = data[4:4 + count]
    return raw  # sArray equivalent: index with raw[offset]


def decode_shape(sArray: bytes, rec_offset: int):
    """Decode the shape record at rec_offset in sArray (the load_bitshapes bytes).

    Returns a dict: file_offset (u32 nibble position of the pixels), width
    (pixels per strip, from left..right), height (number of strips, from
    top..bottom), left, top, rows (one list per strip of runs
    (start, length, index of the run's first pixel in the packed data)) and
    pixel_count (the total set pixels). Despite the names, each "row" is a
    vertical strip of the sprite (see render_shape).
    """
    file_off = sArray[rec_offset] | (sArray[rec_offset + 1] << 8) | (sArray[rec_offset + 2] << 16) | (sArray[rec_offset + 3] << 24)
    top = sArray[rec_offset + 8]
    bottom = sArray[rec_offset + 8 + 1]
    left = sArray[rec_offset + 8 + 2]
    right = sArray[rec_offset + 8 + 3]
    height = bottom - top + 1
    width = right - left + 1
    row_bytes = (width + 7) // 8       # mask bytes per strip
    rows = []
    pixel_count = 0
    base = rec_offset + 8 + 4
    for row in range(height):
        row_off = base + row * row_bytes
        bits = sArray[row_off:row_off + row_bytes]
        runs = []
        col = 0
        # split the strip's mask into runs of set bits
        while col < width:
            while col < width and not (bits[col // 8] & (1 << (col % 8))):
                col += 1
            if col == width:
                break
            start = col
            while col < width and (bits[col // 8] & (1 << (col % 8))):
                col += 1
            runlen = col - start
            runs.append((start, runlen, pixel_count))
            pixel_count += runlen
        rows.append(runs)
    return {
        "file_offset": file_off, "width": width, "height": height,
        "left": left, "top": top, "rows": rows, "pixel_count": pixel_count,
    }


def render_shape(shape, stexels: bytes, palette, pal_offset, boundary_nibbles: int) -> Image.Image:
    """Draw a decoded shape as an RGBA image, transparent where no pixel is set.

    stexels is the whole stexels.bin (4-byte header, then texels); palette
    the list from load_palettes; pal_offset the shape's first colour in it;
    boundary_nibbles the nibble size of wtexels.bin's data, where stexels
    begins in the shared address space. The image is shape["height"] wide
    (one column per strip) and shape["width"] tall. Out-of-range colours
    are drawn magenta.
    """
    # file_offset is in the same nibble-unit continuous address space as wall
    # textures; stexels.bin continues right after wtexels.bin's nibble range.
    byte_pos = (shape["file_offset"] - boundary_nibbles) // 2
    base = 4 + byte_pos                # skip stexels.bin's 4-byte header
    nbytes = (shape["pixel_count"] + 1) // 2
    raw = stexels[base:base + nbytes]
    nibbles = [0] * (nbytes * 2)
    # unpack two 4-bit palette indices per byte, low nibble first
    for i, byte in enumerate(raw):
        nibbles[i * 2] = byte & 0xF
        nibbles[i * 2 + 1] = (byte >> 4) & 0xF

    # The shape's "rows" are really vertical STRIPS (Doom RPG stores sprites
    # column-major, left strip first, like its wall textures) - so strip index
    # is the x coordinate and the run position is y. Getting this backwards
    # produced sprites rotated/transposed 90 degrees in the first builds.
    img = Image.new("RGBA", (shape["height"], shape["width"]), (0, 0, 0, 0))
    px = img.load()
    for col_idx, runs in enumerate(shape["rows"]):
        for start, runlen, pstart in runs:
            for i in range(runlen):
                nib = nibbles[pstart + i] if (pstart + i) < len(nibbles) else 0
                color = palette[pal_offset + nib] if pal_offset + nib < len(palette) else (255, 0, 255)
                px[col_idx, start + i] = (*color, 255)
    return img


def main():
    """Write spr_NNN.png for each sprite id on the command line."""
    jar_dir = Path(sys.argv[1])
    out_dir = Path(sys.argv[2])
    ids = [int(x) for x in sys.argv[3:]]
    out_dir.mkdir(parents=True, exist_ok=True)

    palette = load_palettes(jar_dir / "palettes.bin")
    mappings = load_mappings(jar_dir / "mappings.bin")
    sArray = load_bitshapes(jar_dir / "bitshapes.bin")
    stexels = (jar_dir / "stexels.bin").read_bytes()
    wtexels_header = struct.unpack_from("<I", (jar_dir / "wtexels.bin").read_bytes(), 0)[0]
    boundary_nibbles = wtexels_header * 2     # wtexels.bin's data size in nibbles

    for raw_sid in ids:
        sid = resolve_sprite_id(mappings, raw_sid)
        if sid * 2 + 1 >= len(mappings["sprite_table"]):
            print(f"  id raw={raw_sid} resolved={sid}: out of range for sprite table ({mappings['sprite_count']} entries)")
            continue
        # sprite_table pairs: shape record offset, first palette colour
        rec_off = mappings["sprite_table"][sid * 2]
        pal_off = mappings["sprite_table"][sid * 2 + 1]
        shape = decode_shape(sArray, rec_off)
        if pal_off + 16 > len(palette):
            print(f"  id raw={raw_sid} resolved={sid}: palette offset out of range, skipping")
            continue
        img = render_shape(shape, stexels, palette, pal_off, boundary_nibbles)
        img.save(out_dir / f"spr_{raw_sid:03d}.png")
        print(f"  id raw={raw_sid} resolved={sid}: {shape['width']}x{shape['height']} @ ({shape['left']},{shape['top']}) "
              f"pixels={shape['pixel_count']} fileOff={shape['file_offset']} -> spr_{raw_sid:03d}.png")


if __name__ == "__main__":
    main()
