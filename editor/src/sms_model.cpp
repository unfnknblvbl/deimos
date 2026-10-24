// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// The Master System renderer (rom/render.asm), modelled bit for bit: port
// of Model in tools/live_data.py. The editor's view preview shows exactly
// what the console will draw.
// Licence: GPL-2.0-or-later.
//
// Called by the Map tab's preview and the command-line tool's --render,
// with a LevelData from sms::level_data. The renderer is a column caster:
// for each screen column it keeps the nearest wall (the tallest one), then
// draws the props (billboards) far to near, then the ceiling and floor.
// Fixed-point formats used below:
//   h8      a wall's on-screen height in pixels, 8.8 (256 = 1 pixel);
//   l8, c8  a sideways position or texture column, 8.8, kept to 16 bits
//           as the Z80's registers do;
//   v       a texture row, 8.8 (64 rows = 16384).
// The output "chunk" holds pair bytes; the last step picks a nibble per
// pixel by (x + y) parity, giving colour indices 0..15.
#include "colour.h"
#include "sms_build.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace sms {
namespace {
const int N_HIDX = 1792;   // entries in the height table (ROM banks 7 and 8)

// h8 -> height table index: 1/16-pixel steps below 64 pixels (h8 16384),
// 1/4-pixel steps above, capped at the last entry; hidx_centre gives the
// h8 in the middle of entry i
int hidx(int h8) { return h8 < 16384 ? h8 >> 4 : std::min(N_HIDX - 1, 1024 + ((h8 - 16384) >> 6)); }
int hidx_centre(int i) { return i < 1024 ? i * 16 + 8 : 16384 + (i - 1024) * 64 + 32; }
// the sign of v: -1, 0 or 1
int sgn(int v) { return (v > 0) - (v < 0); }
long long fdiv(long long a, long long b)   // Python //
{
    long long q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

// one height table entry: the wall's screen rows y0..y1-1, the texture
// row v at y0 and its step per row (8.8), and the depth d of a wall that
// tall (world units, for the texture of slanted walls)
struct Row {
    int y0, y1, v0, dv, d;
};

// The height table for a view size (tools/live_data.py's, which the ROM
// stores in banks 7 and 8), computed once per size on first use, thread
// safe. A wall h pixels tall spans CY - 7/16 h to CY + 9/16 h: the eye is
// 28 of the wall's 64 texels below its top.
const std::vector<Row> &table(int size)
{
    static std::vector<Row> t[2];
    static std::once_flag once[2];
    std::call_once(once[size], [size]() {
        int W = VIEW_W[size], H = VIEW_H[size];
        double CY = H / 2.0;
        for (int i = 0; i < N_HIDX; i++) {
            int h8 = hidx_centre(i);
            double h = h8 / 256.0;
            double top = CY - 7 * h / 16, bot = CY + 9 * h / 16;
            int y0 = std::min(H, std::max(0, (int)std::ceil(top - 0.5)));
            int y1 = std::min(H, std::max(0, (int)std::ceil(bot - 0.5)));
            int dv = (int)std::min(65535.0, py_round(64 * 256 / h));        // texels per pixel, 8.8
            int v0 = (int)py_round((y0 + 0.5 - top) * 64 / h * 256);     // the texel at row y0's centre
            v0 = std::max(0, std::min(16383, v0));
            // keep the last row inside the texture
            if (y1 > y0 + 1 && v0 + (y1 - 1 - y0) * dv > 16383) dv = (16383 - v0) / (y1 - 1 - y0);
            // a 64-unit wall at depth d is 32 W / d pixels tall
            int d = (int)std::min<long long>(65535, (long long)32 * W * 256 / h8);
            t[size].push_back({y0, y1, v0, dv, d});
        }
    });
    return t[size];
}
}  // namespace

// see sms_build.h. Returns H x W colour indices (row by row).
std::vector<u8> render(const LevelData &D, int size, int gx, int gz, int f, const std::vector<u8> &line_open,
                       bool low_detail, bool solid_walls)
{
    const int W = VIEW_W[size], H = VIEW_H[size], F = W / 2;   // F: focal length (90-degree view)
    const int st = low_detail ? 2 : 1;      // low detail: even columns only, doubled steps
    const auto &tab = table(size);
    // the screen x of view point (d, l), relative to the centre, 8.8;
    // points outside the 90-degree view clamp just past the edge
    auto project = [&](int d, int l) -> int {
        if (l >= d) return (F + 1) * 256;
        if (l <= -d) return -(F + 1) * 256;
        long long p = (((long long)std::abs(l) * 65536 / d) * F) >> 8;
        return (int)(l >= 0 ? p : -p);
    };
    // the columns x0..x1-1 between two projected x (rounded to pixel
    // centres, clipped to the view; low detail: start on an even column)
    auto col_range = [&](int pa, int pb, int &x0, int &x1) {
        int lo = std::min(pa, pb), hi = std::max(pa, pb);
        x0 = std::max(0, (int)fdiv(lo + 127, 256) + F);
        x1 = std::min(W, (int)fdiv(hi + 127, 256) + F);
        if (low_detail) x0 = (x0 + 1) & ~1;
    };
    // the sideways position l (8.8, 16 bits) where column x0's centre ray
    // meets depth d: l = d * (x0 + 0.5 - F) / F
    auto l8_start = [&](int x0, int d) -> int {
        long long P = (long long)x0 * 256 + 128;
        return (int)((fdiv(P * d, F) - 256LL * d) & 0xFFFF);
    };
    int vx, vz;
    camera(gx, gz, f, vx, vz);
    // per column: the nearest wall's h8, its slot in `walls` (255 = none)
    // and its texture column
    std::vector<int> colh(W, 0), colw(W, 255), colu(W, 0);
    std::vector<int> walls;   // slot -> texture image of each wall drawn
    static const std::pair<std::vector<int>, std::vector<int>> empty;
    // this view's list of walls and props (none: a solid cell)
    auto it = D.pvs.find((gz * 32 + gx) * 4 + f);
    const auto &pv = it == D.pvs.end() ? empty : it->second;
    for (int wi : pv.first) {
        const Seg &s = D.walls[wi];
        int ax = s.ax, az = s.az, bx = s.bx, bz = s.bz, ua = s.ua, ub = s.ub;
        if (s.line != 255 && s.line < (int)line_open.size() && (line_open[s.line] & 1)) continue;   // an open door
        // which side the eye is on (the wall's direction reduced to -1/0/1)
        int sx = sgn(bx - ax), sz = sgn(bz - az);
        int cross = (ax - vx) * sz - (az - vz) * sx;
        if (cross == 0) continue;   // edge-on
        if (cross < 0) {            // from behind: only two-sided walls, ends swapped
            if (!s.two) continue;
            std::swap(ax, bx);
            std::swap(az, bz);
            std::swap(ua, ub);
        }
        int da, la, db, lb;
        view_xf(f, ax - vx, az - vz, da, la);
        view_xf(f, bx - vx, bz - vz, db, lb);
        int slot = (int)walls.size();
        if (da == db) {   // facing the eye: one height for the whole wall
            if (da < 8) continue;
            int x0, x1;
            col_range(project(da, la), project(db, lb), x0, x1);
            if (x1 <= x0) continue;
            int h8 = std::min(65535, 8192 * W / da);
            // texture column = cu + su * l (one texel per unit)
            int su = sgn(ub - ua) * sgn(lb - la);
            int cu = (ua - su * la) & 63;
            int l8 = l8_start(x0, da);
            int dl8 = (int)fdiv(256LL * da, F) * st;   // l's step per drawn column
            walls.push_back(s.tex);
            for (int x = x0; x < x1; x += st) {
                if (h8 > colh[x]) {   // taller = nearer: this wall wins the column
                    colh[x] = h8;
                    colw[x] = slot;
                    colu[x] = (cu + su * (l8 >> 8)) & 63;
                }
                l8 = (l8 + dl8) & 0xFFFF;
            }
            continue;
        }
        // a slanted wall: l = l0 + m * d with m = -1, 0 or 1; clipped to depth NEAR
        if (da < NEAR && db < NEAR) continue;
        int m = (int)fdiv(lb - la, db - da);
        if (m * (db - da) != lb - la || m < -1 || m > 1) continue;   // (never: walls are straight or 45 degrees)
        if (da < NEAR) {
            la = la + m * (NEAR - da);
            da = NEAR;
        } else if (db < NEAR) {
            lb = lb + m * (NEAR - db);
            db = NEAR;
        }
        int l0 = la - m * da;
        if (l0 == 0) continue;   // the line passes through the eye
        int x0, x1;
        col_range(project(da, la), project(db, lb), x0, x1);
        if (x1 <= x0) continue;
        // A column with slope k = (x + 0.5 - F) / F meets the wall at depth
        // l0 / (k - m), where it is 8192 W (k - m) / l0 tall (h8). That is
        // linear in x: hacc (h8 << 8) starts at x0 and steps by dacc.
        long long N0 = 16384LL * (x0 - F) + 8192 - 8192LL * W * m;
        long long hacc = (std::llabs(N0) << 8) / std::abs(l0);
        long long dacc = (16384LL << 8) / std::abs(l0);
        if (l0 < 0) dacc = -dacc;
        dacc *= st;
        // texture column = cz + su * depth (the depth read back from the
        // height table, as the ROM does)
        int su = sgn(ub - ua) * sgn(db - da);
        int cz = (ua - su * da) & 63;
        walls.push_back(s.tex);
        for (int x = x0; x < x1; x += st) {
            int h8 = (int)std::max(1LL, std::min(65535LL, hacc >> 8));
            if (h8 > colh[x]) {
                colh[x] = h8;
                colw[x] = slot;
                int z = tab[hidx(h8)].d;
                colu[x] = (cz + su * z) & 63;
            }
            hacc += dacc;
        }
    }
    // a prop to draw: depth, billboard slot, columns, height, first texture
    // column and its step (8.8), and its place in the list
    struct Bill {
        int d, spr, x0, x1, h8, c8, dc8, order;
    };
    std::vector<Bill> bills;
    for (int si : pv.second) {
        const Scenery &sc = D.scen[si];
        int d, l;
        view_xf(f, sc.x - vx, sc.z - vz, d, l);
        d -= 16;                 // drawn 16 units nearer than its centre
        if (d < NEAR) continue;
        int x0, x1;
        col_range(project(d, l - 32), project(d, l + 32), x0, x1);
        if (x1 <= x0) continue;
        int c8 = (l8_start(x0, d) - (l - 32) * 256) & 0xFFFF;   // 64 units wide: columns from l - 32
        bills.push_back({d, D.scen_slot[si], x0, x1, std::min(65535, 8192 * W / d), c8, (int)fdiv(256LL * d, F) * st,
                         (int)bills.size()});
    }
    // far to near (ties keep the list's order), so nearer props cover farther ones
    std::stable_sort(bills.begin(), bills.end(), [](const Bill &a, const Bill &b) { return a.d > b.d; });
    std::vector<u8> chunk((size_t)W * H, 0);
    const u8 ceil = 0x00;   // colour 0 on both nibbles
    // each column: the wall's texels, the ceiling above, the floor below
    for (int x = 0; x < W; x += st) {
        int y0, y1;
        if (colw[x] == 255) {   // no wall: ceiling and floor meet at the horizon
            y0 = y1 = H / 2;
        } else {
            const Row &r = tab[hidx(colh[x])];
            y0 = r.y0;
            y1 = r.y1;
            const auto &col = D.pool[D.texcols[walls[colw[x]]][colu[x]]];
            int v = r.v0;
            for (int y = y0; y < y1; y++) {
                chunk[y * W + x] = solid_walls ? col[32] : col[v >> 8];   // solid: the texel at v = 32
                v += r.dv;
            }
        }
        for (int y = 0; y < y0; y++) chunk[y * W + x] = ceil;
        for (int y = y1; y < H; y++) chunk[y * W + x] = floor_row_pair(size, y);   // fades towards the horizon
    }
    // the props, through the same height table as walls (64 texels tall)
    for (auto &b : bills) {
        const Row &r = tab[hidx(b.h8)];
        const auto &sp = D.bills[b.spr];
        int c8 = b.c8;
        for (int x = b.x0; x < b.x1; x += st) {
            int c = c8 >> 8;
            c8 = (c8 + b.dc8) & 0xFFFF;
            // past its right edge, behind this column's wall, or an empty column
            if (c > 63 || b.h8 < colh[x] || sp.len[c] == 0) continue;
            int v = r.v0;
            for (int y = r.y0; y < r.y1; y++) {
                int row = v >> 8;
                v += r.dv;
                int k = row - sp.top[c];
                if (k >= 0 && k < sp.len[c]) {   // inside the column's stored run
                    u8 pcol = sp.data[c][k];
                    if (pcol != D.key) chunk[y * W + x] = pcol;
                }
            }
        }
    }
    if (low_detail)   // each drawn column doubled
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x += 2) chunk[y * W + x + 1] = chunk[y * W + x];
    // pair bytes -> colour indices: high nibble where x + y is even
    std::vector<u8> img((size_t)W * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            u8 c = chunk[y * W + x];
            img[y * W + x] = ((x + y) & 1) == 0 ? c >> 4 : c & 15;
        }
    return img;
}

}  // namespace sms
