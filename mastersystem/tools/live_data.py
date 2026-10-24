#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Data for the live renderer (rom/render.asm), and a bit-exact Python model
of it.

The Master System draws every view at run time from:
  - the level's wall pieces (doors shifted like the original, walls split
    into pieces of at most 64 units so textures repeat every 64 units,
    wall decals baked into copies of the textures they cover),
  - a list per (cell, facing) of the walls and scenery that can show
    (worked out here, at 4x screen resolution, with doors open and shut),
  - the textures as columns of 64 "colour pair" bytes: high nibble = the
    colour on even screen pixels (x + y even), low nibble = odd. Pairs are
    the 0.1.5 2x2 ordered dither, now chosen per texel against one
    16-colour palette for the whole level; plain colours are pairs of the
    same colour. The chunky-to-planar step picks the nibble for free.
  - per view size, a table from projected wall height to rows and texture
    stepping, so the Z80 never multiplies or divides per pixel.

Geometry (identical in the model and the Z80 code; W, H = view size,
F = W / 2, CY = H / 2):
  camera   16 units behind the cell centre; view space (d, l) = depth,
           lateral, by swaps and negations of (x - vx, z - vz)
  screen   p = F*256*l/d (1/256 px from the centre, |l| < d), column x is
           covered when p(A) <= (x - F)*256 + 128 < p(B)
  height   h8 = 256 * wall height in px = 8192*W/d; for walls that are not
           parallel to the screen 1/d is linear in x, h8 = N(x) / l0 with
           N(x) = 16384 (x - F) + 8192 - 8192 W m (the wall is l = l0 + m d,
           m in {0, 1, -1}), stepped in 24.8 fixed point
  texture  u runs one texel per world unit along every piece (asserted),
           so u = c + s*l (walls facing the screen) or c + s*d (others)
  rows     table[hidx(h8)] = y0, y1, v0, dv (8.8), d

Usage: live_data.py analysis/level01.json analysis/gamedata.json analysis/level01_sms.json <jar dir> <out dir> [--preview DIR]
(the third argument is read by nobody any more; build.sh still passes it)

Run by build.sh step 2. Outputs in <out dir> (build/gfx/live): the level's
data banks data_N.bin and data_banks.inc (bank 3 starts with the 64-byte
"level header" that tells the ROM where every table is; H_FIELDS),
palette.bin, the separate .bin files of each table (for inspection),
table_large.bin / table_small.bin (the size tables), c2p.bin (the
chunky-to-planar tables), unroll.inc (generated Z80 inner loops and floor
tables), state.inc and next_bank.txt (the first bank free for sprites).
It also checks that rom/live_layout.inc still matches H_FIELDS.

Jargon: a "view" is what the player sees from one (cell, facing); the
"pvs" (potentially visible set) is the per-view list of walls and scenery
ids that can show. "hidx" is the size-table index of a projected height.
"Chunky" means one pair byte per pixel column cell, before it is turned
into the VDP's planar tiles. The tests (test_render.py) render with Model
and compare it with the screen of the ROM, so Model must stay bit-exact.
Licence: GPL-2.0-or-later.
"""
import json
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))
from extract_textures import load_palettes, load_mappings, resolve_wall_id, extract_wall_texture
from sprite_art import SpriteArt
from gen_views import SMS, ok_pair
from colour_lab import lab, dist2, err
from sms_gfx import rgb_to_sms, sms_cram_byte

SIZES = {"large": (160, 96), "small": (128, 80)}   # view width, height in pixels
RENDER_BANK = 6                     # code in slot 1; 7, 8 = the two size tables
FIRST_DATA_BANK = 9                 # banks 9.. hold the level's big data (after bank 3)
# the game state's layout from gen_cdata.py (for L_FLAGS in state.inc)
LAYOUT = json.loads((Path(__file__).resolve().parent.parent / "game" / "gen" / "layout.json").read_text())
NEAR = 20                           # near clip depth for walls seen at an angle, world units
EYE_Z = 36                          # eye height above the floor
N_HIDX = 1792                       # size-table entries per view size
FX = [0, 1, 0, -1]                  # forward x per facing N, E, S, W
FZ = [-1, 0, 1, 0]                  # forward z per facing
BANK = 0x4000                       # one Sega-mapper bank: 16 KB


def hidx(h8):
    """Size-table index of projected height h8 (1/256 px).

    Below 16384 (64 px) one entry per 16 (1/16 px); above, one per 64,
    capped at N_HIDX - 1.
    """
    return h8 >> 4 if h8 < 16384 else min(N_HIDX - 1, 1024 + ((h8 - 16384) >> 6))


def hidx_centre(i):
    """The h8 in the middle of size-table entry i (the inverse of hidx)."""
    return i * 16 + 8 if i < 1024 else 16384 + (i - 1024) * 64 + 32


def view_xf(f, rx, rz):
    """World offset (rx, rz) from the camera -> view space (depth, lateral) for facing f."""
    return [(-rz, rx), (rx, rz), (rz, -rx), (-rx, -rz)][f]


def camera(gx, gz, f):
    """The eye's world position: cell (gx, gz)'s centre, 16 units back from facing f."""
    return gx * 64 + 32 - 16 * FX[f], gz * 64 + 32 - 16 * FZ[f]


def sgn(v):
    """-1, 0 or 1: the sign of v."""
    return (v > 0) - (v < 0)


# ------------------------------------------------------------------ level
def build_walls(lvl, g, art, pal, maps, wtex):
    """Wall pieces + their texture images (megadrive/tools/build_render_data.py).

    lvl is bsp_parser.py's JSON, g gamedata.json, art a SpriteArt, pal,
    maps and wtex the jar's palettes, mappings and wall texels. Returns
    (segs, tex_imgs, keep, n_baked): segs are pieces [ax, az, bx, bz,
    texture image index, line slot (255 none), u at a, u at b, two-sided
    flag, source wall index]; tex_imgs the 64x64 RGB textures (one per
    texture/decal/u-range combination); keep the scenery not baked into a
    wall; n_baked the number of decals baked.
    """
    segs = []
    line_of_wall = {l["line"]: k for k, l in enumerate(g["lines"])}
    for i, w in enumerate(lvl["walls"]):
        ax, az, bx, bz, fl = w["ax"], w["az"], w["bx"], w["bz"], w["flags"]
        # the original loader's 3-unit nudge of one-sided walls (as gamedata.line_cell)
        if fl & 512:
            d = 3 if fl & 8 else (-3 if fl & 16 else 0)
            ax += d
            bx += d
        elif fl & 256:
            d = 3 if fl & 8 else (-3 if fl & 16 else 0)
            az += d
            bz += d
        L = max(abs(bx - ax), abs(bz - az))     # length along the major axis
        n = max(1, (L + 63) // 64)              # pieces of at most 64 units
        for k in range(n):
            t0, t1 = k * 64 / L, min((k + 1) * 64, L) / L
            pa = (round(ax + (bx - ax) * t0), round(az + (bz - az) * t0))
            pb = (round(ax + (bx - ax) * t1), round(az + (bz - az) * t1))
            u0, u1 = k * 64, min((k + 1) * 64, L)
            if fl & 0x8000:                     # texture runs the other way
                u0, u1 = L - u0, L - u1
            base = 64 * (min(u0, u1) // 64)     # keep u within 0..64 for the piece
            segs.append([pa[0], pa[1], pb[0], pb[1], w["tex"], line_of_wall.get(i, 255),
                         u0 - base, u1 - base, fl & 1, i])
    for s in segs:
        dx, dz = s[2] - s[0], s[3] - s[1]
        assert dx == 0 or dz == 0 or abs(dx) == abs(dz), ("wall not axis-aligned or diagonal", s)
        assert abs(s[7] - s[6]) == max(abs(dx), abs(dz)), ("u must run one texel per unit", s)
    # wall decals baked into the texture of the piece they cover
    # (end points) -> (piece, reversed?) to find the piece a decal lies on
    seg_at = {}
    for k, sg in enumerate(segs):
        seg_at[(sg[0], sg[1], sg[2], sg[3])] = (k, False)
        seg_at[(sg[2], sg[3], sg[0], sg[1])] = (k, True)
    baked, keep = {}, []
    for sc in g["scenery"]:
        info = sc["info"]
        if info & 0x780000:
            x, z = sc["x"], sc["z"]
            sg_ = -1 if info & 0x2000000 else 1
            if info & 0x80000:
                p0, p1 = (x - 32 * sg_, z), (x + 32 * sg_, z)
            elif info & 0x100000:
                p0, p1 = (x + 32 * sg_, z), (x - 32 * sg_, z)
            elif info & 0x400000:
                p0, p1 = (x, z - 32 * sg_), (x, z + 32 * sg_)
            else:
                p0, p1 = (x, z + 32 * sg_), (x, z - 32 * sg_)
            hit = seg_at.get(p0 + p1)
            if hit and not segs[hit[0]][8]:     # only onto one-sided walls
                baked.setdefault(hit[0], []).append((sc["tile"], hit[1]))
                continue
        keep.append(sc)

    def texture_rgb(t):
        """Wall texture t (a .bsp texture id) as 64x64 RGB, [v][u]."""
        tid = resolve_wall_id(maps, t)
        off, poff = maps["wall_table"][tid * 2], maps["wall_table"][tid * 2 + 1]
        grid = np.array(extract_wall_texture(wtex, off), dtype=np.uint8)
        return np.array([pal[poff + k] for k in range(16)], np.uint8)[grid][..., :3]

    # one texture image per plain texture, or per (texture, decals, u range)
    # for pieces with decals; each piece's texture field becomes the image index
    tex_key, tex_imgs = {}, []
    for k, sg in enumerate(segs):
        key = (sg[4], tuple(baked.get(k, [])), sg[6], sg[7]) if k in baked else (sg[4],)
        if key not in tex_key:
            img = texture_rgb(sg[4]).copy()
            for tile, rev in baked.get(k, []):
                cv = art.canvas(tile)
                ua, ub = sg[6], sg[7]
                for c in range(64):
                    t = (c + 0.5) / 64
                    sp = 1 - t if rev else t
                    u = int(np.floor(ua + (ub - ua) * sp)) & 63
                    a = cv[:, c, 3] > 0
                    img[a, u] = cv[a, c, :3]
            tex_key[key] = len(tex_imgs)
            tex_imgs.append(img)
        sg[4] = tex_key[key]
    return segs, tex_imgs, keep, sum(len(v) for v in baked.values())


# ------------------------------------------------------------------ colour
def brighter(rgb):
    """The art made a little more vivid before its colours are matched:
    saturation x 1.6 around the pixel's grey, then contrast x 1.15 around
    128, in integers so the editor gets exactly the same (sms_build.cpp):
    c' = 128 + round(23 * (24c - 3(r+g+b) - 1920) / 300), clamped."""
    a = np.asarray(rgb, np.int64)
    s3 = 3 * a.sum(-1, keepdims=True)
    num = 23 * (24 * a - s3 - 1920)
    return np.clip(128 + (num + 150) // 300, 0, 255).astype(np.uint8)


# The floor fades into the ceiling's colour towards the horizon: per tile
# row below it, how many quarters of each 2x2 block show the floor colour
# (2x2 ordered dither). The small view has room for two in-between tiles.
FLOOR_LEVELS = {"large": [0, 1, 2, 3, 4, 4], "small": [0, 1, 3, 4, 4]}
BAYER = ((0, 2), (3, 1))                # 2x2 threshold matrix [y & 1][x & 1]


def floor_level(size, y):
    """Quarters (0..4) of floor colour on view row y for view size size (0 above the horizon)."""
    H = SIZES[size][1]
    lv = FLOOR_LEVELS[size]
    return lv[min(max(y // 8 - H // 16, 0), len(lv) - 1)]


def floor_row_pair(size, y):
    """pair byte of floor row y (ceiling = colour 0, floor = colour 1): the
    pixel at (x, y) shows the high nibble when x + y is even

    So on even rows the high nibble is the even-x pixel, on odd rows the
    odd-x pixel; each gets floor colour where its Bayer threshold is
    below the row's level."""
    lv = floor_level(size, y)
    if y & 1 == 0:
        return (int(BAYER[0][0] < lv) << 4) | int(BAYER[0][1] < lv)
    return (int(BAYER[1][1] < lv) << 4) | int(BAYER[1][0] < lv)


def floor_tables():
    """rom/render.asm: per size 3 x 96 bytes: the floor rows as kept by an
    even column, by an odd column (one nibble each) and whole (low detail)

    Returns assembler lines: floor_rows_<size> (the three 96-byte tables)
    and floor_tiles_<size> (per tile row of the view, the border tile used
    for an all-floor tile row). Called by unrolled_fill()."""
    lines = []
    for size in ("large", "small"):
        e, o, f = [], [], []
        for y in range(96):
            pb = floor_row_pair(size, y)
            hi, lo = pb & 0xF0, pb & 0x0F
            e.append(hi if y % 2 == 0 else lo)
            o.append(lo if y % 2 == 0 else hi)
            f.append(pb)
        lines.append(f"floor_rows_{size}:")
        for t in (e, o, f):
            for k in range(0, 96, 16):
                lines.append("    .db " + ", ".join(f"${v:02X}" for v in t[k:k + 16]))
        # tile row -> border tile for an all-floor row (0 = the ceiling tile)
        H = SIZES[size][1]
        extra = sorted(set(l for l in FLOOR_LEVELS[size] if 0 < l < 4))
        tile = {0: 0, 4: 1, **{l: 14 + i for i, l in enumerate(extra)}}
        lines.append(f"floor_tiles_{size}:")
        lines.append("    .db " + ", ".join(str(tile[floor_level(size, r * 8)]) for r in range(H // 8)))
    return lines


def floor_band_tiles(size):
    """the in-between all-floor tiles (after the 14 border tiles)

    One 8x8 tile of colour indices (0 ceiling, 1 floor) per fade level
    strictly between 0 and 4 that the size uses."""
    out = []
    for l in sorted(set(l for l in FLOOR_LEVELS[size] if 0 < l < 4)):
        t = np.zeros((8, 8), np.uint8)
        for y in range(8):
            for x in range(8):
                t[y, x] = 1 if BAYER[y & 1][x & 1] < l else 0
        out.append(t)
    return out


class Colours:
    """One 16-colour SMS palette for the level: 0 = ceiling, 1 = floor,
    then 14 chosen greedily (CIELAB) where a pixel may be one colour or a
    50/50 pair of two colours at most one SMS level apart per channel.
    Integer errors and tools/colour_lab.py, so the editor makes the same
    choices (editor/src/sms_build.cpp)."""

    def __init__(self, samples, weights, ceil, floor):
        """Choose the palette from samples (Nx3 RGB pixels, made brighter
        first) weighted by weights; ceil and floor fix entries 0 and 1.

        Sets rgb (16x3 float), cand (Lab of every single colour and every
        allowed 50/50 pair), A and B (each candidate's two palette
        entries: equal for a single colour) and cram (16 CRAM bytes).
        """
        samples = brighter(samples)
        snap = lambda c: SMS[int(np.round(c[0] / 85)) * 16 + int(np.round(c[1] / 85)) * 4 + int(np.round(c[2] / 85))]
        sel = [snap(ceil), snap(floor)]
        u, inv = np.unique(samples, axis=0, return_inverse=True)
        cnt = np.bincount(inv.ravel(), weights=weights, minlength=len(u)).astype(np.int64)
        ulab = lab(u)

        def errs(C):
            """Per distinct sample: the smallest integer Lab error to any colour in C."""
            C = lab(np.array(C, np.float64))
            return err(dist2(ulab[:, None, :], C[None, :, :])).min(1)
        cur = errs(sel)
        # each round: the SMS colour (alone or paired with a chosen one)
        # that lowers the weighted total error most
        while len(sel) < 16:
            best = None
            for i in range(64):
                c = SMS[i].astype(np.float64)
                if any(np.array_equal(c, s) for s in sel):
                    continue
                cands = [c] + [(c + s) / 2 for s in sel if ok_pair(c, s)]
                e = np.minimum(cur, errs(cands))
                tot = int((e * cnt).sum())
                if best is None or tot < best[0]:
                    best = (tot, i, e)
            sel.append(SMS[best[1]].astype(np.float64))
            cur = best[2]
        self.rgb = np.array(sel, np.float64)
        P = self.rgb
        C, A, B = list(P), list(range(16)), list(range(16))
        for a in range(16):
            for b in range(a + 1, 16):
                if ok_pair(P[a], P[b]):
                    C.append((P[a] + P[b]) / 2)
                    A.append(a)
                    B.append(b)
        self.cand = lab(np.array(C, np.float64))
        self.A, self.B = np.array(A), np.array(B)
        self.cram = bytes(sms_cram_byte(*rgb_to_sms(*[int(v) for v in c])) for c in sel)

    def pairs(self, rgb):
        """[..., 3] uint8 -> [...] pair bytes (A << 4 | B).

        Each colour (brightened) gets the candidate nearest in Lab; a plain
        colour gives a pair of the same entry twice.
        """
        flat = brighter(rgb.reshape(-1, 3)).astype(np.float64)
        u, inv = np.unique(flat, axis=0, return_inverse=True)
        k = dist2(lab(u)[:, None, :], self.cand[None, :, :]).argmin(1)
        pb = (self.A[k] << 4) | self.B[k]
        return pb[inv.ravel()].reshape(rgb.shape[:-1]).astype(np.uint8)


# ------------------------------------------------------------------ visibility
def visible_walls(W, removed, gx, gz, f, cols=512):
    """Walls that win at least one of `cols` columns (the Mega Drive game's
    build_render_data.py), and the nearest depth per column.

    W is an int array of pieces [ax, az, bx, bz, two-sided]; removed a bool
    mask of pieces to leave out (open doors); (gx, gz, f) the view. Pieces
    seen from behind are dropped unless two-sided (those are flipped), as
    are pieces wholly behind the eye or outside the 90 degree view. Each of
    cols evenly spaced rays across the view is intersected with every
    piece. Returns (set of winning piece indices, nearest depth per ray).
    """
    vx, vz = camera(gx, gz, f)

    def view(x, z):
        """World point(s) -> (depth, lateral) arrays for this facing."""
        rx, rz = x - vx, z - vz
        return [(-rz, rx), (rx, rz), (rz, -rx), (-rx, -rz)][f]
    # 2D cross product: the eye is on the piece's front side
    front = (W[:, 0] - vx) * (W[:, 3] - W[:, 1]) - (W[:, 1] - vz) * (W[:, 2] - W[:, 0]) > 0
    W = W.copy()
    flip = ~front & (W[:, 4] == 1)
    W[flip, 0], W[flip, 2] = W[flip, 2].copy(), W[flip, 0].copy()
    W[flip, 1], W[flip, 3] = W[flip, 3].copy(), W[flip, 1].copy()
    da, la = view(W[:, 0], W[:, 1])
    db, lb = view(W[:, 2], W[:, 3])
    ok = (front | (W[:, 4] == 1)) & ~removed & ~((da < 1) & (db < 1)) & ~((la > da) & (lb > db)) & ~((la < -da) & (lb < -db))
    da, la, db, lb = da[ok].astype(float), la[ok].astype(float), db[ok].astype(float), lb[ok].astype(float)
    idx = np.flatnonzero(ok)
    xs = (np.arange(cols) + 0.5) * (256.0 / cols)
    k = (xs - 128) / 128.0                  # ray slope l / d: -1 (left edge) .. 1 (right)
    # where ray l = k d meets the piece: depth, and t along the piece (0..1)
    dd, dl = db - da, lb - la
    c = dl * da - dd * la
    num = dl[:, None] - k[None, :] * dd[:, None]
    with np.errstate(divide="ignore", invalid="ignore"):
        depth = c[:, None] / num
        t = (k[None, :] * da[:, None] - la[:, None]) / num
    good = (t >= -1e-6) & (t <= 1 + 1e-6) & (depth >= 1)
    depth = np.where(good, depth, np.inf)
    if len(depth) == 0:
        return set(), np.full(cols, np.inf)
    win = depth.argmin(0)
    hit = np.isfinite(depth.min(0))
    return set(int(idx[i]) for i in np.unique(win[hit])), depth.min(0)


def visible_scenery(scen, colz, gx, gz, f, cols=512):
    """Indices of the scenery billboards that can show in view (gx, gz, f).

    colz is the nearest wall depth per ray from visible_walls. A billboard
    counts if it is at least NEAR deep (after the 16-unit nudge), inside
    the view, and nearer than the wall (with 24 units of slack) on some ray
    its 80-unit-wide span (40 either side, a margin over its 64) covers.
    """
    vx, vz = camera(gx, gz, f)
    out = []
    for i, s in enumerate(scen):
        d, l = view_xf(f, s["x"] - vx, s["z"] - vz)
        d -= 16
        if d < NEAR or l - 32 > d or l + 32 < -d:
            continue
        a = int(np.floor((128 + (l - 40) * 128.0 / d) * cols / 256))
        b = int(np.ceil((128 + (l + 40) * 128.0 / d) * cols / 256))
        a, b = max(0, a), min(cols, b)
        if b > a and (colz[a:b] > d - 24).any():
            out.append(i)
    return out


# ------------------------------------------------------------------ tables
def size_table(W, H):
    """hidx -> y0, y1, v0, dv, d  (u8, u8, u16 8.8, u16 8.8, u16).

    For the view size W x H: y0..y1 the rows a wall of that height covers
    (its top is 7/16 of the height above the centre: 28 of 64 units above
    the eye), v0 the texture row at y0's centre and dv the texture step
    per row (8.8 fixed point, kept so v stays below 64), d the depth that
    gives that height. Returns (the packed 8-byte entries, the list of
    tuples).
    """
    CY = H / 2
    out = bytearray()
    rows = []
    for i in range(N_HIDX):
        h8 = hidx_centre(i)
        h = h8 / 256.0
        top, bot = CY - 7 * h / 16, CY + 9 * h / 16
        y0 = min(H, max(0, int(np.ceil(top - 0.5))))
        y1 = min(H, max(0, int(np.ceil(bot - 0.5))))
        dv = min(65535, int(round(64 * 256 / h)))
        v0 = int(round((y0 + 0.5 - top) * 64 / h * 256))
        v0 = max(0, min(16383, v0))
        if y1 > y0 + 1 and v0 + (y1 - 1 - y0) * dv > 16383:
            dv = (16383 - v0) // (y1 - 1 - y0)
        d = min(65535, int(32 * W * 256 // h8))
        rows.append((y0, y1, v0, dv, d))
        out += struct.pack("<BBHHH", y0, y1, v0, dv, d)
    return bytes(out), rows


# ------------------------------------------------------------------ the model
class Model:
    """Bit-exact model of rom/render.asm: render(...) -> HxW colour indices."""

    def __init__(self, data, size, detail=0, solid=0):
        """detail 1 = low detail: only even columns are worked out, with
        doubled steps, and each is shown two pixels wide. solid 1 = each
        wall column in one colour: its texel at v = 32."""
        self.D = data
        self.low = detail
        self.solid = solid
        self.step = 2 if detail else 1
        self.W, self.H = SIZES[size]
        self.floor = np.array([floor_row_pair(size, y) for y in range(self.H)], np.uint8)
        self.F = self.W // 2
        self.tab = data["tables"][size]

    def project(self, d, l):
        """Screen position of view-space point (d, l): 1/256 px from the centre.

        Points at 45 degrees or more to the side are clamped just outside
        the view, at +-(F + 1) * 256. Integer maths as in the Z80 code.
        """
        F = self.F
        if l >= d:
            return (F + 1) * 256
        if l <= -d:
            return -(F + 1) * 256
        p = ((abs(l) * 65536 // d) * F) >> 8
        return p if l >= 0 else -p

    def col_range(self, pa, pb):
        """The screen columns [x0, x1) whose centres lie between projections pa and pb.

        In low detail x0 is rounded up to an even column.
        """
        lo, hi = min(pa, pb), max(pa, pb)
        x0 = ((lo + 127) >> 8) + self.F
        x1 = ((hi + 127) >> 8) + self.F
        x0 = max(0, x0)
        if self.low:
            x0 = (x0 + 1) & ~1
        return x0, min(self.W, x1)

    def l8_start(self, x0, d):
        """8.8 lateral at column x0's centre for something at depth d (mod 2^16)."""
        P = x0 * 256 + 128
        return ((P * d) // self.F - 256 * d) & 0xFFFF

    def render(self, gx, gz, f, l_flags, want_cols=False):
        """Draw view (gx, gz, f) as the ROM does: an H x W array of colour indices.

        l_flags holds each switchable line's flags byte (bit 0 set: open,
        so its wall is skipped). The pass over the pvs walls keeps, per
        column, the tallest (nearest) wall's height, slot and texture
        column; then each column is filled (ceiling pair, wall texels, floor
        rows), billboards are drawn far to near where they are taller than
        the wall, and the pair bytes are split into pixels by checkerboard.
        want_cols: also return (heights, wall slots, texture columns).
        """
        D, W, H, F = self.D, self.W, self.H, self.F
        st = self.step
        vx, vz = camera(gx, gz, f)
        colh = [0] * W
        colw = [255] * W
        colu = [0] * W
        walls = []
        wl, sl = D["pvs"].get((gx, gz, f), ([], []))
        for wi in wl:
            ax, az, bx, bz, tex, line, ua, ub, two = D["walls"][wi]
            if line != 255 and l_flags[line] & 1:
                continue
            sx, sz = sgn(bx - ax), sgn(bz - az)
            cross = (ax - vx) * sz - (az - vz) * sx     # which side of the wall the eye is on
            if cross == 0:
                continue
            if cross < 0:                                # seen from behind
                if not two:
                    continue
                ax, az, bx, bz, ua, ub = bx, bz, ax, az, ub, ua
            da, la = view_xf(f, ax - vx, az - vz)
            db, lb = view_xf(f, bx - vx, bz - vz)
            slot = len(walls)
            if da == db:                                  # facing the screen
                if da < 8:
                    continue
                pa, pb = self.project(da, la), self.project(db, lb)
                x0, x1 = self.col_range(pa, pb)
                if x1 <= x0:
                    continue
                h8 = min(65535, 8192 * W // da)          # the same height across the wall
                su = sgn(ub - ua) * sgn(lb - la)         # u direction per unit of lateral
                cu = (ua - su * la) & 63
                l8 = self.l8_start(x0, da)               # 8.8 lateral at the first column
                dl8 = (256 * da) // F * st               # lateral step per column
                walls.append(tex)
                for x in range(x0, x1, st):
                    if h8 > colh[x]:
                        colh[x], colw[x] = h8, slot
                        colu[x] = (cu + su * (l8 >> 8)) & 63
                    l8 = (l8 + dl8) & 0xFFFF
                continue
            if da < NEAR and db < NEAR:
                continue
            # a wall at an angle: l = l0 + m d, m the slope (0 or +-1, diagonal)
            m = (lb - la) // (db - da)
            assert m * (db - da) == lb - la and m in (-1, 0, 1)
            if da < NEAR:                                # clip the near end to NEAR
                la, da = la + m * (NEAR - da), NEAR
            elif db < NEAR:
                lb, db = lb + m * (NEAR - db), NEAR
            l0 = la - m * da
            if l0 == 0:                                  # edge-on through the eye
                continue
            pa, pb = self.project(da, la), self.project(db, lb)
            x0, x1 = self.col_range(pa, pb)
            if x1 <= x0:
                continue
            # h8 = N(x) / l0 stepped in 24.8 fixed point (see the module docstring)
            N0 = 16384 * (x0 - F) + 8192 - 8192 * W * m
            hacc = (abs(N0) << 8) // abs(l0)
            dacc = (16384 << 8) // abs(l0)
            if l0 < 0:
                dacc = -dacc
            dacc *= st
            su = sgn(ub - ua) * sgn(db - da)             # u direction per unit of depth
            cz = (ua - su * da) & 63
            walls.append(tex)
            for x in range(x0, x1, st):
                h8 = max(1, min(65535, hacc >> 8))
                if h8 > colh[x]:
                    colh[x], colw[x] = h8, slot
                    z = self.tab[hidx(h8)][4]            # the depth from the size table
                    colu[x] = (cz + su * z) & 63
                hacc += dacc
        # billboards, far to near
        bills = []
        for si in sl:
            x_, z_, spr = D["scenery"][si]
            d, l = view_xf(f, x_ - vx, z_ - vz)
            d -= 16
            if d < NEAR:
                continue
            pa, pb = self.project(d, l - 32), self.project(d, l + 32)
            x0, x1 = self.col_range(pa, pb)
            if x1 <= x0:
                continue
            c8 = (self.l8_start(x0, d) - ((l - 32) * 256)) & 0xFFFF   # 8.8 canvas column at x0
            bills.append((d, spr, x0, x1, min(65535, 8192 * W // d), c8, (256 * d) // F * st))
        bills.sort(key=lambda b: -b[0])       # stable: far first, ties in list order
        img = np.zeros((H, W), np.uint8)
        cf = D["ceil_pair"], D["floor_pair"]
        chunk = np.zeros((H, W), np.uint8)    # pair bytes
        # fill each column: ceiling above the wall, the wall, floor below
        for x in range(0, W, st):
            if colw[x] == 255:                           # no wall: ceiling meets floor at the centre
                y0 = y1 = H // 2
            else:
                y0, y1, v0, dv, _d = self.tab[hidx(colh[x])]
                col = D["texcols"][walls[colw[x]]][colu[x]]
                if self.solid:                           # solid walls: one texel for the column
                    chunk[y0:y1, x] = col[32]
                else:
                    v = v0
                    for y in range(y0, y1):
                        chunk[y, x] = col[v >> 8]
                        v += dv
            chunk[:y0, x] = cf[0]
            chunk[y1:, x] = self.floor[y1:]
        for d, spr, x0, x1, h8, c8, dc8 in bills:
            y0, y1, v0, dv, _d = self.tab[hidx(h8)]
            top, ln, data = D["sprites"][spr]
            for x in range(x0, x1, st):
                c = c8 >> 8
                c8 = (c8 + dc8) & 0xFFFF
                if c > 63 or h8 < colh[x] or ln[c] == 0:  # off the canvas, behind the wall, or empty
                    continue
                v = v0
                for y in range(y0, y1):
                    r = v >> 8
                    v += dv
                    k = r - top[c]
                    if 0 <= k < ln[c]:
                        p = data[c][k]
                        if p != D["key"]:
                            chunk[y, x] = p
        if self.low:                                     # low detail: odd columns copy even ones
            chunk[:, 1::2] = chunk[:, 0::2]
        ys, xs = np.mgrid[0:H, 0:W]
        img = np.where(((xs + ys) & 1) == 0, chunk >> 4, chunk & 15).astype(np.uint8)
        if want_cols:
            return img, (colh, colw, colu)
        return img


# ------------------------------------------------------------------ border
BORDER_TEX = 11                     # the jar's brown rock, like Doom's border flat
VIEW_POS = {"large": (4, 6), "small": (5, 8)}       # tile row, column of the view
BORDER_BASE = {"large": 368, "small": 40}            # first border tile in VRAM
# (the border is the Doom-style frame of rock around the view; its tiles are
# 2 solid (ceiling, floor colour), 4 rock, 8 frame, then the floor fade tiles)


def wall_texture_by_id(tid, pal, maps, wtex):
    """Wall texture slot tid (already resolved) as 64x64 RGB, [v][u]."""
    off, poff = maps["wall_table"][tid * 2], maps["wall_table"][tid * 2 + 1]
    grid = np.array(extract_wall_texture(wtex, off), dtype=np.uint8)
    return np.array([pal[poff + k] for k in range(16)], np.uint8)[grid][..., :3]


def frame_tiles():
    """The frame around the view, 8 tiles (t, b, l, r, tl, tr, bl, br): a
    grey metal bevel, raised on the outside and sunk on the inside, cut
    from one 24x24 picture (a 1-tile frame around a 1-tile hole)."""
    hi, mid, lo, dark = (190, 190, 190), (120, 120, 120), (60, 60, 60), (20, 20, 20)
    f = np.zeros((24, 24, 3), np.uint8)
    f[:] = mid
    f[0, :] = hi                     # outside: lit top/left, shaded bottom/right
    f[:, 0] = hi
    f[23, :] = lo
    f[:, 23] = lo
    f[1, 1:23] = (160, 160, 160)
    f[1:23, 1] = (160, 160, 160)
    f[6:8, 6:18] = dark              # inside: shadow on top/left, light bottom/right
    f[6:18, 6:8] = dark
    f[16:18, 6:18] = hi
    f[6:18, 16:18] = hi
    f[6:8, 16:18] = hi
    f[16:18, 6:8] = dark
    cut = {"tl": (0, 0), "t": (0, 1), "tr": (0, 2), "l": (1, 0), "r": (1, 2), "bl": (2, 0), "b": (2, 1), "br": (2, 2)}
    return {k: f[r * 8:r * 8 + 8, c * 8:c * 8 + 8] for k, (r, c) in cut.items()}


def make_border(name, col, tex):
    """Tiles (2 solid + 2x2 rock + 8 frame) and the name table of screen
    rows 2..17 (u16 entries, view area 0).

    name is the view size, col the level's Colours, tex the rock texture
    (RGB). Returns (tile patterns, 4bpp planar, 32 bytes each; the name
    table, 16 rows x 32 u16 entries, VRAM tile numbers from BORDER_BASE;
    a preview picture of colour indices)."""
    W, H = SIZES[name]
    rows, strips = H // 8, W // 8
    vr, vc = VIEW_POS[name]
    base = BORDER_BASE[name]

    def to_idx(rgb, y0=0, x0=0):
        """RGB art -> colour indices through the level palette, dithered by checkerboard."""
        pb = col.pairs(rgb)
        ys, xs = np.mgrid[0:rgb.shape[0], 0:rgb.shape[1]]
        return np.where(((ys + y0 + xs + x0) & 1) == 0, pb >> 4, pb & 15).astype(np.uint8)
    tiles = [np.zeros((8, 8), np.uint8), np.ones((8, 8), np.uint8)]
    patch = to_idx(tex[8:24, 8:24])         # a 16x16 piece of rock: 4 tiles, repeated
    for ty in range(2):
        for tx in range(2):
            tiles.append(patch[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8])
    kinds = ["t", "b", "l", "r", "tl", "tr", "bl", "br"]
    ft = frame_tiles()
    for k in kinds:
        tiles.append(to_idx(ft[k]))
    tid = {k: 6 + i for i, k in enumerate(kinds)}      # frame tiles follow the 2 solid + 4 rock
    tiles += floor_band_tiles(name)
    nmap = bytearray()
    shot = np.zeros((16 * 8, 256), np.uint8)
    for r in range(2, 18):
        for c in range(32):
            inside = vr <= r < vr + rows and vc <= c < vc + strips
            if inside:
                t = None
            elif r == vr - 1 and vc - 1 <= c <= vc + strips:
                t = tid["tl"] if c == vc - 1 else tid["tr"] if c == vc + strips else tid["t"]
            elif r == vr + rows and vc - 1 <= c <= vc + strips:
                t = tid["bl"] if c == vc - 1 else tid["br"] if c == vc + strips else tid["b"]
            elif vr <= r < vr + rows and c == vc - 1:
                t = tid["l"]
            elif vr <= r < vr + rows and c == vc + strips:
                t = tid["r"]
            else:                       # rock, in a 2x2 repeat
                t = 2 + (r & 1) * 2 + (c & 1)
            nmap += struct.pack("<H", 0 if t is None else base + t)
            if t is not None:
                shot[(r - 2) * 8:(r - 1) * 8, c * 8:c * 8 + 8] = tiles[t]
    blob = bytearray()
    for t in tiles:
        for y in range(8):
            planes = [0, 0, 0, 0]
            for x in range(8):
                v = int(t[y, x])
                for p in range(4):
                    if v & (1 << p):
                        planes[p] |= 1 << (7 - x)
            blob += bytes(planes)
    return bytes(blob), bytes(nmap), shot


# ------------------------------------------------------------------ main
def check_layout_inc():
    """rom/live_layout.inc must match H_FIELDS (it is part of the engine)"""
    p = Path(__file__).resolve().parent.parent / "rom" / "live_layout.inc"
    want = live_layout_inc()
    if not p.exists() or p.read_text().split(";; ---")[0] != want:
        raise SystemExit(f"{p} is out of date with H_FIELDS in tools/live_data.py:\n{want}")


def main():
    """Build all the renderer's data for one level and write it (see the module docstring)."""
    lvl_path, gd_path, sms_path, jar, out = (Path(a) for a in sys.argv[1:6])
    out.mkdir(parents=True, exist_ok=True)
    check_layout_inc()
    lvl = json.loads(lvl_path.read_text())
    g = json.loads(gd_path.read_text())
    cells = [0 if w else 1 for w in json.loads(gd_path.read_text())["wall"]]   # every open cell
    pal = load_palettes(jar / "palettes.bin")
    maps = load_mappings(jar / "mappings.bin")
    wtex = (jar / "wtexels.bin").read_bytes()
    art = SpriteArt(jar)

    segs, tex_imgs, scen, n_baked = build_walls(lvl, g, art, pal, maps, wtex)
    bill_tiles = sorted({s["tile"] for s in scen})
    left_decals = [s for s in scen if s["info"] & 0x780000]
    print(f"walls: {len(lvl['walls'])} -> {len(segs)} pieces, {len(tex_imgs)} textures, {n_baked} decals baked, "
          f"{len(left_decals)} free-standing decals drawn as billboards, {len(scen)} billboards "
          f"({len(bill_tiles)} sprites)")
    # A decal not on a wall (bars or a railing standing across an opening,
    # often two-sided in the original) has no wall to be baked into; the
    # Master System draws it as a billboard at its centre instead: the same
    # picture seen straight on, and visible from both sides.

    # ---- colours: weighted by how often each texture is used
    use = np.bincount([s[4] for s in segs], minlength=len(tex_imgs))
    samples, weights = [], []
    for t, img in enumerate(tex_imgs):
        samples.append(img.reshape(-1, 3))
        weights.append(np.full(4096, max(1, use[t]), np.int64))
    for t in bill_tiles:
        cv = art.canvas(t)
        a = cv[..., 3] > 0
        samples.append(cv[a][:, :3])
        weights.append(np.full(int(a.sum()), 2, np.int64))
    # the border rock counts heavily, so its browns get colours
    border_img = wall_texture_by_id(BORDER_TEX, pal, maps, wtex)
    samples.append(border_img[:16, :16].reshape(-1, 3))
    weights.append(np.full(256, 40, np.int64))
    col = Colours(np.concatenate(samples), np.concatenate(weights), g.get("sms_ceiling_rgb", g["ceiling_rgb"]),
                  g.get("sms_floor_rgb", g["floor_rgb"]))

    # ---- texture columns, deduplicated
    pool, pool_idx, texcols, texcol_ids = [], {}, [], []
    for img in tex_imgs:
        pb = col.pairs(img)                    # [v][u]
        cols_, ids = [], []
        for u in range(64):
            c = bytes(pb[:, u])
            if c not in pool_idx:
                pool_idx[c] = len(pool)
                pool.append(c)
            cols_.append(c)
            ids.append(pool_idx[c])
        texcols.append(cols_)
        texcol_ids.append(ids)
    ceil_pair, floor_pair = 0x00, 0x11       # palette entries 0 and 1, undithered

    # ---- billboard sprites: per column top, length, pair bytes; key = an unused pair byte
    spr_pairs = {}
    used = set(b for c in pool for b in c)
    for t in bill_tiles:
        cv = art.canvas(t)
        spr_pairs[t] = (col.pairs(cv[..., :3]), cv[..., 3] > 0)
        used |= set(np.unique(spr_pairs[t][0][spr_pairs[t][1]]).tolist())
    key = next(k for k in range(255, -1, -1) if k not in used)   # the highest unused pair byte
    sprites = []
    for t in bill_tiles:
        pb, a = spr_pairs[t]
        top, ln, data = [], [], []
        for c in range(64):
            rows = np.flatnonzero(a[:, c])
            if len(rows) == 0:
                top.append(0)
                ln.append(0)
                data.append(b"")
                continue
            r0, r1 = int(rows[0]), int(rows[-1]) + 1
            top.append(r0)
            ln.append(r1 - r0)
            data.append(bytes(int(pb[r, c]) if a[r, c] else key for r in range(r0, r1)))
        sprites.append((top, ln, data))
    spr_slot = {t: i for i, t in enumerate(bill_tiles)}
    scenery = [(s["x"], s["z"], spr_slot[s["tile"]]) for s in scen]

    # ---- visibility per (cell, facing)
    walls = [(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8]) for s in segs]
    Wn = np.array([[w[0], w[1], w[2], w[3], w[8]] for w in walls], dtype=np.int64)
    doors = np.array([w[5] != 255 for w in walls])
    # per open cell and facing: walls visible with doors shut or open, and
    # the billboards in front of the doors-shut walls
    pvs = {}
    for gz in range(32):
        for gx in range(32):
            if cells[gz * 32 + gx] == 0:
                continue
            for f in range(4):
                vo, colz = visible_walls(Wn, doors, gx, gz, f)
                vc, _ = visible_walls(Wn, np.zeros(len(walls), bool), gx, gz, f)
                pvs[(gx, gz, f)] = (sorted(vo | vc), visible_scenery(scen, colz, gx, gz, f))
    nw = [len(v[0]) for v in pvs.values()]
    ns = [len(v[1]) for v in pvs.values()]
    print(f"views: {len(pvs)}; walls per view avg {np.mean(nw):.1f} max {max(nw)}; "
          f"billboards per view avg {np.mean(ns):.1f} max {max(ns)}")

    tables, rows = {}, {}
    for name, (W, H) in SIZES.items():
        blob, rows[name] = size_table(W, H)
        (out / f"table_{name}.bin").write_bytes(blob)
    data = {"walls": walls, "pvs": pvs, "scenery": scenery, "texcols": texcols, "sprites": sprites,
            "key": key, "ceil_pair": ceil_pair, "floor_pair": floor_pair, "tables": rows,
            "palette": col.rgb.astype(np.uint8), "cram": col.cram}
    for name in SIZES:
        tiles, nmap, preview_img = make_border(name, col, border_img)
        data.setdefault("border", {})[name] = (tiles, nmap)
        data.setdefault("border_preview", {})[name] = preview_img
    write_blobs(out, data, pool, texcol_ids)
    if "--preview" in sys.argv:
        preview(Path(sys.argv[sys.argv.index("--preview") + 1]), data, lvl_path, jar, len(g["lines"]))
    return data


def write_blobs(out, D, pool, texcol_ids):
    """Binary files for the ROM (layout: docs/rom_format.md).

    out is the output directory, D the data dict built by main(), pool the
    unique 64-byte texture columns and texcol_ids each texture's 64 column
    ids into it. Also writes the banked images, header, c2p tables,
    generated loops and next_bank.txt, and prints a size summary.
    """
    (out / "palette.bin").write_bytes(D["cram"])
    w = bytearray()
    for (ax, az, bx, bz, tex, line, ua, ub, two) in D["walls"]:
        w += struct.pack("<hhhhBBBBBxxx", ax, az, bx, bz, tex, line, ua, ub, two)
    (out / "walls.bin").write_bytes(bytes(w))
    # visibility lists: n_walls, wall ids (u16), n_billboards, scenery ids (u8)
    blob, index = bytearray(), [0] * 4096
    for (gx, gz, f), (wl, sl) in sorted(D["pvs"].items(), key=lambda kv: (kv[0][1], kv[0][0], kv[0][2])):
        rec = bytes([len(wl)]) + b"".join(struct.pack("<H", i) for i in wl) + bytes([len(sl)]) + bytes(sl)
        assert len(rec) <= 96, f"view {gx},{gz},{f}: too many walls/props visible (the renderer's list holds 96 bytes)"
        if (len(blob) % BANK) + len(rec) > BANK:
            blob += bytes(BANK - len(blob) % BANK)
        index[(gz * 32 + gx) * 4 + f] = len(blob) + 1          # 0 = no list
        blob += rec
    (out / "pvs_index.bin").write_bytes(b"".join(struct.pack("<H", v) for v in index))
    (out / "pvs.bin").write_bytes(bytes(blob))
    (out / "texcols.bin").write_bytes(b"".join(struct.pack("<H", c) for ids in texcol_ids for c in ids))
    (out / "columns.bin").write_bytes(b"".join(pool))
    # scenery: x, z (s16), sprite (u8), pad
    (out / "scenery.bin").write_bytes(b"".join(struct.pack("<hhBx", *s) for s in D["scenery"]))
    # billboard sprites: per sprite 64 x u16 offset (from the sprite start) of
    # the column's 64-byte block (one pair byte per canvas row, key =
    # transparent; 0 = empty column), then the blocks, 64-aligned
    sp, offs = bytearray(), []
    for top, ln, data in D["sprites"]:
        rec = bytearray(128)
        body = bytearray()
        for c in range(64):
            if ln[c] == 0:
                continue
            col = bytearray([D["key"]] * 64)
            col[top[c]:top[c] + ln[c]] = data[c]
            struct.pack_into("<H", rec, c * 2, 128 + len(body))
            body += col
        rec += body
        if (len(sp) % BANK) + len(rec) > BANK:
            sp += bytes(BANK - len(sp) % BANK)
        offs.append(len(sp))
        sp += rec
    (out / "bsprites.bin").write_bytes(bytes(sp))
    # directory: offset (24-bit), pad, then the canvas rows the sprite uses
    # (first, end) so a billboard only covers those screen rows
    ext = []
    for top, ln, data in D["sprites"]:
        used = [c for c in range(64) if ln[c]]
        ext.append((min(top[c] for c in used), max(top[c] + ln[c] for c in used)) if used else (0, 0))
    (out / "bsprite_dir.bin").write_bytes(b"".join(struct.pack("<IBBH", o, a, b, 0) for o, (a, b) in zip(offs, ext)))
    # ---- the level's data, placed in ROM banks: bank 3 starts with the level
    # header (H_* below, rom/live_layout.inc), small tables follow wherever
    # they fit (never across a bank), the big blobs start on bank boundaries.
    # Banks 4-8 belong to the engine (strings, title, renderer, its tables).
    small = [("walls", bytes(w), 16), ("pvs_index", b"".join(struct.pack("<H", v) for v in index), 256),
             ("texcols", b"".join(struct.pack("<H", c) for ids in texcol_ids for c in ids), 2),
             ("scenery", b"".join(struct.pack("<hhBx", *s_) for s_ in D["scenery"]), 2),
             ("bsprite_dir", b"".join(struct.pack("<IBBH", o, a_, b_, 0) for o, (a_, b_) in zip(offs, ext)), 2),
             ("btiles_large", D["border"]["large"][0], 32), ("btiles_small", D["border"]["small"][0], 32),
             ("bmap_large", D["border"]["large"][1], 2), ("bmap_small", D["border"]["small"][1], 2),
             ("palette", D["cram"], 16)]
    big = [("pvs", bytes(blob)), ("columns", b"".join(pool)), ("bsprites", bytes(sp))]
    place, images = pack_level(small, big)
    hdr = make_header(place, D["key"], len(D["border"]["large"][0]) // 32, len(D["border"]["small"][0]) // 32,
                      len(D["walls"]))
    images[3][0:len(hdr)] = hdr
    with open(out / "data_banks.inc", "w") as fp:
        fp.write("; generated by tools/live_data.py: the level's data banks\n")
        for bank in sorted(images):
            (out / f"data_{bank}.bin").write_bytes(bytes(images[bank]))
            fp.write(f".BANK {bank} SLOT 2\n.ORGA $8000\n.incbin \"build/gfx/live/data_{bank}.bin\"\n")
    (out / "c2p.bin").write_bytes(c2p_tables())           # bank 5 at $A000 (beside the title)
    (out / "next_bank.txt").write_text(str(max(images) + 1))
    (out / "state.inc").write_text(f"; generated by tools/live_data.py from game/gen/layout.json\n"
                                   f".define L_FLAGS          ${LAYOUT['fields']['l_flags'][0] + 0xC400:04X}\n")

    (out / "unroll.inc").write_text(unrolled_fill())
    print(f"data: walls {len(w)} B, visibility {len(blob) // 1024} KB (+8 KB index), texture columns "
          f"{len(pool)} = {len(pool) * 64 // 1024} KB, billboards {len(sp) // 1024} KB, tables 2 x {N_HIDX * 8 // 1024} KB")


HEADER_SIZE = 64                    # the level header at $8000 in bank 3
H_FIELDS = [   # name, size: rom/live_layout.inc has the same offsets
    ("magic", 2), ("version", 1), ("walls_bank", 1), ("walls_addr", 2), ("pvs_index_bank", 1),
    ("pvs_index_addr", 2), ("pvs_bank", 1), ("tex_bank", 1), ("texcols_bank", 1), ("texcols_addr", 2),
    ("scenery_bank", 1), ("scenery_addr", 2), ("bspr_bank", 1), ("bsprite_dir_bank", 1), ("bsprite_dir_addr", 2),
    ("border_bank", 1), ("btiles_large_addr", 2), ("btiles_small_addr", 2), ("bmap_large_addr", 2),
    ("bmap_small_addr", 2), ("palette_bank", 1), ("palette_addr", 2), ("bill_key", 1), ("n_btiles_large", 1),
    ("n_btiles_small", 1), ("n_walls", 2)]
LEVEL_BANKS = [3] + list(range(FIRST_DATA_BANK, 256))   # banks the level data may use, in order


def pack_level(small, big):
    """-> {name: (bank, address)}, {bank: bytearray(16 KB)}

    small is a list of (name, bytes, alignment): packed one after another
    from just after the header in bank 3, aligned, moving to the next
    bank when one would cross a bank end. The border's tiles and maps are
    kept together in one bank (the ROM pages one bank for them). big is a
    list of (name, bytes): each starts on a fresh bank and may span
    several. Addresses are slot-2 CPU addresses ($8000-$BFFF).
    """
    images, place = {}, {}
    banks = iter(LEVEL_BANKS)
    bank, pos = next(banks), HEADER_SIZE
    images[bank] = bytearray(BANK)
    border = [k for k, _, _ in small if k.startswith(("btiles", "bmap"))]
    border_size = sum(len(d) + 32 for k, d, _ in small if k in border)
    for name, data, align in small:
        pos = (pos + align - 1) // align * align
        need = border_size if name == border[0] else len(data)
        if pos + need > BANK:          # (the border's tiles and maps share a bank: one bank number)
            bank, pos = next(banks), 0
            images[bank] = bytearray(BANK)
        images[bank][pos:pos + len(data)] = data
        place[name] = (bank, 0x8000 + pos)
        pos += len(data)
    for name, data in big:
        bank = next(banks)
        place[name] = (bank, 0x8000)
        for i in range(0, max(1, len(data)), BANK):
            if i:
                bank = next(banks)
            images[bank] = bytearray(data[i:i + BANK].ljust(BANK, b"\0"))
    bb = {place[k][0] for k in border}
    assert len(bb) == 1, "border tiles and maps must share a bank"
    return place, images


def make_header(place, key, nbl, nbs, n_walls):
    """The 64-byte level header: every field of H_FIELDS, little-endian, zero padded.

    place is pack_level's {name: (bank, address)}; key the billboard
    transparent byte; nbl and nbs the border tile counts per size; n_walls
    the number of wall pieces. Big tables record only their first bank.
    """
    v = {"magic": b"LV", "version": 1, "bill_key": key, "n_btiles_large": nbl, "n_btiles_small": nbs,
         "n_walls": n_walls}
    for k in ("walls", "pvs_index", "texcols", "scenery", "bsprite_dir", "palette"):
        v[k + "_bank"], v[k + "_addr"] = place[k]
    v["pvs_bank"], v["tex_bank"], v["bspr_bank"] = place["pvs"][0], place["columns"][0], place["bsprites"][0]
    v["border_bank"] = place["btiles_large"][0]
    for k in ("btiles_large", "btiles_small", "bmap_large", "bmap_small"):
        v[k + "_addr"] = place[k][1]
    out = bytearray()
    for name, size in H_FIELDS:
        x = v[name]
        out += x if isinstance(x, bytes) else x.to_bytes(size, "little")
    assert len(out) <= HEADER_SIZE
    return bytes(out).ljust(HEADER_SIZE, b"\0")


def live_layout_inc():
    """Text of rom/live_layout.inc: an H_<FIELD> offset .define per header field."""
    lines = ["; Deimos - Doom RPG on retro consoles",            # the licence notice every source file carries
             "; Copyright (C) 2026 unfnknblvbl and contributors",
             "; SPDX-License-Identifier: GPL-2.0-or-later",
             "; Free software under the GNU General Public License, version 2 or (at your",
             "; option) any later version, with NO WARRANTY: see the LICENSE file.",
             "",
             "; generated by tools/live_data.py: offsets in the level header (bank 3, $8000)"]
    off = 0
    for name, size in H_FIELDS:
        lines.append(f".define H_{name.upper():20s} {off}")
        off += size
    lines.append(f".define H_SIZE                 {HEADER_SIZE}")
    return "\n".join(lines) + "\n"


def c2p_tables():
    """Chunky-to-planar tables (rom/render.asm c2p_*_row): the chunky byte of
    a pixel holds only the nibble it shows (high when x + y is even), so two
    neighbours OR together into X. Page (parity*16 + plane*4 + pair) maps X
    to that plane's two bits for pixels 2*pair and 2*pair + 1.

    32 pages of 256 bytes (8 KB). Odd rows (parity 1) swap which nibble is
    the left pixel."""
    out = bytearray()
    for parity in (0, 1):
        for k in range(4):
            for j in range(4):
                for X in range(256):
                    a, b = (X >> 4, X & 15) if parity == 0 else (X & 15, X >> 4)   # colours of 2j, 2j+1
                    out.append((((a >> k) & 1) << (7 - 2 * j)) | (((b >> k) & 1) << (6 - 2 * j)))
    return bytes(out)


def unrolled_fill():
    """96 copies of the texture inner loop, twice (which copies keep the high
    nibble): entry at copy 96 - n draws n rows (rom/render.asm fill_column).

    Returns the text of unroll.inc: tfill_e / tfill_o (texture rows,
    masking the nibble the even or odd column keeps, alternating per row),
    tfill_f (low detail), zfill, ffill and afill (solid runs), plus the
    floor tables. In the texture loops HL is the 8.8 texture row (H the
    row), DE the step, BC the column's texels (C = row) and IY the
    chunky column buffer."""
    lines = ["; generated by tools/live_data.py"]
    for name, first in (("tfill_e", 0xF0), ("tfill_o", 0x0F)):
        lines.append(f"{name}:")
        for k in range(96):
            m = first if k % 2 == 0 else first ^ 0xFF
            lines += ["    ld c, h", "    ld a, (bc)", f"    and ${m:02X}", f"    ld (iy+{k}), a", "    add hl, de"]
        lines.append("    ret")
    # low detail: whole pair bytes (each column pair is one wide pixel)
    lines.append("tfill_f:")
    for k in range(96):
        lines += ["    ld c, h", "    ld a, (bc)", f"    ld (iy+{k}), a", "    add hl, de"]
    lines.append("    ret")
    # solid runs: zero (ceiling, colour 0 in either nibble), and the floor
    # pair's two nibbles alternating (a on even copies, c on odd)
    lines.append("zfill:")
    lines += ["    ld (hl), a", "    inc l"] * 96
    lines.append("    ret")
    lines.append("ffill:")                # floor rows: copied from floor_rows_*
    lines += ["    ldi"] * 96
    lines.append("    ret")
    lines += floor_tables()
    lines.append("afill:")
    for k in range(96):
        lines += [f"    ld (hl), {'a' if k % 2 == 0 else 'c'}", "    inc l"]
    lines.append("    ret")
    return "\n".join(lines) + "\n"


def preview(dirp, D, lvl_path, jar, n_lines):
    """Write preview PNGs into dirp: a few views from Model above the
    floating-point ViewRenderer's, and the whole screen with its border.

    D is main()'s data dict; n_lines the number of switchable lines (all
    drawn shut).
    """
    from view_render import ViewRenderer
    dirp.mkdir(parents=True, exist_ok=True)
    P = D["palette"]
    # (x, z, facing) views to show; ones not in the level are skipped
    shots = [(6, 2, 2), (6, 2, 1), (4, 5, 1), (18, 7, 2), (14, 13, 3), (13, 15, 0), (3, 20, 1)]
    for name in SIZES:
        m = Model(D, name)
        ref = ViewRenderer(lvl_path, jar, *SIZES[name])
        row = []
        for (x, z, f) in shots:
            if (x, z, f) not in D["pvs"]:
                continue
            img = m.render(x, z, f, [0] * n_lines)
            r = ref.render(x, z, f)
            row.append(np.concatenate([P[img], r], 0))
        # the screen: border + the first view
        scr = P[D["border_preview"][name]].copy()
        vr, vc = VIEW_POS[name]
        W, H = SIZES[name]
        scr[(vr - 2) * 8:(vr - 2) * 8 + H, vc * 8:vc * 8 + W] = row[0][:H]
        Image.fromarray(scr).resize((512, 256), Image.NEAREST).save(dirp / f"screen_{name}.png")
        sheet = np.concatenate([np.pad(r, ((0, 0), (0, 4), (0, 0))) for r in row], 1)
        Image.fromarray(sheet).resize((sheet.shape[1] * 2, sheet.shape[0] * 2), Image.NEAREST).save(dirp / f"live_{name}.png")
    print("previews in", dirp)


if __name__ == "__main__":
    main()
