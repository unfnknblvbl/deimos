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
"""
import struct
from functools import lru_cache
from pathlib import Path

import numpy as np

from extract_textures import load_palettes, load_mappings, resolve_sprite_id
from extract_sprites import load_bitshapes, decode_shape, render_shape

JAR = Path(__file__).resolve().parent.parent / "assets" / "jar"  # the default jar directory (build.sh's ASSETS)


# The ammo packs (even tiles 82..90: Halon Cans, Bullet Clips, Shell Clips,
# Rockets, Cell Clips) have no picture of their own (their sprite id is 0,
# the axe's): the original game draws the single item, tile - 1, four times
# around the spot. PLURAL_OFFSETS: the copies' column offsets, drawn in this
# order (the middle one last, in front).
PLURAL_TILES = (82, 84, 86, 88, 90)
PLURAL_OFFSETS = (-16, 16, 0)


class SpriteArt:
    """Loads the jar's sprite data once and returns each sprite on its 64x64
    canvas (canvas()), caching the results. Used by build_render_data.py.
    """
    def __init__(self, jar_dir: Path = JAR):
        """Reads the palettes, the mappings, the shapes and the sprite texels from jar_dir."""
        self.jar_dir = jar_dir
        self.palette = load_palettes(jar_dir / "palettes.bin")
        self.mappings = load_mappings(jar_dir / "mappings.bin")
        self.shapes = load_bitshapes(jar_dir / "bitshapes.bin")
        self.stexels = (jar_dir / "stexels.bin").read_bytes()
        self.boundary = struct.unpack_from("<I", (jar_dir / "wtexels.bin").read_bytes(), 0)[0] * 2  # where stexels starts, in nibbles
        self._cache = {}

    def canvas(self, tile: int) -> np.ndarray:
        """64x64x4 uint8 RGBA, [row][col], row 0 = top (z=64).

        tile: the sprite's tile number (a .bsp sprite type or an entity
        definition's tile); 305 and up is a wall texture shown as a sprite.
        """
        if tile in self._cache:
            return self._cache[tile]
        if tile >= 305:
            # a wall texture shown as a sprite (the original's info $40000:
            # 64 x 64, drawn without transparency)
            from extract_textures import resolve_wall_id, extract_wall_texture
            tid = resolve_wall_id(self.mappings, tile - 305)
            off, poff = self.mappings["wall_table"][tid * 2], self.mappings["wall_table"][tid * 2 + 1]
            wtex = getattr(self, "_wtex", None)
            if wtex is None:
                wtex = self._wtex = (self.jar_dir / "wtexels.bin").read_bytes()
            grid = np.array(extract_wall_texture(wtex, off), dtype=np.uint8)
            rgb = np.array([self.palette[poff + k] for k in range(16)], np.uint8)[grid]
            out = np.zeros((64, 64, 4), np.uint8)
            out[..., :3] = rgb
            out[..., 3] = 255
            self._cache[tile] = out
            return out
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
        rec = self.mappings["sprite_table"][sid * 2]
        pal = self.mappings["sprite_table"][sid * 2 + 1]
        sh = decode_shape(self.shapes, rec)
        img = np.array(render_shape(sh, self.stexels, self.palette, pal, self.boundary).convert("RGBA"))
        out = np.zeros((64, 64, 4), np.uint8)
        h, w = img.shape[:2]
        r0, c0 = sh["left"], sh["top"]  # (swapped on purpose: see the module docstring)
        h2, w2 = min(h, 64 - r0), min(w, 64 - c0)  # clip to the canvas
        out[r0:r0 + h2, c0:c0 + w2] = img[:h2, :w2]
        self._cache[tile] = out
        return out


@lru_cache(maxsize=1)
def default_art():
    """A shared SpriteArt for the default jar directory (made once)."""
    return SpriteArt()
