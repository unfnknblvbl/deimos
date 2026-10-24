#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Draws Deimos's own title-screen art, from code (no outside images), into
art/:

  title_bg.png     256 x 224 RGB: the background of every console's title
                   screen - stars, the moon Deimos, and Mars's horizon
                   along the bottom
  title_logo.png   RGBA: the word DEIMOS in angular metal letters, with
                   transparent surroundings (drawn over the background by
                   tools/build_md_ui.py, and the editor's copy of it)
  title_icon.png   48 x 48 RGB: the moon on its own, for the Master System
                   and Game Gear title screens, which have room for a small
                   picture only

The PNGs are committed, so a build does not run this; run it again only to
change the art:  python3 tools/make_title_art.py art
Everything is drawn at 4 times the size and then averaged down (smooth
edges), with a fixed random seed (the same picture every run).
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

SS = 4                      # supersampling: draw this many times larger, then average down


def down(a, f=SS):
    """Averages each f x f block of the array a (H, W, C) into one pixel."""
    h, w = a.shape[0] // f, a.shape[1] // f
    return a[:h * f, :w * f].reshape(h, f, w, f, -1).mean(axis=(1, 3))


BAYER4 = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) / 16


def ramp_dither(v, ramp):
    """Maps values v (H x W, 0..1) onto the colours of `ramp` (dark to light)
    with a 4 x 4 ordered dither between neighbouring colours: few exact
    colours, so the consoles' palettes keep them all. Returns H x W x 3."""
    ramp = np.array(ramp, float)
    f = np.clip(v, 0, 1) * (len(ramp) - 1)
    h, w = v.shape
    th = np.tile(BAYER4, (h // 4 + 1, w // 4 + 1))[:h, :w]
    i = np.minimum(np.floor(f + th).astype(int), len(ramp) - 1)
    return ramp[i]


# The colours the art is snapped to: Mega Drive levels (0, 52, 87, 116, 144,
# 172, 206, 255 a channel), so a Mega Drive palette holds them exactly.
MARS_RAMP = [(52, 0, 0), (87, 0, 0), (116, 52, 0), (144, 52, 0), (172, 87, 52), (206, 116, 52)]
MOON_RAMP = [(52, 52, 52), (87, 87, 87), (144, 116, 116), (172, 172, 144)]
# the Master System / Game Gear picture: exact SMS greys (its text palette has them)
ICON_RAMP = [(85, 85, 85), (170, 170, 170), (255, 255, 255)]


def smooth_noise(rng, h, w, cell):
    """Smooth random values (0..1) over an h x w grid: random values on a
    coarse grid of `cell`-pixel squares, blended bilinearly (cosine-eased)."""
    gh, gw = h // cell + 2, w // cell + 2
    g = rng.random((gh, gw))
    y = np.arange(h) / cell
    x = np.arange(w) / cell
    y0, x0 = y.astype(int), x.astype(int)
    fy, fx = y - y0, x - x0
    fy = (1 - np.cos(fy * np.pi)) / 2          # ease the blend so no grid lines show
    fx = (1 - np.cos(fx * np.pi)) / 2
    a = g[y0][:, x0] * (1 - fx) + g[y0][:, x0 + 1] * fx
    b = g[y0 + 1][:, x0] * (1 - fx) + g[y0 + 1][:, x0 + 1] * fx
    return a * (1 - fy[:, None]) + b * fy[:, None]


def moon(rng, size, light=(-0.75, -0.45)):
    """An RGBA picture (size x size, at SS times that) of a small lumpy moon
    like Deimos: an uneven outline, a few soft craters, lit from `light`
    (a direction in the picture's plane; the rest of the light comes from
    the viewer). Returns the full-size (supersampled) float array."""
    n = size * SS
    yy, xx = np.mgrid[0:n, 0:n]
    cx = cy = n / 2
    dx, dy = (xx - cx) / (n * 0.42), (yy - cy) / (n * 0.36)    # an oval: Deimos is not round
    ang = np.arctan2(dy, dx)
    r = np.hypot(dx, dy)
    lump = 1 + 0.07 * np.sin(3 * ang + 0.7) + 0.05 * np.sin(5 * ang + 2.1) + 0.03 * np.sin(8 * ang)
    inside = r < lump
    # a sphere-like normal from the distance to the edge, for the shading
    z = np.sqrt(np.clip(1 - (r / lump) ** 2, 0, 1))
    nx, ny = dx / lump, dy / lump
    lx, ly = light
    lz = 0.55
    ln = np.sqrt(lx * lx + ly * ly + lz * lz)
    shade = np.clip((nx * lx + ny * ly + z * lz) / ln, 0, 1)
    # craters: darker dishes with a lighter rim on the side facing the light
    tex = 0.85 + 0.25 * smooth_noise(rng, n, n, max(2, n // 6))
    for _ in range(7):
        ccx, ccy = rng.uniform(0.25, 0.75) * n, rng.uniform(0.28, 0.72) * n
        cr = rng.uniform(0.05, 0.12) * n
        d = np.hypot(xx - ccx, yy - ccy) / cr
        tex = tex - 0.28 * np.clip(1 - d, 0, 1) ** 0.7
        rim = np.exp(-((d - 1.0) ** 2) / 0.04) * np.clip(-((xx - ccx) * lx + (yy - ccy) * ly) / cr, 0, 1)
        tex = tex + 0.15 * rim
    v = np.clip(shade * tex, 0, 1) ** 0.85         # brightness, 0..1 (snapped to MOON_RAMP later)
    alpha = inside.astype(float)
    return np.dstack([v, v, v, alpha * 255])


def background(rng):
    """The 256 x 224 title background (an RGB uint8 array)."""
    W, H = 256, 224
    n_w, n_h = W * SS, H * SS
    img = np.zeros((n_h, n_w, 3))
    # space: near black at the top, a little purple towards the horizon
    t = np.linspace(0, 1, n_h)[:, None]
    img[:] = 0                                          # space: black
    # stars: mostly faint points, a few brighter ones with a small cross
    stars = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(stars)
    for _ in range(170):
        x, y = rng.integers(0, W), rng.integers(0, 160)
        b = int(rng.choice([70, 100, 130, 170, 220], p=[0.35, 0.25, 0.2, 0.12, 0.08]))
        tint = rng.choice(3)
        col = (b, b, min(255, b + 30)) if tint == 0 else (min(255, b + 20), b, b - 10 if b > 10 else b) if tint == 1 else (b, b, b)
        d.point((int(x), int(y)), fill=col)
        if b >= 220:
            for ex, ey in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                d.point((int(x) + ex, int(y) + ey), fill=(b // 2, b // 2, b // 2))
    st = np.asarray(stars).astype(float)
    st = np.repeat(np.repeat(st, SS, axis=0), SS, axis=1)
    img = np.maximum(img, st)
    # Mars: a huge disc whose top edge is the horizon, its centre far below
    yy, xx = np.mgrid[0:n_h, 0:n_w]
    mcx, mcy, mr = n_w * 0.45, (150 + 690) * SS, 690 * SS   # (the horizon at line 150 in the middle)
    dist = np.hypot(xx - mcx, yy - mcy)
    on = dist < mr
    depth = np.clip((mr - dist) / (60 * SS), 0, 1)              # 0 at the limb, 1 well inside
    surf = smooth_noise(rng, n_h, n_w, 26 * SS) * 0.6 + smooth_noise(rng, n_h, n_w, 9 * SS) * 0.4
    lit = np.clip(1 - (xx / n_w) * 0.35, 0.55, 1)                 # the sun from the left
    k = (1 - depth) ** 0.7 * lit * (0.85 + 0.25 * surf)    # brightest at the limb, darker below (the text goes there)
    out = down(np.clip(img, 0, 255))
    kd = down(k[..., None])[..., 0]
    ond = down(on[..., None].astype(float))[..., 0] > 0.5
    out = np.where(ond[..., None], ramp_dither(kd, MARS_RAMP), out)
    # its thin atmosphere: one line of the ramp's brightest colour along the limb
    edge = ond & ~np.roll(ond, 1, axis=0)
    out[edge] = MARS_RAMP[-1]
    # the moon Deimos, high on the right
    mm = down(moon(rng, 34))
    a = mm[..., 3] > 127                                  # (hard edge: no in-between colours)
    x0, y0 = 214, 8
    col = ramp_dither(mm[..., 0], MOON_RAMP)
    region = out[y0:y0 + mm.shape[0], x0:x0 + mm.shape[1]]
    region[a] = col[a]
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


# The letters, on a grid of units: each letter is 6 units wide and 9 high,
# made of polygons (lists of (x, y) unit points). Angular, with cut corners.
C = 1.4    # how far a corner is cut
T = 1.9    # stroke thickness
LETTERS = {
    "D": [[(0, 0), (6 - C, 0), (6, C), (6, 9 - C), (6 - C, 9), (0, 9)]],
    "E": [[(0, 0), (6, 0), (6, T), (T, T), (T, 3.6), (5, 3.6), (5, 3.6 + T), (T, 3.6 + T),
           (T, 9 - T), (6, 9 - T), (6, 9), (0, 9)]],
    "I": [[(1.05, 0), (1.05 + T + 0.4, 0), (1.05 + T + 0.4, 9), (1.05, 9)]],
    "M": [[(0, 9), (0, C * 0.5), (0.6, 0), (T, 0), (3, 3.6), (6 - T, 0), (5.4, 0), (6, C * 0.5),
           (6, 9), (6 - T, 9), (6 - T, 3.8), (3, 6.4), (T, 3.8), (T, 9)]],
    "O": [[(C, 0), (6 - C, 0), (6, C), (6, 9 - C), (6 - C, 9), (C, 9), (0, 9 - C), (0, C)]],
    "S": [[(C, 0), (6, 0), (6, T), (T + 0.2, T), (T + 0.2, 3.6), (6 - C, 3.6), (6, 3.6 + C),
           (6, 9 - C), (6 - C, 9), (0, 9), (0, 9 - T), (6 - T - 0.2, 9 - T), (6 - T - 0.2, 3.6 + T),
           (C, 3.6 + T), (0, 3.6 + T - C), (0, C)]],
}
HOLES = {   # the inside cut out of D and O
    "D": [[(T, T), (6 - T - 0.5, T), (6 - T, T + 0.5), (6 - T, 9 - T - 0.5), (6 - T - 0.5, 9 - T), (T, 9 - T)]],
    "O": [[(T + 0.3, T), (6 - T - 0.3, T), (6 - T, T + 0.3), (6 - T, 9 - T - 0.3), (6 - T - 0.3, 9 - T),
           (T + 0.3, 9 - T), (T, 9 - T - 0.3), (T, T + 0.3)]],
}
WIDTHS = {"I": 4.5}


def logo(word="DEIMOS", unit=5.2, gap=1.3):
    """The word in metal letters: an RGBA uint8 array. Each letter is filled
    with a gradient (pale yellow at the top through orange to deep red), has
    a dark outline and a bright edge along its top."""
    widths = [WIDTHS.get(ch, 6) for ch in word]
    pad = 3
    W = int((sum(widths) + gap * (len(word) - 1)) * unit) + 2 * pad
    H = int(9 * unit) + 2 * pad
    mask = Image.new("L", (W * SS, H * SS), 0)
    d = ImageDraw.Draw(mask)
    x = pad
    for ch, w in zip(word, widths):
        for poly in LETTERS[ch]:
            d.polygon([((x + px * unit) * SS, (pad + py * unit) * SS) for px, py in poly], fill=255)
        for poly in HOLES.get(ch, []):
            d.polygon([((x + px * unit) * SS, (pad + py * unit) * SS) for px, py in poly], fill=0)
        x += (w + gap) * unit
    m = np.asarray(mask).astype(float) / 255
    # the outline: the letters grown by about 1.5 pixels
    grow = m.copy()
    r = int(1.6 * SS)
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            if dx * dx + dy * dy <= r * r:
                grow = np.maximum(grow, np.roll(np.roll(m, dy, 0), dx, 1))
    # the fill: a vertical gradient, with a bevel - lighter where the letter
    # edge above is near (pixels just below the letter's top edge)
    t = np.linspace(0, 1, H * SS)[:, None]
    top = np.array([255, 236, 140])
    mid = np.array([240, 130, 30])
    bot = np.array([150, 24, 12])
    grad = np.where(t < 0.45, top + (mid - top) * (t / 0.45), mid + (bot - mid) * ((t - 0.45) / 0.55))
    fill = np.broadcast_to(grad[:, None, :] if grad.ndim == 2 else grad, (H * SS, W * SS, 3)).copy()
    above = np.roll(m, int(1.2 * SS), 0)                    # the letter, moved down: 0 just under its top edge
    edge = np.clip(m - above, 0, 1)
    fill = fill + edge[..., None] * 70
    below = np.roll(m, -int(1.2 * SS), 0)
    shadow = np.clip(m - below, 0, 1)
    fill = fill * (1 - 0.45 * shadow[..., None])
    outline = np.array([28, 6, 4])
    rgb = outline * (1 - m[..., None]) + fill * m[..., None]
    rgba = np.dstack([rgb, grow * 255])
    out = down(rgba)
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


def icon(rng):
    """The 48 x 48 Master System / Game Gear picture: the moon on black."""
    m = down(moon(rng, 44))
    out = np.zeros((48, 48, 3))
    a = m[..., 3] > 127
    col = ramp_dither(m[..., 0], ICON_RAMP)
    out[2:46, 2:46][a] = col[a]
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


def main():
    """make_title_art.py OUT_DIR: writes the three PNGs (see the module docstring)."""
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "art")
    out.mkdir(parents=True, exist_ok=True)
    Image.fromarray(background(np.random.default_rng(1877))).save(out / "title_bg.png")   # (1877: the year Deimos was found)
    Image.fromarray(logo(), "RGBA").save(out / "title_logo.png")
    Image.fromarray(icon(np.random.default_rng(1877))).save(out / "title_icon.png")
    print("title art:", ", ".join(str(out / f) for f in ("title_bg.png", "title_logo.png", "title_icon.png")))


if __name__ == "__main__":
    main()
