#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Renders every (standable cell, facing) view of level 1 with view_render.py,
converts each to SMS tiles, and reports the numbers that decide the ROM
layout: identical-view dedupe, unique tiles per view, and the global tile
pool size. Writes analysis/views.pkl for pack_views.py.

Colour model per view:
  bank 0 (16): slot 0 = ceiling grey, slot 1 = floor grey, slots 2-15 =
               14 wall colours chosen per view by a greedy search over the
               SMS 64-colour space, minimising CIELAB error where every
               pixel may be reproduced either by one colour or by a 50/50
               2x2 ordered-dither pair of two colours that are at most one
               SMS level apart per channel (so dither partners are always
               similar, no red/teal speckle). The textures' blue-grey steel
               has no exact SMS colour, so pair-mixing is what keeps the
               tone right; a naive nearest-colour snap turned it teal.
               Ordered rather than error-diffused dither, so identical
               texture areas in different views still make identical,
               deduplicable tiles.
  Tiles that are pure ceiling or pure floor become the two shared solid
  tiles (so the big flat areas cost nothing).

Status: this is the 0.1.x pre-rendered-view pipeline (views stored as
pictures in the ROM). The 0.2 ROM draws its views live and build.sh no
longer runs this script, but other tools still import from it: SMS and
ok_pair (live_data.py, build_sprites.py) and quantize_view and tiles_of
(pack_views.py). The dither described above is, in quantize_view, a
checkerboard: pixel (x, y) takes the pair's first colour where x + y is
even and the second where it is odd. BAYER2 and DITHER_SPREAD are not
used any more.

Usage: gen_views.py <level.json> <level_sms.json> <jar_dir> <out.pkl>
(level_sms.json gives the grid: w, h and a cell type per cell.)
"""
import json
import pickle
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))
from view_render import ViewRenderer, VIEW_W, VIEW_H
from sms_gfx import rgb_to_sms, sms_to_rgb

FLOOR, DOOR = 1, 2              # cell types in level_sms.json's "cell" grid
TW, TH = VIEW_W // 8, VIEW_H // 8   # the view in 8x8 tiles
BAYER2 = np.array([[0, 2], [3, 1]], dtype=np.float32) / 4.0 - 0.375   # -0.375..0.375
DITHER_SPREAD = 48.0


def snap_sms(rgb):
    """An (r, g, b) colour moved to the nearest SMS colour (channels 0, 85, 170, 255)."""
    return sms_to_rgb(*rgb_to_sms(*rgb))


def choose_palette(wall_pixels: np.ndarray, n=14, exclude=()):
    """wall_pixels: Nx3 uint8. Picks the n most-used colours of the SMS
    64-colour space among the snapped wall pixels (the textures are already
    16-colour art, so counting snapped colours beats median-cutting in 24-bit
    and snapping afterwards, which merged shades into duplicates).

    exclude lists colours to skip. Returns up to n (r, g, b) tuples, most
    used first. (Not called by the current tools.)"""
    if len(wall_pixels) == 0:
        return []
    q = np.round(wall_pixels.astype(np.float32) / 255.0 * 3.0).astype(np.int64)
    keys = q[:, 0] * 16 + q[:, 1] * 4 + q[:, 2]       # SMS colour number 0..63 (r*16 + g*4 + b)
    cnt = np.bincount(keys, minlength=64)
    out = []
    for k in np.argsort(-cnt):
        if cnt[k] == 0:
            break
        c = sms_to_rgb(k // 16, (k // 4) % 4, k % 4)
        if c in exclude:
            continue
        out.append(c)
        if len(out) == n:
            break
    return out


def to_lab(rgb):
    """[..., 3] colours 0..255 -> float32 CIELAB (D65).

    Plain numpy float32: the bits can differ between machines, which is why
    the tools that must match the editor use colour_lab.py instead.
    """
    c=np.asarray(rgb,np.float32)/255.0
    c=np.where(c>0.04045,((c+0.055)/1.055)**2.4,c/12.92)
    M=np.array([[0.4124,0.3576,0.1805],[0.2126,0.7152,0.0722],[0.0193,0.1192,0.9505]],np.float32)
    xyz=c@M.T/np.array([0.95047,1.0,1.08883],np.float32)
    f=np.where(xyz>0.008856,np.cbrt(xyz),7.787*xyz+16/116)
    L=116*f[...,1]-16; a=500*(f[...,0]-f[...,1]); b=200*(f[...,1]-f[...,2])
    return np.stack([L,a,b],-1).astype(np.float32)
# the 64 Master System colours as float32 RGB, colour number r*16 + g*4 + b
SMS=np.array([sms_to_rgb(a,b,c) for a in range(4) for b in range(4) for c in range(4)],dtype=np.float32)
MAXSTEP=85.0                    # one SMS level per channel, in 0..255 units
# ok_pair(a, b): True if colours a and b may be dithered together: no
# channel differs by more than one SMS level
def ok_pair(a,b): return np.abs(a-b).max()<=MAXSTEP+0.5
def quantize_view(img, ceil, floor, n=14):
    """Returns (index_grid HxW uint8 0..15, palette16 list of RGB).

    img is an HxWx3 view picture; ceil and floor the RGB colours of its
    plain ceiling and floor pixels, which become palette entries 0 and 1
    (snapped to SMS colours). Then n more colours are chosen greedily: each
    round adds the SMS colour that most lowers the total squared Lab error
    of the remaining (wall) pixels, where a pixel may be matched by a
    single colour or by the 50/50 mix of an ok_pair. Finally every wall
    pixel gets its best single colour or pair, a pair being drawn as a
    checkerboard.
    """
    h,w,_=img.shape
    isc=np.all(img==np.array(ceil,np.uint8),axis=2); isf=np.all(img==np.array(floor,np.uint8),axis=2)
    mask=~(isc|isf); px=img[mask].astype(np.float32)     # the wall (and sprite) pixels
    fixed=[SMS[int(np.round(c[0]/85))*16+int(np.round(c[1]/85))*4+int(np.round(c[2]/85))] for c in (ceil,floor)]
    allsel=[f for f in fixed]
    if len(px)==0:
        # nothing but ceiling and floor: two colours, no search
        pal=[tuple(int(v) for v in c) for c in allsel]
        idx=np.zeros((h,w),np.uint8); idx[isf]=1; return idx,pal
    # work on the distinct colours, weighted by their pixel counts
    u,inv,cnt=np.unique(px,axis=0,return_inverse=True,return_counts=True)
    ulab=to_lab(u)
    def errs(C):
        """Per distinct pixel colour: the smallest squared Lab distance to any colour in C."""
        C=to_lab(np.array(C,np.float32)); return (((ulab[:,None,:]-C[None,:,:])**2).sum(2)).min(1)
    cur=errs(allsel)
    # greedy search over the 64 SMS colours, n rounds
    for step in range(n):
        best=None
        for i in range(64):
            c=SMS[i]
            if any(np.array_equal(c,s) for s in allsel): continue
            cands=[c]+[(c+s)/2 for s in allsel if ok_pair(c,s)]
            e=np.minimum(cur,errs(cands)); tot=(e*cnt).sum()
            if best is None or tot<best[0]: best=(tot,i,e)
        tot,i,e=best; allsel.append(SMS[i]); cur=e
    pal=[tuple(int(v) for v in c) for c in allsel]
    # every candidate: each palette colour alone (A = B = itself), then every
    # allowed pair's mix (A, B the two entries)
    P=np.array(allsel,np.float32); m=len(P)
    C=[p for p in P]; A=list(range(m)); B=list(range(m))
    for a in range(m):
        for b in range(a+1,m):
            if ok_pair(P[a],P[b]): C.append((P[a]+P[b])/2); A.append(a); B.append(b)
    C=np.array(C,np.float32); A=np.array(A); B=np.array(B)
    k=(((ulab[:,None,:]-to_lab(C)[None,:,:])**2).sum(2)).argmin(1)[inv]   # best candidate per pixel
    idx=np.zeros((h,w),np.uint8); idx[isf]=1
    ys,xs=np.nonzero(mask); par=(ys+xs)&1               # checkerboard parity
    idx[ys,xs]=np.where(par==0,A[k],B[k])
    return idx,pal


def tiles_of(idx: np.ndarray):
    """Returns list of TW*TH tile tuples (64 nibbles each), row-major."""
    out = []
    for ty in range(TH):
        for tx in range(TW):
            block = idx[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8]
            out.append(tuple(block.ravel().tolist()))
    return out


def main():
    """Render, quantise and deduplicate every view, print the statistics, write the pickle."""
    level_json, level_sms_json, jar_dir, out_pkl = map(Path, sys.argv[1:5])
    d = json.loads(level_sms_json.read_text())
    w, h, cell = d["w"], d["h"], d["cell"]
    r = ViewRenderer(level_json, jar_dir)
    # every floor cell, seen facing each of the 4 directions
    standable = [(x, z) for z in range(h) for x in range(w) if cell[z * w + x] == FLOOR]
    print(f"{len(standable)} standable cells -> {len(standable) * 4} views")

    views = {}          # (x,z,f) -> view_id
    view_data = []      # view_id -> dict(palette, tiles(list of tile keys), image_hash)
    image_index = {}    # hash of quantised view -> view_id
    tile_pool = {}      # (palette_tuple, tile_tuple) -> global tile id
    per_view_unique = []
    for n, (x, z) in enumerate(standable):
        for f in range(4):
            img = r.render(x, z, f)
            idx, pal = quantize_view(img, r.ceiling_rgb, r.floor_rgb)
            key = (tuple(pal), idx.tobytes())
            if key in image_index:             # the same picture as an earlier view
                views[(x, z, f)] = image_index[key]
                continue
            vid = len(view_data)
            image_index[key] = vid
            views[(x, z, f)] = vid
            tl = tiles_of(idx)
            uniq = set(tl)
            per_view_unique.append(len(uniq))
            pk = tuple(pal)
            # tiles are only shared between views with the same palette
            for t in uniq:
                tile_pool.setdefault((pk, t), len(tile_pool))
            view_data.append({"palette": pal, "tiles": tl, "cell": (x, z, f)})
        if n % 50 == 0:
            print(f"  {n}/{len(standable)} cells, {len(view_data)} distinct views, pool {len(tile_pool)} tiles")

    pu = np.array(per_view_unique)
    print(f"distinct views: {len(view_data)} of {len(views)}")
    print(f"unique tiles/view: min {pu.min()} median {int(np.median(pu))} mean {pu.mean():.1f} max {pu.max()}; "
          f">384: {(pu > 384).sum()}, >200: {(pu > 200).sum()}")
    print(f"global tile pool: {len(tile_pool)} tiles = {len(tile_pool) * 32 / 1024:.0f} KB raw")
    palettes = {v["palette"] and tuple(v["palette"]) for v in view_data}
    print(f"distinct palettes: {len(palettes)}")
    with open(out_pkl, "wb") as fp:
        pickle.dump({"views": views, "view_data": view_data, "w": w, "h": h,
                     "ceiling": r.ceiling_rgb, "floor": r.floor_rgb}, fp)
    print("wrote", out_pkl)


if __name__ == "__main__":
    main()
