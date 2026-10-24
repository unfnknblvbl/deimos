#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""Doom RPG's font (a.png in the jar: 16 x 9 cells of 9x12 pixels, '!'
first, white glyphs with a black outline), squeezed into 8x8 tiles.
Licence: GPL-2.0-or-later."""
from pathlib import Path

import numpy as np
from PIL import Image


def load_font_sheet(jar_dir: Path):
    """The font's glyphs from a.png in jar_dir: {character: 12x9 bool array},
    True where the glyph's white core is (palette index 1; the outline is
    dropped). Characters '!' to '~', plus an empty space.
    """
    a = np.array(Image.open(jar_dir / "a.png"))
    glyphs = {}
    for i in range(16 * 9):
        ch = 33 + i
        if ch > 126:
            break
        r, c = divmod(i, 16)  # the cell's row and column on the sheet
        cell = a[r * 12:r * 12 + 12, c * 9:c * 9 + 9]
        glyphs[chr(ch)] = (cell == 1)          # white core
    glyphs[" "] = np.zeros((12, 9), bool)
    return glyphs


def squeeze_rows(rows):
    """Drop one row from an 8-row glyph: a row equal to its neighbour,
    preferring the middle.

    rows: an 8 x n array; returns 7 x n. If no two neighbouring rows are
    equal, row 4 goes.
    """
    order = [4, 3, 5, 2, 6, 1, 7, 0]
    for k in order:
        if k > 0 and np.array_equal(rows[k], rows[k - 1]):
            return np.delete(rows, k, axis=0)
    for k in order:
        if k < 7 and np.array_equal(rows[k], rows[k + 1]):
            return np.delete(rows, k, axis=0)
    return np.delete(rows, 4, axis=0)


def glyph_tile(g):
    """12x9 bool core -> 8x8 list of 0/1.

    (It returns an 8x8 bool array.) The glyph is placed in columns 1..6
    and rows 1..7 of the tile, leaving a border for the outline the caller
    adds. Capitals (rows 2..9) are squeezed to 7 rows; lower-case letters
    and punctuation keep their lower 7 rows; glyphs with an ascender or a
    descender keep the 7 rows ending at their bottom.
    """
    out = np.zeros((8, 8), bool)
    ys = np.nonzero(g.any(axis=1))[0]
    if len(ys) == 0:
        return out
    core = g[:, 2:8]                           # columns 2..7 hold every glyph
    top, bot = ys.min(), ys.max()
    if top >= 2 and bot <= 9:
        body = core[2:10]                      # rows 2..9: caps height
        if top <= 3:                           # uses the full cap height: squeeze
            body = squeeze_rows(body)
            out[1:8, 1:7] = body
        else:                                  # lowercase / punctuation below the cap line
            out[1:8, 1:7] = body[1:8]
    else:                                      # ascender at 1 or descender at 10
        body = core[max(1, bot - 6):bot + 1]
        out[8 - len(body):8, 1:7] = body
    return out
