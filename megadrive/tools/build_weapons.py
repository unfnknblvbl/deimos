#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The player's weapon, drawn over the bottom of the view with hardware
sprites, as the original does (Combat_drawWeapon: sprite 240 + weapon,
frame +1 while firing, placed by the wpinfo table, 1 canvas unit = view
width / 128 pixels).

For each weapon (0..8) and frame (idle, firing): the visible part is cut
into sprites of up to 32x32 pixels (empty ones skipped). Colours: palette
line 3, which the weapon shares with the text and the floor and ceiling
(the view has the other three): its own 9 colours in slots 4..12 and the
dialog colours of slots 1, 2, 3 and 15 (white, yellow, grey, dark blue).
The 9 are chosen from the frame's Mega Drive colours, the one worst served
first (pixels x distance); every pixel takes the nearest of the 13.

Output: build/gen/pack/wp_*.bin (the data pack, tools/md_pack.py) + build/gen/weapon_data.h
  wp_frames[18]: { tiles offset (in 32-byte tiles), number of tiles,
                   palette index, first sprite, number of sprites }
  wp_sprites[]:  { x, y (view pixels), size (VDP sprite size bits), tile }
Usage: build_weapons.py <jar dir> <out dir>
Licence: GPL-2.0-or-later.
"""
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))
from sprite_art import SpriteArt
from extract_textures import resolve_sprite_id
from extract_sprites import decode_shape, render_shape
from build_md_ui import md_colour, tile_bytes, c_arr, DLG_RGB

# Combat.c wpinfo: shots, hold, idle x, idle y, attack x, attack y
WPINFO = [(1, 30, 20, 25, 20, 0), (1, 50, 0, 15, 0, 0), (1, 35, 0, 15, 0, 0), (1, 35, 0, 12, 0, 0),
          (3, 15, 0, 15, 0, 0), (1, 40, 0, 15, 0, 0), (3, 10, 0, 15, 0, 0), (1, 50, 0, 15, 0, 0),
          (1, 50, 0, 13, 0, 5)]
VIEW_W, VIEW_H = 256, 160  # the frame the weapon is laid out in (the 3D view's size until it shrank to 208x128; src/weapon.c puts this frame's bottom centre on the view's)
OWN = list(range(4, 13))   # the weapon's own slots of palette line 3
LEV = [0, 52, 87, 116, 144, 172, 206, 255]  # the Mega Drive's 8 levels per channel (as build_md_ui.MD_LEVELS)
MAX_TILES = 256          # VRAM: 192 tiles at $E800, then 64 at $D000 (render/ui keep clear)
SPLIT = 192  # tile index where the second VRAM area starts


def canvas(art, sid):
    """Sprite slot sid on its 64x64 canvas, as a Pillow RGBA image (the
    shape pasted where its header puts it; see sprite_art.py for why "top"
    is the x position).
    """
    rec = art.mappings["sprite_table"][sid * 2]
    pal = art.mappings["sprite_table"][sid * 2 + 1]
    sh = decode_shape(art.shapes, rec)
    img = render_shape(sh, art.stexels, art.palette, pal, art.boundary).convert("RGBA")
    cv = Image.new("RGBA", (64, 64))
    cv.paste(img, (sh["top"], sh["left"]))
    return cv


def cdist(a, b):
    """distance of two Mega Drive colours: squared level differences, 3 4 2

    (red, green and blue weighted 3, 4 and 2, on the 0..255 scale).
    """
    d = [LEV[(a >> s) & 7] - LEV[(b >> s) & 7] for s in (1, 5, 9)]
    return 3 * d[0] * d[0] + 4 * d[1] * d[1] + 2 * d[2] * d[2]


def frame_colours(rgb):
    """a frame's opaque pixels -> (palette line 3 as the frame wants it,
    {Mega Drive colour: slot}). The editor does the same (md_build.cpp).

    rgb: the opaque pixels' colours. Slots 1, 2, 3, 15 are the fixed dialog
    colours; slots 4..12 are filled one at a time with the colour whose
    pixel count x distance to the nearest chosen colour is largest. Each
    colour then takes its nearest slot (the lowest slot on ties).
    """
    fixed = {1: md_colour(DLG_RGB[0]), 2: md_colour(DLG_RGB[1]), 3: md_colour(DLG_RGB[2]),
             15: md_colour(DLG_RGB[3])}
    weight = {}
    for c in rgb:
        k = md_colour(c)
        weight[k] = weight.get(k, 0) + 1
    cand = sorted(weight)
    slots = dict(fixed)
    mind = {c: min(cdist(c, f) for f in fixed.values()) for c in cand}
    for s in OWN:
        best, bv = None, 0
        for c in cand:
            v = weight[c] * mind[c]
            if v > bv:
                best, bv = c, v
        if best is None:
            break
        slots[s] = best
        for c in cand:
            mind[c] = min(mind[c], cdist(c, best))
    order = sorted(slots)
    slot_of = {}
    for c in cand:
        slot_of[c] = min(order, key=lambda s: (cdist(c, slots[s]), s))
    pal = [slots.get(i, 0) for i in range(16)]
    return pal, slot_of


def main():
    """build_weapons.py <jar dir> <out dir>: for each of the 9 weapons and its
    2 frames, scales the sprite x2 onto a view-sized screen at its wpinfo
    place, picks its colours, cuts it into hardware sprites of up to 4x4
    tiles, and writes <out>/pack/wp_frames.bin, wp_sprites.bin, wp_pal.bin,
    wp_tiles.bin and <out>/weapon_data.h (layouts in the module docstring;
    wp_sprites has 4 words per sprite). Raises SystemExit if a frame needs
    more than MAX_TILES tiles.
    """
    jar, out = Path(sys.argv[1]), Path(sys.argv[2])
    art = SpriteArt(jar)
    frames, sprites, tiles, pals = [], [], [], []
    for w in range(9):
        sid = resolve_sprite_id(art.mappings, 240 + w)  # weapon sprites are tiles 240 + weapon; the firing frame is the next slot
        for f in range(2):
            cv = canvas(art, sid + f)
            px, py = (WPINFO[w][4], WPINFO[w][5]) if f else (WPINFO[w][2], WPINFO[w][3])
            x0, y0 = 128 + (px - 32) * 2, VIEW_H - (64 - py) * 2  # canvas centred, bottom row on the view's bottom, 2 pixels per unit
            big = cv.resize((128, 128), Image.NEAREST)
            scr = Image.new("RGBA", (VIEW_W, VIEW_H))
            scr.paste(big, (x0, y0), big)
            a = np.array(scr)
            alpha = a[..., 3] > 0
            ys, xs = np.nonzero(alpha)
            pal, slot_of = frame_colours(a[alpha][:, :3])
            idx = np.zeros(alpha.shape, np.uint8)
            idx[alpha] = [slot_of[md_colour(c)] for c in a[alpha][:, :3]]
            bx0, bx1, by0, by1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1  # the opaque area's bounding box
            bx0 -= bx0 % 8
            by0 -= by0 % 8
            first_spr, first_tile = len(sprites), len(tiles)
            # cover the box with sprites of up to 32x32 pixels (4x4 tiles), skipping empty ones
            for sy in range(by0, by1, 32):
                for sx in range(bx0, bx1, 32):
                    wt = min(4, (min(bx1, VIEW_W) - sx + 7) // 8)
                    ht = min(4, (min(by1, VIEW_H) - sy + 7) // 8)
                    block = np.zeros((ht * 8, wt * 8), np.uint8)
                    part = idx[sy:sy + ht * 8, sx:sx + wt * 8]
                    block[:part.shape[0], :part.shape[1]] = part
                    if not block.any():
                        continue
                    t0 = len(tiles) - first_tile
                    if t0 < SPLIT < t0 + wt * ht:        # a sprite's tiles must be contiguous in VRAM
                        tiles += [bytes(32)] * (SPLIT - t0)
                        t0 = SPLIT
                    for tx in range(wt):                 # sprite tiles go down columns
                        for ty in range(ht):
                            tiles.append(tile_bytes(block[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8].ravel()))
                    sprites += [sx, sy, ((wt - 1) << 2) | (ht - 1), t0]  # VDP size bits: width-1 in bits 2..3, height-1 in 0..1
            nt = len(tiles) - first_tile
            if nt > MAX_TILES:
                raise SystemExit(f"weapon {w} frame {f}: {nt} tiles")
            frames += [first_tile, nt, len(pals) // 16, first_spr // 4, (len(sprites) - first_spr) // 4]  # 5 words per frame
            pals += pal
    pk = out / "pack"
    pk.mkdir(exist_ok=True)
    for name, fmt, vals in [("wp_frames", "H", frames), ("wp_sprites", "H", sprites), ("wp_pal", "H", pals)]:
        (pk / f"{name}.bin").write_bytes(struct.pack(f">{len(vals)}{fmt}", *[int(v) for v in vals]))
    (pk / "wp_tiles.bin").write_bytes(b"".join(tiles))
    h = ["/* generated by tools/build_weapons.py - do not edit */", "#ifndef WEAPON_DATA_H",
         "#define WEAPON_DATA_H", "/* in the data pack (src/pack.c) */",
         "extern const unsigned short *wp_frames, *wp_sprites, *wp_pal;",
         "extern const unsigned char *wp_tiles;", f"#define WP_MAX_TILES {MAX_TILES}",
         f"#define WP_SPLIT {SPLIT}", "#endif"]
    (out / "weapon_data.h").write_text("\n".join(h) + "\n")
    (out / "weapon_data.c").unlink(missing_ok=True)  # (left over from older builds)
    print(f"weapons: 18 frames, {len(sprites) // 4} sprites, {len(tiles)} tiles")


if __name__ == "__main__":
    main()
