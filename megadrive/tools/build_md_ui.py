#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The Mega Drive game - the graphics outside the 3D view, from the original game's
own images (the jar): font, status-bar faces and icons, title screen.

  ui_font      96 tiles, ASCII 32..127: colour 1 = letter, 15 = background
               (the 8x8 core of Doom RPG's font, a.png, as on the SMS)
  ui_faces     9 faces (l.png, 18x20 doubled to 36x40 in a 5x5-tile
               block) - health levels, hurt, looking around
  ui_icons     status-bar icons (m.png): health, armour, weapons, ammo
  title_*      the title screen: tiles, name table, two palettes
               (in fact one 16-colour palette, title_pal; colour 0 is
               left unused because it is transparent)
  palettes     line 2 (status bar: white, face colours, black)
               line 3 (dialog boxes: white, yellow, dark blue box)

Output: build/gen/ui_data.c / .h
Usage:  build_md_ui.py <jar dir with every png> <out dir> <version> [title word]
        (the title word is no longer drawn - every console's title shows
        the DEIMOS logo alone - but the ports' build scripts still pass one)
Licence: GPL-2.0-or-later.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))
from doom_font import load_font_sheet, glyph_tile
from portable_img import load_indexed, load_rgb, load_rgba, resize_bilinear, blend, greedy_palette

# the Mega Drive's 8 output levels per colour channel (0..255 scale), as measured
MD_LEVELS = np.array([0, 52, 87, 116, 144, 172, 206, 255])


def md_level(v):
    """The Mega Drive level (0..7) nearest to the 8-bit channel value v."""
    return int(np.abs(MD_LEVELS - v).argmin())


# the dialog colours (palette line 3: slots 1, 2, 3 and 15), which the
# weapon's frames use too (tools/build_weapons.py)
DLG_RGB = [(255, 255, 255), (255, 210, 0), (150, 150, 150), (10, 20, 70)]

# the title screen's layout (rows of the 256 x 224 picture): the DEIMOS logo (art/title_logo.png) from LOGO_Y, the subtitle
# "A Doom RPG Engine" at SUBTITLE_Y, the version at 170, PRESS START at 196
LOGO_Y = 36
SUBTITLE_Y = 94


def md_colour(rgb):
    """An RGB colour (0..255 channels) as a Mega Drive colour word, 0000 BBB0 GGG0 RRR0."""
    r, g, b = (md_level(c) for c in rgb[:3])
    return (b << 9) | (g << 5) | (r << 1)


def md_rgb(c):
    """A Mega Drive colour word back to (r, g, b) on the 0..255 scale."""
    return (MD_LEVELS[(c >> 1) & 7], MD_LEVELS[(c >> 5) & 7], MD_LEVELS[(c >> 9) & 7])


def tile_bytes(px):
    """64 colour indices (row-major) -> 32 bytes of Mega Drive 4bpp.

    Each byte holds two pixels, the left one in the high nibble.
    """
    out = bytearray()
    for i in range(0, 64, 2):
        out.append(((px[i] & 15) << 4) | (px[i + 1] & 15))
    return bytes(out)


def c_arr(name, ctype, vals, per=32):
    """A C array definition as text, in the .assets section, `per` values a line (generated code)."""
    lines = [f'const {ctype} {name}[{len(vals)}] __attribute__((section(".assets"))) = {{']
    for i in range(0, len(vals), per):
        lines.append("    " + ",".join(str(int(v)) for v in vals[i:i + per]) + ",")
    lines.append("};")
    return "\n".join(lines)


def lv(c):
    """an MD colour -> its levels (r, g, b), 0..7"""
    return ((c >> 1) & 7, (c >> 5) & 7, (c >> 9) & 7)


def lv_dist(a, b):
    """Squared distance between two Mega Drive colours, in levels (0..7 per channel)."""
    la, lb = lv(a), lv(b)
    return sum((x - y) ** 2 for x, y in zip(la, lb))


def quantise(a, n, fixed=()):
    """HxWx3 uint8 -> (index array, list of MD colours): `fixed` first, then
    tools/portable_img.greedy_palette in level space; each pixel takes the
    nearest (the first of equals)

    n: the most colours the palette may have.
    """
    md = np.array([md_colour(c) for c in a.reshape(-1, 3)])
    uniq, cnt = np.unique(md, return_counts=True)
    pal = greedy_palette([int(u) for u in uniq], [int(c) for c in cnt], n, fixed, lv_dist)
    near = {int(u): min(range(len(pal)), key=lambda k: (lv_dist(int(u), pal[k]), k)) for u in uniq}
    idx = np.array([near[int(c)] for c in md])
    return idx.reshape(a.shape[0], a.shape[1]), pal


def main():
    """build_md_ui.py <jar dir> <out dir> <version> [title word]: builds the
    font, the status-bar faces and icons, the dialog palette and the title
    screen, and writes <out>/ui_data.c and ui_data.h (plus
    title_preview.png, to look at). Tile data is column-major per block
    (5x5 tiles per face, 2x2 per icon), as src/ui.c draws them.
    """
    jar, out, version = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
    word = sys.argv[4] if len(sys.argv) > 4 else "MEGA"
    glyphs = load_font_sheet(jar)

    # ---- font: fg 1, bg 15 ----
    font = []
    for code in range(32, 128):
        ch = chr(code) if code < 127 else " "  # code 127 (DEL) is blank
        g = glyph_tile(glyphs.get(ch, glyphs[" "]))
        font += list(tile_bytes([1 if v else 15 for v in g.ravel()]))

    # ---- status bar palette (line 2): 0 -, 1 white, 2..14 face/icons, 15 black ----
    faces_img = load_rgb(jar / "l.png")          # 18 x 180
    icons_img = load_rgb(jar / "m.png")          # 13 x 117
    both = np.zeros((180, 18 + 13, 3), np.uint8)
    both[:, :18] = faces_img
    both[:icons_img.shape[0], 18:] = icons_img
    _, hud_pal = quantise(both, 15, fixed=[md_colour((0, 0, 0)), md_colour((255, 255, 255))])  # the faces and icons share one palette
    # order: 0 transparent, 1 white, 2..14 the rest, 15 black
    hud_pal16 = [0, md_colour((255, 255, 255))] + [c for c in hud_pal if c not in
                                                   (md_colour((0, 0, 0)), md_colour((255, 255, 255)))][:13]
    hud_pal16 += [0] * (15 - len(hud_pal16))
    hud_pal16.append(md_colour((0, 0, 0)))
    P = np.array([md_rgb(c) for c in hud_pal16], np.int64)  # the palette as RGB, to match pixels against

    def to_idx(a, bg=15):
        """Maps an RGB image to the status-bar palette: each pixel the nearest of
        colours 1..14; near-black pixels (channel sum < 40) become `bg`.
        """
        a = a.astype(np.int64)
        d = ((a[:, :, None, :] - P[None, None, 1:15, :]) ** 2).sum(-1)
        idx = d.argmin(-1) + 1
        dark = (a.sum(-1) < 40)
        idx[dark] = bg
        return idx

    # l.png: 9 faces of 18 x 20 (the original draws face n from y = n * 20):
    # 0-3 health (full .. a quarter), 4-7 hurt (from ahead, the right, the
    # left, behind... as the original picks them), 8 the grin after a pick-up
    faces = []
    for k in range(9):
        f = faces_img[k * 20:k * 20 + 20].repeat(2, 0).repeat(2, 1)       # 36 x 40
        block = np.zeros((40, 40, 3), np.uint8)
        block[0:40, 2:38] = f  # centred in a 40x40 (5x5-tile) block
        idx = to_idx(block)
        for tx in range(5):          # column-major 5x5 block
            for ty in range(5):
                faces += list(tile_bytes(idx[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8].ravel()))

    # icons: 9 of 13x13 (m.png), each in a 2x2-tile block, column-major
    icons = []
    for k in range(9):
        block = np.zeros((16, 16, 3), np.uint8)
        block[1:14, 1:14] = icons_img[k * 13:k * 13 + 13]
        idx = to_idx(block)
        white = (block.min(-1) > 235)          # the sheet's white background
        idx[white] = 15
        for tx in range(2):
            for ty in range(2):
                icons += list(tile_bytes(idx[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8].ravel()))

    # ---- dialog palette (line 3): 1 white, 2 yellow, 3 grey, 15 dark blue ----
    dlg_pal = [0] + [md_colour(c) for c in DLG_RGB[:3]] + [0] * 11 + [md_colour(DLG_RGB[3])]

    # ---- title screen: 256x224 composed here, then cut into tiles ----
    W, H = 256, 224
    # Deimos's own art (art/, drawn by tools/make_title_art.py): the
    # background (stars, the moon Deimos, Mars's horizon) and the DEIMOS logo
    art = Path(__file__).resolve().parent.parent / "art"
    title = np.array(Image.open(art / "title_bg.png").convert("RGB"))
    logo = np.array(Image.open(art / "title_logo.png").convert("RGBA"))
    blend(title, logo, (W - logo.shape[1]) // 2, LOGO_Y)

    sheet = np.array(load_indexed(jar / "a.png")[0])  # the font sheet's palette indices (1 = a glyph's white core)

    def glyph(ch):
        """white core of a glyph, trimmed to its columns"""
        if ch == " ":
            return np.zeros((12, 3), bool)
        i = ord(ch) - 33
        r, c = divmod(i, 16)
        cell = sheet[r * 12:r * 12 + 12, c * 9:c * 9 + 9] == 1
        cols = np.nonzero(cell.any(0))[0]
        return cell[:, cols.min():cols.max() + 1] if len(cols) else np.zeros((12, 3), bool)

    def text(img, s, y, colour, scale=1):
        """Doom RPG's font, proportional, with a dark shadow

        Draws string s centred across the title image at row y, in `colour`,
        each pixel scale x scale, with a black shadow one pixel down-right.
        """
        gl = [glyph(ch) for ch in s]
        total = sum(g.shape[1] + 1 for g in gl) - 1
        x = (W - total * scale) // 2
        if x < 0:
            raise SystemExit(f"title text too wide: {s!r} ({total} px)")
        for g in gl:
            for yy, xx in zip(*np.nonzero(g)):
                for sy in range(scale):
                    for sx in range(scale):
                        img[y + yy * scale + sy + 1, x + xx * scale + sx + 1] = (0, 0, 0)
            for yy, xx in zip(*np.nonzero(g)):
                for sy in range(scale):
                    for sx in range(scale):
                        img[y + yy * scale + sy, x + xx * scale + sx] = colour
            x += (g.shape[1] + 1) * scale
        return img

    text(title, "A Doom RPG Engine", SUBTITLE_Y, (225, 225, 225))   # under the logo
    text(title, "Version " + version, 170, (170, 170, 170))
    text(title, "PRESS START", 196, (255, 210, 0))
    idx, tpal = quantise(title, 15)
    tpal16 = [0] + tpal + [0] * (15 - len(tpal))
    # index 0 must never be used by the picture (it is transparent): shift
    idx = idx + 1
    tiles, tmap, seen = [], [], {}  # unique tiles and the name table (a tile index per 8x8 cell)
    for ty in range(H // 8):
        for tx in range(W // 8):
            b = tile_bytes(idx[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8].ravel())
            if b not in seen:
                seen[b] = len(tiles)
                tiles.append(b)
            tmap.append(seen[b])
    title_tiles = [v for t in tiles for v in t]
    PT = np.array([md_rgb(c) for c in tpal16], np.uint8)
    Image.fromarray(PT[idx]).save(out / "title_preview.png")

    h = ["/* generated by tools/build_md_ui.py - do not edit */", "#ifndef UI_DATA_H", "#define UI_DATA_H",
         f'#define GAME_VERSION "{version}"',
         "#define UI_FONT_TILES 96", "#define UI_FACE_TILES 25", "#define UI_ICON_TILES 4",
         f"#define TITLE_N_TILES {len(tiles)}",
         "extern const unsigned char ui_font[], ui_faces[], ui_icons[], title_tiles[];",
         "extern const unsigned short ui_hud_pal[16], ui_dlg_pal[16], title_pal[16], title_map[];",
         "#endif"]
    c = ['#include "ui_data.h"',
         c_arr("ui_font", "unsigned char", font),
         c_arr("ui_faces", "unsigned char", faces),
         c_arr("ui_icons", "unsigned char", icons),
         c_arr("ui_hud_pal", "unsigned short", hud_pal16, 16),
         c_arr("ui_dlg_pal", "unsigned short", dlg_pal, 16),
         c_arr("title_tiles", "unsigned char", title_tiles),
         c_arr("title_map", "unsigned short", tmap),
         c_arr("title_pal", "unsigned short", tpal16, 16)]
    (out / "ui_data.h").write_text("\n".join(h) + "\n")
    (out / "ui_data.c").write_text("\n\n".join(c) + "\n")
    print(f"ui: font 96 tiles, faces 9x25, icons 9x4, title {len(tiles)} tiles")


if __name__ == "__main__":
    main()
