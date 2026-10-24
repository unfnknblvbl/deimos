#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Renders every (cell, facing) view of Sector 1 with the pixel-faithful
offline renderer (view_render.py: walls + baked scenery), in every state of
the doors/secret walls that visibly change it, converts each picture to SMS
tiles with its own 16-colour palette, ZX0-compresses it and packs the lot
into 16KB ROM banks plus the lookup tables the game uses.

Inputs:  analysis/level01.json (walls), analysis/gamedata.json (switchable
         lines, scenery, which cells are reachable), the jar's art.
Outputs (into <gfx_dir>):
  view_bank_NN.bin   16KB banks of view blobs (first = FIRST_VIEW_BANK)
  view_banks.inc     WLA-DX include placing them
  view_index.bin     view id -> [bank u8][addr lo][addr hi]   (slot-2 address)
  view_cells.bin     one 16KB bank (paged into slot 2 at $8000):
                       u16[1024] per map cell: address of the cell's 4 records
                       (0 = no views: never standable)
                       record per facing 0..3:
                         u8 n            switchable lines that change this view (0..3)
                         u8 slot[n]      their line slots (gamedata "lines")
                         u16 vid[1<<n]   view for each open/closed combination,
                                         bit i of the index = line slot[i] open
  views_stats.txt

View blob layout:
  u8 nA, u8 nB      tiles in stream A / B (each <= HALF_TILES)
  u8[16]            bank-0 CRAM palette
  ZX0 stream        stream A tile patterns
  ZX0 stream        stream B tile patterns
  ZX0 stream        24x16 tilemap: 384 low bytes of the raw tile index, then
                    48 bytes of packed bit-8 flags
Two streams of at most 80 tiles keep the decompression buffer at 2560 bytes,
which is what leaves the game logic its RAM.

Status: the 0.1.x pre-rendered-view build. build.sh no longer runs it (the
0.2 ROM draws views live from live_data.py's data); it is kept for
reference and comparison.

Usage: pack_views.py <level.json> <gamedata.json> <jar_dir> <gfx_dir>
Rendering and packing run in a multiprocessing pool; each worker process
loads the renderer and game data once (_init) into _R and _G.
"""
import os, shutil
import json
import multiprocessing as mp
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from view_render import ViewRenderer, VIEW_W, VIEW_H
from sms_gfx import TileSet, rgb_to_sms, sms_cram_byte
import gen_views

TW, TH = VIEW_W // 8, VIEW_H // 8   # the view in tiles (24 x 16)
MAX_VIEW_TILES = 160          # one VRAM tile set: every view double-buffers
HALF_TILES = 80               # tiles per ZX0 stream
BANK_SIZE = 0x4000            # one Sega-mapper bank: 16 KB
FIRST_VIEW_BANK = 6
MAX_LINES_PER_VIEW = 3        # at most 2^3 = 8 door-state variants per view
VISIBLE_PIXELS = 300          # a door must change at least this many pixels to get its own variant
ZX0 = Path(os.environ.get("ZX0") or shutil.which("zx0") or "zx0")   # the compressor
DIRS = [(0, -1), (1, 0), (0, 1), (-1, 0)]     # (dx, dz) for facing N, E, S, W

_R = None                       # per worker: the ViewRenderer
_G = None                       # per worker: gamedata.json


def zx0_compress(data: bytes) -> bytes:
    """data compressed by the zx0 command-line tool (through temporary files)."""
    if not data:
        data = b"\x00"   # zx0 rejects empty input; the decoder writes 1 junk byte nobody uploads
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "in.bin"
        dst = Path(td) / "out.zx0"
        src.write_bytes(data)
        subprocess.run([str(ZX0), "-f", str(src), str(dst)], check=True, capture_output=True)
        return dst.read_bytes()


def merge_down_to(tiles_list, pal, limit):
    """Reduces a view to <= limit unique tiles so it always fits one VRAM
    tile set (no blank frame on any step). Greedy agglomerative merge on
    2x2-averaged real colour, weighted by the screen area of the rarer
    tile. Returns a remap dict tile->tile."""
    uniq = list(dict.fromkeys(tiles_list))
    if len(uniq) <= limit:
        return {}
    P = np.array(list(pal) + [(0, 0, 0)] * (16 - len(pal)), dtype=np.float32)
    X = np.array([P[np.array(t)].reshape(8, 8, 3) for t in uniq])
    B = ((X[:, 0::2, 0::2] + X[:, 1::2, 0::2] + X[:, 0::2, 1::2] + X[:, 1::2, 1::2]) / 4).reshape(len(uniq), -1)
    D = ((B[:, None, :] - B[None, :, :]) ** 2).sum(2)     # squared distance between tiles
    np.fill_diagonal(D, np.inf)
    w = np.zeros(len(uniq), dtype=np.float32)
    pos = {t: i for i, t in enumerate(uniq)}
    for t in tiles_list:
        w[pos[t]] += 1
    alive = np.ones(len(uniq), dtype=bool)
    parent = {}
    n_alive = len(uniq)
    # merge the cheapest pair (distance x the rarer tile's use) until few enough
    while n_alive > limit:
        cost = D * np.minimum(w[:, None], w[None, :])
        cost[~alive, :] = np.inf
        cost[:, ~alive] = np.inf
        i, j = np.unravel_index(int(cost.argmin()), cost.shape)
        drop, keep = (i, j) if w[i] < w[j] else (j, i)
        parent[drop] = keep
        w[keep] += w[drop]
        alive[drop] = False
        n_alive -= 1

    def root(k):
        """The tile that tile number k was finally merged into."""
        while k in parent:
            k = parent[k]
        return k
    return {uniq[k]: uniq[root(k)] for k in parent}


def pack_view(args):
    """Turn one quantised view into its ROM blob (layout in the module docstring).

    args is (index bytes VIEW_H x VIEW_W, palette). Returns (blob, number
    of unique tiles, number of tiles merged away to fit MAX_VIEW_TILES).
    """
    idx_bytes, pal = args
    idx = np.frombuffer(idx_bytes, dtype=np.uint8).reshape(VIEW_H, VIEW_W)
    tl = gen_views.tiles_of(idx)
    uniq = list(dict.fromkeys(tl))
    merged = 0
    if len(uniq) > MAX_VIEW_TILES:
        remap = merge_down_to(tl, pal, MAX_VIEW_TILES)
        tl = [remap.get(t, t) for t in tl]
        uniq = list(dict.fromkeys(tl))
        merged = len(remap)
    ts = TileSet()
    for t in uniq:
        ts.add_tile(t)
    patterns = ts.pack_tiles_binary()
    n = len(uniq)
    nA = min(n, HALF_TILES)
    nB = n - nA
    assert nB <= HALF_TILES
    tmap = bytearray()
    hi = bytearray(TW * TH // 8)                 # bit 8 of each tile index, 1 bit per map cell
    for k, t in enumerate(tl):
        i = ts.tile_index[t]
        tmap.append(i & 0xFF)
        if i & 0x100:
            hi[k >> 3] |= 1 << (k & 7)
    tmap += hi
    cram = bytes(sms_cram_byte(*rgb_to_sms(*c)) for c in pal)
    blob = bytes([nA, nB]) + cram + zx0_compress(patterns[:nA * 32]) + \
        zx0_compress(patterns[nA * 32:]) + zx0_compress(bytes(tmap))
    return blob, n, merged


def _init(level_json, jar_dir, gamedata):
    """Pool worker start-up: load the renderer and game data into _R and _G."""
    global _R, _G
    _R = ViewRenderer(Path(level_json), Path(jar_dir))
    _G = json.loads(Path(gamedata).read_text())


def render_cell(cell):
    """-> list of 4 (facing) entries: (line_slots, [(idx_bytes, pal) per variant])

    cell is (x, z); returns (cell, that list). For each facing, every
    switchable line in front and within 12 cells is tried removed; those
    that change at least VISIBLE_PIXELS pixels (the 3 biggest at most)
    become the view's line slots, and one picture is made for each
    combination of them open (removed) or closed.
    """
    x, z = cell
    r, g = _R, _G
    lines = g["lines"]
    scen = g["scenery"]
    all_walls = r.walls
    out = []
    for f in range(4):
        fx, fz = DIRS[f]
        base = r.render(x, z, f, walls=all_walls, sprites=scen)
        cand = []
        for slot, l in enumerate(lines):
            dx, dz = l["x"] - x, l["z"] - z
            if dx * fx + dz * fz < 0 or max(abs(dx), abs(dz)) > 12:   # behind or far
                continue
            walls = [w for i, w in enumerate(all_walls) if i != l["line"]]
            img = r.render(x, z, f, walls=walls, sprites=scen)
            diff = int(np.any(img != base, axis=2).sum())
            if diff >= VISIBLE_PIXELS:
                cand.append((diff, slot))
        cand.sort(reverse=True)
        slots = sorted(s for _d, s in cand[:MAX_LINES_PER_VIEW])
        variants = []
        for v in range(1 << len(slots)):
            removed = {lines[s]["line"] for i, s in enumerate(slots) if v >> i & 1}
            img = base if not removed else r.render(
                x, z, f, walls=[w for i, w in enumerate(all_walls) if i not in removed], sprites=scen)
            idx, pal = gen_views.quantize_view(img, r.ceiling_rgb, r.floor_rgb)
            variants.append((idx.tobytes(), tuple(pal)))
        out.append((slots, variants))
    return cell, out


def reachable(g):
    """Cells reachable from the spawn by 4-way steps over cells with no wall
    and no static blocker (a flood fill; doors count as passable), sorted
    by z then x."""
    w, s = g["wall"], g["static_block"]
    sx, sz, _ = g["spawn"]
    seen = {(sx, sz)}
    st = [(sx, sz)]
    while st:
        x, z = st.pop()
        for dx, dz in DIRS:
            n = (x + dx, z + dz)
            if 0 <= n[0] < 32 and 0 <= n[1] < 32 and n not in seen and not w[n[1] * 32 + n[0]] \
                    and not s[n[1] * 32 + n[0]]:
                seen.add(n)
                st.append(n)
    return sorted(seen, key=lambda c: (c[1], c[0]))


def main():
    """Render, pack and bank every view, write the tables and statistics."""
    level_json, gamedata, jar_dir, out_dir = sys.argv[1:5]
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    g = json.loads(Path(gamedata).read_text())
    cells = reachable(g)
    print(f"{len(cells)} reachable cells")

    keys = {}            # (idx bytes, pal) -> vid
    key_list = []
    per_cell = {}
    # render all cells in parallel; identical pictures share one view id
    with mp.Pool(initializer=_init, initargs=(level_json, jar_dir, gamedata)) as pool:
        for n, (cell, facings) in enumerate(pool.imap(render_cell, cells, chunksize=2)):
            recs = []
            for slots, variants in facings:
                vids = []
                for k in variants:
                    if k not in keys:
                        keys[k] = len(key_list)
                        key_list.append(k)
                    vids.append(keys[k])
                recs.append((slots, vids))
            per_cell[cell] = recs
            if n % 25 == 0:
                print(f"  rendered {n}/{len(cells)} cells, {len(key_list)} distinct views", flush=True)
        print(f"packing {len(key_list)} views", flush=True)
        packed = pool.map(pack_view, key_list, chunksize=8)

    blobs = [p[0] for p in packed]
    stats_n = np.array([p[1] for p in packed])
    merged_total = sum(p[2] for p in packed)

    # ---- banks ----
    banks, cur, index = [], bytearray(), bytearray()
    for blob in blobs:
        if len(cur) + len(blob) > BANK_SIZE:
            banks.append(bytes(cur))
            cur = bytearray()
        bank_no = FIRST_VIEW_BANK + len(banks)
        addr = 0x8000 + len(cur)
        index += bytes([bank_no, addr & 0xFF, addr >> 8])
        cur += blob
    if cur:
        banks.append(bytes(cur))

    # ---- cell records ----
    cellbank = bytearray(2048)                   # the u16[1024] cell table; records follow
    for z in range(32):
        for x in range(32):
            if (x, z) not in per_cell:
                continue
            addr = 0x8000 + len(cellbank)
            cellbank[(z * 32 + x) * 2:(z * 32 + x) * 2 + 2] = bytes([addr & 0xFF, addr >> 8])
            for slots, vids in per_cell[(x, z)]:
                cellbank.append(len(slots))
                cellbank += bytes(slots)
                for v in vids:
                    cellbank += bytes([v & 0xFF, v >> 8])
    assert len(cellbank) <= BANK_SIZE, len(cellbank)

    for old in out_dir.glob("view_bank_*.bin"):
        old.unlink()
    for i, b in enumerate(banks):
        (out_dir / f"view_bank_{FIRST_VIEW_BANK + i:03d}.bin").write_bytes(b)
    (out_dir / "view_index.bin").write_bytes(bytes(index))
    (out_dir / "view_cells.bin").write_bytes(bytes(cellbank))
    with open(out_dir / "view_banks.inc", "w") as fp:
        for i in range(len(banks)):
            bn = FIRST_VIEW_BANK + i
            fp.write(f".BANK {bn} SLOT 2\n.ORGA $8000\n.incbin \"build/gfx/view_bank_{bn:03d}.bin\"\n\n")
    nvar = sum(1 for recs in per_cell.values() for s, _v in recs if s)
    total = sum(map(len, blobs))
    stats = (f"views: {len(blobs)} distinct pictures for {len(per_cell) * 4} (cell, facing) slots; "
             f"{nvar} slots have door/secret-wall variants\n"
             f"unique tiles/view: min {stats_n.min()} median {int(np.median(stats_n))} max {stats_n.max()}, "
             f"views needing tile merge: {(stats_n >= MAX_VIEW_TILES).sum()} ({merged_total} tiles merged)\n"
             f"packed: {total / 1024:.0f} KB in {len(banks)} banks, avg {total / len(blobs):.0f} B/view; "
             f"banks {FIRST_VIEW_BANK}..{FIRST_VIEW_BANK + len(banks) - 1}; cell table {len(cellbank)} B\n")
    (out_dir / "views_stats.txt").write_text(stats)
    print(stats)
    assert FIRST_VIEW_BANK + len(banks) <= 256, "views do not fit a 4MB ROM"


if __name__ == "__main__":
    main()
