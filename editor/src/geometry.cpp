// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Licence: GPL-2.0-or-later.
//
// See geometry.h. The arithmetic copies the Python tools step by step
// (py_round for Python's round, the same double maths), so both make the
// same wall pieces and the same visibility lists.
#include "geometry.h"
#include "colour.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>

const int FX[4] = {0, 1, 0, -1}, FZ[4] = {-1, 0, 1, 0};   // N E S W

namespace {
// What makes a texture image unique: a plain texture is shared by every
// piece using it (ua = ub = -1); a texture with decals baked in is one
// image per piece (its decals and its columns).
struct Key {           // (tex, baked decals, ua, ub) or (tex,)
    int tex;
    std::vector<std::pair<int, int>> baked;
    int ua, ub;
    bool operator<(const Key &o) const
    {
        return std::tie(tex, baked, ua, ub) < std::tie(o.tex, o.baked, o.ua, o.ub);
    }
};
}  // namespace

// see geometry.h. Three steps: cut the walls into pieces, bake the wall
// decals that lie exactly on a one-sided piece into its texture, then give
// each distinct (texture, decals) one image in tex_imgs.
Geometry build_geometry(const Project &p)
{
    Geometry g;
    std::map<int, int> line_of_wall;   // wall index -> its door slot
    for (size_t k = 0; k < p.lines.size(); k++) line_of_wall[p.lines[k].wall] = (int)k;
    for (size_t i = 0; i < p.walls.size(); i++) {
        const Wall &w = p.walls[i];
        int ax = w.ax, az = w.az, bx = w.bx, bz = w.bz, fl = w.flags;
        // the original's nudge: flag 512 moves the wall along x, 256 along
        // z, by +3 (flag 8) or -3 (flag 16) (doors sit inside their cell)
        if (fl & 512) {
            int d = (fl & 8) ? 3 : ((fl & 16) ? -3 : 0);
            ax += d;
            bx += d;
        } else if (fl & 256) {
            int d = (fl & 8) ? 3 : ((fl & 16) ? -3 : 0);
            az += d;
            bz += d;
        }
        // the length along its major axis (a 45-degree wall counts its x or z extent)
        int L = std::max(std::abs(bx - ax), std::abs(bz - az));
        if (L == 0) throw std::runtime_error("wall " + std::to_string(i) + " has no length");
        // cut into n pieces of 64 (the last may be shorter)
        int n = std::max(1, (L + 63) / 64);
        for (int k = 0; k < n; k++) {
            double t0 = (double)(k * 64) / L, t1 = (double)std::min((k + 1) * 64, L) / L;
            int pa0 = (int)py_round(ax + (bx - ax) * t0), pa1 = (int)py_round(az + (bz - az) * t0);
            int pb0 = (int)py_round(ax + (bx - ax) * t1), pb1 = (int)py_round(az + (bz - az) * t1);
            // texture columns: one per unit along the wall, counted from
            // B instead of A when flag 0x8000 (mirrored) is set
            int u0 = k * 64, u1 = std::min((k + 1) * 64, L);
            if (fl & 0x8000) {
                u0 = L - u0;
                u1 = L - u1;
            }
            int base = 64 * (std::min(u0, u1) / 64);   // bring the columns into 0..64
            auto it = line_of_wall.find((int)i);
            g.segs.push_back({pa0, pa1, pb0, pb1, w.tex, it == line_of_wall.end() ? 255 : it->second, u0 - base,
                              u1 - base, fl & 1, (int)i});
        }
    }
    // the consoles' renderers only handle axis-aligned and 45-degree
    // pieces with one texel per unit
    for (auto &s : g.segs) {
        int dx = s.bx - s.ax, dz = s.bz - s.az;
        if (!(dx == 0 || dz == 0 || std::abs(dx) == std::abs(dz)))
            throw std::runtime_error("wall " + std::to_string(s.wall) +
                                     " is neither straight nor at 45 degrees (the consoles can't draw it)");
        if (std::abs(s.ub - s.ua) != std::max(std::abs(dx), std::abs(dz)))
            throw std::runtime_error("wall " + std::to_string(s.wall) + ": its texture doesn't run one texel per unit");
    }
    // wall decals: baked into the texture of the piece they cover
    // piece by its ends, both ways round -> (piece, reversed)
    std::map<std::tuple<int, int, int, int>, std::pair<int, bool>> seg_at;
    for (size_t k = 0; k < g.segs.size(); k++) {
        auto &s = g.segs[k];
        seg_at[{s.ax, s.az, s.bx, s.bz}] = {(int)k, false};
        seg_at[{s.bx, s.bz, s.ax, s.az}] = {(int)k, true};
    }
    std::map<int, std::vector<std::pair<int, int>>> baked;   // seg -> (tile, reversed)
    for (auto &sc : p.scenery) {
        u32 info = sc.info;
        if (info & 0x780000) {   // a wall decal: one of the orientation bits
            // the 64-unit span it covers, from p0 to p1 (mirrored: reversed)
            int x = sc.x, z = sc.z, sg = (info & 0x2000000) ? -1 : 1;
            int p0x, p0z, p1x, p1z;
            if (info & 0x80000) {
                p0x = x - 32 * sg; p0z = z; p1x = x + 32 * sg; p1z = z;
            } else if (info & 0x100000) {
                p0x = x + 32 * sg; p0z = z; p1x = x - 32 * sg; p1z = z;
            } else if (info & 0x400000) {
                p0x = x; p0z = z - 32 * sg; p1x = x; p1z = z + 32 * sg;
            } else {
                p0x = x; p0z = z + 32 * sg; p1x = x; p1z = z - 32 * sg;
            }
            // exactly on a one-sided piece: bake it in; otherwise it stays
            // a prop (drawn as a sprite)
            auto hit = seg_at.find({p0x, p0z, p1x, p1z});
            if (hit != seg_at.end() && !g.segs[hit->second.first].two) {
                baked[hit->second.first].push_back({sc.tile, hit->second.second ? 1 : 0});
                g.n_baked++;
                continue;
            }
        }
        g.scenery.push_back(sc);
    }
    // one image per Key; seg.tex becomes an index into tex_imgs
    std::map<Key, int> tex_key;
    for (size_t k = 0; k < g.segs.size(); k++) {
        Seg &sg = g.segs[k];
        auto bk = baked.find((int)k);
        Key key = bk != baked.end() ? Key{sg.tex, bk->second, sg.ua, sg.ub} : Key{sg.tex, {}, -1, -1};
        auto it = tex_key.find(key);
        if (it == tex_key.end()) {
            auto t = p.textures.find(sg.tex);
            if (t == p.textures.end())
                throw std::runtime_error("wall " + std::to_string(sg.wall) + " uses texture " + std::to_string(sg.tex) +
                                         ", which has no picture");
            Art a = p.flat(t->second);
            std::vector<RGB> img(4096);
            for (int i = 0; i < 4096; i++) img[i] = a.at(i);
            if (bk != baked.end())
                for (auto &d : bk->second) {
                    auto s = p.sprites.find(d.first);
                    if (s == p.sprites.end())
                        throw std::runtime_error("a wall decal uses sprite " + std::to_string(d.first) +
                                                 ", which has no picture");
                    Art cv = p.flat(s->second);
                    // decal column c lands on texture column u: its centre
                    // mapped across the piece's ua..ub (backwards if reversed)
                    for (int c = 0; c < 64; c++) {
                        double t = (c + 0.5) / 64;
                        double sp = d.second ? 1 - t : t;
                        int u = (int)std::floor(sg.ua + (sg.ub - sg.ua) * sp) & 63;
                        for (int r = 0; r < 64; r++)
                            if (!cv.clear(r * 64 + c)) img[r * 64 + u] = cv.at(r * 64 + c);
                    }
                }
            it = tex_key.emplace(key, (int)g.tex_imgs.size()).first;
            g.tex_imgs.push_back(std::move(img));
            g.tex_src.push_back(sg.tex);
            g.tex_plain.push_back(bk == baked.end());
        }
        sg.tex = it->second;
    }
    return g;
}

// see geometry.h. A brute-force ray cast: each column's ray against every
// candidate piece, keeping the nearest hit.
void visible_walls(const std::vector<Seg> &segs, const std::vector<char> &removed, int gx, int gz, int f,
                   std::vector<int> &out, std::vector<double> *colz, int cols)
{
    int vx, vz;
    camera(gx, gz, f, vx, vz);
    // a candidate piece: its index and its ends in view space
    struct W {
        int i;
        double da, la, db, lb;
    };
    std::vector<W> ws;
    for (size_t i = 0; i < segs.size(); i++) {
        const Seg &s = segs[i];
        // which side the eye is on: cross product (A - eye) x (B - A) > 0 = the front
        long long cr = (long long)(s.ax - vx) * (s.bz - s.az) - (long long)(s.az - vz) * (s.bx - s.ax);
        bool front = cr > 0;
        int ax = s.ax, az = s.az, bx = s.bx, bz = s.bz;
        if (!front && s.two) {   // seen from behind: swap the ends
            std::swap(ax, bx);
            std::swap(az, bz);
        }
        int da, la, db, lb;
        view_xf(f, ax - vx, az - vz, da, la);
        view_xf(f, bx - vx, bz - vz, db, lb);
        // drop pieces that are back-facing, removed, wholly behind the eye
        // (d < 1), or wholly right (l > d) or left (l < -d) of the view
        bool ok = (front || s.two) && !removed[i] && !(da < 1 && db < 1) && !(la > da && lb > db) &&
                  !(la < -da && lb < -db);
        if (ok) ws.push_back({(int)i, (double)da, (double)la, (double)db, (double)lb});
    }
    const double INF = std::numeric_limits<double>::infinity();
    std::vector<double> best(cols, INF);
    std::vector<int> win(cols, -1);
    for (size_t w = 0; w < ws.size(); w++) {
        const W &a = ws[w];
        // the column's ray is l = k * d; the piece is (da, la) + t (dd, dl).
        // Solving: t = (k da - la) / (dl - k dd), depth = c / (dl - k dd).
        double dd = a.db - a.da, dl = a.lb - a.la;
        double c = dl * a.da - dd * a.la;
        for (int x = 0; x < cols; x++) {
            double xs = (x + 0.5) * (256.0 / cols);   // the column's centre on a 256-wide screen
            double k = (xs - 128) / 128.0;            // its slope l / d, -1..1
            double num = dl - k * dd;
            double depth = c / num;
            double t = (k * a.da - a.la) / num;
            // the hit must be on the piece (with a little slack) and ahead
            bool good = (t >= -1e-6) && (t <= 1 + 1e-6) && (depth >= 1);
            if (!good) continue;
            if (depth < best[x]) {
                best[x] = depth;
                win[x] = (int)w;
            }
        }
    }
    // the pieces that won at least one column, in index order
    std::vector<char> seen(ws.size(), 0);
    for (int x = 0; x < cols; x++)
        if (win[x] >= 0 && std::isfinite(best[x])) seen[win[x]] = 1;
    out.clear();
    for (size_t w = 0; w < ws.size(); w++)
        if (seen[w]) out.push_back(ws[w].i);
    if (colz) *colz = best;
}

// see geometry.h: the props (indices into scen) that can show. A prop is
// taken as 80 units wide (l +- 40), 16 units nearer than its centre, and
// shows if any column it covers has a wall more than 24 units behind that.
std::vector<int> visible_scenery(const std::vector<Scenery> &scen, const std::vector<double> &colz, int gx, int gz,
                                 int f, int cols)
{
    int vx, vz;
    camera(gx, gz, f, vx, vz);
    std::vector<int> out;
    for (size_t i = 0; i < scen.size(); i++) {
        int d, l;
        view_xf(f, scen[i].x - vx, scen[i].z - vz, d, l);
        d -= 16;
        if (d < NEAR || l - 32 > d || l + 32 < -d) continue;   // too near, or outside the view
        // the columns a..b-1 it covers (projected l -+ 40)
        int a = (int)std::floor((128 + (l - 40) * 128.0 / d) * cols / 256);
        int b = (int)std::ceil((128 + (l + 40) * 128.0 / d) * cols / 256);
        a = std::max(0, a);
        b = std::min(cols, b);
        bool any = false;
        for (int x = a; x < b; x++)
            if (colz[x] > d - 24) any = true;
        if (b > a && any) out.push_back((int)i);
    }
    return out;
}

// see geometry.h: the same test as visible_scenery for a thing at the
// centre of every cell (nearer limit: 4); bit (cz * 32 + cx) of a 128-byte
// map is set for each cell that shows
Bytes visible_cells(const std::vector<double> &colz, int gx, int gz, int f, int cols)
{
    int vx, vz;
    camera(gx, gz, f, vx, vz);
    Bytes bits(128, 0);
    for (int cz = 0; cz < 32; cz++)
        for (int cx = 0; cx < 32; cx++) {
            int d, l;
            view_xf(f, cx * 64 + 32 - vx, cz * 64 + 32 - vz, d, l);
            d -= 16;
            if (d < 4 || l - 32 > d || l + 32 < -d) continue;
            int a = (int)std::floor((128 + (l - 40) * 128.0 / d) * cols / 256);
            int b = (int)std::ceil((128 + (l + 40) * 128.0 / d) * cols / 256);
            a = std::max(0, a);
            b = std::min(cols, b);
            bool any = false;
            for (int x = a; x < b; x++)
                if (colz[x] > d - 24) any = true;
            if (b > a && any) {
                int k = cz * 32 + cx;
                bits[k >> 3] |= 1 << (k & 7);
            }
        }
    return bits;
}
