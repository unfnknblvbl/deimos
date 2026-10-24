#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Doom RPG sprite art placed on its 64x64 world canvas.

Every sprite (monster, item, prop, wall decal) is authored on a 64x64-unit
canvas whose bottom row touches the floor: canvas row r is at height
z = 64 - r and canvas column c is at lateral offset c - 32 from the sprite's
position (Render_renderSprite / Render_drawSpriteSpan in the GPL DoomRPG-RE
reimplementation, which matches the J2ME original). The packed shape only
stores the occupied rectangle; its header gives where that rectangle sits:
"top" is the first canvas COLUMN (the shape is stored as vertical strips)
and "left" the first canvas ROW.

Used by the view renderers and data builders (view_render.py, live_data.py,
build_sprites.py) to get a sprite as an RGBA array. It reads the J2ME data
files palettes.bin, mappings.bin, bitshapes.bin, stexels.bin and wtexels.bin
through the decoders in extract_textures.py and extract_sprites.py.
"""
import struct
from functools import lru_cache
from pathlib import Path

import numpy as np

from extract_textures import load_palettes, load_mappings, resolve_sprite_id
from extract_sprites import load_bitshapes, decode_shape, render_shape

# the unpacked DoomRPG.jar (assets/jar next to tools/)
JAR = Path(__file__).resolve().parent.parent / "assets" / "jar"


# The ammo packs (even tiles 82..90: Halon Cans, Bullet Clips, Shell Clips,
# Rockets, Cell Clips) have no picture of their own (their sprite id is 0,
# the axe's): the original game draws the single item, tile - 1, four times
# around the spot. PLURAL_OFFSETS: the copies' column offsets, drawn in this
# order (the middle one last, in front).
PLURAL_TILES = (82, 84, 86, 88, 90)
PLURAL_OFFSETS = (-16, 16, 0)


class SpriteArt:
    """The game's sprites, decoded on demand and cached by tile number."""

    def __init__(self, jar_dir: Path = JAR):
        """Load the palettes, sprite mappings, shapes and texels from jar_dir.

        boundary is the first word of wtexels.bin times two: the size of the
        wall texel data, which render_shape needs to address sprite texels
        (they are indexed past the wall texels).
        """
        self.palette = load_palettes(jar_dir / "palettes.bin")
        self.mappings = load_mappings(jar_dir / "mappings.bin")
        self.shapes = load_bitshapes(jar_dir / "bitshapes.bin")
        self.stexels = (jar_dir / "stexels.bin").read_bytes()
        self.boundary = struct.unpack_from("<I", (jar_dir / "wtexels.bin").read_bytes(), 0)[0] * 2
        self._cache = {}

    def canvas(self, tile: int) -> np.ndarray:
        """Return sprite tile number tile drawn on its 64x64 world canvas.

        The result is a 64x64x4 uint8 RGBA array, [row][col], row 0 = top
        (z=64), transparent (alpha 0) outside the shape. The tile number is
        resolved through the mappings to a sprite id and palette, the shape
        is decoded and drawn, then pasted at its canvas offset (clipped to
        the canvas). Cached: later calls return the same array.
        """
        if tile in self._cache:
            return self._cache[tile]
        if tile in PLURAL_TILES:
            # an ammo pack (Bullet Clips, Halon Cans, ...): the original game
            # draws the single item's sprite (tile - 1) four times, 16 units
            # each side of the spot (r.java); from the front that is three
            # copies side by side, the middle one in front
            one = self.canvas(tile - 1)
            out = np.zeros((64, 64, 4), np.uint8)
            for dx in PLURAL_OFFSETS:
                src = np.roll(one, dx, axis=1)
                if dx > 0:
                    src[:, :dx] = 0
                elif dx < 0:
                    src[:, dx:] = 0
                out = np.where(src[..., 3:4] > 0, src, out)
            self._cache[tile] = out
            return out
        sid = resolve_sprite_id(self.mappings, tile)
        # sprite_table holds pairs: [shape record, palette index] per sprite id
        rec = self.mappings["sprite_table"][sid * 2]
        pal = self.mappings["sprite_table"][sid * 2 + 1]
        sh = decode_shape(self.shapes, rec)
        img = np.array(render_shape(sh, self.stexels, self.palette, pal, self.boundary).convert("RGBA"))
        out = np.zeros((64, 64, 4), np.uint8)
        h, w = img.shape[:2]
        r0, c0 = sh["left"], sh["top"]
        h2, w2 = min(h, 64 - r0), min(w, 64 - c0)
        out[r0:r0 + h2, c0:c0 + w2] = img[:h2, :w2]
        self._cache[tile] = out
        return out


@lru_cache(maxsize=1)
def default_art():
    """The one shared SpriteArt loaded from the default JAR directory."""
    return SpriteArt()
