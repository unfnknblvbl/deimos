#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The Virtual Boy's view test: the 3D view, pixel for pixel, against the
build machine's renderer (test/view_host.c, through tools/vb_view_ref.c),
for many views - the picture (fb) and each eye's stereo picture on the
screen.

  vb_view_test.py <campaign.bin> [out dir]

It makes the view list (each map's start, facing each way, and four free
camera views between cells and between facings there), builds the view test cartridge
(./build.sh viewtest with it) and the build machine's reference program,
runs both, and compares for each view:
  - fb, the 128 x 88 picture in colour slots (the Virtual Boy's work RAM)
    with view_host's (its FB_OFF layout);
  - each eye's 256 x 176 view on the screen (the core's side-by-side
    picture, its red levels back to shades) with the picture this script
    works out from view_host's: each slot's and backdrop row's level (the
    luma table read from src/gfx.c), its 2 x 2 shading, and each eye's
    column map from the columns' depths - an implementation of
    src/gfx.c's rules written again here.
It prints the views that differ and a summary; with an out dir, it saves
the first views' screens there. Needs VB_CORE (vb_harness.py) and cc.
Licence: GPL-2.0-or-later.
"""
import re
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
VB_DIR = HERE.parent
ROOT = VB_DIR.parent
sys.path.insert(0, str(HERE))
from vb_harness import VB  # noqa: E402

VIEW_W, VIEW_H = 128, 88
FB_BYTES = 12288                       # (view.h: 128 x 88 in 96-row blocks)
NAMES_X0, VIEW_Y0 = 64, 8              # the view on the screen


def fb_off(x, y):
    """view.h's FB_OFF at 128 x 88: a pixel's offset in view_host's picture."""
    return ((x >> 3) * 3 + (y >> 5)) * 256 + (x & 7) * 32 + (y & 31)


def luma_table():
    """src/gfx.c's LUMA_LEVEL (64 levels), read from the source."""
    src = (VB_DIR / "src" / "gfx.c").read_text()
    m = re.search(r"LUMA_LEVEL\[64\] = \{([^}]*)\}", src)
    t = [int(v) for v in m.group(1).replace("\n", " ").split(",")]
    assert len(t) == 64
    return t


LUMA = luma_table()
BAYER = [[0, 2], [3, 1]]


def level15(c):
    """A BGR555 colour's level (0..12)."""
    r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31
    return LUMA[(77 * r + 150 * g + 29 * b) >> 7]


def shade_at(level, x, y):
    """The shade (0..3) of a level at screen pixel (x, y): a 2 x 2 ordered dither."""
    return (level >> 2) + (1 if (level & 3) > BAYER[y & 1][x & 1] else 0)


def backdrop_levels(floor_rgb, ceil_rgb):
    """Each of the 88 view rows' backdrop level: the SNES's shading at line 2 r."""
    def rgb15(c):
        return (c[0] >> 3) | ((c[1] >> 3) << 5) | ((c[2] >> 3) << 10)
    fl, ce = rgb15(floor_rgb), rgb15(ceil_rgb)
    out = []
    for r in range(VIEW_H):
        line = 2 * r
        t = ((87 - line) * 2 + 1) * 256 // 176 if line < 88 else ((line - 88) * 2 + 1) * 256 // 176
        f = 77 + ((t * 230) >> 8)
        c = ce if line < 88 else fl
        o = 0
        for k in range(3):
            v = (((c >> (5 * k)) & 31) * f) >> 8
            o |= min(v, 31) << (5 * k)
        out.append(level15(o))
    return out


def depth_of(h):
    """A column's depth (1 near .. 8 far) from its nearest thing's height in pixels."""
    return 1 if h >= 128 else 1 + (((128 - h) * 7 + 64) >> 7)


def stereo_map(depth, eye):
    """Eye's screen column -> view column (None: black), as src/gfx.c's stereo_map."""
    src, dep = [None] * 256, [255] * 256
    for s in range(VIEW_W):
        d = depth[s]
        p = 2 * s + (d - d // 2 if eye else -(d // 2))
        for x in (p, p + 1):
            if 0 <= x < 256 and d <= dep[x]:
                src[x], dep[x] = s, d
    covered = [x for x in range(256) if src[x] is not None]
    for x in range(256):
        if src[x] is not None:
            continue
        right = next((c for c in covered if c > x), None)
        if x == 0 or src[x - 1] is None or right is None:
            continue
        pick = right if dep[right] > dep[x - 1] else x - 1
        src[x], dep[x] = src[pick], dep[pick]
    return src


def expected_eye(pic, cgram, col_h, spr_h, floor_rgb, ceil_rgb, eye):
    """The view's 256 x 176 shades on eye's screen, worked out from the reference."""
    bg = backdrop_levels(floor_rgb, ceil_rgb)
    lev = np.zeros((VIEW_H, VIEW_W), int)
    for x in range(VIEW_W):
        for y in range(VIEW_H):
            p = pic[y][x]
            lev[y, x] = bg[y] if p == 0 else level15(cgram[p - 32])
    depth = [depth_of(max(col_h[s], spr_h[s])) for s in range(VIEW_W)]
    m = stereo_map(depth, eye)
    out = np.zeros((176, 256), int)
    for X in range(256):
        if m[X] is None:
            continue
        for Y in range(176):
            out[Y, X] = shade_at(lev[Y // 2, m[X]], X + NAMES_X0, Y + VIEW_Y0)
    return out


def views(spawns):
    """The view list: per map its start facing each way, and four free-camera views (half and a
    quarter of a step ahead; half of a quarter turn, and a quarter of one the other way)."""
    out = []
    for m, x, z, f in spawns:
        for k in range(4):
            out.append((m, x, z, k, 0, 0, 0, 0))
        cx, cz = x * 64 + 32, z * 64 + 32
        dx, dz = [(0, -32), (32, 0), (0, 32), (-32, 0)][f]     # (half a step ahead)
        out.append((m, x, z, f, 1, (f * 64) & 255, cx + dx, cz + dz))
        out.append((m, x, z, f, 1, (f * 64) & 255, cx + dx // 2, cz + dz // 2))   # (a quarter step)
        out.append((m, x, z, f, 1, (f * 64 + 32) & 255, cx, cz))           # (half a turn)
        out.append((m, x, z, f, 1, (f * 64 - 16) & 255, cx, cz))           # (a quarter of a turn step, the other way)
    return out


def pack(vs):
    """The list in src/viewtest.c's format."""
    b = bytearray(b"VIEW" + struct.pack(">H", len(vs)))
    for m, x, z, f, cam, a, cx, cz in vs:
        b += bytes([m, x, z, f, cam, a]) + struct.pack(">hh", cx, cz)
    return bytes(b)


def main():
    """Run the test (see the module docstring)."""
    camp = sys.argv[1]
    out_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else None
    obj = VB_DIR / "obj"
    obj.mkdir(exist_ok=True)
    ref = obj / "vb_view_ref"
    subprocess.run(["cc", "-O2", "-std=gnu99", "-w", "-DVIEW_FX", "-o", str(ref), str(HERE / "vb_view_ref.c"),
                    str(ROOT / "view/view.c"), str(ROOT / "core/rules.c"), str(ROOT / "core/gen/data.c")], check=True)
    spawns = [tuple(map(int, l.split())) for l in
              subprocess.run([str(ref), camp, "spawns"], check=True, capture_output=True, text=True).stdout.split("\n") if l]
    vs = views(spawns)
    (obj / "views.bin").write_bytes(pack(vs))
    subprocess.run([str(ref), camp, str(obj / "views.bin"), str(obj / "views_ref.bin")], check=True)
    subprocess.run(["./build.sh", "viewtest"], cwd=VB_DIR, check=True, env={**__import__("os").environ,
                                                                         "SCRIPT": str(obj / "views.bin")})
    refs = (obj / "views_ref.bin").read_bytes()
    rec = FB_BYTES + 2 * 192 + 4 * VIEW_W + 6
    vb = VB(str(VB_DIR / "out/deimos_vb_view.vb"), str(obj / "vb.syms"))
    bad_fb = bad_eye = 0
    for i, v in enumerate(vs):
        while vb.u16("vt_done") != i + 1:
            vb.run_frames(1)
        r = refs[i * rec:(i + 1) * rec]
        pic = [[r[fb_off(x, y)] for x in range(VIEW_W)] for y in range(VIEW_H)]
        cgram = struct.unpack_from("<192H", r, FB_BYTES)
        col_h = struct.unpack_from(f"<{VIEW_W}H", r, FB_BYTES + 384)
        spr_h = struct.unpack_from(f"<{VIEW_W}H", r, FB_BYTES + 384 + 2 * VIEW_W)
        floor_rgb, ceil_rgb = r[-6:-3], r[-3:]
        got = vb.mem(vb.sym["fb"], VIEW_W * VIEW_H)
        ok_fb = all(got[x * VIEW_H + y] == pic[y][x] for x in range(VIEW_W) for y in range(VIEW_H))
        eyes = vb.eyes()
        diffs = []
        for e in (0, 1):
            want = expected_eye(pic, cgram, col_h, spr_h, floor_rgb, ceil_rgb, e)
            have = eyes[e][VIEW_Y0:VIEW_Y0 + 176, NAMES_X0:NAMES_X0 + 256]
            diffs.append(int((want != have).sum()))
        if not ok_fb:
            bad_fb += 1
        if any(diffs):
            bad_eye += 1
        if not ok_fb or any(diffs):
            print(f"view {i} {v}: fb {'ok' if ok_fb else 'DIFFERS'}, eyes differ in {diffs} pixels")
        if out_dir and i < 12:
            out_dir.mkdir(parents=True, exist_ok=True)
            vb.screenshot().save(out_dir / f"view_{i:02d}.png")
        vb.poke_mem(vb.sym["vt_go"], struct.pack("<H", i + 1))
    print(f"{len(vs)} views ({len(spawns)} maps): fb differs in {bad_fb}, the eyes in {bad_eye}")


if __name__ == "__main__":
    main()
