#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Everything the live renderer needs for one level, as big-endian tables
in build/gen/pack/ (tools/md_pack.py puts them in the ROM's data pack,
where the editor can rewrite them):

  r_walls      every wall segment: ax, az, bx, bz (world units, 64 per
               cell), texture slot, door/line slot (0xFF = a plain wall)
  r_tex        wall textures, 64x64 4-bit colour indices, column-major
               (u * 64 + v), one byte per texel, + 16 Mega Drive colours
               and a colour histogram per texture (for the per-view palette)
  r_spr        sprites (monsters, items, props, decals) on their 64x64
               world canvas, stored as vertical spans: per column a start
               row and a length, then the texels; + colours + histogram
  r_scenery    the level's fixed props and decals
  r_def_spr    entity type -> sprite slot
  floor / ceiling colour

Colours: the jar's RGB565 -> the Mega Drive's 3 bits per channel, using
the console's measured DAC levels.

(The list above names the C variables of render_data.h; the tables
themselves are those of tables(), whose docstring gives each layout. A
texture has up to 32 colours, not 16. The campaign ROM no longer goes
through md_pack.py: tools/md_campaign.py calls tables() for each map and
puts the results in the campaign block.)

Terms: a "view" is what the player sees from one cell facing one way
(32 x 32 cells x 4 facings); "PVS" (potentially visible set) is the list
of wall pieces that can be seen in a view, worked out here so the 68000
only projects those.

Usage: build_render_data.py analysis/level01.json analysis/gamedata.json <jar dir> build/gen
Licence: GPL-2.0-or-later.
"""
import json
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from extract_textures import load_palettes, load_mappings, resolve_wall_id, extract_wall_texture
from sprite_art import SpriteArt

# the portal's textures (the walls of its line entities: eType 7 eSub 3,
# tile 339, and eSub 4, tile 338) -> theirs once it is closed; both fixed in
# the original's code. Shared with tools/source_container.py and the editor.
PORTAL_CLOSED_TEX = {34: 92, 33: 91}
MAX_SWAPS = 5  # the map pack header has room for 5 swap pairs

R_MAX_TEX = 48          # sizes of the renderer's per-texture / per-sprite tables in RAM
R_MAX_SPR = 64
# the Mega Drive's 8 output levels per colour channel (0..255 scale), as measured
MD_LEVELS = np.array([0, 52, 87, 116, 144, 172, 206, 255])


def md_level(v):
    """The Mega Drive level (0..7) nearest to the 8-bit channel value v."""
    return int(np.abs(MD_LEVELS - v).argmin())


def md_colour(rgb):
    """An RGB colour (0..255 channels) as a Mega Drive colour word:
    0000 BBB0 GGG0 RRR0 (3 bits per channel, the low bit of each nibble 0).
    """
    r, g, b = (md_level(c) for c in rgb[:3])
    return (b << 9) | (g << 5) | (r << 1)


def c_arr(name, ctype, vals, per=24, section=True):
    """A C array definition as text: `const ctype name[n] = {...};`, per
    values a line, in the .assets section unless section is False.
    """
    sec = ' __attribute__((section(".assets")))' if section else ""
    lines = [f"const {ctype} {name}[{len(vals)}]{sec} = {{"]
    for i in range(0, len(vals), per):
        lines.append("    " + ",".join(str(int(v)) for v in vals[i:i + per]) + ",")
    lines.append("};")
    return "\n".join(lines)


def hist16(idx, mask=None):
    """16 relative weights summing to ~255 (never 0 for a used colour).

    idx: an array of colour indices (0..15); mask: which of them count
    (the opaque pixels). The ROM uses these histograms to choose palettes.
    """
    v = idx[mask] if mask is not None else idx.ravel()
    h = np.bincount(v.ravel(), minlength=16)[:16].astype(np.float64)
    if h.sum() == 0:
        return [0] * 16
    w = h / h.sum() * 255
    return [max(1, int(round(x))) if c else 0 for x, c in zip(w, h)]


# the facing directions' x and z steps (0 north, 1 east, 2 south, 3 west), as game_model.DIRS
FX = [0, 1, 0, -1]
FZ = [-1, 0, 1, 0]


def visible_walls(W, removed, gx, gz, f, cols=512, want_depth=False):
    """Indices of walls that win at least one of `cols` columns (the
    reference projection, sampled at twice the screen resolution).

    W: one row per wall piece [ax, az, bx, bz, two_sided] (world units);
    removed: a mask of walls to ignore (open doors); (gx, gz, f): the cell
    and facing. The camera stands 16 units behind the cell centre and sees
    90 degrees: a point at depth d and sideways offset l falls in column
    128 + 128*l/d of 256. With want_depth, also returns the nearest wall
    depth per column (inf where no wall).
    """
    vx = gx * 64 + 32 - 16 * FX[f]  # the camera: the cell centre, 16 units back
    vz = gz * 64 + 32 - 16 * FZ[f]
    def view(x, z):
        """A world point relative to the camera, as (depth ahead, offset to the right) for facing f."""
        rx, rz = x - vx, z - vz
        return [(-rz, rx), (rx, rz), (rz, -rx), (-rx, -rz)][f]
    # one-sided walls face one way (Render_drawLines); two-sided ones are flipped
    front = (W[:, 0] - vx) * (W[:, 3] - W[:, 1]) - (W[:, 1] - vz) * (W[:, 2] - W[:, 0]) > 0
    W = W.copy()
    flip = ~front & (W[:, 4] == 1)
    W[flip, 0], W[flip, 2] = W[flip, 2].copy(), W[flip, 0].copy()
    W[flip, 1], W[flip, 3] = W[flip, 3].copy(), W[flip, 1].copy()
    da, la = view(W[:, 0], W[:, 1])
    db, lb = view(W[:, 2], W[:, 3])
    # keep walls that face the camera (or are two-sided), are not removed, are not wholly
    # behind it, and are not wholly outside the 90-degree cone on one side
    ok = (front | (W[:, 4] == 1)) & ~removed & ~((da < 1) & (db < 1)) & ~((la > da) & (lb > db)) & ~((la < -da) & (lb < -db))
    da, la, db, lb = da[ok].astype(float), la[ok].astype(float), db[ok].astype(float), lb[ok].astype(float)
    idx = np.flatnonzero(ok)
    # line through A, B in (depth, lat); depth along the ray of column x
    # each column's ray: lateral = k * depth, k from -1 to 1
    xs = (np.arange(cols) + 0.5) * (256.0 / cols)
    k = (xs - 128) / 128.0
    dd, dl = db - da, lb - la
    # intersection of the ray with the wall's line A + t(B - A): depth = c / num, t the
    # position along the wall (0..1 means the ray hits the wall itself)
    c = dl * da - dd * la
    num = dl[:, None] - k[None, :] * dd[:, None]
    with np.errstate(divide="ignore", invalid="ignore"):
        depth = c[:, None] / num
        t = (k[None, :] * da[:, None] - la[:, None]) / num
    good = (t >= -1e-6) & (t <= 1 + 1e-6) & (depth >= 1)
    depth = np.where(good, depth, np.inf)
    if not len(depth):
        return (set(), np.full(cols, np.inf)) if want_depth else set()
    win = depth.argmin(0)  # the nearest wall in each column
    hit = np.isfinite(depth.min(0))
    vis = set(int(idx[i]) for i in np.unique(win[hit]))
    if want_depth:
        return vis, depth.min(0) if len(depth) else np.full(cols, np.inf)
    return vis


def visible_cells(colz, gx, gz, f, cols=512):
    """1024-bit map of the cells whose billboard (a thing standing in the
    cell's centre) can show in at least one column of this view.

    colz: the nearest wall depth per column (from visible_walls). A thing
    counts as visible where it is in front of that wall (with 24 units of
    margin) over its screen width (80 units wide, wider than a sprite).
    """
    vx = gx * 64 + 32 - 16 * FX[f]  # the same camera
    vz = gz * 64 + 32 - 16 * FZ[f]
    bits = bytearray(128)
    for cz in range(32):
        for cx in range(32):
            rx, rz = cx * 64 + 32 - vx, cz * 64 + 32 - vz
            d, l = [(-rz, rx), (rx, rz), (rz, -rx), (-rx, -rz)][f]
            d -= 16  # (as the code has it: the thing's depth taken 16 units nearer)
            if d < 4 or l - 32 > d or l + 32 < -d:  # too close, or wholly outside the view
                continue
            a = int(np.floor((128 + (l - 40) * 128.0 / d) * cols / 256))  # its left and right screen columns
            b = int(np.ceil((128 + (l + 40) * 128.0 / d) * cols / 256))
            a, b = max(0, a), min(cols, b)
            if b > a and (colz[a:b] > d - 24).any():
                k = cz * 32 + cx
                bits[k >> 3] |= 1 << (k & 7)
    return bits


def tables(lvl, g, jar):
    """One map's render tables: {name: (struct format char, values)} plus
    'counts'. Things hidden at the start that are not entities are left out
    (only SHOW could bring them, and it only shows entities here).

    lvl: the parsed .bsp (bsp_parser); g: the map's game data (gamedata);
    jar: the directory of the jar's files. The tables:
      walls      8 shorts per wall piece: ax, az, bx, bz, texture slot, line
                 slot (255 = a plain wall), ua | two_sided << 8, ub (u: the
                 texture column at each end)
      tex        the textures, 4096 colour indices each, column-major (u*64+v)
      tex_pal    32 Mega Drive colours per texture; tex_hist their weights
      pvs_index  per (cell*4 + facing): the offset in pvs of its list
      pvs        lists: a count, then the walls that can be seen from there
                 ("potentially visible set"; entry 0 is an empty list)
      vis_index, vis  per view: which cells' things can show (1024-bit maps)
      spr_cols   per sprite and column: top row and length of the opaque span
      spr_off    each sprite's start in spr_data; spr_coff each column's start
                 (from the sprite's start); spr_data the span texels (255 =
                 transparent hole)
      spr_pal, spr_hist  16 colours per sprite and their weights
      scenery    5 shorts per fixed prop or decal: x, z, sprite slot, info >>
                 16 (its flags), cell (-1: fixed, else HIDE can remove it)
      def_spr    per entity definition: its sprite slot
    counts: n_walls, n_tex, n_spr, n_scenery, the floor and ceiling colours
    (Mega Drive words) and the portal's texture swaps (old slot, new slot).
    Raises SystemExit if a limit (R_MAX_TEX, R_MAX_SPR, colours) is broken.
    """
    g = dict(g)
    g["scenery"] = [s for s in g["scenery"] if not s["info"] & 0x10000]
    pal = load_palettes(jar / "palettes.bin")
    maps = load_mappings(jar / "mappings.bin")
    wtex = (jar / "wtexels.bin").read_bytes()
    art = SpriteArt(jar)

    # ---- walls: shifted like the original (Render.c: flags 256/512 move
    # door frames by 3 units), split into pieces of at most 64 units so a
    # texture repeats every 64 units (u runs 0..length, reversed by flag
    # 0x8000) and a wall decal covers exactly one piece ----
    segs = []        # [ax, az, bx, bz, tex_id, line, ua, ub, two_sided, wall index]
    line_of_wall = {l["line"]: k for k, l in enumerate(g["lines"])}
    for i, w in enumerate(lvl["walls"]):
        ax, az, bx, bz, fl = w["ax"], w["az"], w["bx"], w["bz"], w["flags"]
        if fl & 512:
            d = 3 if fl & 8 else (-3 if fl & 16 else 0)
            ax += d
            bx += d
        elif fl & 256:
            d = 3 if fl & 8 else (-3 if fl & 16 else 0)
            az += d
            bz += d
        L = max(abs(bx - ax), abs(bz - az))
        n = max(1, (L + 63) // 64)
        for k in range(n):
            t0, t1 = k * 64 / L, min((k + 1) * 64, L) / L
            pa = (round(ax + (bx - ax) * t0), round(az + (bz - az) * t0))
            pb = (round(ax + (bx - ax) * t1), round(az + (bz - az) * t1))
            u0, u1 = k * 64, min((k + 1) * 64, L)
            if fl & 0x8000:  # texture runs the other way
                u0, u1 = L - u0, L - u1
            base = 64 * (min(u0, u1) // 64)  # u relative to the piece's own 64-texel tile
            segs.append([pa[0], pa[1], pb[0], pb[1], w["tex"], line_of_wall.get(i, 255),
                         u0 - base, u1 - base, fl & 1, i])

    # ---- wall decals: baked into the texture of the piece they cover ----
    seg_at = {}
    for k, sg in enumerate(segs):
        seg_at[(sg[0], sg[1], sg[2], sg[3])] = (k, False)
        seg_at[(sg[2], sg[3], sg[0], sg[1])] = (k, True)
    baked = {}       # seg -> [(tile, reversed)]
    keep_scenery = []
    for sc in g["scenery"]:
        info = sc["info"]
        if info & 0x780000:  # a wall-mounted sprite (a decal): its side of the wall
            x, z = sc["x"], sc["z"]
            sgn = -1 if info & 0x2000000 else 1  # flipped
            if info & 0x80000:
                p0, p1 = (x - 32 * sgn, z), (x + 32 * sgn, z)
            elif info & 0x100000:
                p0, p1 = (x + 32 * sgn, z), (x - 32 * sgn, z)
            elif info & 0x400000:
                p0, p1 = (x, z - 32 * sgn), (x, z + 32 * sgn)
            else:
                p0, p1 = (x, z + 32 * sgn), (x, z - 32 * sgn)
            hit = seg_at.get(p0 + p1)
            if hit and not segs[hit[0]][8]:  # only on one-sided walls
                baked.setdefault(hit[0], []).append((sc["tile"], hit[1]))
                continue
        keep_scenery.append(sc)

    def texture_rgb(t):
        """Wall texture t from the jar as a 64x64x3 RGB array, indexed [v][u]."""
        tid = resolve_wall_id(maps, t)
        off, poff = maps["wall_table"][tid * 2], maps["wall_table"][tid * 2 + 1]
        grid = np.array(extract_wall_texture(wtex, off), dtype=np.uint8)     # [v][u]
        return np.array([pal[poff + k] for k in range(16)], np.uint8)[grid]  # [v][u][3]

    tex_key = {}
    tex_imgs = []
    for k, sg in enumerate(segs):
        key = (sg[4], tuple(baked.get(k, [])), sg[6], sg[7]) if k in baked else (sg[4],)  # a piece with decals gets its own copy of the texture
        if key not in tex_key:
            img = texture_rgb(sg[4]).copy()
            for tile, rev in baked.get(k, []):
                cv = art.canvas(tile)
                ua, ub = sg[6], sg[7]
                for c in range(64):
                    t = (c + 0.5) / 64
                    sp = 1 - t if rev else t
                    u = int(np.floor(ua + (ub - ua) * sp)) & 63  # the texture column under decal column c
                    a = cv[:, c, 3] > 0
                    img[a, u] = cv[a, c, :3]
            tex_key[key] = len(tex_imgs)
            tex_imgs.append(img)
        sg[4] = tex_key[key]
    # the Reactor's portal: once both power couplings are destroyed the
    # original gives the portal's lines texture 92 and its frame's 91
    # (Entity_powerCouplingDied). Those textures go after the walls' own;
    # 'swaps' says which slot becomes which
    swaps = []
    for key, slot in sorted(tex_key.items(), key=lambda kv: kv[1]):
        if len(key) == 1 and key[0] in PORTAL_CLOSED_TEX:
            swaps.append((slot, len(tex_imgs)))
            tex_imgs.append(texture_rgb(PORTAL_CLOSED_TEX[key[0]]))
    walls = segs
    wall_rows = []
    for sg in walls:
        wall_rows += [sg[0], sg[1], sg[2], sg[3], sg[4], sg[5], sg[6] | (sg[8] << 8), sg[7]]  # R_WALL_STRIDE = 8
    tex_px, tex_pal, tex_hist = [], [], []
    for img in tex_imgs:
        cols, idx = np.unique(img.reshape(-1, 3), axis=0, return_inverse=True)
        if len(cols) > 32:
            raise SystemExit(f"texture with {len(cols)} colours")
        idx = idx.reshape(64, 64).astype(np.uint8)                          # [v][u]
        tex_px += list(idx.T.ravel())                                        # u*64 + v
        tex_pal += [md_colour(c) for c in cols] + [0] * (32 - len(cols))  # up to 32 colours per texture
        h = np.bincount(idx.ravel(), minlength=32)[:32].astype(float)
        tex_hist += [max(1, int(round(x / h.sum() * 255))) if x else 0 for x in h]
    tex_ids = tex_imgs

    # ---- potentially visible walls per (cell, facing): doors all open, and
    # all closed (opening a door only ever removes an occluder) ----
    W = np.array([[w[0], w[1], w[2], w[3], w[8]] for w in walls], dtype=np.int64)
    door_walls = np.array([w[5] != 255 for w in walls])
    pvs_index = [0] * 4096
    pvs = [0]                        # entry 0 = an empty list
    total = 0
    vis_index = [0] * 4096
    vis_maps = [bytes([255]) * 128]   # map 0 = every cell (for cells you can't stand in)
    for gz in range(32):
        for gx in range(32):
            if g["wall"][gz * 32 + gx]:
                continue
            for f in range(4):
                vo, colz = visible_walls(W, door_walls, gx, gz, f, want_depth=True)
                v = vo | visible_walls(W, np.zeros(len(walls), bool), gx, gz, f)  # seen with doors open or closed
                vis_index[(gz * 32 + gx) * 4 + f] = len(vis_maps)
                vis_maps.append(bytes(visible_cells(colz, gx, gz, f)))
                pvs_index[(gz * 32 + gx) * 4 + f] = len(pvs)
                pvs.append(len(v))  # the list: its length, then the wall numbers
                pvs += sorted(v)
                total += len(v)
    if len(pvs) > 65535:
        raise SystemExit("visibility table too big")
    g["scenery"] = keep_scenery
    print(f"walls: {len(lvl['walls'])} -> {len(walls)} pieces; {sum(len(v) for v in baked.values())} decals baked "
          f"into {len(baked)} pieces, {sum(1 for s in keep_scenery if s['info'] & 0x780000)} left as decals")

    # ---- sprites: every tile the level can show ----
    tiles = sorted({d["tile"] for d in g["defs"]} | {s["tile"] for s in g["scenery"]})
    spr_slot = {t: i for i, t in enumerate(tiles)}
    spr_cols, spr_off, spr_data, spr_pal, spr_hist, spr_coff = [], [], [], [], [], []
    for t in tiles:
        cv = art.canvas(t)                               # 64x64 RGBA [row][col]
        alpha = cv[..., 3] > 0
        cols = np.unique(cv[alpha][:, :3], axis=0) if alpha.any() else np.zeros((0, 3), np.uint8)
        # map every pixel to the closest of its (<=16) colours
        if len(cols) > 16:
            raise SystemExit(f"sprite {t}: {len(cols)} colours")
        idx = np.zeros((64, 64), np.uint8)
        if len(cols):
            d = ((cv[..., None, :3].astype(int) - cols[None, None].astype(int)) ** 2).sum(-1)
            idx = d.argmin(-1).astype(np.uint8)
        p16 = [md_colour(c) for c in cols] + [0] * (16 - len(cols))
        spr_pal += p16
        spr_hist += hist16(idx, alpha)
        spr_off.append(len(spr_data))
        for c in range(64):
            rows = np.flatnonzero(alpha[:, c])
            spr_coff.append(len(spr_data) - spr_off[-1])
            if len(rows) == 0:
                spr_cols += [0, 0]
                continue
            top, bot = int(rows[0]), int(rows[-1]) + 1
            spr_cols += [top, bot - top]
            # transparent holes inside a span: colour 0xFF
            for r in range(top, bot):
                spr_data.append(int(idx[r, c]) if alpha[r, c] else 255)
    if len(spr_data) > 0xFFFFFF:
        raise SystemExit("sprite data too big")

    # ---- scenery (fixed props and decals) + entity types; the cell a prop
    # belongs to (-1: always there): S.static_hidden takes it away ----
    scen = []
    for s in g["scenery"]:
        scen += [s["x"], s["z"], spr_slot[s["tile"]], (s["info"] >> 16) & 0xFFFF, s.get("cell", -1)]
    def_spr = [spr_slot[d["tile"]] for d in g["defs"]]

    if len(tex_ids) > R_MAX_TEX or len(tiles) > R_MAX_SPR:
        raise SystemExit(f"{len(tex_ids)} textures / {len(tiles)} sprites: the limits are {R_MAX_TEX} / {R_MAX_SPR}")
    print(f"render data: {len(walls)} walls, {len(tex_ids)} textures ({len(tex_px) // 1024} KB), "
          f"{len(tiles)} sprites ({len(spr_data) // 1024} KB), {len(g['scenery'])} scenery; "
          f"visible walls per view: {total / max(1, sum(1 for v in pvs_index if v)):.1f} avg")
    return {"walls": ("h", wall_rows), "tex": ("B", tex_px), "tex_pal": ("H", tex_pal),
            "tex_hist": ("H", tex_hist), "spr_cols": ("B", spr_cols), "spr_off": ("I", spr_off),
            "spr_data": ("B", spr_data), "spr_coff": ("H", spr_coff),
            "pvs_index": ("H", pvs_index), "pvs": ("H", pvs), "spr_pal": ("H", spr_pal),
            "spr_hist": ("H", spr_hist), "scenery": ("h", scen), "def_spr": ("B", def_spr),
            "vis_index": ("H", vis_index), "vis": ("B", [b for m in vis_maps for b in m]),
            "counts": {"n_walls": len(walls), "n_tex": len(tex_ids), "n_spr": len(tiles),
                       "swaps": swaps[:MAX_SWAPS],
                       "n_scenery": len(g["scenery"]),
                       "floor": md_colour(g["floor_rgb"]), "ceiling": md_colour(g["ceiling_rgb"])}}


def write_common(out):
    """render_data.h / .c: what the code keeps (the sine table, the limits).

    Writes them into directory `out`. The header (generated code) declares
    the pointers src/pack.c sets to the current map's tables.
    """
    h = ["/* generated by tools/build_render_data.py - do not edit */",
         "#ifndef RENDER_DATA_H", "#define RENDER_DATA_H",
         "/* the level's tables are in the data pack (src/pack.c), found at start-up */",
         f"#define R_MAX_TEX {R_MAX_TEX}", f"#define R_MAX_SPR {R_MAX_SPR}",
         "#define R_WALL_STRIDE 8   /* ax az bx bz tex line ua|two_sided<<8 ub */",
         "extern unsigned short r_n_walls, r_n_tex, r_n_spr, r_n_scenery, r_floor_colour, r_ceiling_colour;",
         "extern const short *r_walls;",
         "extern const unsigned char *r_tex_addr[];  /* each texture: 4096 texels */",
         "extern const unsigned short *r_tex_pal, *r_tex_hist;   /* 32 per texture */",
         "extern const unsigned char *r_spr_cols;  /* 128 per sprite: top, len per column */",
         "extern const unsigned long *r_spr_off;",
         "extern const unsigned short *r_spr_coff;   /* 64 per sprite: column data offsets */",
         "extern const unsigned short *r_pvs_index, *r_pvs;  /* walls to draw per (cell*4 + facing) */",
         "extern const unsigned char *r_spr_data;",
         "extern const unsigned short *r_spr_pal, *r_spr_hist;",
         "extern const short *r_scenery;        /* x, z, sprite, info>>16, cell (-1: fixed) */",
         "#define R_SCENERY_STRIDE 5",
         "extern const unsigned char *r_def_spr;",
         "extern const short r_sin[1024];        /* sin, 1024 steps per turn, x 16384 */",
         "extern const unsigned short *r_vis_index;  /* (cell*4 + facing) -> word offset of a list; $FFFF all */",
         "extern const unsigned short *r_vis;        /* lists of cells whose things can show, $FFFF ends */",
         "#endif"]
    c = ['/* generated by tools/build_render_data.py - do not edit */', '#include "render_data.h"',
         c_arr("r_sin", "short", [round(16384 * np.sin(2 * np.pi * i / 1024)) for i in range(1024)], 16, False)]
    (out / "render_data.h").write_text("\n".join(h) + "\n")
    (out / "render_data.c").write_text("\n\n".join(c) + "\n")


def main():
    """One map's tables as files in <out>/pack (the single-map pack).

    build_render_data.py <level json> <gamedata json> <jar dir> <out>:
    writes <out>/pack/<table>.bin (big-endian), counts.json and the
    common render_data.h/.c. (md_campaign.py calls tables() directly.)
    """
    lvl_path, gd_path, jar, out = (Path(a) for a in sys.argv[1:5])
    t = tables(json.loads(lvl_path.read_text()), json.loads(gd_path.read_text()), jar)
    pk = out / "pack"
    pk.mkdir(parents=True, exist_ok=True)
    for name, (fmt, vals) in t.items():
        if name != "counts":
            (pk / f"{name}.bin").write_bytes(struct.pack(f">{len(vals)}{fmt}", *[int(v) for v in vals]))
    (pk / "counts.json").write_text(json.dumps(t["counts"]))
    write_common(out)


if __name__ == "__main__":
    main()
