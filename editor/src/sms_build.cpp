// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Licence: GPL-2.0-or-later.
//
// The Master System builder (see sms_build.h). In order:
//   colours      make_colours picks the level's 16 colours, brighter()
//                and Colours::pair map any RGB to a pair byte;
//   border       make_border: the rock around the view and its frame;
//   sprites      sprite_palette, sprite_record: hardware sprites;
//   packing      pack_level, make_header: the level's tables into banks;
//   level_data   all of the above for a Project;
//   build        the whole ROM: blobs, tables, sprites, container, the
//                ROM directory, size and checksum.
// Every choice copies the Python tools' arithmetic so the ROM comes out
// byte for byte the same.
#include "sms_build.h"
#include "colour.h"
#include "container.h"
#include "levelblob.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>

namespace sms {
namespace {

typedef std::array<double, 3> D3;   // an RGB colour as doubles (averages may be .5)

const int BORDER_POS[2][2] = {{4, 6}, {5, 8}};    // tile row, column of the view
const int BORDER_BASE[2] = {368, 40};             // first border tile in VRAM
const int HEADER_SIZE = 64;                       // the level header at bank 3, $8000
const int VIEW_LIST_MAX = 96;                     // rom/render.asm r_pvs
const int LEVEL_ADDR = 0xA000, LEVEL_MAX = 0x2000, STRINGS_MAX = 0x2000;   // (strings: bank 4, $8000-$9FFF)
const int MISC_BANK = 4, SPR_DIR_SMALL = 0xB000, SPR_DIR_LARGE = 0xB800;
const int ROM_DIR = 0x7FC0;                       // the ROM directory (bank 1)

// conversions between RGB, D3, the console's colour i (0..63) and Lab
D3 d3(RGB c) { return {(double)c.r, (double)c.g, (double)c.b}; }
RGB rgb3(const D3 &c) { return {(u8)c[0], (u8)c[1], (u8)c[2]}; }
D3 sms3(int i) { return d3(sms_rgb(i)); }
// two colours may be mixed in a pair when no channel differs by more than
// one console step (85), so the dither doesn't show
bool ok_pair(const D3 &a, const D3 &b)
{
    double m = std::max({std::fabs(a[0] - b[0]), std::fabs(a[1] - b[1]), std::fabs(a[2] - b[2])});
    return m <= 85.0 + 0.5;
}
// the 50/50 mix of two colours
D3 avg(const D3 &a, const D3 &b) { return {(a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2}; }
Lab lab3(const D3 &c) { return lab(c[0], c[1], c[2]); }
// RGB <-> 0xRRGGBB (map keys)
u32 key_of(RGB c) { return ((u32)c.r << 16) | ((u32)c.g << 8) | c.b; }
RGB rgb_of(u32 k) { return {(u8)(k >> 16), (u8)(k >> 8), (u8)k}; }

// quarters of each 2x2 block showing the floor colour, per tile row below
// the horizon (tools/live_data.py FLOOR_LEVELS), and the 2x2 order
const std::vector<int> FLOOR_LEVELS[2] = {{0, 1, 2, 3, 4, 4}, {0, 1, 3, 4, 4}};
const int BAYER[2][2] = {{0, 2}, {3, 1}};

// the floor level (0..4 quarters) of view pixel row y: by tile row below
// the horizon (the view's middle), the last entry repeated
int floor_level(int size, int y)
{
    const auto &lv = FLOOR_LEVELS[size];
    int b = std::min(std::max(y / 8 - VIEW_H[size] / 16, 0), (int)lv.size() - 1);
    return lv[b];
}

// the console colour (r*16 + g*4 + b) nearest c, channel by channel
int snap_index(RGB c)
{
    return (int)py_round(c.r / 85.0) * 16 + (int)py_round(c.g / 85.0) * 4 + (int)py_round(c.b / 85.0);
}

// tools/live_data.py Colours
// Greedy choice of the level's 16 colours: 0 and 1 are the ceiling and
// floor (snapped to console colours); then, 14 times, the console colour
// that most lowers the total error, where each used colour (counts: RGB
// -> weight) is matched to its nearest candidate: a chosen colour alone or
// mixed with another (ok_pair). Then fills col's candidate list.
// fixed: the level's 16 colours chosen by the user (a console's fixed
// palette, ConsoleOwn::palette), snapped to console colours, instead of
// the greedy choice (nullptr: choose them)
void make_colours(const std::map<u32, int64_t> &counts, RGB ceil, RGB floor, Colours &col, const RGB *fixed)
{
    std::vector<Lab> ul;
    std::vector<int64_t> cnt;
    for (auto &kv : counts) {
        ul.push_back(lab(rgb_of(kv.first)));
        cnt.push_back(kv.second);
    }
    const size_t U = ul.size();
    std::vector<D3> sel = {sms3(snap_index(ceil)), sms3(snap_index(floor))};
    // out[k] = used colour k's error against its nearest colour in C
    auto errs = [&](const std::vector<D3> &C, std::vector<int64_t> &out) {
        std::vector<Lab> cl;
        for (auto &c : C) cl.push_back(lab3(c));
        out.assign(U, std::numeric_limits<int64_t>::max());
        for (size_t k = 0; k < U; k++)
            for (auto &l : cl) out[k] = std::min(out[k], lab_err(dist2(ul[k], l)));
    };
    std::vector<int64_t> cur, e;   // cur: each used colour's error so far
    if (fixed) {   // the user's palette: no choosing
        sel.clear();
        for (int i = 0; i < 16; i++) sel.push_back(sms3(snap_index(fixed[i])));
    } else
        errs(sel, cur);
    while (sel.size() < 16) {
        // try each console colour not yet chosen (it adds itself and its
        // mixes with the chosen ones); keep the lowest weighted total
        int best_i = -1;
        int64_t best_tot = 0;
        std::vector<int64_t> best_e;
        for (int i = 0; i < 64; i++) {
            D3 c = sms3(i);
            bool taken = false;
            for (auto &s : sel)
                if (s == c) taken = true;
            if (taken) continue;
            std::vector<D3> cands = {c};
            for (auto &s : sel)
                if (ok_pair(c, s)) cands.push_back(avg(c, s));
            errs(cands, e);
            int64_t tot = 0;
            for (size_t k = 0; k < U; k++) {
                e[k] = std::min(cur[k], e[k]);
                tot += e[k] * cnt[k];
            }
            if (best_i < 0 || tot < best_tot) {
                best_i = i;
                best_tot = tot;
                best_e = e;
            }
        }
        sel.push_back(sms3(best_i));
        cur = best_e;
    }
    for (int i = 0; i < 16; i++) {
        col.rgb[i] = rgb3(sel[i]);
        col.cram[i] = sms_cram(col.rgb[i]);
    }
    // the candidates: the 16 colours (pair i, i), then the allowed mixes a < b
    col.cand.clear();
    col.A.clear();
    col.B.clear();
    for (int i = 0; i < 16; i++) {
        col.cand.push_back(lab3(sel[i]));
        col.A.push_back(i);
        col.B.push_back(i);
    }
    for (int a = 0; a < 16; a++)
        for (int b = a + 1; b < 16; b++)
            if (ok_pair(sel[a], sel[b])) {
                col.cand.push_back(lab3(avg(sel[a], sel[b])));
                col.A.push_back(a);
                col.B.push_back(b);
            }
    col.cache.clear();
}

std::mutex cache_mu;   // guards every Colours::cache (one lock for all)

// ---------------------------------------------------------------- border
typedef std::vector<std::array<RGB, 8 * 8>> Tiles;   // (not used)

// the view's grey bevelled frame, drawn as a 24 x 24 picture and cut into
// its 8 x 8 corners and edges ("tl", "t", "tr", "l", "r", "bl", "b", "br")
std::map<std::string, std::vector<RGB>> frame_tiles()
{
    const RGB hi{190, 190, 190}, mid{120, 120, 120}, lo{60, 60, 60}, dark{20, 20, 20}, lt{160, 160, 160};
    std::vector<RGB> f(24 * 24, mid);
    // fill rows y0..y1-1, columns x0..x1-1 with c
    auto set = [&](int y0, int y1, int x0, int x1, RGB c) {
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) f[y * 24 + x] = c;
    };
    set(0, 1, 0, 24, hi);
    set(0, 24, 0, 1, hi);
    set(23, 24, 0, 24, lo);
    set(0, 24, 23, 24, lo);
    set(1, 2, 1, 23, lt);
    set(1, 23, 1, 2, lt);
    set(6, 8, 6, 18, dark);
    set(6, 18, 6, 8, dark);
    set(16, 18, 6, 18, hi);
    set(6, 18, 16, 18, hi);
    set(6, 8, 16, 18, hi);
    set(16, 18, 6, 8, dark);
    const char *names[8] = {"tl", "t", "tr", "l", "r", "bl", "b", "br"};
    const int rc[8][2] = {{0, 0}, {0, 1}, {0, 2}, {1, 0}, {1, 2}, {2, 0}, {2, 1}, {2, 2}};
    std::map<std::string, std::vector<RGB>> out;
    for (int k = 0; k < 8; k++) {
        std::vector<RGB> t(64);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) t[y * 8 + x] = f[(rc[k][0] * 8 + y) * 24 + rc[k][1] * 8 + x];
        out[names[k]] = t;
    }
    return out;
}

// The border for view size `size`: the tiles (4 bit planes per 8-pixel
// row, 32 bytes a tile) and the name table of screen rows 2..17 (u16 per
// cell: VRAM tile BORDER_BASE + tile, 0 inside the view). Tiles: 0 all
// ceiling colour, 1 all floor colour, 2..5 a 16 x 16 patch of the border
// texture repeated, 6..13 the frame, then the fading floor's in-between
// tiles. border_idx gets a 256-pixel-wide picture of it for the preview.
void make_border(int size, Colours &col, const Art &tex, LevelData &d)
{
    int W = VIEW_W[size], H = VIEW_H[size];
    int rows = H / 8, strips = W / 8;
    int vr = BORDER_POS[size][0], vc = BORDER_POS[size][1], base = BORDER_BASE[size];
    // an RGB picture -> colour indices, dithered as the renderer does
    auto to_idx = [&](const std::vector<RGB> &rgb, int w, int h) {
        std::vector<u8> out(w * h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                u8 pb = col.pair(rgb[y * w + x]);
                out[y * w + x] = ((y + x) & 1) == 0 ? pb >> 4 : pb & 15;
            }
        return out;
    };
    std::vector<std::vector<u8>> tiles;
    tiles.push_back(std::vector<u8>(64, 0));
    tiles.push_back(std::vector<u8>(64, 1));
    std::vector<RGB> patch_rgb(16 * 16);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) patch_rgb[y * 16 + x] = tex.at((8 + y) * 64 + 8 + x);
    auto patch = to_idx(patch_rgb, 16, 16);
    for (int ty = 0; ty < 2; ty++)
        for (int tx = 0; tx < 2; tx++) {
            std::vector<u8> t(64);
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) t[y * 8 + x] = patch[(ty * 8 + y) * 16 + tx * 8 + x];
            tiles.push_back(t);
        }
    const char *kinds[8] = {"t", "b", "l", "r", "tl", "tr", "bl", "br"};
    auto ft = frame_tiles();
    std::map<std::string, int> tid;
    for (int i = 0; i < 8; i++) {
        tiles.push_back(to_idx(ft[kinds[i]], 8, 8));
        tid[kinds[i]] = 6 + i;
    }
    // the in-between all-floor tiles of the fading floor
    std::set<int> mids;
    for (int l : FLOOR_LEVELS[size])
        if (l > 0 && l < 4) mids.insert(l);
    for (int l : mids) {
        std::vector<u8> t(64);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) t[y * 8 + x] = BAYER[y & 1][x & 1] < l ? 1 : 0;
        tiles.push_back(t);
    }
    Bytes nmap;
    std::vector<u8> shot(16 * 8 * 256, 0);
    // the name table: 16 rows x 32 columns, the frame one cell around the
    // view, the rock patch everywhere else (in a 2 x 2 checker of its tiles)
    for (int r = 2; r < 18; r++)
        for (int c = 0; c < 32; c++) {
            bool inside = vr <= r && r < vr + rows && vc <= c && c < vc + strips;
            int t = -1;
            if (inside) t = -1;
            else if (r == vr - 1 && vc - 1 <= c && c <= vc + strips)
                t = c == vc - 1 ? tid["tl"] : c == vc + strips ? tid["tr"] : tid["t"];
            else if (r == vr + rows && vc - 1 <= c && c <= vc + strips)
                t = c == vc - 1 ? tid["bl"] : c == vc + strips ? tid["br"] : tid["b"];
            else if (vr <= r && r < vr + rows && c == vc - 1)
                t = tid["l"];
            else if (vr <= r && r < vr + rows && c == vc + strips)
                t = tid["r"];
            else
                t = 2 + (r & 1) * 2 + (c & 1);
            put16(nmap, t < 0 ? 0 : base + t);
            if (t >= 0)
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) shot[((r - 2) * 8 + y) * 256 + c * 8 + x] = tiles[t][y * 8 + x];
        }
    // the tiles in the VDP's format: per row, 4 bytes, one per bit plane,
    // leftmost pixel in bit 7
    Bytes blob;
    for (auto &t : tiles)
        for (int y = 0; y < 8; y++) {
            u8 planes[4] = {0, 0, 0, 0};
            for (int x = 0; x < 8; x++) {
                int v = t[y * 8 + x];
                for (int p = 0; p < 4; p++)
                    if (v & (1 << p)) planes[p] |= 1 << (7 - x);
            }
            blob.insert(blob.end(), planes, planes + 4);
        }
    d.border_tiles[size] = blob;
    d.border_map[size] = nmap;
    d.border_idx[size] = shot;
}

// ---------------------------------------------------------------- hardware sprites
// a view size for the hardware sprites: the view's screen position and
// size in pixels, and the most 8 x 16 sprites across (cols) and down (rows)
struct SizeInfo {
    int x0, y0, w, h, cols, rows;
};
const SizeInfo SIZES[2] = {{48, 32, 160, 96, 7, 4}, {64, 40, 128, 80, 5, 4}};
const int EYE_Z = 36;   // the eye's height above the floor in texels (a canvas is 64 high)

// A thing's 15 sprite colours (out[1..15]; out[0] = transparent, black):
// greedy, like make_colours but with no mixes: each step adds the console
// colour that most lowers the weighted error of the canvas's colours,
// stopping early when every colour is matched exactly.
void sprite_palette(const Art &c, RGB out[16])
{
    std::map<u32, int64_t> cnt;
    for (int i = 0; i < 4096; i++)
        if (!c.clear(i)) cnt[key_of(c.at(i))]++;
    std::vector<Lab> ul;
    std::vector<int64_t> uc;
    for (auto &kv : cnt) {
        ul.push_back(lab(rgb_of(kv.first)));
        uc.push_back(kv.second);
    }
    std::vector<int> sel;
    std::vector<int64_t> cur, e;
    bool have_cur = false;
    for (int step = 0; step < 15 && !ul.empty(); step++) {
        int bi = -1;
        int64_t bt = 0;
        std::vector<int64_t> be;
        for (int i = 0; i < 64; i++) {
            bool taken = false;
            for (int s : sel)
                if (sms_rgb(s) == sms_rgb(i)) taken = true;
            if (taken) continue;
            Lab l = lab(sms_rgb(i));
            e.resize(ul.size());
            int64_t tot = 0;
            for (size_t k = 0; k < ul.size(); k++) {
                e[k] = lab_err(dist2(ul[k], l));
                if (have_cur) e[k] = std::min(cur[k], e[k]);
                tot += e[k] * uc[k];
            }
            if (bi < 0 || tot < bt) {
                bi = i;
                bt = tot;
                be = e;
            }
        }
        if (bi < 0) break;
        sel.push_back(bi);
        cur = be;
        have_cur = true;
        if (*std::max_element(cur.begin(), cur.end()) == 0) break;
    }
    out[0] = {0, 0, 0};
    for (int i = 1; i < 16; i++) out[i] = i - 1 < (int)sel.size() ? sms_rgb(sel[i - 1]) : RGB{0, 0, 0};
}

// Integer resampling weights, w[out][in]: area_weights for shrinking
// (each output pixel covers n_in / n_out input pixels; weights in units
// of 1 / n_out), nearest_weights for enlarging (the input pixel under the
// output pixel's centre, weight 1).
std::vector<std::vector<int64_t>> area_weights(int n_in, int n_out)
{
    std::vector<std::vector<int64_t>> w(n_out, std::vector<int64_t>(n_in, 0));
    for (int o = 0; o < n_out; o++) {
        int64_t a = (int64_t)o * n_in, b = (int64_t)(o + 1) * n_in;
        for (int64_t j = a / n_out; j < std::min<int64_t>(n_in, (b + n_out - 1) / n_out); j++)
            w[o][j] = std::max<int64_t>(0, std::min(b, (j + 1) * n_out) - std::max(a, j * n_out));
    }
    return w;
}

std::vector<std::vector<int64_t>> nearest_weights(int n_in, int n_out)
{
    std::vector<std::vector<int64_t>> w(n_out, std::vector<int64_t>(n_in, 0));
    for (int o = 0; o < n_out; o++) w[o][((2 * o + 1) * n_in) / (2 * n_out)] = 1;
    return w;
}

// One hardware sprite record: thing canvas c seen from k cells away.
// The canvas's opaque box is scaled (as the 3D view would show it, but at
// most cols x 8 by rows x 16 pixels), resampled with integer weights, and
// each pixel with enough coverage takes the nearest of the 15 colours.
// Record: cols, rows, screen x, screen y - 1 (the VDP draws a sprite one
// line below its Y), 4 zero bytes, then the 8 x 16 sprites column by
// column, each as two 8 x 8 tiles of 32 bytes (4 bit planes per row).
Bytes sprite_record(const Art &c, const RGB pal[16], int k, const SizeInfo &z)
{
    int r0 = 64, r1 = 0, c0 = 64, c1 = 0;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++)
            if (!c.clear(y * 64 + x)) {
                r0 = std::min(r0, y);
                r1 = std::max(r1, y + 1);
                c0 = std::min(c0, x);
                c1 = std::max(c1, x + 1);
            }
    bool empty = r1 <= r0;
    if (empty) {   // nothing opaque: a 1 x 1 box at the bottom centre
        r0 = 63;
        r1 = 64;
        c0 = 31;
        c1 = 32;
    }
    double focal = z.w / 2.0;          // 90-degree view: focal length = half the width
    double s = focal / (64.0 * k);     // pixels per texel at k cells (64 units each)
    int w = c1 - c0, h = r1 - r0;
    // s2: the scale actually used, shrunk to fit the sprite limits
    double s2 = std::min({s, (double)(z.cols * 8) / w, (double)(z.rows * 16) / h});
    int W = std::max(1, (int)py_round(w * s2)), H = std::max(1, (int)py_round(h * s2));
    // where it stands on screen: centre x and bottom y, at the true scale s
    double cx = z.x0 + z.w / 2.0 + ((c0 + c1) / 2.0 - 32) * s;
    double by = z.y0 + z.h / 2.0 - (64 - r1 - EYE_Z) * s;
    auto wx = W < w ? area_weights(w, W) : nearest_weights(w, W);
    auto wy = H < h ? area_weights(h, H) : nearest_weights(h, H);
    // integer area average of alpha and alpha * colour
    std::vector<int64_t> a(w * h), ch_[3];
    for (int k2 = 0; k2 < 3; k2++) ch_[k2].assign(w * h, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int i = (r0 + y) * 64 + c0 + x;
            int64_t al = (!empty && !c.clear(i)) ? 255 : 0;
            a[y * w + x] = al;
            RGB v = c.clear(i) ? RGB{0, 0, 0} : c.at(i);
            ch_[0][y * w + x] = al * v.r;
            ch_[1][y * w + x] = al * v.g;
            ch_[2][y * w + x] = al * v.b;
        }
    // (wy @ M @ wx.T): first along x, then along y
    auto apply = [&](const std::vector<int64_t> &m) {
        std::vector<int64_t> t(h * W, 0), o(H * W, 0);
        for (int y = 0; y < h; y++)
            for (int ox = 0; ox < W; ox++) {
                int64_t acc = 0;
                for (int x = 0; x < w; x++) acc += m[y * w + x] * wx[ox][x];
                t[y * W + ox] = acc;
            }
        for (int oy = 0; oy < H; oy++)
            for (int ox = 0; ox < W; ox++) {
                int64_t acc = 0;
                for (int y = 0; y < h; y++) acc += wy[oy][y] * t[y * W + ox];
                o[oy * W + ox] = acc;
            }
        return o;
    };
    auto asum = apply(a);
    std::vector<int64_t> num[3] = {apply(ch_[0]), apply(ch_[1]), apply(ch_[2])};
    std::vector<int64_t> sx(W), sy(H);
    for (int ox = 0; ox < W; ox++) {
        sx[ox] = 0;
        for (int x = 0; x < w; x++) sx[ox] += wx[ox][x];
    }
    for (int oy = 0; oy < H; oy++) {
        sy[oy] = 0;
        for (int y = 0; y < h; y++) sy[oy] += wy[oy][y];
    }
    // the sprite grid: whole 8 x 16 sprites, the picture centred across
    // and standing on the bottom
    int cols = std::min(z.cols, (W + 7) / 8), rows = std::min(z.rows, (H + 15) / 16);
    int cw = cols * 8, chh = rows * 16;
    std::vector<u8> grid((size_t)cw * chh, 0);
    Lab P[15];
    for (int i = 0; i < 15; i++) P[i] = lab(pal[i + 1]);
    int ox0 = (cw - W) / 2, oy0 = chh - H;
    for (int y = 0; y < H; y++) {
        int gy = y + oy0;
        if (gy < 0 || gy >= chh) continue;
        for (int x = 0; x < W && ox0 + x < cw; x++) {
            int64_t as = asum[y * W + x];
            // opaque only when at least 110/255 of the area was covered
            if (!(as >= 110 * sy[y] * sx[x])) continue;
            // the colour: num / as, rounded half up, in integers
            int64_t den = std::max<int64_t>(2 * as, 1);
            double rr = (double)((2 * num[0][y * W + x] + as) / den);
            double gg = (double)((2 * num[1][y * W + x] + as) / den);
            double bb = (double)((2 * num[2][y * W + x] + as) / den);
            Lab l = lab(rr, gg, bb);
            int bi = 0;
            double bd = std::numeric_limits<double>::infinity();
            for (int i = 0; i < 15; i++) {
                double dd = dist2(l, P[i]);
                if (dd < bd) {
                    bd = dd;
                    bi = i;
                }
            }
            grid[(size_t)gy * cw + ox0 + x] = bi + 1;
        }
    }
    // its top-left corner on screen, kept inside the view (x) and below its top (y)
    int x0 = (int)py_round(cx - cw / 2.0), y0 = (int)py_round(by - chh);
    x0 = std::max(z.x0, std::min(x0, z.x0 + z.w - cw));
    y0 = std::max(z.y0, y0);
    Bytes rec = {(u8)cols, (u8)rows, (u8)x0, (u8)((y0 - 1) & 0xFF), 0, 0, 0, 0};
    for (int cc = 0; cc < cols; cc++)
        for (int r = 0; r < rows * 2; r++)
            for (int yy = 0; yy < 8; yy++) {
                u8 planes[4] = {0, 0, 0, 0};
                for (int xx = 0; xx < 8; xx++) {
                    int v = grid[(size_t)(r * 8 + yy) * cw + cc * 8 + xx];
                    for (int p = 0; p < 4; p++)
                        if (v & (1 << p)) planes[p] |= 1 << (7 - xx);
                }
                rec.insert(rec.end(), planes, planes + 4);
            }
    return rec;
}

// ---------------------------------------------------------------- packing
// a small table to place: its name (the header's field prefix), bytes,
// and alignment in the bank
struct Item {
    std::string name;
    Bytes data;
    int align;
};

// the banks the level may use, in order: 3, then 9..255
std::vector<int> level_banks()
{
    std::vector<int> b = {LEVEL_BANK};
    for (int i = FIRST_DATA_BANK; i < 256; i++) b.push_back(i);
    return b;
}

// Places the level's tables in banks (tools/live_data.py's packing):
// the small ones one after another from bank 3 after the header, moving to
// the next bank when one doesn't fit; the border's tiles and maps must
// share a bank (the renderer pages one in), so the first of them asks for
// room for all. The big ones (pvs, columns, bsprites) each start on a new
// bank and run over as many as they need. place gets name -> (bank, slot-2
// address), images bank -> its 16 KB. Throws when something doesn't fit.
void pack_level(const std::vector<Item> &small, const std::vector<std::pair<std::string, Bytes>> &big,
                std::map<std::string, std::pair<int, int>> &place, std::map<int, Bytes> &images)
{
    auto banks = level_banks();
    size_t bi = 0;
    // the next free bank
    auto next = [&]() {
        if (bi >= banks.size()) throw std::runtime_error("the level's data doesn't fit in a 4 MB ROM");
        return banks[bi++];
    };
    int bank = next(), pos = HEADER_SIZE;
    images[bank] = Bytes(BANK, 0);
    std::vector<std::string> border;
    size_t border_size = 0;
    for (auto &it : small)
        if (it.name.rfind("btiles", 0) == 0 || it.name.rfind("bmap", 0) == 0) {
            border.push_back(it.name);
            border_size += it.data.size() + 32;   // (+ room for alignment)
        }
    for (auto &it : small) {
        if (it.data.size() > (size_t)BANK) throw std::runtime_error("the level's " + it.name + " table is bigger than 16 KB");
        pos = (pos + it.align - 1) / it.align * it.align;   // align up
        size_t need = it.name == border[0] ? border_size : it.data.size();
        if (pos + need > (size_t)BANK) {
            bank = next();
            pos = 0;
            images[bank] = Bytes(BANK, 0);
        }
        memcpy(&images[bank][pos], it.data.data(), it.data.size());
        place[it.name] = {bank, 0x8000 + pos};
        pos += (int)it.data.size();
    }
    for (auto &it : big) {
        bank = next();
        place[it.first] = {bank, 0x8000};
        size_t n = std::max<size_t>(1, it.second.size());
        for (size_t i = 0; i < n; i += BANK) {
            if (i) bank = next();
            Bytes b(BANK, 0);
            if (i < it.second.size()) memcpy(b.data(), it.second.data() + i, std::min<size_t>(BANK, it.second.size() - i));
            images[bank] = b;
        }
    }
    std::set<int> bb;
    for (auto &n : border) bb.insert(place[n].first);
    if (bb.size() != 1) throw std::runtime_error("internal: border tiles and maps must share a bank");
}

// the level header's fields and sizes in order (rom/live_layout.inc,
// tools/live_data.py H_FIELDS); little-endian, 64 bytes in all
const std::pair<const char *, int> H_FIELDS[] = {
    {"magic", 2}, {"version", 1}, {"walls_bank", 1}, {"walls_addr", 2}, {"pvs_index_bank", 1},
    {"pvs_index_addr", 2}, {"pvs_bank", 1}, {"tex_bank", 1}, {"texcols_bank", 1}, {"texcols_addr", 2},
    {"scenery_bank", 1}, {"scenery_addr", 2}, {"bspr_bank", 1}, {"bsprite_dir_bank", 1}, {"bsprite_dir_addr", 2},
    {"border_bank", 1}, {"btiles_large_addr", 2}, {"btiles_small_addr", 2}, {"bmap_large_addr", 2},
    {"bmap_small_addr", 2}, {"palette_bank", 1}, {"palette_addr", 2}, {"bill_key", 1}, {"n_btiles_large", 1},
    {"n_btiles_small", 1}, {"n_walls", 2}};

// the 64-byte level header from where pack_level put things, the
// billboards' transparent key, the border tile counts and the wall count
Bytes make_header(std::map<std::string, std::pair<int, int>> &place, int key, int nbl, int nbs, int n_walls)
{
    std::map<std::string, int> v = {{"version", 1}, {"bill_key", key}, {"n_btiles_large", nbl},
                                    {"n_btiles_small", nbs}, {"n_walls", n_walls}};
    for (const char *k : {"walls", "pvs_index", "texcols", "scenery", "bsprite_dir", "palette"}) {
        v[std::string(k) + "_bank"] = place[k].first;
        v[std::string(k) + "_addr"] = place[k].second;
    }
    // the big tables: only their first bank
    v["pvs_bank"] = place["pvs"].first;
    v["tex_bank"] = place["columns"].first;
    v["bspr_bank"] = place["bsprites"].first;
    v["border_bank"] = place["btiles_large"].first;
    for (const char *k : {"btiles_large", "btiles_small", "bmap_large", "bmap_small"})
        v[std::string(k) + "_addr"] = place[k].second;
    Bytes out;
    for (auto &f : H_FIELDS) {
        if (std::string(f.first) == "magic") {
            out.push_back('L');
            out.push_back('V');
            continue;
        }
        int x = v.at(f.first);
        for (int i = 0; i < f.second; i++) out.push_back((x >> (8 * i)) & 0xFF);
    }
    out.resize(HEADER_SIZE, 0);
    return out;
}

// copies b into the ROM at a slot-2 address ($8000-$BFFF) of bank
void put_bytes(Bytes &rom, int bank, int addr, const Bytes &b)
{
    memcpy(&rom[(size_t)bank * BANK + (addr - 0x8000)], b.data(), b.size());
}

}  // namespace

// see sms_build.h. Saturation x 1.6 around the pixel's grey (the mean of
// r, g, b), then contrast x 1.15 around 128, done in integers: per
// channel 128 + floor((23 * (24 v - 3 * sum - 1920) + 150) / 300), clamped
// to 0..255 (tools/live_data.py brighter)
RGB brighter(RGB c)
{
    int s3 = 3 * (c.r + c.g + c.b);
    auto one = [&](int v) {
        int num = 23 * (24 * v - s3 - 1920);
        int q = num + 150;
        q = q >= 0 ? q / 300 : -((-q + 299) / 300);      // floor division, as Python's //
        return (u8)std::min(255, std::max(0, 128 + q));
    };
    return RGB{one(c.r), one(c.g), one(c.b)};
}

// see sms_build.h: the dither of floor row y as a pair byte of colours 0
// (ceiling) and 1 (floor), from the 2 x 2 Bayer order (even rows use its
// first row, odd rows its second, swapped to suit the pair's nibbles)
u8 floor_row_pair(int size, int y)
{
    int lv = floor_level(size, y);
    if ((y & 1) == 0) return (u8)(((BAYER[0][0] < lv) << 4) | (BAYER[0][1] < lv));
    return (u8)(((BAYER[1][1] < lv) << 4) | (BAYER[1][0] < lv));
}

// see sms_build.h. Nearest candidate by CIELAB distance; results are
// cached (looked up and stored under cache_mu; two threads may both
// compute the same colour, with the same result).
u8 Colours::pair(RGB c)
{
    c = brighter(c);
    u32 k = key_of(c);
    {
        std::lock_guard<std::mutex> g(cache_mu);
        auto it = cache.find(k);
        if (it != cache.end()) return it->second;
    }
    Lab l = lab(c);
    size_t bi = 0;
    double bd = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < cand.size(); i++) {
        double d = dist2(l, cand[i]);
        if (d < bd) {
            bd = d;
            bi = i;
        }
    }
    u8 v = (u8)((A[bi] << 4) | B[bi]);
    std::lock_guard<std::mutex> g(cache_mu);
    cache[k] = v;
    return v;
}

// see sms_build.h
void sprite_colours(const Art &canvas, RGB pal[16]) { sprite_palette(canvas, pal); }

// see sms_build.h: the palette, then one record per distance 1..4 cells
SpriteSet sprite_set(const Art &canvas, int size)
{
    SpriteSet s;
    sprite_palette(canvas, s.pal);
    for (int i = 0; i < 16; i++) s.cram[i] = sms_cram(s.pal[i]);
    for (int k = 1; k <= 4; k++) s.records[k - 1] = sprite_record(canvas, s.pal, k, SIZES[size]);
    return s;
}

// see sms_build.h
bool level_data(const Project &p, LevelData &d, bool with_pvs, const BuildOptions &opt, std::string &err)
{
    try {   // wall pieces, decals, texture images
        d.geo = build_geometry(p);
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }
    auto &segs = d.geo.segs;
    d.walls = segs;
    if (segs.size() > 65535) {   // wall ids are u16 in the visibility lists
        err = "too many wall pieces";
        return false;
    }
    if (d.geo.tex_imgs.size() > 255) {
        err = "too many different wall textures (with wall decals): " + std::to_string(d.geo.tex_imgs.size()) +
              ", the Master System version allows 255";
        return false;
    }
    // billboard slots: every sprite tile among the props, ascending
    // (unbaked decals' tiles are included, though decals aren't drawn below)
    std::set<int> bt;
    for (auto &s : d.geo.scenery) bt.insert(s.tile);
    d.bill_tiles.assign(bt.begin(), bt.end());
    for (int t : d.bill_tiles)
        if (!p.sprites.count(t)) {
            err = "a prop uses sprite " + std::to_string(t) + ", which has no picture";
            return false;
        }
    std::map<int, int> slot;
    for (size_t i = 0; i < d.bill_tiles.size(); i++) slot[d.bill_tiles[i]] = (int)i;
    d.scen.clear();
    d.scen_slot.clear();
    // the props drawn as billboards: every one not baked into a wall,
    // including decals standing free (bars or a railing across an opening:
    // a billboard at the decal's centre, seen from both sides; as
    // tools/live_data.py)
    for (auto &s : d.geo.scenery) {
        d.scen.push_back(s);
        d.scen_slot.push_back(slot[s.tile]);
    }
    if (d.scen.size() > 255) {   // prop ids are u8 in the lists
        err = "too many props: " + std::to_string(d.scen.size()) + " (the Master System version allows 255)";
        return false;
    }
    // ---- colours: weighted by how often each texture is used
    std::vector<int64_t> use(d.geo.tex_imgs.size(), 0);
    for (auto &s : segs) use[s.tex]++;
    std::map<u32, int64_t> counts;
    for (size_t t = 0; t < d.geo.tex_imgs.size(); t++)
        for (auto &c : d.geo.tex_imgs[t]) counts[key_of(c)] += std::max<int64_t>(1, use[t]);
    // the props' colours count 2 per pixel, the border patch's 40
    std::vector<Art> bill_art;
    for (int t : d.bill_tiles) {
        bill_art.push_back(p.flat(p.sprites.at(t)));
        const Art &cv = bill_art.back();
        for (int i = 0; i < 4096; i++)
            if (!cv.clear(i)) counts[key_of(cv.at(i))] += 2;
    }
    Art border = p.flat(p.border);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) counts[key_of(border.at(y * 64 + x))] += 40;
    opt.say("choosing the level's 16 colours...");
    // the colours are chosen for the brightened art
    std::map<u32, int64_t> bright;
    for (auto &kv : counts) bright[key_of(brighter(rgb_of(kv.first)))] += kv.second;
    make_colours(bright, p.sms_ceil(), p.sms_flo(), d.col,
                 p.has_level_palette ? p.level_palette : nullptr);   // the level's 16 colours (or the console's fixed ones)
    // ---- texture columns, deduplicated
    // every texture image becomes 64 columns of 64 pair bytes; identical
    // columns are stored once (pool), and texcols maps each image's
    // column u to its pool index ("column id")
    std::map<std::array<u8, 64>, int> pool_idx;
    d.pool.clear();
    d.texcols.clear();
    for (auto &img : d.geo.tex_imgs) {
        std::array<u16, 64> ids;
        std::vector<u8> pb(4096);
        for (int i = 0; i < 4096; i++) pb[i] = d.col.pair(img[i]);
        for (int u = 0; u < 64; u++) {
            std::array<u8, 64> c;
            for (int v = 0; v < 64; v++) c[v] = pb[v * 64 + u];
            auto it = pool_idx.find(c);
            if (it == pool_idx.end()) {
                it = pool_idx.emplace(c, (int)d.pool.size()).first;
                d.pool.push_back(c);
            }
            ids[u] = (u16)it->second;
        }
        d.texcols.push_back(ids);
    }
    // ---- billboard sprites; key = an unused pair byte
    std::set<int> used;
    for (auto &c : d.pool)
        for (u8 b : c) used.insert(b);
    std::vector<std::vector<u8>> spb;
    for (auto &cv : bill_art) {
        std::vector<u8> pb(4096);
        for (int i = 0; i < 4096; i++) {
            pb[i] = d.col.pair(cv.clear(i) ? RGB{0, 0, 0} : cv.at(i));
            if (!cv.clear(i)) used.insert(pb[i]);
        }
        spb.push_back(pb);
    }
    // the transparent key: the highest pair byte nothing uses
    d.key = 0;
    for (int k = 255; k >= 0; k--)
        if (!used.count(k)) {
            d.key = (u8)k;
            break;
        }
    if (used.size() >= 256) {
        err = "every colour pair is in use: no byte left to mean 'transparent'";
        return false;
    }
    // each billboard: per column, the run from its first to its last
    // opaque row (clear pixels inside it become the key)
    d.bills.clear();
    for (size_t t = 0; t < bill_art.size(); t++) {
        LevelData::Bill b;
        for (int c = 0; c < 64; c++) {
            int r0 = -1, r1 = -1;
            for (int r = 0; r < 64; r++)
                if (!bill_art[t].clear(r * 64 + c)) {
                    if (r0 < 0) r0 = r;
                    r1 = r + 1;
                }
            if (r0 < 0) {
                b.top[c] = 0;
                b.len[c] = 0;
                continue;
            }
            b.top[c] = (u8)r0;
            b.len[c] = (u8)(r1 - r0);
            for (int r = r0; r < r1; r++)
                b.data[c].push_back(bill_art[t].clear(r * 64 + c) ? d.key : spb[t][r * 64 + c]);
        }
        d.bills.push_back(b);
    }
    // ---- border (both view sizes)
    for (int s = 0; s < 2; s++) make_border(s, d.col, border, d);
    if (!with_pvs) return true;
    // ---- visibility per (cell, facing)
    opt.say("working out what can be seen from each cell...");
    // A door may be open or closed, so each view lists what shows either
    // way: the walls seen with every door removed (doors) plus those seen
    // with all in place (none). The props use the depths with doors open.
    std::vector<char> doors(segs.size()), none(segs.size(), 0);
    for (size_t i = 0; i < segs.size(); i++) doors[i] = segs[i].line != 255;
    // only open cells: the player never stands in a solid one
    std::vector<int> cells;
    for (int i = 0; i < 1024; i++)
        if (!p.wall[i]) cells.push_back(i);
    std::vector<std::pair<std::vector<int>, std::vector<int>>> res(cells.size() * 4);
    std::atomic<int> done(0);
    parallel_for((int)cells.size(), opt.n_threads(), [&](int ci) {
        if (opt.cancelled()) return;
        int gx = cells[ci] % 32, gz = cells[ci] / 32;
        for (int f = 0; f < 4; f++) {
            std::vector<int> vo, vc;
            std::vector<double> colz;
            visible_walls(segs, doors, gx, gz, f, vo, &colz);
            visible_walls(segs, none, gx, gz, f, vc, nullptr);
            std::set<int> u(vo.begin(), vo.end());
            u.insert(vc.begin(), vc.end());
            res[ci * 4 + f] = {std::vector<int>(u.begin(), u.end()), visible_scenery(d.scen, colz, gx, gz, f)};
        }
        int k = ++done;   // progress: this step is 10%..70% of the build
        if (k % 16 == 0) opt.at(0.1f + 0.6f * k / cells.size());
    });
    if (opt.cancelled()) {
        err = "cancelled";
        return false;
    }
    d.pvs.clear();
    for (size_t ci = 0; ci < cells.size(); ci++)
        for (int f = 0; f < 4; f++) d.pvs[cells[ci] * 4 + f] = res[ci * 4 + f];
    return true;
}

// see sms_build.h
bool is_rom(const Bytes &rom)
{
    return rom.size() >= 0x8000 && memcmp(&rom[0x7FF0], "TMR SEGA", 8) == 0 &&
           memcmp(&rom[ROM_DIR], "DRPGDIR1", 8) == 0;
}

// see sms_build.h
bool is_gg(const Bytes &rom)
{
    int region = rom.size() >= 0x8000 ? rom[0x7FFF] >> 4 : 0;   // the header's region code (high nibble)
    return region >= 5 && region <= 7;
}

// see sms_build.h (the caller has checked is_rom). The ROM directory's
// layout is written at the end of build() below.
bool read_project(const Bytes &rom, Project &p, std::string &err)
{
    const u8 *dir = &rom[ROM_DIR];
    if (dir[8] < 3) {   // the directory's version
        err = "this Master System ROM was made by an older version of the port (before 0.2.8); "
              "rebuild it with a current one";
        return false;
    }
    int bank = dir[13], n = dir[14];   // the container's first bank and bank count
    u32 size = get32(dir + 16);        // and its size in bytes
    if (n == 0 || (size_t)bank * BANK + size > rom.size()) {
        err = "this ROM has no copy of the level for the editor";
        return false;
    }
    if (!read_container(&rom[(size_t)bank * BANK], size, p, err)) return false;
    p.sms_plain = !p.has_sms_colours;      // (a ROM from before: its own ceiling and floor colours)
    return true;
}

// see sms_build.h. rom_in must be a ROM of this port (the engine part is
// kept). Progress: 0..72% level_data, then tables, sprites (to 90%), the
// container and the header.
bool build(Bytes &rom_in, const Project &p_in, const BuildOptions &opt, BuildStats &stats, std::string &err)
{
    // the Master System's ceiling and floor go into the ROM's copy of the
    // level (its usual black and dark red, unless it has its own or is one
    // from an older ROM that had none)
    Project p = p_in;
    if (!p.has_sms_colours && !p.sms_plain) {
        p.sms_ceiling = p.sms_ceil();
        p.sms_floor = p.sms_flo();
        p.has_sms_colours = true;
    }
    // the game's data comes from q: the level as this console builds it (its
    // own pictures, texts and palette); the ROM's editable copy (the
    // container, below) from p, every console's versions kept
    const Project q = p.for_console(opt.console);
    Bytes level, strings;
    try {   // the rules' tables (these also check the limits)
        level = make_level_blob(q, false);
        strings = make_strings_blob(q, false, opt.console == "gg");   // (the Game Gear's buttons and box)
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }
    for (auto &dd : q.defs)
        if (!q.sprites.count(dd.tile)) {
            err = "thing type '" + dd.name + "' uses sprite " + std::to_string(dd.tile) + ", which has no picture";
            return false;
        }
    LevelData d;
    opt.at(0.02f);
    if (!level_data(q, d, true, opt, err)) return false;
    opt.at(0.72f);
    // ---- the renderer's tables
    // walls: 16 bytes per piece (docs/rom_format.md): s16 ax, az, bx, bz,
    // texture image, door slot (255 none), ua, ub, two-sided, 3 spare
    Bytes walls;
    for (auto &s : d.walls) {
        put16(walls, (u16)s.ax);
        put16(walls, (u16)s.az);
        put16(walls, (u16)s.bx);
        put16(walls, (u16)s.bz);
        walls.push_back((u8)s.tex);
        walls.push_back((u8)s.line);
        walls.push_back((u8)s.ua);
        walls.push_back((u8)s.ub);
        walls.push_back((u8)s.two);
        walls.push_back(0);
        walls.push_back(0);
        walls.push_back(0);
    }
    // the visibility lists: per view, n walls, their u16 ids, n props, their
    // u8 ids; never across a bank. index[cell * 4 + f] = 1 + the list's
    // offset (0 = none: a solid cell)
    Bytes blob;
    std::vector<int> index(4096, 0);
    for (auto &kv : d.pvs) {
        auto &wl = kv.second.first;
        auto &sl = kv.second.second;
        Bytes rec;
        rec.push_back((u8)wl.size());
        for (int i : wl) put16(rec, i);
        rec.push_back((u8)sl.size());
        for (int i : sl) rec.push_back((u8)i);
        if (rec.size() > (size_t)VIEW_LIST_MAX || wl.size() > 255) {
            int c = kv.first / 4;
            err = "too much can be seen from cell " + std::to_string(c % 32) + "," + std::to_string(c / 32) +
                  " facing " + "NESW"[kv.first & 3] + ": " + std::to_string(wl.size()) + " wall pieces and " +
                  std::to_string(sl.size()) + " props (the Master System's list holds 96 bytes)";
            return false;
        }
        // would cross a bank boundary: pad to the next bank
        if ((blob.size() % BANK) + rec.size() > (size_t)BANK) blob.resize(blob.size() + (BANK - blob.size() % BANK), 0);
        index[kv.first] = (int)blob.size() + 1;
        blob.insert(blob.end(), rec.begin(), rec.end());
    }
    for (int v : index)
        if (v > 0xFFFF) {
            err = "the visibility lists are too big (more than 64 KB)";
            return false;
        }
    Bytes pvs_index;
    for (int v : index) put16(pvs_index, v);
    // texcols: per texture image 64 u16 column ids; columns: the pool, 64 bytes each
    Bytes texcols;
    for (auto &ids : d.texcols)
        for (u16 c : ids) put16(texcols, c);
    Bytes columns;
    for (auto &c : d.pool) columns.insert(columns.end(), c.begin(), c.end());
    // scenery: s16 x, z, u8 billboard slot, pad
    Bytes scenery;
    for (size_t i = 0; i < d.scen.size(); i++) {
        put16(scenery, (u16)d.scen[i].x);
        put16(scenery, (u16)d.scen[i].z);
        scenery.push_back((u8)d.scen_slot[i]);
        scenery.push_back(0);
    }
    // billboard sprites: 64 u16 column offsets (0 = empty column), then a
    // 64-byte block per non-empty column (key outside its run); never
    // across a bank. offs = each sprite's offset from the first bank.
    Bytes sp;
    std::vector<u32> offs;
    for (auto &b : d.bills) {
        Bytes rec(128, 0), body;
        for (int c = 0; c < 64; c++) {
            if (b.len[c] == 0) continue;
            Bytes col(64, d.key);
            memcpy(&col[b.top[c]], b.data[c].data(), b.len[c]);
            unsigned o = 128 + (unsigned)body.size();
            rec[c * 2] = o & 0xFF;
            rec[c * 2 + 1] = o >> 8;
            body.insert(body.end(), col.begin(), col.end());
        }
        rec.insert(rec.end(), body.begin(), body.end());
        if ((sp.size() % BANK) + rec.size() > (size_t)BANK) sp.resize(sp.size() + (BANK - sp.size() % BANK), 0);
        offs.push_back((u32)sp.size());
        sp.insert(sp.end(), rec.begin(), rec.end());
    }
    // the billboard directory: u32 offset, first row, end row, u16 0
    Bytes bdir;
    for (size_t i = 0; i < d.bills.size(); i++) {
        auto &b = d.bills[i];
        int a = 255, e = 0;
        bool any = false;
        for (int c = 0; c < 64; c++)
            if (b.len[c]) {
                any = true;
                a = std::min(a, (int)b.top[c]);
                e = std::max(e, b.top[c] + b.len[c]);
            }
        if (!any) a = e = 0;
        put32(bdir, offs[i]);
        bdir.push_back((u8)a);
        bdir.push_back((u8)e);
        put16(bdir, 0);
    }
    // the small tables (with their alignments) and the big ones, in the
    // order tools/live_data.py packs them
    std::vector<Item> small = {{"walls", walls, 16},
                               {"pvs_index", pvs_index, 256},
                               {"texcols", texcols, 2},
                               {"scenery", scenery, 2},
                               {"bsprite_dir", bdir, 2},
                               {"btiles_large", d.border_tiles[0], 32},
                               {"btiles_small", d.border_tiles[1], 32},
                               {"bmap_large", d.border_map[0], 2},
                               {"bmap_small", d.border_map[1], 2},
                               {"palette", Bytes(d.col.cram, d.col.cram + 16), 16}};
    std::vector<std::pair<std::string, Bytes>> big = {{"pvs", blob}, {"columns", columns}, {"bsprites", sp}};
    std::map<std::string, std::pair<int, int>> place;
    std::map<int, Bytes> images;
    try {
        pack_level(small, big, place, images);
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }
    Bytes hdr = make_header(place, d.key, (int)d.border_tiles[0].size() / 32, (int)d.border_tiles[1].size() / 32,
                            (int)d.walls.size());
    memcpy(images[LEVEL_BANK].data(), hdr.data(), hdr.size());   // the header at bank 3, $8000
    int next_bank = images.rbegin()->first + 1;   // the first bank after the level's tables

    // ---- the ROM image: a full 4 MB map, level-owned parts cleared
    Bytes rom = rom_in;
    rom.resize((size_t)256 * BANK, 0);
    // clear: the level blob's area in bank 2, bank 3, the strings, the two
    // sprite directories and palettes ($B000-$BFFF of bank 4), banks 9 on
    memset(&rom[(size_t)2 * BANK + (LEVEL_ADDR - 0x8000)], 0, LEVEL_MAX);
    memset(&rom[(size_t)LEVEL_BANK * BANK], 0, BANK);
    memset(&rom[(size_t)MISC_BANK * BANK], 0, STRINGS_MAX);
    memset(&rom[(size_t)MISC_BANK * BANK + (SPR_DIR_SMALL - 0x8000)], 0, 0x1000);
    memset(&rom[(size_t)FIRST_DATA_BANK * BANK], 0, rom.size() - (size_t)FIRST_DATA_BANK * BANK);
    for (auto &kv : images) memcpy(&rom[(size_t)kv.first * BANK], kv.second.data(), BANK);
    put_bytes(rom, 2, LEVEL_ADDR, level);
    put_bytes(rom, MISC_BANK, 0x8000, strings);

    // ---- hardware sprites, large then small view
    // Per view size, every thing type's 4 records packed into banks after
    // the tables, and a directory in bank 4 (64 types x 4 distances x
    // 4 bytes: bank, u16 address, 0). The large view's pass also collects
    // each type's 16 colour RAM bytes, stored after the large directory.
    opt.say("drawing the sprites at each distance...");
    int first_sprite = next_bank;
    Bytes pals;
    for (int size = 0; size < 2; size++) {
        std::vector<SpriteSet> sets(q.defs.size());
        parallel_for((int)q.defs.size(), opt.n_threads(),
                     [&](int i) { sets[i] = sprite_set(q.flat(q.sprites.at(q.defs[i].tile)), size); });
        Bytes dir;
        if (size == 0)
            for (auto &s : sets) pals.insert(pals.end(), s.cram, s.cram + 16);
        int bank = next_bank;
        size_t used = 0;
        for (auto &s : sets)
            for (int k = 0; k < 4; k++) {
                const Bytes &r = s.records[k];
                if (used + r.size() > (size_t)BANK) {   // a record never crosses a bank
                    bank++;
                    used = 0;
                }
                if (bank >= 256) {
                    err = "the sprites don't fit in a 4 MB ROM";
                    return false;
                }
                unsigned addr = 0x8000 + (unsigned)used;
                dir.push_back((u8)bank);
                dir.push_back(addr & 0xFF);
                dir.push_back(addr >> 8);
                dir.push_back(0);
                memcpy(&rom[(size_t)bank * BANK + used], r.data(), r.size());
                used += r.size();
            }
        if (q.defs.size() > 64) {
            err = "too many thing types";
            return false;
        }
        dir.resize(64 * 4 * 4, 0);
        put_bytes(rom, MISC_BANK, size == 0 ? SPR_DIR_LARGE : SPR_DIR_SMALL, dir);
        next_bank = bank + 1;
    }
    pals.resize(64 * 16, 0);
    put_bytes(rom, MISC_BANK, SPR_DIR_LARGE + 1024, pals);
    int n_sprite = next_bank - first_sprite;
    opt.at(0.9f);

    // ---- the editable level
    opt.say("packing the editable level...");
    Bytes src = opt.locked ? locked_container() : make_container(p);   // (locked: no editable copy)
    int first_src = next_bank, n_src = (int)((src.size() + BANK - 1) / BANK);
    if (first_src + n_src > 256) {
        err = "no room left in the 4 MB ROM for the editor's copy of the level";
        return false;
    }
    memcpy(&rom[(size_t)first_src * BANK], src.data(), src.size());
    // ROM directory (version 3) at $7FC0: "DRPGDIR1", version, first data
    // bank (9), number of data banks, first sprite bank, sprite bank count,
    // first container bank, container bank count, 0, u32 container size
    u8 *dir = &rom[ROM_DIR];
    memcpy(dir, "DRPGDIR1", 8);
    int last_data = images.rbegin()->first;
    dir[8] = 3;
    dir[9] = FIRST_DATA_BANK;
    dir[10] = (u8)std::max(0, last_data - 8);
    dir[11] = (u8)first_sprite;
    dir[12] = (u8)n_sprite;
    dir[13] = (u8)first_src;
    dir[14] = (u8)n_src;
    dir[15] = 0;
    for (int i = 0; i < 4; i++) dir[16 + i] = (src.size() >> (8 * i)) & 0xFF;

    // ---- size (power of two, at least 128 KB) and header (tools/rom_size.py)
    // the last bank with anything in it
    int last = 0;
    for (int b = 0; b < 256; b++)
        for (int i = 0; i < BANK; i++)
            if (rom[(size_t)b * BANK + i]) {
                last = b;
                break;
            }
    if ((size_t)(last + 1) * BANK > 0x100000) {
        err = "the level needs " + std::to_string((last + 1) * 16) +
              " KB: more than the 1 MB cartridge (fewer different textures, sprites or "
              "open cells would make it smaller)";
        return false;
    }
    size_t size = BANK;
    while (size < (size_t)(last + 1) * BANK) size *= 2;
    size = std::max<size_t>(size, 0x20000);
    rom.resize(size);
    // the "TMR SEGA" header's size code: $F 128 KB, $0 256 KB, $1 512 KB, $2 1 MB
    int code = size == 0x20000 ? 0xF : size == 0x40000 ? 0x0 : size == 0x80000 ? 0x1 : 0x2;
    // checksum: the byte sum of everything but the header ($7FF0-$7FFF),
    // 16 bits, little-endian at $7FFA
    u32 chk = 0;
    for (size_t i = 0; i < 0x7FF0; i++) chk += rom[i];
    for (size_t i = 0x8000; i < size; i++) chk += rom[i];
    chk &= 0xFFFF;
    rom[0x7FFA] = chk & 0xFF;
    rom[0x7FFB] = chk >> 8;
    int region = rom[0x7FFF] >> 4;           // (the engine's: 4 the Master System, 7 the Game Gear)
    rom[0x7FFF] = (region ? region << 4 : 0x40) | code;   // region (none: 4) | size code
    rom_in = std::move(rom);
    stats.rom_kb = (int)(size / 1024);
    stats.used_kb = (last + 1) * 16;
    stats.free_kb = (64 - last - 1) * 16;   // of the 1 MB cartridge
    stats.notes.push_back(std::to_string(d.walls.size()) + " wall pieces, " + std::to_string(d.geo.tex_imgs.size()) +
                          " textures, " + std::to_string(d.pool.size()) + " texture columns, " +
                          std::to_string(d.pvs.size()) + " views, " + std::to_string(d.bills.size()) +
                          " prop sprites");
    opt.at(1.0f);
    return true;
}

}  // namespace sms
