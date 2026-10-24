#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The Game Boy Color's resources, made from the SNES game's data (the SNES
cartridge from its resources' bank on: the resources the SNES makes from
the original game, snes_ui_res.py's DEIMRES1, then the campaign's block).
The editor makes the same bytes the same way (editor/src/gbc.cpp): every
step in whole numbers, the first of equals always taken.

  "GBRES1", u16 number of entries, u32 the block's offset (from the SNES
  data's start: the Game Boy's ROM bank 64), then u32 per entry (from the
  start), the entries:
  0  font      96 glyphs (32..127), 8 rows, a byte a row: the 6 pixels in
               bits 6..1 (the SNES's 8x8 font, its columns 1..6)
  1  faces     9 faces of 5 x 5 tiles (40 x 40), row by row, Game Boy tiles
  2  face_pal  4 colours (BGR555, little-endian)
  3  title     the logo (the SNES title's, two thirds the size: 20 x 10
               tiles): u16 number of tiles, the tiles, the map (200 tile
               numbers), its attributes (palette 0..6), 7 palettes
  4  weapons   18 frames (weapon w, firing f: 2 w + f): u16 offset each
               (from the entry), then per frame: u8 sprites, u8 tiles, 8
               colours (two OBJ palettes: 0 unused, 3 colours), per sprite
               y, x (the view's pixels), tile, palette; the tiles (8 x 16)
  5  maps      per map 530 bytes: per 9-bit Mega Drive colour (b2..0 g2..0
               r2..0) its family * 4 + shade (1..3); then the three
               families' three shades (BGR555, little-endian)
  6  sound     from a ROM bank's start: "GBSN", u16 songs[3], u16 number of
               effects, u16 each (from "GBSN"); a song: 7 bytes (each
               voice's channel: 0, 1 square, 2 wave, 3 noise, 255 none),
               then the Mega Drive game's song (u8 loops, the stream); an effect:
               The Mega Drive game's stream (the SNES's spc_streams, as they are)

Usage: gb_res.py <snes.sfc> OUT.bin [preview dir]
(gb_pack.py calls build() directly; the preview dir gets gb_logo.png.)

The entries' consumers: src/gfx.c (font, faces, face_pal, title,
weapons, the maps' family colours), src/vdraw_map.c (the maps' tables),
src/snd.c (sound). The header and the offsets are big-endian; the
offsets in the header count from "GBRES1". Entries start on even offsets,
the sound on a 16 KB boundary (snd.c maps it at $4000).

Colour words: a Mega Drive colour is 0000 bbb0 ggg0 rrr0 (3 bits per
component, LEV the levels the SNES ports turn them into); a Game Boy
Color colour is BGR555 (0bbbbbgg gggrrrrr).
Licence: GPL-2.0-or-later.
"""
import struct
import sys
from pathlib import Path

LEV = [0, 52, 87, 116, 144, 172, 206, 255]   # a Mega Drive colour level (0..7) -> 8-bit


def md_rgb(c):
    """a Mega Drive colour word -> (r, g, b), 0..255"""
    return (LEV[(c >> 1) & 7], LEV[(c >> 5) & 7], LEV[(c >> 9) & 7])


def gb_colour(rgb):
    """(r, g, b) 0..255 -> a BGR555 word, rounded to the nearest"""
    r, g, b = ((v * 31 + 127) // 255 for v in rgb[:3])
    return r | (g << 5) | (b << 10)


def gb_pal(cols):
    """colours -> their BGR555 words, little-endian (as the palette ports take them)"""
    return b"".join(struct.pack("<H", gb_colour(c)) for c in cols)


def gb_tile(idx):
    """8 rows of 8 indices 0..3 -> 16 bytes"""
    out = bytearray()
    for y in range(8):
        lo = hi = 0
        for x in range(8):
            v = idx[y][x]
            lo |= (v & 1) << (7 - x)
            hi |= ((v >> 1) & 1) << (7 - x)
        out += bytes((lo, hi))
    return bytes(out)


def md_tile(t, at):
    """a Mega Drive tile (4 bits a pixel, left pixel in the high nibble; 32
    bytes at t[at:]) -> 8 rows of 8 indices"""
    return [[(t[at + y * 4 + x // 2] >> (4 if x % 2 == 0 else 0)) & 15 for x in range(8)] for y in range(8)]


def dist(a, b):
    """the squared RGB distance"""
    return (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2


def luma(c):
    """brightness, x100 (the 30 / 59 / 11 weights)"""
    return 30 * c[0] + 59 * c[1] + 11 * c[2]


def combos(n, k, start=0):
    """k of 0..n-1, in order"""
    if k == 0:
        yield ()
        return
    for i in range(start, n - k + 1):
        for rest in combos(n, k - 1, i + 1):
            yield (i,) + rest


def set_error(hist, D, c):
    """the cost of standing for histogram hist with colours c: each colour's
    count times its distance (D) to the nearest of c"""
    return sum(hist[i] * min(D[i][j] for j in c) for i in range(len(hist)) if hist[i])


def best_set(hist, D, k, cands):
    """the k colours of cands that stand for the histogram best"""
    best = None
    for p in combos(len(cands), k):
        c = tuple(cands[i] for i in p)
        e = set_error(hist, D, c)
        if best is None or e < best[0]:
            best = (e, c)
    return best[1]


def nearest(D, i, c):
    """the place in c of the colour nearest colour i"""
    best, at = None, 0
    for k, j in enumerate(c):
        if best is None or D[i][j] < best:
            best, at = D[i][j], k
    return at


# ------------------------------------------------------------ the SNES's data
class Snes:
    """The SNES ROM's data: its resources (snes_ui_res.py's DEIMRES1 entries,
    ent[i]) and the campaign's block, found through the header bytes
    $FFB7 / $FFB6 (their banks); data: from the resources to the last
    non-$FF byte, as gb_pack.py copies it."""

    def __init__(self, rom):
        self.res_at = (rom[0xFFB7] & 0x3F) * 0x10000   # (HiROM bank $Cx -> file offset)
        self.blk_at = (rom[0xFFB6] & 0x3F) * 0x10000
        end = len(rom)
        while end > self.res_at and rom[end - 1] == 0xFF:
            end -= 1
        self.data = rom[self.res_at:end]
        r = self.data
        assert r[:8] == b"DEIMRES1"
        n = struct.unpack_from("<H", r, 8)[0]
        self.ent = []
        for i in range(n):
            o, s = struct.unpack_from("<II", r, 12 + 8 * i)
            self.ent.append(r[o:o + s])
        self.block = rom[self.blk_at:end]

    def words(self, i):
        """entry i as big-endian u16 words"""
        b = self.ent[i]
        return [struct.unpack_from(">H", b, 2 * k)[0] for k in range(len(b) // 2)]


# snes_ui_res.py NAMES
E_FONT, E_FACES, E_ICONS, E_HUD_PAL, E_DLG_PAL, E_TITLE_TILES, E_TITLE_MAP, E_TITLE_PAL, E_SPC = range(9)


def font(s):
    """the SNES's (Mega Drive) font: 96 glyphs, a byte a row, colour-1 pixels
    of columns 1..6 set in bits 6..1"""
    t = s.ent[E_FONT]
    out = bytearray()
    for g in range(96):
        px = md_tile(t, 32 * g)
        for y in range(8):
            b = 0
            for x in range(1, 7):
                if px[y][x] == 1:
                    b |= 0x80 >> x
            out.append(b)
    return bytes(out)


def faces(s):
    """the 9 faces (5 x 5 Mega Drive tiles each, column by column) as Game
    Boy tiles (row by row), and their one 4-colour palette: black, then the
    used colours in three brightness groups (each the weighted mean)"""
    t = s.ent[E_FACES]
    pal = [md_rgb(c) for c in s.words(E_HUD_PAL)]
    imgs = []
    hist = [0] * 16
    for f in range(9):
        im = [[15] * 40 for _ in range(40)]
        for tx in range(5):
            for ty in range(5):                 # (the Mega Drive's tiles go column by column)
                px = md_tile(t, 32 * (f * 25 + tx * 5 + ty))
                for y in range(8):
                    for x in range(8):
                        im[ty * 8 + y][tx * 8 + x] = px[y][x]
        imgs.append(im)
        for row in im:
            for v in row:
                hist[v] += 1
    # the colours (1..14) in three groups by brightness, a third of the pixels each
    used = sorted((i for i in range(1, 15) if hist[i]), key=lambda i: (luma(pal[i]), i))
    total = sum(hist[i] for i in used)
    group = {}
    acc = 0
    for i in used:
        group[i] = min(2, acc * 3 // total)
        acc += hist[i]
    cols = [(0, 0, 0)]
    for g in range(3):
        m = [i for i in used if group[i] == g]
        w = sum(hist[i] for i in m) or 1
        cols.append(tuple(sum(hist[i] * pal[i][k] for i in m) // w for k in range(3)))
    tiles = bytearray()
    for im in imgs:
        idx = [[(group[v] + 1) if v in group else 0 for v in row] for row in im]
        for ty in range(5):
            for tx in range(5):
                tiles += gb_tile([r[tx * 8:tx * 8 + 8] for r in idx[ty * 8:ty * 8 + 8]])
    return bytes(tiles), gb_pal(cols)


# ------------------------------------------------------------ the title's logo
CROP_X, CROP_Y, CROP_W, CROP_H = 8, 8, 240, 120        # (the SNES title's moon and DEIMOS logo: -> 160 x 80)
# the SNES title's subtitle line ("A Doom RPG Engine", shadow included): too
# small to read at two thirds, so its rows are covered with the starry rows
# under it, and the Game Boy Color prints the subtitle in its own font
SUB_Y0, SUB_Y1 = 92, 110


def title(s):
    """the title's logo, cut from the SNES title screen and scaled to two
    thirds (160 x 80): its unique tiles, map, palette per tile and 7
    palettes (the entry's bytes), and a preview (rows of RGB)"""
    tiles, tmap = s.ent[E_TITLE_TILES], s.words(E_TITLE_MAP)
    pal = [md_rgb(c) for c in s.words(E_TITLE_PAL)]
    W, H = 256, 224
    src = [[0] * W for _ in range(H)]
    for ty in range(H // 8):
        for tx in range(W // 8):
            px = md_tile(tiles, 32 * (tmap[ty * 32 + tx] & 0x7FF))
            for y in range(8):
                for x in range(8):
                    src[ty * 8 + y][tx * 8 + x] = px[y][x]
    for y in range(SUB_Y0, SUB_Y1):                # (the subtitle's rows: the stars below instead)
        src[y] = list(src[y + SUB_Y1 - SUB_Y0])
    # two thirds: each pixel the colour that covers most of its 1.5 x 1.5
    # (weights 2 and 1 a side)
    wt = ([(0, 2), (1, 1)], [(1, 1), (2, 2)])

    def span(o):
        """output pixel o -> its two source pixels and their weights"""
        return [(3 * (o // 2) + d, w) for d, w in wt[o % 2]]
    out = []
    for oy in range(CROP_H * 2 // 3):
        row = []
        for ox in range(CROP_W * 2 // 3):
            cnt = [0] * 16
            for sy, wy in span(oy):
                for sx, wx in span(ox):
                    cnt[src[CROP_Y + sy][CROP_X + sx]] += wy * wx
            row.append(max(range(16), key=lambda i: (cnt[i], -i)))
        out.append(row)
    D = [[dist(pal[i], pal[j]) for j in range(16)] for i in range(16)]
    TW, TH = 20, 10
    hists = []
    for ty in range(TH):
        for tx in range(TW):
            h = [0] * 16
            for y in range(8):
                for x in range(8):
                    h[out[ty * 8 + y][tx * 8 + x]] += 1
            hists.append(h)
    # 7 palettes of 4 colours: the tiles grouped by their main colour first,
    # then (6 times) each palette the best 4 for its tiles, each tile the
    # palette that stands for it best
    main = [max(range(16), key=lambda i: (h[i], -i)) for h in hists]
    tally = [0] * 16
    for m in main:
        tally[m] += 1
    seeds = sorted(range(16), key=lambda i: (-tally[i], i))[:7]
    lab = [seeds.index(m) if m in seeds else min(range(7), key=lambda j: (D[m][seeds[j]], j)) for m in main]
    sets = [None] * 7
    cands = list(range(16))
    for _ in range(6):
        for j in range(7):
            h = [0] * 16
            for t, l in enumerate(lab):
                if l == j:
                    for i in range(16):
                        h[i] += hists[t][i]
            sets[j] = best_set(h, D, 4, cands) if sum(h) else (0, 1, 2, 3)
        lab = [min(range(7), key=lambda j: (set_error(h, D, sets[j]), j)) for h in hists]
    # the tiles in the chosen palettes; equal tiles stored once
    gtiles, seen, gmap, attr = [], {}, [], []
    for t in range(len(hists)):
        ty, tx = divmod(t, TW)
        c = sets[lab[t]]
        idx = [[nearest(D, out[ty * 8 + y][tx * 8 + x], c) for x in range(8)] for y in range(8)]
        b = gb_tile(idx)
        if b not in seen:
            seen[b] = len(gtiles)
            gtiles.append(b)
        gmap.append(seen[b])
        attr.append(lab[t])
    assert len(gtiles) <= 256
    pals = b"".join(gb_pal([pal[i] for i in c]) for c in sets)
    data = struct.pack(">H", len(gtiles)) + b"".join(gtiles) + bytes(gmap) + bytes(attr) + pals
    prev = []                                           # (the preview picture)
    for y in range(80):
        row = []
        for x in range(160):
            c = sets[lab[(y // 8) * TW + x // 8]]
            row.append(pal[c[nearest(D, out[y][x], c)]])
        prev.append(row)
    return data, prev


# ------------------------------------------------------------ the weapons
def weapons(blk):
    """the 18 weapon frames from the campaign block's weapon tables (the Mega Drive game's
    sprites, drawn on a canvas), at half size, cut into 8 x 16 sprites with
    two 3-colour palettes; the entry: u16 offsets, then each frame's record
    (see the module's docstring)"""
    u32 = lambda o: struct.unpack_from(">I", blk, o)[0]  # noqa: E731
    u16 = lambda o: struct.unpack_from(">H", blk, o)[0]  # noqa: E731
    wt = u32(16)                                         # (the block header's weapons table)
    tab = [u32(wt + 8 * k) for k in range(4)]           # frames, sprites, palettes, tiles
    recs = []
    for fid in range(18):
        f = tab[0] + fid * 10                           # (a frame: 10 bytes)
        t0, pal_i, sp_i, n = u16(f), u16(f + 4), u16(f + 6), u16(f + 8)    # (first tile, palette, first sprite, sprites)
        pal = [md_rgb(u16(tab[2] + pal_i * 32 + 2 * k)) for k in range(16)]
        canvas = {}
        for k in range(n):
            s = tab[1] + (sp_i + k) * 8                 # (a Mega Drive sprite: x, y, size, tile)
            x, y, sz, tile = u16(s), u16(s + 2), u16(s + 4), u16(s + 6)
            w8, h8 = ((sz >> 2) & 3) + 1, (sz & 3) + 1  # (its size in tiles; tiles go column by column)
            for cx in range(w8):
                for cy in range(h8):
                    px = md_tile(blk, tab[3] + (t0 + tile + cx * h8 + cy) * 32)
                    for r in range(8):
                        for c in range(8):
                            if px[r][c]:
                                canvas[(y + cy * 8 + r, x + cx * 8 + c)] = px[r][c]
        y0 = min(p[0] for p in canvas)
        y1 = max(p[0] for p in canvas) + 1
        x0 = min(p[1] for p in canvas)
        x1 = max(p[1] for p in canvas) + 1
        y0 -= (y0 + 16) % 2                              # (the SNES's view from y 8, at twice the size)
        x0 -= x0 % 2
        h, w = (y1 - y0 + 1) // 2, (x1 - x0 + 1) // 2
        # half size: each pixel the most common colour of its 2 x 2 (at least
        # two pixels drawn, else transparent)
        small = [[0] * w for _ in range(h)]
        for yy in range(h):
            for xx in range(w):
                cnt = [0] * 16
                for dy in range(2):
                    for dx in range(2):
                        v = canvas.get((y0 + 2 * yy + dy, x0 + 2 * xx + dx), 0)
                        if v:
                            cnt[v] += 1
                if sum(cnt) >= 2:
                    small[yy][xx] = max(range(1, 16), key=lambda i: (cnt[i], -i))
        gy0, gx0 = (y0 + 16) // 2 + 4, x0 // 2          # (where it starts in the Game Boy's view)
        D = [[dist(pal[i], pal[j]) for j in range(16)] for i in range(16)]
        # the sprites (8 x 16) and two palettes of 3: split by redness, then 4 rounds
        cells = []
        for sy in range(0, h, 16):
            for sx in range(0, w, 8):
                hst = [0] * 16
                for y in range(sy, min(h, sy + 16)):
                    for x in range(sx, min(w, sx + 8)):
                        hst[small[y][x]] += 1
                hst[0] = 0
                if sum(hst):
                    cells.append((sy, sx, hst))
        red = [sum(hst[i] * (pal[i][0] - pal[i][2]) for i in range(16)) * 16 // sum(hst) for _, _, hst in cells]
        # (// rounds down, negative numbers too: the editor's floordiv)
        order = sorted(range(len(cells)), key=lambda k: (red[k], k))
        lab = [0] * len(cells)
        for r, k in enumerate(order):
            lab[k] = 1 if 2 * r >= len(cells) else 0
        sets = [None, None]
        cands = list(range(1, 16))
        for _ in range(4):
            for j in range(2):
                hh = [0] * 16
                for k, (_, _, hst) in enumerate(cells):
                    if lab[k] == j:
                        for i in range(16):
                            hh[i] += hst[i]
                sets[j] = best_set(hh, D, 3, cands) if sum(hh) else (1, 2, 3)
            lab = [min(range(2), key=lambda j: (set_error(hst, D, sets[j]), j)) for _, _, hst in cells]
        spr, tl = [], bytearray()
        for k, (sy, sx, hst) in enumerate(cells):
            c = sets[lab[k]]
            idx = [[(nearest(D, small[y][x], c) + 1) if (y < h and x < w and small[y][x]) else 0
                    for x in range(sx, sx + 8)] for y in range(sy, sy + 16)]
            spr.append((gy0 + sy, gx0 + sx, len(tl) // 16, lab[k]))
            tl += gb_tile(idx[:8]) + gb_tile(idx[8:])
        assert len(tl) // 16 <= 128                     # (gfx.c's staging room: 128 tiles)
        pals = b"".join(gb_pal([(0, 0, 0)] + [pal[i] for i in c]) for c in sets)
        rec = bytes((len(spr), len(tl) // 16)) + pals
        for y, x, t, j in spr:
            rec += bytes((y & 255, x & 255, t, j))
        recs.append(rec + bytes(tl))
    head, body = bytearray(), bytearray()
    for r in recs:
        head += struct.pack(">H", 36 + len(body))     # (36: the 18 offsets themselves)
        body += r
    return bytes(head + body)


# ------------------------------------------------------------ the maps' colours
def chroma(c):
    """the colour less its brightness: its hue and saturation"""
    y = luma(c) // 100
    return (c[0] - y, c[1] - y, c[2] - y)


def families(C):
    """9 colours -> 3 groups of 3 (each holding its lowest member first in
    order) whose colours' hues lie closest"""
    ch = [chroma(c) for c in C]
    best = None
    for a in combos(9, 3):
        if a[0] != 0:
            continue
        rest = [i for i in range(9) if i not in a]
        for b in combos(6, 3):
            b2 = tuple(rest[i] for i in b)
            if b2[0] != rest[0]:
                continue
            c3 = tuple(i for i in rest if i not in b2)
            cost = 0
            for g in (a, b2, c3):
                cost += dist(ch[g[0]], ch[g[1]]) + dist(ch[g[0]], ch[g[2]]) + dist(ch[g[1]], ch[g[2]])
            if best is None or cost < best[0]:
                best = (cost, (a, b2, c3))
    return [sorted(g, key=lambda i: (luma(C[i]), i)) for g in best[1]]


def maps(blk):
    """per map: its colours (the textures' and sprites' palettes, weighted by
    their histograms) reduced to 9 (k-means), grouped in 3 families of 3
    shades (one Game Boy palette each); the 512-byte table gives each 9-bit
    Mega Drive colour its nearest one's family * 4 + shade, then the
    families' colours (18 bytes)"""
    u32 = lambda o: struct.unpack_from(">I", blk, o)[0]  # noqa: E731
    u16 = lambda o: struct.unpack_from(">H", blk, o)[0]  # noqa: E731
    out = bytearray()
    for m in range(u32(4) & 0xFF):
        pk = u32(32 + 32 * m)
        n_tex, n_spr = u16(pk + 10), u16(pk + 12)
        ent = lambda e: u32(pk + 32 + 8 * e)              # noqa: E731  (entry e's offset: 2 tex_pal, 3 tex_hist, 9 spr_pal, 10 spr_hist)
        w = {}
        for t in range(n_tex):
            for k in range(32):
                c = u16(ent(2) + 64 * t + 2 * k)
                w[c] = w.get(c, 0) + u16(ent(3) + 64 * t + 2 * k) + 1
        for s in range(n_spr):
            for k in range(16):
                c = u16(ent(9) + 32 * s + 2 * k)
                w[c] = w.get(c, 0) + u16(ent(10) + 32 * s + 2 * k) + 1
        cols = sorted(w)
        rgb = [md_rgb(c) for c in cols]
        wt = [w[c] for c in cols]
        n = len(cols)
        # 9 colours: the heaviest first, then each the farthest (by weight x
        # distance) from those chosen; then 8 rounds of weighted means
        C = [rgb[max(range(n), key=lambda i: (wt[i], -i))]]
        while len(C) < 9:
            i = max(range(n), key=lambda i: (wt[i] * min(dist(rgb[i], c) for c in C), -i))
            C.append(rgb[i])
        for _ in range(8):
            lab = [min(range(9), key=lambda j: (dist(p, C[j]), j)) for p in rgb]
            for j in range(9):
                tw = sum(wt[i] for i in range(n) if lab[i] == j)
                if tw:
                    C[j] = tuple(sum(wt[i] * rgb[i][k] for i in range(n) if lab[i] == j) // tw for k in range(3))
        fams = families(C)
        place = {}
        for f, g in enumerate(fams):
            for s, i in enumerate(g):
                place[i] = (f << 2) | (s + 1)
        table = bytearray(512)
        for i9 in range(512):                           # (i9: b2..0 g2..0 r2..0)
            p = md_rgb(((i9 & 7) << 1) | (((i9 >> 3) & 7) << 5) | (((i9 >> 6) & 7) << 9))
            table[i9] = place[min(range(9), key=lambda j: (dist(p, C[j]), j))]
        out += table + b"".join(gb_pal([C[i] for i in g]) for g in fams)
    return bytes(out)


# ------------------------------------------------------------ the sound
def song_map(stream):
    """a song's voice -> channel map (7 bytes): the three busiest of voices
    0..4 get a channel; the lowest of them (by mean note) the wave channel
    (2), the others the squares (0, 1); voices 5, 6 (the drums) the noise
    (3). stream starts with its loop byte"""
    notes = [[] for _ in range(7)]
    i = 1
    while i < len(stream):
        c = stream[i]
        if c == 0xFF:
            break
        if c < 0x80:                                    # (a wait)
            i += 1
        elif c < 0x88:                                  # (a note on: note, attenuation)
            notes[c & 7].append(stream[i + 1])
            i += 3
        elif c < 0x90:
            i += 1
        else:
            i += 2
    top = sorted((v for v in range(5) if notes[v]), key=lambda v: (-len(notes[v]), v))[:3]
    m = [255] * 7
    if top:
        bass = min(top, key=lambda v: (sum(notes[v]) * 1000 // len(notes[v]), v))
        m[bass] = 2
        ch = 0
        for v in top:
            if v != bass:
                m[v] = ch
                ch += 1
    m[5] = m[6] = 3
    return bytes(m)


def sound(s):
    """the SNES's spc_streams (ARAM addresses from $2000, little-endian) as
    the Game Boy's GBSN blob (offsets from its start, big-endian), each song
    with its voice map before it"""
    sp = s.ent[E_SPC]
    a = struct.unpack_from("<3HH", sp, 0)
    n = a[3]
    fx = list(struct.unpack_from(f"<{n}H", sp, 8))
    starts = [x - 0x2000 for x in list(a[:3]) + fx] + [len(sp)]
    head = 4 + 6 + 2 + 2 * n
    body = bytearray()
    at = []
    for k in range(len(starts) - 1):
        st = sp[starts[k]:starts[k + 1]]
        at.append(head + len(body))
        if k < 3:
            body += song_map(st)
        body += st
    return b"GBSN" + struct.pack(">3HH", *at[:3], n) + struct.pack(f">{n}H", *at[3:]) + bytes(body)


def build(rom):
    """(Snes data, the resources' bytes, the logo's preview) from the SNES ROM"""
    s = Snes(rom)
    ft, fp = faces(s)
    t, prev = title(s)
    ents = [font(s), ft, fp, t, weapons(s.block), maps(s.block), sound(s)]
    head = bytearray(b"GBRES1" + struct.pack(">HI", len(ents), s.blk_at - s.res_at) + bytes(4 * len(ents)))
    body = bytearray()
    for i, e in enumerate(ents):
        while (len(head) + len(body)) % (0x4000 if i == 6 else 2):    # (the sound on a bank boundary)
            body.append(0)
        struct.pack_into(">I", head, 12 + 4 * i, len(head) + len(body))
        body += e
    return s, bytes(head + body), prev


def main():
    """Command line: see the module's docstring."""
    rom = Path(sys.argv[1]).read_bytes()
    _, out, prev = build(rom)
    Path(sys.argv[2]).write_bytes(out)
    print(f"{sys.argv[2]}: {len(out)} bytes")
    if len(sys.argv) > 3:
        from PIL import Image
        im = Image.new("RGB", (160, 80))
        im.putdata([tuple(p) for row in prev for p in row])
        Path(sys.argv[3]).mkdir(parents=True, exist_ok=True)
        im.resize((480, 240), Image.NEAREST).save(Path(sys.argv[3]) / "gb_logo.png")


if __name__ == "__main__":
    main()
