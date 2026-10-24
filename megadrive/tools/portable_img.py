#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Image steps the Doom RPG editor repeats byte for byte (editor/src/assets.*):
integer arithmetic only, every tie broken the same way, so the graphics a
build makes from the user's DoomRPG.jar come out identical in Python and in
the editor, on any computer. (Pillow is only used to read the PNGs: its
reading is exact, its resizing and quantising are not reproducible.)

The same file in both repositories (mastersystem, deimos).
Licence: GPL-2.0-or-later.
"""
import numpy as np
from PIL import Image


def load_indexed(path):
    """a palette PNG -> (indices HxW uint8, palette [(r, g, b)] 256, alpha [a] 256)

    The palette is padded to 256 entries; alpha comes from the PNG's
    transparency (one transparent index, or a table). Asserts the PNG is
    paletted.
    """
    im = Image.open(path)
    assert im.mode == "P", path
    idx = np.array(im)
    raw = im.getpalette() or []
    raw = raw + [0] * (768 - len(raw))
    pal = [tuple(raw[i * 3:i * 3 + 3]) for i in range(256)]
    alpha = [255] * 256
    t = im.info.get("transparency")
    if isinstance(t, int):
        alpha[t] = 0
    elif isinstance(t, (bytes, bytearray)):
        for i, a in enumerate(t):
            alpha[i] = a
    return idx, pal, alpha


def load_rgb(path):
    """HxWx3 uint8 (a palette PNG's colours, transparency ignored)"""
    idx, pal, _ = load_indexed(path)
    return np.array(pal, np.uint8)[idx]


def load_rgba(path):
    """HxWx4 uint8: a palette PNG's colours with their alpha."""
    idx, pal, alpha = load_indexed(path)
    p = np.array([list(c) + [a] for c, a in zip(pal, alpha)], np.uint8)
    return p[idx]


def _axis(n_out, n_in):
    """per output position: (first source, second source, weight of the second in 1/256)

    For resizing n_in samples to n_out: each output sample's centre mapped
    back to the source (centres aligned), in 8.8 fixed point, clamped to
    the source's ends.
    """
    out = []
    for o in range(n_out):
        num = (2 * o + 1) * n_in - n_out          # 2 * n_out * (the source centre)
        f = 0 if num < 0 else num * 256 // (2 * n_out)
        f = min(f, (n_in - 1) * 256)
        i0 = f >> 8
        out.append((i0, min(i0 + 1, n_in - 1), f & 255))
    return out


def resize_bilinear(a, w, h):
    """HxWxC uint8 -> hxwxC: bilinear with weights in 1/256, rounded

    Integer only, so the editor's C++ copy gives the same bytes. (w, h:
    the new width and height.)
    """
    a = a.astype(np.int64)
    xs, ys = _axis(w, a.shape[1]), _axis(h, a.shape[0])
    out = np.zeros((h, w, a.shape[2]), np.int64)
    for y, (y0, y1, wy) in enumerate(ys):
        for x, (x0, x1, wx) in enumerate(xs):
            top = a[y0, x0] * (256 - wx) + a[y0, x1] * wx
            bot = a[y1, x0] * (256 - wx) + a[y1, x1] * wx
            out[y, x] = (top * (256 - wy) + bot * wy + 32768) >> 16  # two 1/256 weights: /65536, rounded
    return out.astype(np.uint8)


def blend(dst, src_rgba, x0, y0):
    """paste src (HxWx4) over dst (HxWx3) at (x0, y0), by its alpha

    dst is changed in place (and returned); pixels outside dst are skipped.
    """
    for y in range(src_rgba.shape[0]):
        for x in range(src_rgba.shape[1]):
            a = int(src_rgba[y, x, 3])
            if a == 0:
                continue
            X, Y = x0 + x, y0 + y
            if not (0 <= X < dst.shape[1] and 0 <= Y < dst.shape[0]):
                continue
            for c in range(3):
                d, s = int(dst[Y, X, c]), int(src_rgba[y, x, c])
                t = (s - d) * a + 128
                dst[Y, X, c] = d + ((t + (t >> 8)) >> 8)  # d + (s - d) * a / 255, rounded, in integers
    return dst


def greedy_palette(colours, counts, n, fixed=(), dist=None):
    """n colours for an image: `fixed` first, then the most used, then each
    time the colour with the largest count x squared distance to the chosen
    ones. colours: distinct values in ascending order, counts: how many
    pixels each. Ties: the earlier in ascending-count-descending order.

    dist(a, b): the distance function to use (squared distance). Returns
    the palette as a list (shorter than n if the image has fewer colours).
    """
    pal = list(fixed)
    order = sorted(range(len(colours)), key=lambda i: (-counts[i], colours[i]))
    cands = [colours[i] for i in order if colours[i] not in pal]
    w = {colours[i]: counts[i] for i in range(len(colours))}
    while len(pal) < n and cands:
        if pal:
            score = [w[c] * min(dist(c, p) for p in pal) for c in cands]
        else:
            score = [w[c] for c in cands]
        best = cands[score.index(max(score))]
        pal.append(best)
        cands.remove(best)
    return pal
