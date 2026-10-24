// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Licence: GPL-2.0-or-later.
//
// The true-colour first-person renderer (declared in render.h), used by
// console_view.cpp for the previews of the Mega Drive and the consoles that
// share its level format. Three passes:
// 1. walls: each wall piece (Seg, geometry.h) is turned into view space,
//    clipped at the near plane, projected, and its columns compete in a
//    per-column depth buffer (the nearest wins);
// 2. each column is filled with its winning wall's texture column;
// 3. props and things are drawn as 64x64 billboards, far to near, only where
//    nearer than that column's wall.
// Units: world units (64 per cell); view space is d (depth, forward) and
// l (sideways, positive to the right), see view_xf in geometry.h.
#include "render.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
// the eye's height above the floor and the walls' height, world units
const double EYE_Z = 36, WALL_H = 64;

// The camera and the picture, shared by the passes.
struct View {
    int vx, vz, f;          // camera position (world units) and facing
    double F, hw, hh;       // focal length in pixels; half the width and height
    int w, h;               // picture size
    std::vector<u8> *cov;   // ViewOptions::covered, or null
};

// Draws column c (0..63) of a sprite at screen column x, the sprite standing
// on the floor at depth d (0..64 units high). Transparent pixels are skipped;
// drawn pixels are marked in v.cov.
void sample_column(std::vector<RGB> &img, const View &v, int x, double d, const Art &canvas, int c)
{
    if (c < 0 || c >= 64) return;   // outside the sprite
    for (int y = 0; y < v.h; y++) {
        // the world height seen at the centre of pixel row y, then the
        // sprite row there (row 0 is the top, at height 64)
        double z = EYE_Z + (v.hh - (y + 0.5)) * d / v.F;
        int r = (int)std::floor(64 - z);
        if (r < 0 || r >= 64) continue;
        if (canvas.clear(r * 64 + c)) continue;
        img[y * v.w + x] = canvas.at(r * 64 + c);
        if (v.cov) (*v.cov)[y * v.w + x] = 1;
    }
}
}  // namespace

void render_view(const Project &p, const Geometry &geo, int gx, int gz, int f, const ViewOptions &o,
                 std::vector<RGB> &img)
{
    View v;
    camera(gx, gz, f, v.vx, v.vz);   // the eye: 16 units behind the cell's centre
    v.f = f;
    v.w = o.w;
    v.h = o.h;
    v.F = o.focal > 0 ? o.focal : o.w / 2.0;   // default: a 90 degree view
    v.cov = o.covered;
    if (v.cov) v.cov->assign((size_t)o.w * o.h, 0);
    v.hw = o.w / 2.0;
    v.hh = o.h / 2.0;   // the horizon is the middle row
    const int w = o.w, h = o.h;
    // the background: ceiling colour above the horizon, floor colour below
    img.assign((size_t)w * h, RGB{});
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) img[y * w + x] = y < h / 2 ? p.ceiling : p.floor;
    // per screen column: the nearest wall's depth, its texture image
    // (index into geo.tex_imgs, -1 none) and texture column 0..63
    std::vector<double> zbuf(w, std::numeric_limits<double>::infinity());
    std::vector<int> col_tex(w, -1), col_u(w, 0);
    const double near_ = 1.0;   // the near clipping plane, world units
    // ---- pass 1: the walls into the depth buffer
    for (const Seg &s : geo.segs) {
        // an open door is not drawn
        if (s.line != 255 && s.line < (int)o.line_open.size() && o.line_open[s.line]) continue;
        // cross product (A - eye) x (B - A): positive when the eye is in front of the wall
        long long cr = (long long)(s.ax - v.vx) * (s.bz - s.az) - (long long)(s.az - v.vz) * (s.bx - s.ax);
        if (cr <= 0 && !s.two) continue;   // one-sided walls show their front only
        // both ends into view space (d forward, l right)
        int ida, ila, idb, ilb;
        view_xf(f, s.ax - v.vx, s.az - v.vz, ida, ila);
        view_xf(f, s.bx - v.vx, s.bz - v.vz, idb, ilb);
        double da = ida, la = ila, db = idb, lb = ilb, ua = s.ua, ub = s.ub;
        if (da < near_ && db < near_) continue;   // wholly behind the eye
        // one end behind the near plane: move it along the wall to the plane
        // (and its texture column with it)
        if (da < near_) {
            double t = (near_ - da) / (db - da);
            la += (lb - la) * t;
            ua += (ub - ua) * t;
            da = near_;
        } else if (db < near_) {
            double t = (near_ - db) / (da - db);
            lb += (la - lb) * t;
            ub += (ua - ub) * t;
            db = near_;
        }
        // project: screen x = centre + l * F / d; make A the left end
        double sxa = v.hw + la * v.F / da, sxb = v.hw + lb * v.F / db;
        if (sxa > sxb) {
            std::swap(sxa, sxb);
            std::swap(da, db);
            std::swap(ua, ub);
        }
        // the columns whose centres (x + 0.5) lie in [sxa, sxb), within the picture
        int x0 = std::max(0, (int)std::ceil(sxa - 0.5)), x1 = std::min(w, (int)std::ceil(sxb - 0.5));
        if (x1 <= x0 || sxb - sxa < 1e-9) continue;   // no column, or seen edge-on
        // perspective-correct interpolation: 1/d and u/d are linear on screen
        double iza = 1.0 / da, izb = 1.0 / db, uza = ua * iza, uzb = ub * izb, span = sxb - sxa;
        for (int x = x0; x < x1; x++) {
            double t = std::min(1.0, std::max(0.0, (x + 0.5 - sxa) / span));   // 0 at A, 1 at B
            double iz = iza + (izb - iza) * t;
            double depth = 1.0 / iz;
            if (depth < zbuf[x]) {   // nearer than what the column has: this wall wins it
                zbuf[x] = depth;
                col_tex[x] = s.tex;
                col_u[x] = ((int)std::floor((uza + (uzb - uza) * t) / iz)) & 63;   // texture column, wrapped to 0..63
            }
        }
    }
    // ---- pass 2: each column's wall, floor (height 0) to top (height 64)
    for (int x = 0; x < w; x++) {
        int t = col_tex[x];
        if (t < 0 || t >= (int)geo.tex_imgs.size()) continue;   // no wall in this column
        const auto &tex = geo.tex_imgs[t];
        double scale = v.F / zbuf[x];   // pixels per world unit at that depth
        // the wall's top and bottom on screen
        double top = v.hh - (WALL_H - EYE_Z) * scale, bot = v.hh + EYE_Z * scale;
        int y0 = std::max(0, (int)std::ceil(top - 0.5)), y1 = std::min(h, (int)std::ceil(bot - 0.5));
        for (int y = y0; y < y1; y++) {
            // texture row: 0 at the top of the wall, 63 at the floor
            int vv = std::max(0, std::min(63, (int)(((y + 0.5) - top) / (bot - top) * 64)));
            img[y * w + x] = tex[vv * 64 + col_u[x]];   // textures are [v][u]
            if (v.cov) (*v.cov)[y * w + x] = 1;
        }
    }
    // billboards: props and things, far to near
    // ---- pass 3
    struct Item {
        double d, l;   // view-space depth (already 16 nearer, see add) and sideways offset
        int tile;      // sprite id
    };
    std::vector<Item> items;
    // queues a billboard at world position (x, z); its depth is taken 16
    // units nearer than its centre, and it is skipped if that is under 4
    auto add = [&](int tile, int x, int z) {
        int d, l;
        view_xf(f, x - v.vx, z - v.vz, d, l);
        if (d - 16 < 4) return;
        items.push_back({(double)(d - 16), (double)l, tile});
    };
    // props, but not wall decals (info bits 0x780000: the decal's
    // orientation); decals not baked into a wall texture are left out here
    for (const Scenery &s : geo.scenery)
        if (!(s.info & 0x780000)) add(s.tile, s.x, s.z);
    // things not hidden, at their cell's centre
    if (o.things)
        for (const Entity &e : p.entities)
            if (!e.hidden && e.def >= 0 && e.def < (int)p.defs.size()) add(p.defs[e.def].tile, e.x * 64 + 32, e.z * 64 + 32);
    // farthest first, so nearer ones paint over them (stable: equal depths keep their order)
    std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.d > b.d; });
    for (const Item &it : items) {
        auto sp = p.sprites.find(it.tile);
        if (sp == p.sprites.end()) continue;   // no picture for that sprite id
        Art canvas = p.flat(sp->second);       // the picture with its colours
        // the sprite is 64 units wide, centred on l: its screen span
        double sx0 = v.hw + (it.l - 32) * v.F / it.d, sx1 = v.hw + (it.l + 32) * v.F / it.d;
        for (int x = std::max(0, (int)std::floor(sx0)); x < std::min(w, (int)std::ceil(sx1)); x++) {
            if (it.d > zbuf[x]) continue;   // behind this column's wall
            // the sideways position the column's centre hits, relative to the sprite's centre
            double a = (x + 0.5 - v.hw) * it.d / v.F - it.l;
            sample_column(img, v, x, it.d, canvas, (int)std::floor(a + 32));   // sprite column a + 32
        }
    }
}
