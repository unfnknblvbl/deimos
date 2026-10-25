#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Hardware-sprite art for every entity type of level01 (monsters, people,
pickups, barrels, crates, fire), at 1..4 cells straight ahead.

Placement follows the original billboard projection (Render_renderSprite:
canvas bottom on the floor, 1 canvas unit = 1 world unit, drawn 16 units
nearer than it is), so a thing k cells ahead is at depth 64k and scale
FOCAL/(64k), FOCAL being half the view's width (80 large, 64 small). (The
code here uses depth 64k with no 16-unit nudge, and an older note said
96/(64k): record() is what the ROM gets.) The SMS shows at most 8 sprites per line and the game reserves
56 sprite tiles, so a record is at most 7 x 4 sprites of 8x16 (56x64 px);
anything bigger at 1 cell is scaled down to fit, keeping its feet where
they belong.

Outputs (into <gfx_dir>):
  (the names below carry the size: sprite_dir_<size>.bin and so on)
  sprite_dir.bin          u8 bank, u16 addr, u8 0  per (def, distance 1..4) for
                          64 defs (1024 bytes), then 16 bytes of CRAM (bank 1)
                          per def (1024 bytes)
  sprite_bank_NNN.bin     the records: u8 cols, rows, x0, y0, pad[4], then
                          cols*rows*2 tiles of 32 bytes, column-major
                          (col 0 top tile, col 0 bottom tile, ...)
  sprite_banks.inc        WLA-DX include placing the banks
  sprite_sheet.png        preview

Usage: python3 build_sprites.py analysis/gamedata.json <gfx_dir> <first_bank> <large|small>
Writes sprite_dir_<size>.bin, sprite_bank_<size>_NNN.bin, sprite_banks_<size>.inc;
prints the next free bank.

Run twice by build.sh (step 3), once per view size, each run starting at
the bank the last one printed. A "def" is an entity definition from
gamedata.json (one per kind of thing in the level, up to 64); its art is the
64x64 canvas from sprite_art.py. A "record" is one def drawn at one distance:
the hardware sprites' tile patterns plus where to put them on screen.
"""
import json
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))
from sprite_art import default_art
from sms_gfx import rgb_to_sms, sms_cram_byte, sms_to_rgb
from gen_views import SMS
from colour_lab import lab, dist2, err

# per view size (rom/render.asm): view position on screen, size, focal
# length (= half the width), and the largest record (its patterns go in
# tiles 0..; the small view keeps its border tiles at 40..55)
SIZES = {"large": dict(x0=48, y0=32, w=160, h=96, cols=7, rows=4),
         "small": dict(x0=64, y0=40, w=128, h=80, cols=5, rows=4)}
# the current size's values (set by use_size; these are the large view's)
VIEW_X0, VIEW_Y0, VIEW_W, VIEW_H = 48, 32, 160, 96
FOCAL = 80.0
EYE_Z = 36                      # the eye's height above the floor, world units
MAX_COLS, MAX_ROWS = 7, 4       # the largest record, in 8x16 sprites


def use_size(name):
    """Set the module globals (view position and size, FOCAL, record limits) for view size name."""
    global VIEW_X0, VIEW_Y0, VIEW_W, VIEW_H, FOCAL, MAX_COLS, MAX_ROWS
    z = SIZES[name]
    VIEW_X0, VIEW_Y0, VIEW_W, VIEW_H = z["x0"], z["y0"], z["w"], z["h"]
    FOCAL = z["w"] / 2.0
    MAX_COLS, MAX_ROWS = z["cols"], z["rows"]
BANK_SIZE = 0x4000              # one Sega-mapper bank: 16 KB


def def_palette(canvas):
    """15 SMS colours for a sprite (+ transparent 0): greedy CIELAB like the
    level palette (tools/colour_lab.py: the editor makes the same choice).

    canvas is the 64x64 RGBA art. Each round adds the one of the 64 SMS
    colours (gen_views.SMS) that most lowers the total error of the art's
    pixels (each pixel's integer Lab error to its nearest chosen colour,
    weighted by how many pixels share that RGB). Stops early when every
    pixel is matched exactly. Returns 16 (r, g, b) tuples, entry 0 black
    for transparent, unused entries black."""
    px = canvas[canvas[..., 3] > 0][:, :3].astype(np.float64)
    u, cnt = np.unique(px, axis=0, return_counts=True)
    ul = lab(u)
    cnt = cnt.astype(np.int64)
    sel = []
    cur = None
    for _ in range(15):
        best = None
        for i in range(64):
            if any(np.array_equal(SMS[i], s) for s in sel):
                continue
            e = err(dist2(ul, lab(SMS[i].astype(np.float64))[None, :]))
            if cur is not None:
                e = np.minimum(cur, e)
            tot = int((e * cnt).sum())
            if best is None or tot < best[0]:
                best = (tot, i, e)
        if best is None:
            break
        sel.append(SMS[best[1]])
        cur = best[2]
        if cur.max() == 0:
            break
    pal = [(0, 0, 0)] + [tuple(int(v) for v in c) for c in sel]
    while len(pal) < 16:
        pal.append((0, 0, 0))
    return pal


def area_weights(n_in, n_out):
    """[n_out, n_in] integer overlaps: output o covers source [o, o + 1) * n_in / n_out
    (in units of 1 / n_out source pixels); each row sums to n_in.

    Used to shrink: multiplying by these weights sums each output pixel's
    share of the source exactly, with no rounding."""
    w = np.zeros((n_out, n_in), np.int64)
    for o in range(n_out):
        a, b = o * n_in, (o + 1) * n_in
        for j in range(a // n_out, min(n_in, (b + n_out - 1) // n_out)):
            w[o, j] = max(0, min(b, (j + 1) * n_out) - max(a, j * n_out))
    return w


def nearest_weights(n_in, n_out):
    """[n_out, n_in] 0/1 weights picking for each output the source pixel under its centre."""
    w = np.zeros((n_out, n_in), np.int64)
    for o in range(n_out):
        w[o, ((2 * o + 1) * n_in) // (2 * n_out)] = 1
    return w


def scale_art(rgba, W, H):
    """the art scaled to W x H: area average when shrinking, nearest pixel
    when growing; integers only (the editor does the same). -> (rgb, on)

    rgba is HxWx4 art. on is True where at least 110/255 of the covered
    source area is opaque; rgb is the average colour of the opaque part
    (rounded), so transparent pixels never darken the edges."""
    h, w = rgba.shape[:2]
    wx = area_weights(w, W) if W < w else nearest_weights(w, W)
    wy = area_weights(h, H) if H < h else nearest_weights(h, H)
    a = (rgba[..., 3] > 0).astype(np.int64) * 255     # opaque mask, 0 or 255
    tot = wy.sum(1)[:, None] * wx.sum(1)[None, :]     # total weight per output pixel
    asum = wy @ a @ wx.T                               # opaque weight x 255
    on = asum >= 110 * tot
    rgb = np.zeros((H, W, 3), np.int64)
    for k in range(3):
        num = wy @ (a * rgba[..., k].astype(np.int64)) @ wx.T
        rgb[..., k] = (2 * num + asum) // np.maximum(2 * asum, 1)
    return rgb, on


def record(canvas, pal, k):
    """One sprite record: the def's canvas seen k cells (1..4) straight ahead.

    pal is the def's 16-colour palette (def_palette). The art's bounding box
    is scaled by FOCAL / (64 k), or less if it would not fit MAX_COLS x
    MAX_ROWS sprites, and placed bottom-centred in a grid of 8x16 sprites;
    the record is then placed so the art's bottom centre lands where the
    projection puts it (clamped inside the view horizontally, and below its
    top). Returns (record bytes, the grid of colour indices, (x0, y0)):
    8 header bytes (cols, rows, x0, SAT y, 4 zero) then the tiles, 32 bytes
    each, 4 bitplanes per row, column by column, 2 x rows tiles per column.
    """
    a = canvas[..., 3] > 0
    ys, xs = np.nonzero(a)
    r0, r1, c0, c1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    s = FOCAL / (64.0 * k)
    w, h = c1 - c0, r1 - r0
    s2 = min(s, MAX_COLS * 8 / w, MAX_ROWS * 16 / h)  # the scale it is drawn at
    W, H = max(1, round(w * s2)), max(1, round(h * s2))
    # true screen position of the art's bottom centre
    cx = VIEW_X0 + VIEW_W / 2 + ((c0 + c1) / 2 - 32) * s
    by = VIEW_Y0 + VIEW_H / 2 - (64 - r1 - EYE_Z) * s  # canvas row r is at height 64 - r
    rgb, on = scale_art(canvas[r0:r1, c0:c1], W, H)
    cols = min(MAX_COLS, (W + 7) // 8)
    rows = min(MAX_ROWS, (H + 15) // 16)
    cw, ch = cols * 8, rows * 16
    grid = np.zeros((ch, cw), np.uint8)
    P = lab(np.array(pal[1:], np.float64))     # the 15 opaque colours in Lab
    ox = (cw - W) // 2                         # centred horizontally
    oy = ch - H                                # bottom-aligned
    # nearest palette colour per pixel (1..15; 0 stays transparent)
    idx = dist2(lab(rgb.astype(np.float64))[:, :, None, :], P[None, None]).argmin(-1) + 1
    sub = np.where(on, idx, 0)
    grid[max(0, oy):, ox:ox + W] = sub[max(0, -oy):, :cw - ox][:ch]
    x0 = int(round(cx - cw / 2))
    y0 = int(round(by - ch))
    x0 = max(VIEW_X0, min(x0, VIEW_X0 + VIEW_W - cw))
    y0 = max(VIEW_Y0, y0)
    data = bytearray()
    # the tile patterns: per column of 8x16 sprites, top tile to bottom
    for c in range(cols):
        for r in range(rows * 2):
            for yy in range(8):
                planes = [0, 0, 0, 0]
                for xx in range(8):
                    v = int(grid[r * 8 + yy, c * 8 + xx])
                    for p in range(4):
                        if v & (1 << p):
                            planes[p] |= 1 << (7 - xx)
                data += bytes(planes)
    # SAT y is the line before the sprite's first line
    hdr = struct.pack("<BBBBBBBB", cols, rows, x0, (y0 - 1) & 0xFF, 0, 0, 0, 0)
    return hdr + bytes(data), grid, (x0, y0)


def main():
    """Build one view size's sprite directory, banks and preview (arguments from sys.argv).

    Prints the summary on stderr and the next free bank on stdout (build.sh
    reads it).
    """
    gd, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    first_bank = int(sys.argv[3])
    size = sys.argv[4]
    use_size(size)
    g = json.loads(gd.read_text())
    art = default_art()
    banks, cur = [], bytearray()
    directory, pals = bytearray(), bytearray()
    sheet = Image.new("RGB", (len(g["defs"]) * 60, 4 * 70), (40, 0, 40))
    for d in g["defs"]:
        canvas = art.canvas(d["tile"])
        pal = def_palette(canvas)
        # the def's 16 CRAM bytes (loaded into the sprite palette, bank 1)
        pals += bytes(sms_cram_byte(*rgb_to_sms(*c)) for c in pal)
        for k in (1, 2, 3, 4):
            rec, grid, _pos = record(canvas, pal, k)
            if len(cur) + len(rec) > BANK_SIZE:   # records never cross a bank
                banks.append(bytes(cur))
                cur = bytearray()
            addr = 0x8000 + len(cur)           # where it is seen in slot 2
            directory += bytes([first_bank + len(banks), addr & 0xFF, addr >> 8, 0])
            cur += rec
            P = np.array(pal, np.uint8)
            im = Image.fromarray(P[grid])
            sheet.paste(im, (d["id"] * 60, (k - 1) * 70))
    banks.append(bytes(cur))
    assert len(g["defs"]) <= 64
    # pad to the fixed 64 defs: 4 distances x 4 bytes, then 16 CRAM bytes each
    directory = directory.ljust(64 * 4 * 4, b"\0")
    pals = pals.ljust(64 * 16, b"\0")
    (out_dir / f"sprite_dir_{size}.bin").write_bytes(bytes(directory + pals))
    for old in out_dir.glob(f"sprite_bank_{size}_*.bin"):
        old.unlink()
    with open(out_dir / f"sprite_banks_{size}.inc", "w") as fp:
        for i, b in enumerate(banks):
            fn = f"sprite_bank_{size}_{first_bank + i:03d}.bin"
            (out_dir / fn).write_bytes(b)
            fp.write(f".BANK {first_bank + i} SLOT 2\n.ORGA $8000\n.incbin \"build/gfx/{fn}\"\n\n")
    sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST).save(out_dir / f"sprite_sheet_{size}.png")
    print(f"sprites ({size}): {len(g['defs'])} defs x 4 distances in {len(banks)} banks "
          f"({first_bank}..{first_bank + len(banks) - 1}), {sum(map(len, banks)) // 1024} KB", file=sys.stderr)
    print(first_bank + len(banks))


if __name__ == "__main__":
    main()
