#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Helpers for packing graphics into Sega Master System VDP (Mode 4) format:
- CRAM colour quantization (2 bits/channel, byte = --BBGGRR)
- 8x8 4bpp planar tile packing
- A simple tile deduplicator + tilemap builder

Used by the art builders (build_ui_gfx.py, build_sprites.py, live_data.py
and others). The Master System VDP's colour RAM (CRAM) holds 32 colours:
16 for the background (palette bank 0) and 16 the background or sprites
can use (bank 1). A tile is 8x8 pixels of 4-bit colour indices, stored as
4 bitplanes per row. A name table (tilemap) entry is 16 bits: tile number
bits 0-8, then horizontal flip, vertical flip, palette bank and priority.
"""
from PIL import Image


def rgb_to_sms(r, g, b):
    """Quantize an 8-bit RGB triple to the nearest SMS 2-bit/channel colour,
    returned as (r2,g2,b2) each in 0..3."""
    def q(v):
        """One 0..255 channel -> the nearest of 0, 85, 170, 255, as 0..3."""
        return round(v / 255 * 3)
    return q(r), q(g), q(b)


def sms_cram_byte(r2, g2, b2):
    """Pack three 0..3 channels into an SMS CRAM byte %00BBGGRR."""
    return (b2 & 3) << 4 | (g2 & 3) << 2 | (r2 & 3)


def sms_to_rgb(r2, g2, b2):
    """Three 0..3 SMS channels -> an 8-bit (r, g, b) triple (x85 each)."""
    def e(v):
        """0..3 -> 0..255."""
        return v * 85
    return e(r2), e(g2), e(b2)


class TileSet:
    """Collects unique 8x8 tiles (each a tuple of 64 palette-index values,
    0-15) and assigns them indices, building the packed VRAM tile data and
    a 16-colour (or dual 16-colour) palette along the way."""

    def __init__(self):
        """Start with no tiles and both palette banks black."""
        self.tiles = []          # list of 64-tuples (palette indices 0..15)
        self.tile_index = {}     # tuple -> index
        self.palettes = [[(0, 0, 0)] * 16, [(0, 0, 0)] * 16]  # 2 banks of 16 RGB colours (already SMS-quantized)

    def set_palette(self, bank: int, colors16):
        """colors16: list of up to 16 (r,g,b) 0-255 tuples; quantized to SMS here."""
        pal = [(0, 0, 0)] * 16
        for i, c in enumerate(colors16[:16]):
            r2, g2, b2 = rgb_to_sms(*c)
            pal[i] = (r2, g2, b2)
        self.palettes[bank] = pal

    def add_tile(self, pixel_idx_64) -> int:
        """Index of the tile with these 64 pixel indices, adding it if new."""
        key = tuple(pixel_idx_64)
        if key in self.tile_index:
            return self.tile_index[key]
        idx = len(self.tiles)
        self.tiles.append(key)
        self.tile_index[key] = idx
        return idx

    def slice_image_indices(self, idx_grid, tiles_w, tiles_h):
        """idx_grid: 2D list [row][col] of palette indices, size (tiles_h*8)x(tiles_w*8).
        Returns a 2D list [ty][tx] of tile indices (adds new tiles as needed)."""
        out = [[0] * tiles_w for _ in range(tiles_h)]
        for ty in range(tiles_h):
            for tx in range(tiles_w):
                block = []
                for y in range(8):
                    row = idx_grid[ty * 8 + y]
                    block.extend(row[tx * 8: tx * 8 + 8])
                out[ty][tx] = self.add_tile(block)
        return out

    def pack_tiles_binary(self) -> bytes:
        """SMS tile format: 32 bytes/tile, 8 rows x 4 planes, 1 byte/plane/row."""
        out = bytearray()
        for tile in self.tiles:
            for row in range(8):
                planes = [0, 0, 0, 0]
                for col in range(8):
                    val = tile[row * 8 + col] & 0xF
                    bitpos = 7 - col       # leftmost pixel is bit 7
                    for p in range(4):     # bit p of the colour goes to plane p
                        if val & (1 << p):
                            planes[p] |= (1 << bitpos)
                out.extend(planes)
        return bytes(out)

    def pack_palette_binary(self) -> bytes:
        """The 32 CRAM bytes: bank 0's 16 colours, then bank 1's."""
        out = bytearray()
        for bank in range(2):
            for (r2, g2, b2) in self.palettes[bank]:
                out.append(sms_cram_byte(r2, g2, b2))
        return bytes(out)


def image_to_indices_via_palette(img: Image.Image, palette16):
    """Map an RGB image's pixels to the closest colour in palette16 (list of
    (r,g,b) 0-255), returning a 2D list of palette indices (0..15).

    Both the pixels and the palette are first quantised to SMS 0..3
    channels; the distance is squared, ties go to the lower index."""
    w, h = img.size
    px = img.convert("RGB").load()
    # Precompute SMS-quantized palette for nearest-match in SMS colour space
    sms_pal = [rgb_to_sms(*c) for c in palette16]
    out = [[0] * w for _ in range(h)]
    cache = {}
    for y in range(h):
        for x in range(w):
            c = px[x, y]
            if c in cache:
                out[y][x] = cache[c]
                continue
            cr, cg, cb = rgb_to_sms(*c)
            best, best_d = 0, None
            for i, (r2, g2, b2) in enumerate(sms_pal):
                d = (cr - r2) ** 2 + (cg - g2) ** 2 + (cb - b2) ** 2
                if best_d is None or d < best_d:
                    best, best_d = i, d
            out[y][x] = best
            cache[c] = best
    return out


def tilemap_entry(tile_index, palette_bank=0, hflip=False, vflip=False, priority=False):
    """The two bytes (low, high) of an SMS name table entry.

    Low byte: tile number bits 0-7. High byte: bit 0 tile number bit 8,
    bit 1 horizontal flip, bit 2 vertical flip, bit 3 palette bank (1 = the
    sprite palette), bit 4 priority (drawn in front of sprites).
    """
    lo = tile_index & 0xFF
    hi = (tile_index >> 8) & 1
    hi |= (1 if hflip else 0) << 1
    hi |= (1 if vflip else 0) << 2
    hi |= (palette_bank & 1) << 3
    hi |= (1 if priority else 0) << 4
    return lo, hi
