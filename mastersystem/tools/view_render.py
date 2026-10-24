#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Offline reference renderer: draws the Doom RPG level-1 first-person view for
a given (cell, facing) the way the original J2ME engine does, so the SMS
build can be generated from pixel-faithful frames instead of a hand-made
approximation.

The projection and camera constants were taken from the J2ME decompile
(class r, which the GPL DoomRPG-RE reimplementation's Render.c matches
line for line):
  - camera sits 16 units BEHIND the cell centre along the facing
    (Render_render: vx = viewx - 16*cos, vy = viewy + 16*sin)
  - eye height viewZ = 36, walls run z=0..64 (Game.c: viewZ = 36)
  - focal length = screenWidth/2 -> 90 degree horizontal FOV
    (projectVertex: sx = halfW + lateral * W / (2*depth))
  - wall screen height = 32*W/depth, top = halfH - (64-36)*W/(2*depth)
  - textures are 64 texels across one 64-unit wall, perspective-correct u
  - ceiling/floor are SOLID colours from the .bsp header (r.h() fills the
    top half with one colour row and the bottom half with the other); the
    J2ME build has no distance shading or fog at all.
Angles: 256 units per turn, forward = (cos a, -sin a). Facing 0=N is a=64,
1=E a=0, 2=S a=128, 3=W a=192 (matches dx/dz tables in main.asm).

Coordinates: world x and z (the .bsp's), 64 units per grid cell; in this
file the z axis is called y in the camera maths. A wall is a dict from
bsp_parser.py (ax, az, bx, bz, tex). The renderer is floating point and not
bit-exact: live_data.py has the integer model of the ROM's renderer and
uses this one only as a visual reference (its --preview). LevelTextures is
also used by pack_source.py to check the editor's texture source; the
pre-rendered-view tools (gen_views.py, pack_views.py) render with
ViewRenderer.

Usage: view_render.py <level.json> <jar_dir> <out.png> x z facing
"""
import json
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))
from extract_textures import load_palettes, load_mappings, resolve_wall_id, extract_wall_texture

VIEW_W, VIEW_H = 192, 128       # default picture size (the 0.1.x view)
EYE_Z = 36                      # eye height above the floor, world units
WALL_H = 64                     # wall height, world units
CAM_BACK = 16                   # the camera sits this far behind the cell centre
FACING_ANGLE_DEG = {0: 90.0, 1: 0.0, 2: 270.0, 3: 180.0}   # angle a in degrees, forward=(cos a, -sin a)


class LevelTextures:
    """The level's wall textures from the jar, decoded on demand and cached."""

    def __init__(self, jar_dir: Path):
        """Load the palettes, mappings and wall texels from jar_dir."""
        self.palette = load_palettes(jar_dir / "palettes.bin")
        self.mappings = load_mappings(jar_dir / "mappings.bin")
        self.wtexels = (jar_dir / "wtexels.bin").read_bytes()
        self.cache = {}

    def texture_rgb(self, raw_tex_id: int) -> np.ndarray:
        """64x64x3 uint8, [v][u]: wall texture raw_tex_id (a .bsp texture id) in RGB."""
        if raw_tex_id in self.cache:
            return self.cache[raw_tex_id]
        tid = resolve_wall_id(self.mappings, raw_tex_id)
        file_off = self.mappings["wall_table"][tid * 2]
        pal_off = self.mappings["wall_table"][tid * 2 + 1]
        grid = extract_wall_texture(self.wtexels, file_off)
        pal = np.array(self.palette[pal_off:pal_off + 16], dtype=np.uint8)
        arr = pal[np.array(grid, dtype=np.int32)]
        self.cache[raw_tex_id] = arr
        return arr


class ViewRenderer:
    """Draws first-person views of one level (walls, then optional scenery)."""

    def __init__(self, level_json: Path, jar_dir: Path, w=VIEW_W, h=VIEW_H):
        """Load the level's walls and colours from level_json (bsp_parser.py's
        output) and the textures from jar_dir; w x h is the picture size."""
        d = json.loads(level_json.read_text())
        self.walls = d["walls"]
        hdr = d["header"]
        # r.h(): top half <- colour "d", bottom half <- colour "e"
        self.ceiling_rgb = tuple(hdr["color_d"])
        self.floor_rgb = tuple(hdr["color_e"])
        self.tex = LevelTextures(jar_dir)
        self.w, self.h = w, h
        self.f = w / 2.0                # focal length in pixels (90 degree field of view)
        self.half_w, self.half_h = w / 2.0, h / 2.0

    def camera(self, gx, gz, facing):
        """(vx, vy, fx, fy): the eye position in world units and the unit forward vector for cell (gx, gz) and facing."""
        a = math.radians(FACING_ANGLE_DEG[facing])
        fx, fy = math.cos(a), -math.sin(a)          # forward
        cx, cy = gx * 64 + 32, gz * 64 + 32
        vx, vy = cx - CAM_BACK * fx, cy - CAM_BACK * fy
        return vx, vy, fx, fy

    def render(self, gx, gz, facing, texture_override=None, walls=None, sprites=None) -> np.ndarray:
        """Returns HxWx3 uint8 image. walls: list of wall dicts (default: all
        of the level's); sprites: scenery to draw (see draw_sprites)."""
        w, h = self.w, self.h
        if walls is None:
            walls = self.walls
        vx, vy, fx, fy = self.camera(gx, gz, facing)
        rx, ry = -fy, fx                             # right vector (screen x increases)
        img = np.zeros((h, w, 3), dtype=np.uint8)
        img[: h // 2] = self.ceiling_rgb
        img[h // 2:] = self.floor_rgb
        # per screen column: the nearest wall's depth, texture, u and depth
        zbuf = np.full(w, np.inf)
        col_tex = np.full(w, -1, dtype=np.int32)
        col_u = np.zeros(w, dtype=np.int32)
        col_depth = np.zeros(w)

        near = 1.0                      # near clipping plane, world units
        for wall in walls:
            ax, ay, bx, by = wall["ax"], wall["az"], wall["bx"], wall["bz"]
            # view space: depth along forward, lateral along right
            da = (ax - vx) * fx + (ay - vy) * fy
            la = (ax - vx) * rx + (ay - vy) * ry
            db = (bx - vx) * fx + (by - vy) * fy
            lb = (bx - vx) * rx + (by - vy) * ry
            ua, ub = 0.0, 64.0
            if da < near and db < near:
                continue
            # clip against near plane
            if da < near:
                t = (near - da) / (db - da)
                da, la, ua = near, la + (lb - la) * t, ua + (ub - ua) * t
            elif db < near:
                t = (near - db) / (da - db)
                db, lb, ub = near, lb + (la - lb) * t, ub + (ua - ub) * t
            sxa = self.half_w + la * self.f / da
            sxb = self.half_w + lb * self.f / db
            if sxa > sxb:               # make a the left end
                sxa, sxb, da, db, ua, ub = sxb, sxa, db, da, ub, ua
            x0 = max(0, int(math.ceil(sxa)))
            x1 = min(w, int(math.ceil(sxb)))
            if x1 <= x0:
                continue
            # perspective-correct interpolation in 1/z
            iza, izb = 1.0 / da, 1.0 / db
            uza, uzb = ua * iza, ub * izb
            span = sxb - sxa
            xs = np.arange(x0, x1)
            t = (xs + 0.0 - sxa) / span
            iz = iza + (izb - iza) * t
            uz = uza + (uzb - uza) * t
            depth = 1.0 / iz
            u = (uz / iz).astype(np.int32) & 63     # texture column 0..63
            closer = depth < zbuf[x0:x1]
            idx = xs[closer]
            zbuf[idx] = depth[closer]
            col_tex[idx] = wall["tex"]
            col_u[idx] = u[closer]
            col_depth[idx] = depth[closer]

        # rasterise the winning column per screen x
        for x in range(w):
            t = col_tex[x]
            if t < 0:
                continue
            d = col_depth[x]
            scale = self.f / d                       # pixels per world unit at this depth
            top = self.half_h - (WALL_H - EYE_Z) * scale
            bot = self.half_h + EYE_Z * scale
            y0 = max(0, int(math.ceil(top)))
            y1 = min(h, int(math.ceil(bot)))
            if y1 <= y0:
                continue
            tex = self.tex.texture_rgb(t if texture_override is None else texture_override)
            ys = np.arange(y0, y1)
            v = (((ys + 0.5) - top) / (bot - top) * 64).astype(np.int32)   # texture row at pixel centres
            v = np.clip(v, 0, 63)
            img[y0:y1, x] = tex[v, col_u[x]]
        if sprites:
            self.draw_sprites(img, zbuf, vx, vy, fx, fy, sprites)
        return img

    # ------------------------------------------------------------ sprites
    def _sample_column(self, img, x, d, canvas, c, additive):
        """Draws canvas column c at screen column x for a sprite at depth d.

        img is modified in place. For each screen row the world height z it
        shows at depth d is found, giving canvas row 64 - z; transparent
        texels are skipped. additive: add the colour (saturating) instead
        of replacing it.
        """
        if not 0 <= c < 64:
            return
        h = self.h
        ys = np.arange(h)
        z = EYE_Z + (self.half_h - (ys + 0.5)) * d / self.f
        r = np.floor(64 - z).astype(np.int32)
        ok = (r >= 0) & (r < 64)
        ys, r = ys[ok], r[ok]
        px = canvas[r, c]
        on = px[:, 3] > 0
        ys, px = ys[on], px[on, :3]
        if additive:
            img[ys, x] = np.minimum(255, img[ys, x].astype(np.int32) + px).astype(np.uint8)
        else:
            img[ys, x] = px

    def draw_sprites(self, img, zbuf, vx, vy, fx, fy, sprites):
        """Scenery, as the original draws it (Render_renderSprite):
          - billboards (no orientation flag): face the camera, drawn 16 units
            nearer than they are (vert1.x -= 16), skipped closer than 4
          - decals (info & 0x780000): a 64-unit quad lying along a wall
            (0x80000/0x100000 along x, 0x400000/0x200000 along z; 0x2000000
            mirrors), at its true depth; they win depth ties with the wall
        Painter's order far->near, each column tested against the walls.

        img is drawn into in place; zbuf holds the walls' depth per column;
        (vx, vy) and (fx, fy) are the camera from camera(); sprites are
        gamedata scenery dicts (tile, info, x, z)."""
        from sprite_art import default_art
        art = default_art()
        rx, ry = -fy, fx
        items = []
        for s in sprites:
            t, info = s["tile"], s["info"]
            # (the original also adds an additive glow sprite to lamps
            # 135/140/131; at SMS resolution it read as a stray orange bar,
            # so the port leaves it out)
            layers = [(t, False)]
            for tile, add in layers:
                x, z = s["x"], s["z"]
                if info & 0x780000:
                    # a decal: its two end points, 32 units either side
                    sgn = -1 if info & 0x2000000 else 1
                    if info & 0x80000:
                        p0, p1 = (x - 32 * sgn, z), (x + 32 * sgn, z)
                    elif info & 0x100000:
                        p0, p1 = (x + 32 * sgn, z), (x - 32 * sgn, z)
                    elif info & 0x400000:
                        p0, p1 = (x, z - 32 * sgn), (x, z + 32 * sgn)
                    else:
                        p0, p1 = (x, z + 32 * sgn), (x, z - 32 * sgn)
                    dm = ((x - vx) * fx + (z - vy) * fy)
                    items.append((dm, "decal", tile, add, (p0, p1)))
                else:
                    d = (x - vx) * fx + (z - vy) * fy - 16
                    if d < 4:
                        continue
                    l = (x - vx) * rx + (z - vy) * ry
                    items.append((d, "bill", tile, add, (d, l)))
        items.sort(key=lambda it: -it[0])      # farthest first
        for _dm, kind, tile, add, geo in items:
            canvas = art.canvas(tile)
            if kind == "bill":
                d, l = geo
                sx0 = self.half_w + (l - 32) * self.f / d
                sx1 = self.half_w + (l + 32) * self.f / d
                for x in range(max(0, int(math.floor(sx0))), min(self.w, int(math.ceil(sx1)))):
                    if d > zbuf[x]:
                        continue
                    # the lateral offset this pixel column shows, from the sprite centre
                    a = (x + 0.5 - self.half_w) * d / self.f - l
                    self._sample_column(img, x, d, canvas, int(math.floor(a + 32)), add)
            else:
                # a decal: projected and clipped like a wall
                (ax, ay), (bx, by) = geo
                da = (ax - vx) * fx + (ay - vy) * fy
                la = (ax - vx) * rx + (ay - vy) * ry
                db = (bx - vx) * fx + (by - vy) * fy
                lb = (bx - vx) * rx + (by - vy) * ry
                ua, ub = 0.0, 64.0
                near = 1.0
                if da < near and db < near:
                    continue
                if da < near:
                    t = (near - da) / (db - da)
                    da, la, ua = near, la + (lb - la) * t, ua + (ub - ua) * t
                elif db < near:
                    t = (near - db) / (da - db)
                    db, lb, ub = near, lb + (la - lb) * t, ub + (ua - ub) * t
                sxa = self.half_w + la * self.f / da
                sxb = self.half_w + lb * self.f / db
                if sxa > sxb:
                    sxa, sxb, da, db, ua, ub = sxb, sxa, db, da, ub, ua
                if sxb - sxa < 1e-6:
                    continue
                iza, izb = 1.0 / da, 1.0 / db
                for x in range(max(0, int(math.ceil(sxa))), min(self.w, int(math.ceil(sxb)))):
                    t = (x - sxa) / (sxb - sxa)
                    iz = iza + (izb - iza) * t
                    d = 1.0 / iz
                    if d > zbuf[x] + 0.5:
                        continue
                    u = (ua * iza + (ub * izb - ua * iza) * t) / iz
                    self._sample_column(img, x, d, canvas, int(math.floor(u)), add)


if __name__ == "__main__":
    # render one view to a PNG
    level_json, jar_dir, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    gx, gz, facing = int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6])
    r = ViewRenderer(level_json, jar_dir)
    im = Image.fromarray(r.render(gx, gz, facing))
    im.save(out)
    print("wrote", out)
