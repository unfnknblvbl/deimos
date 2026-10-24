// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// The Game Boy Color cartridge (gbc.h): its resources made from the SNES
// image as gbc/tools/gb_res.py makes them (whole numbers throughout, the
// first of equals always taken: the same bytes), and its packing
// (gbc/tools/gb_pack.py).
// Licence: GPL-2.0-or-later.
//
// The Game Boy resources ("GBRES1") are 7 entries made from the SNES
// image's resources (font, faces, title) and its campaign block (weapon,
// maps' colours, sound): the Game Boy Color has 4 colours per tile from 8
// palettes, so every picture is reduced. Colours here are C3 (r, g, b in
// 0..255) from Mega Drive CRAM words via the DAC levels LEV; a Game Boy
// colour is a 15-bit word (r | g << 5 | b << 10, 5 bits each),
// little-endian.
#include "gbc.h"
#include "engine.h"
#include "rom.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>

namespace gbc {

typedef std::array<int, 3> C3;   // r, g, b, 0..255
static const int LEV[8] = {0, 52, 87, 116, 144, 172, 206, 255};   // the Mega Drive's 8 levels

// a Mega Drive CRAM word (----BBB-GGG-RRR-) as C3
static C3 md_rgb(unsigned c) { return {LEV[(c >> 1) & 7], LEV[(c >> 5) & 7], LEV[(c >> 9) & 7]}; }
// squared RGB distance
static long dist(const C3 &a, const C3 &b)
{
    long d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2];
    return d0 * d0 + d1 * d1 + d2 * d2;
}
// brightness x 100 (30/59/11 weights)
static long luma(const C3 &c) { return 30L * c[0] + 59L * c[1] + 11L * c[2]; }
// Python's // (rounds towards minus infinity)
static long floordiv(long a, long b)
{
    long q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}
// C3 -> a Game Boy Color word (each channel 0..31, rounded)
static unsigned gb_colour(const C3 &c)
{
    unsigned r = (unsigned)(c[0] * 31 + 127) / 255, g = (unsigned)(c[1] * 31 + 127) / 255,
             b = (unsigned)(c[2] * 31 + 127) / 255;
    return r | (g << 5) | (b << 10);
}
// appends the colours as little-endian Game Boy words
static void gb_pal(Bytes &out, const std::vector<C3> &cols)
{
    for (auto &c : cols) {
        unsigned v = gb_colour(c);
        out.push_back((u8)v);
        out.push_back((u8)(v >> 8));
    }
}
// 64 indices 0..3 (row by row) -> a Game Boy tile
// (16 bytes: per row the low bit plane, then the high one; left pixel in bit 7)
static void gb_tile(Bytes &out, const int *idx)
{
    for (int y = 0; y < 8; y++) {
        int lo = 0, hi = 0;
        for (int x = 0; x < 8; x++) {
            int v = idx[y * 8 + x];
            lo |= (v & 1) << (7 - x);
            hi |= ((v >> 1) & 1) << (7 - x);
        }
        out.push_back((u8)lo);
        out.push_back((u8)hi);
    }
}
// a Mega Drive tile at t[at] -> 64 indices
static void md_tile(const Bytes &t, size_t at, int *px)
{
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) px[y * 8 + x] = (t[at + y * 4 + x / 2] >> (x % 2 == 0 ? 4 : 0)) & 15;
}
// big-endian reads at an offset (unchecked) and append
static unsigned be16(const Bytes &b, size_t o) { return (b[o] << 8) | b[o + 1]; }
static u32 be32(const Bytes &b, size_t o) { return ((u32)b[o] << 24) | ((u32)b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]; }
static void put_be16(Bytes &b, unsigned v)
{
    b.push_back((u8)(v >> 8));
    b.push_back((u8)v);
}

// the distances between every two colours of a palette
typedef std::vector<std::vector<long>> Dist;
static Dist dists(const std::vector<C3> &pal)
{
    Dist D(pal.size(), std::vector<long>(pal.size()));
    for (size_t i = 0; i < pal.size(); i++)
        for (size_t j = 0; j < pal.size(); j++) D[i][j] = dist(pal[i], pal[j]);
    return D;
}
// the error of showing a histogram (pixel count per palette colour) with
// only the colours in c: each colour's count x its distance to the nearest
static long set_error(const std::vector<long> &hist, const Dist &D, const std::vector<int> &c)
{
    long e = 0;
    for (size_t i = 0; i < hist.size(); i++)
        if (hist[i]) {
            long m = -1;
            for (int j : c)
                if (m < 0 || D[i][j] < m) m = D[i][j];
            e += hist[i] * m;
        }
    return e;
}
// the k of cands that stand for the histogram best (combinations in order)
// (every k-combination is tried, in lexicographic order; ties keep the first)
static std::vector<int> best_set(const std::vector<long> &hist, const Dist &D, int k, const std::vector<int> &cands)
{
    int n = (int)cands.size();
    std::vector<int> p(k), best;
    for (int i = 0; i < k; i++) p[i] = i;
    long be = -1;
    for (;;) {
        std::vector<int> c(k);
        for (int i = 0; i < k; i++) c[i] = cands[p[i]];
        long e = set_error(hist, D, c);
        if (be < 0 || e < be) {
            be = e;
            best = c;
        }
        // the next combination of positions p
        int i = k - 1;
        while (i >= 0 && p[i] == n - k + i) i--;
        if (i < 0) break;
        p[i]++;
        for (int j = i + 1; j < k; j++) p[j] = p[j - 1] + 1;
    }
    return best;
}
// the position in c of the colour nearest colour i (ties: the first)
static int nearest(const Dist &D, int i, const std::vector<int> &c)
{
    long best = -1;
    int at = 0;
    for (size_t k = 0; k < c.size(); k++)
        if (best < 0 || D[i][c[k]] < best) {
            best = D[i][c[k]];
            at = (int)k;
        }
    return at;
}

// ------------------------------------------------------------ the SNES's data
// the parts of a SNES image the Game Boy needs
struct Snes {
    size_t res_at = 0, blk_at = 0;   // where its resources and its campaign block start
    Bytes data, block;               // from res_at / blk_at to the last byte that isn't $FF
    std::vector<Bytes> ent;          // the resources' entries (E_* below)
    // entry i as big-endian words
    std::vector<unsigned> words(int i) const
    {
        std::vector<unsigned> w;
        for (size_t k = 0; k + 1 < ent[i].size(); k += 2) w.push_back(be16(ent[i], k));
        return w;
    }
};
// the SNES resources' entries (the Mega Drive's formats)
enum { E_FONT, E_FACES, E_ICONS, E_HUD_PAL, E_DLG_PAL, E_TITLE_TILES, E_TITLE_MAP, E_TITLE_PAL, E_SPC };

// finds the resources and the block through the SNES header ($FFB7: the
// resources' bank, $FFB6: the block's, each $C0 + n) and reads the
// resources' directory ("DRPGRES1", u16 count at 8, then from 12 per
// entry u32 offset and u32 size, little-endian)
static bool parse(const Bytes &rom, Snes &s, std::string &err)
{
    if (rom.size() < 0x10000) return err = "not a SNES image", false;
    s.res_at = (size_t)(rom[0xFFB7] & 0x3F) * 0x10000;
    s.blk_at = (size_t)(rom[0xFFB6] & 0x3F) * 0x10000;
    size_t end = rom.size();
    while (end > s.res_at && rom[end - 1] == 0xFF) end--;
    if (s.res_at + 12 > end || s.blk_at < s.res_at || s.blk_at > end || memcmp(&rom[s.res_at], "DRPGRES1", 8) != 0)
        return err = "the SNES image has no resources", false;
    s.data.assign(rom.begin() + (long)s.res_at, rom.begin() + (long)end);
    s.block.assign(rom.begin() + (long)s.blk_at, rom.begin() + (long)end);
    unsigned n = get16(&s.data[8]);
    for (unsigned i = 0; i < n; i++) {
        u32 o = get32(&s.data[12 + 8 * i]), sz = get32(&s.data[16 + 8 * i]);
        if ((size_t)o + sz > s.data.size()) return err = "the SNES image's resources are damaged", false;
        s.ent.emplace_back(s.data.begin() + o, s.data.begin() + o + sz);
    }
    if (s.ent.size() < 9) return err = "the SNES image's resources are too old", false;
    return true;
}

// the font: 96 glyphs, 8 bytes each, one bit per pixel (the Mega Drive
// font's colour-1 pixels, columns 1..6 only)
static Bytes font(const Snes &s)
{
    const Bytes &t = s.ent[E_FONT];
    Bytes out;
    int px[64];
    for (int g = 0; g < 96; g++) {
        md_tile(t, 32 * g, px);
        for (int y = 0; y < 8; y++) {
            int b = 0;
            for (int x = 1; x < 7; x++)
                if (px[y * 8 + x] == 1) b |= 0x80 >> x;
            out.push_back((u8)b);
        }
    }
    return out;
}

// The status bar's 9 faces (40 x 40, 25 tiles each, column by column):
// colours 1..14, sorted by brightness, are split into 3 bands holding a
// third of the pixels each; each band becomes its average colour (0 and
// 15 become colour 0). tiles: 9 x 25 Game Boy tiles row by row; pal_out:
// one 4-colour palette (black, then the bands dark to light).
static void faces(const Snes &s, Bytes &tiles, Bytes &pal_out)
{
    const Bytes &t = s.ent[E_FACES];
    std::vector<C3> pal;
    for (unsigned c : s.words(E_HUD_PAL)) pal.push_back(md_rgb(c));
    std::vector<std::vector<int>> imgs;
    std::vector<long> hist(16, 0);
    int px[64];
    for (int f = 0; f < 9; f++) {
        std::vector<int> im(40 * 40, 15);
        for (int tx = 0; tx < 5; tx++)
            for (int ty = 0; ty < 5; ty++) {
                md_tile(t, 32 * (f * 25 + tx * 5 + ty), px);
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) im[(ty * 8 + y) * 40 + tx * 8 + x] = px[y * 8 + x];
            }
        for (int v : im) hist[v]++;
        imgs.push_back(im);
    }
    std::vector<int> used;
    for (int i = 1; i < 15; i++)
        if (hist[i]) used.push_back(i);
    std::stable_sort(used.begin(), used.end(), [&](int a, int b) {
        long la = luma(pal[a]), lb = luma(pal[b]);
        return la != lb ? la < lb : a < b;
    });
    long total = 0;
    for (int i : used) total += hist[i];
    std::map<int, int> group;
    long acc = 0;
    for (int i : used) {
        group[i] = (int)std::min(2L, acc * 3 / total);
        acc += hist[i];
    }
    std::vector<C3> cols = {{0, 0, 0}};
    for (int g = 0; g < 3; g++) {
        long w = 0, sum[3] = {0, 0, 0};
        for (int i : used)
            if (group[i] == g) {
                w += hist[i];
                for (int k = 0; k < 3; k++) sum[k] += hist[i] * pal[i][k];
            }
        if (!w) w = 1;
        cols.push_back({(int)(sum[0] / w), (int)(sum[1] / w), (int)(sum[2] / w)});
    }
    for (auto &im : imgs)
        for (int ty = 0; ty < 5; ty++)
            for (int tx = 0; tx < 5; tx++) {
                int idx[64];
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) {
                        int v = im[(ty * 8 + y) * 40 + tx * 8 + x];
                        auto it = group.find(v);
                        idx[y * 8 + x] = it == group.end() ? 0 : it->second + 1;
                    }
                gb_tile(tiles, idx);
            }
    gb_pal(pal_out, cols);
}

// ------------------------------------------------------------ the title's logo
// The SNES title's 240 x 120 logo area, reduced to 2/3 (160 x 80, 20 x 10
// tiles) by a weighted vote, then given 7 palettes of 4 of its 16 colours:
// tiles are grouped by their main colour, then 6 rounds of (best 4-colour
// set per group, each tile to the set that shows it best). out: u16 tile
// count, the unique tiles, the 200-byte map, the 200 palette numbers, the
// 7 palettes.
static bool title(const Snes &s, Bytes &out, std::string &err)
{
    const int CROP_X = 8, CROP_Y = 8, CROP_W = 240, CROP_H = 120, W = 256, H = 224;   // (gb_res.py's: the moon and the DEIMOS logo)
    const int SUB_Y0 = 92, SUB_Y1 = 110;   // the subtitle's rows, covered with the stars under them (gb_res.py)
    const Bytes &tiles = s.ent[E_TITLE_TILES];
    std::vector<unsigned> tmap = s.words(E_TITLE_MAP);
    std::vector<C3> pal;
    for (unsigned c : s.words(E_TITLE_PAL)) pal.push_back(md_rgb(c));
    if (pal.size() < 16 || tmap.size() < 32 * 28) return err = "the SNES title is damaged", false;
    std::vector<int> src(W * H);   // the whole SNES title as colour indices
    int px[64];
    for (int ty = 0; ty < H / 8; ty++)
        for (int tx = 0; tx < W / 8; tx++) {
            size_t at = 32 * (size_t)(tmap[ty * 32 + tx] & 0x7FF);
            if (at + 32 > tiles.size()) return err = "the SNES title is damaged", false;
            md_tile(tiles, at, px);
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) src[(ty * 8 + y) * W + tx * 8 + x] = px[y * 8 + x];
        }
    for (int y = SUB_Y0; y < SUB_Y1; y++)
        for (int x = 0; x < W; x++) src[y * W + x] = src[(y + SUB_Y1 - SUB_Y0) * W + x];
    // 3 source pixels -> 2: output pixel 0 takes source 0 (weight 2) and 1
    // (weight 1), output pixel 1 takes source 1 (1) and 2 (2); the colour
    // with the most weight wins
    static const int WT[2][2][2] = {{{0, 2}, {1, 1}}, {{1, 1}, {2, 2}}};   // (offset, weight)
    const int OW = CROP_W * 2 / 3, OH = CROP_H * 2 / 3;
    std::vector<int> img(OW * OH);
    for (int oy = 0; oy < OH; oy++)
        for (int ox = 0; ox < OW; ox++) {
            int cnt[16] = {0};
            for (int a = 0; a < 2; a++)
                for (int b = 0; b < 2; b++) {
                    int sy = 3 * (oy / 2) + WT[oy % 2][a][0], wy = WT[oy % 2][a][1];
                    int sx = 3 * (ox / 2) + WT[ox % 2][b][0], wx = WT[ox % 2][b][1];
                    cnt[src[(CROP_Y + sy) * W + CROP_X + sx]] += wy * wx;
                }
            int best = 0;
            for (int i = 1; i < 16; i++)
                if (cnt[i] > cnt[best]) best = i;
            img[oy * OW + ox] = best;
        }
    Dist D = dists(pal);
    const int TW = 20, TH = 10;
    std::vector<std::vector<long>> hists;
    for (int ty = 0; ty < TH; ty++)
        for (int tx = 0; tx < TW; tx++) {
            std::vector<long> h(16, 0);
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) h[img[(ty * 8 + y) * OW + tx * 8 + x]]++;
            hists.push_back(h);
        }
    size_t NT = hists.size();
    std::vector<int> mainc(NT);
    for (size_t t = 0; t < NT; t++) {
        int best = 0;
        for (int i = 1; i < 16; i++)
            if (hists[t][i] > hists[t][best]) best = i;
        mainc[t] = best;
    }
    std::vector<int> tally(16, 0);
    for (int m : mainc) tally[m]++;
    std::vector<int> order(16);
    for (int i = 0; i < 16; i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return tally[a] != tally[b] ? tally[a] > tally[b] : a < b; });
    // the 7 most common main colours seed the groups
    std::vector<int> seeds(order.begin(), order.begin() + 7);
    std::vector<int> lab(NT);
    for (size_t t = 0; t < NT; t++) {
        auto it = std::find(seeds.begin(), seeds.end(), mainc[t]);
        if (it != seeds.end()) lab[t] = (int)(it - seeds.begin());
        else {
            int bj = 0;
            for (int j = 1; j < 7; j++)
                if (D[mainc[t]][seeds[j]] < D[mainc[t]][seeds[bj]]) bj = j;
            lab[t] = bj;
        }
    }
    std::vector<std::vector<int>> sets(7);
    std::vector<int> cands(16);
    for (int i = 0; i < 16; i++) cands[i] = i;
    for (int round = 0; round < 6; round++) {
        for (int j = 0; j < 7; j++) {
            std::vector<long> h(16, 0);
            long sum = 0;
            for (size_t t = 0; t < NT; t++)
                if (lab[t] == j)
                    for (int i = 0; i < 16; i++) h[i] += hists[t][i];
            for (long v : h) sum += v;
            sets[j] = sum ? best_set(h, D, 4, cands) : std::vector<int>{0, 1, 2, 3};
        }
        for (size_t t = 0; t < NT; t++) {
            int bj = 0;
            long be = set_error(hists[t], D, sets[0]);
            for (int j = 1; j < 7; j++) {
                long e = set_error(hists[t], D, sets[j]);
                if (e < be) {
                    be = e;
                    bj = j;
                }
            }
            lab[t] = bj;
        }
    }
    // the tiles, identical ones stored once
    std::vector<Bytes> gtiles;
    std::map<Bytes, int> seen;
    Bytes gmap, attr;
    for (size_t t = 0; t < NT; t++) {
        int ty = (int)t / TW, tx = (int)t % TW, idx[64];
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) idx[y * 8 + x] = nearest(D, img[(ty * 8 + y) * OW + tx * 8 + x], sets[lab[t]]);
        Bytes b;
        gb_tile(b, idx);
        auto it = seen.find(b);
        if (it == seen.end()) {
            it = seen.emplace(b, (int)gtiles.size()).first;
            gtiles.push_back(b);
        }
        gmap.push_back((u8)it->second);
        attr.push_back((u8)lab[t]);
    }
    if (gtiles.size() > 200) return err = "the title's logo has too many tiles for the Game Boy", false;
    put_be16(out, (unsigned)gtiles.size());
    for (auto &g : gtiles) out.insert(out.end(), g.begin(), g.end());
    out.insert(out.end(), gmap.begin(), gmap.end());
    out.insert(out.end(), attr.begin(), attr.end());
    for (auto &c : sets) {
        std::vector<C3> cols;
        for (int i : c) cols.push_back(pal[i]);
        gb_pal(out, cols);
    }
    return true;
}

// ------------------------------------------------------------ the weapons
// The 18 weapon frames, read back from the block's Mega Drive sprites,
// halved in size (a 2 x 2 vote, at least 2 opaque pixels), cut into 8 x 16
// sprites and given 2 palettes of 3 colours (sprites sorted by redness
// into two halves, then 4 rounds of best sets). out: a u16 offset per
// frame, then per frame: sprite count, tile count, the 2 palettes (4
// colours, the first transparent), per sprite y, x, first tile, palette,
// then the tiles. (Only part of the reads below are bounds-checked.)
static bool weapons(const Bytes &blk, Bytes &out, std::string &err)
{
    auto chk = [&](size_t o, size_t n) { return o + n <= blk.size(); };   // n bytes at o are inside
    if (!chk(16, 4)) return err = "no weapons in the block", false;
    u32 wt = be32(blk, 16);   // the weapon directory
    if (!chk(wt, 32)) return err = "no weapons in the block", false;
    u32 tab[4];
    for (int k = 0; k < 4; k++) tab[k] = be32(blk, wt + 8 * k);
    std::vector<Bytes> recs;
    for (int fid = 0; fid < 18; fid++) {
        size_t f = tab[0] + fid * 10;
        if (!chk(f, 10)) return err = "the weapons are damaged", false;
        unsigned t0 = be16(blk, f), pal_i = be16(blk, f + 4), sp_i = be16(blk, f + 6), n = be16(blk, f + 8);
        std::vector<C3> pal;
        for (int k = 0; k < 16; k++) pal.push_back(md_rgb(be16(blk, tab[2] + pal_i * 32 + 2 * k)));
        // the frame's opaque pixels: (y, x) on the 256 x 160 view -> colour
        std::map<std::pair<int, int>, int> canvas;
        int px[64];
        for (unsigned k = 0; k < n; k++) {
            size_t s = tab[1] + (sp_i + k) * 8;
            int x = (int)be16(blk, s), y = (int)be16(blk, s + 2), sz = (int)be16(blk, s + 4), tile = (int)be16(blk, s + 6);
            int w8 = ((sz >> 2) & 3) + 1, h8 = (sz & 3) + 1;
            for (int cx = 0; cx < w8; cx++)
                for (int cy = 0; cy < h8; cy++) {
                    size_t at = tab[3] + (size_t)(t0 + tile + cx * h8 + cy) * 32;
                    if (!chk(at, 32)) return err = "the weapons are damaged", false;
                    md_tile(blk, at, px);
                    for (int r = 0; r < 8; r++)
                        for (int c = 0; c < 8; c++)
                            if (px[r * 8 + c]) canvas[{y + cy * 8 + r, x + cx * 8 + c}] = px[r * 8 + c];
                }
        }
        if (canvas.empty()) return err = "a weapon frame is empty", false;
        int y0 = 1 << 30, y1 = -1, x0 = 1 << 30, x1 = -1;
        for (auto &p : canvas) {
            y0 = std::min(y0, p.first.first);
            y1 = std::max(y1, p.first.first + 1);
            x0 = std::min(x0, p.first.second);
            x1 = std::max(x1, p.first.second + 1);
        }
        // align the box to even screen pixels (y counted from 16 above the view)
        y0 -= (y0 + 16) % 2;
        x0 -= x0 % 2;
        int h = (y1 - y0 + 1) / 2, w = (x1 - x0 + 1) / 2;
        std::vector<int> small(h * w, 0);
        for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < w; xx++) {
                int cnt[16] = {0}, tot = 0;
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++) {
                        auto it = canvas.find({y0 + 2 * yy + dy, x0 + 2 * xx + dx});
                        if (it != canvas.end() && it->second) {
                            cnt[it->second]++;
                            tot++;
                        }
                    }
                if (tot >= 2) {
                    int best = 1;
                    for (int i = 2; i < 16; i++)
                        if (cnt[i] > cnt[best]) best = i;
                    small[yy * w + xx] = best;
                }
            }
        int gy0 = (y0 + 16) / 2 + 4, gx0 = x0 / 2;   // the frame's corner on the Game Boy screen
        Dist D = dists(pal);
        struct Cell {
            int sy, sx;
            std::vector<long> hst;
        };
        std::vector<Cell> cells;
        for (int sy = 0; sy < h; sy += 16)
            for (int sx = 0; sx < w; sx += 8) {
                std::vector<long> hst(16, 0);
                for (int y = sy; y < std::min(h, sy + 16); y++)
                    for (int x = sx; x < std::min(w, sx + 8); x++) hst[small[y * w + x]]++;
                hst[0] = 0;
                long sum = 0;
                for (long v : hst) sum += v;
                if (sum) cells.push_back({sy, sx, hst});
            }
        size_t NC = cells.size();
        // each sprite's average red minus blue (x16), for the first split
        std::vector<long> red(NC);
        for (size_t k = 0; k < NC; k++) {
            long a = 0, sum = 0;
            for (int i = 0; i < 16; i++) {
                a += cells[k].hst[i] * (pal[i][0] - pal[i][2]);
                sum += cells[k].hst[i];
            }
            red[k] = floordiv(a * 16, sum);
        }
        std::vector<int> order(NC);
        for (size_t k = 0; k < NC; k++) order[k] = (int)k;
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return red[a] != red[b] ? red[a] < red[b] : a < b; });
        std::vector<int> lab(NC, 0);
        for (size_t r = 0; r < NC; r++) lab[order[r]] = 2 * r >= NC ? 1 : 0;
        std::vector<std::vector<int>> sets(2);
        std::vector<int> cands;
        for (int i = 1; i < 16; i++) cands.push_back(i);
        for (int round = 0; round < 4; round++) {
            for (int j = 0; j < 2; j++) {
                std::vector<long> hh(16, 0);
                long sum = 0;
                for (size_t k = 0; k < NC; k++)
                    if (lab[k] == j)
                        for (int i = 0; i < 16; i++) hh[i] += cells[k].hst[i];
                for (long v : hh) sum += v;
                sets[j] = sum ? best_set(hh, D, 3, cands) : std::vector<int>{1, 2, 3};
            }
            for (size_t k = 0; k < NC; k++) lab[k] = set_error(cells[k].hst, D, sets[1]) < set_error(cells[k].hst, D, sets[0]) ? 1 : 0;
        }
        Bytes tl, spr;
        for (size_t k = 0; k < NC; k++) {
            int idx[128];
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 8; x++) {
                    int yy = cells[k].sy + y, xx = cells[k].sx + x;
                    idx[y * 8 + x] = (yy < h && xx < w && small[yy * w + xx]) ? nearest(D, small[yy * w + xx], sets[lab[k]]) + 1 : 0;
                }
            spr.push_back((u8)((gy0 + cells[k].sy) & 255));
            spr.push_back((u8)((gx0 + cells[k].sx) & 255));
            spr.push_back((u8)(tl.size() / 16));
            spr.push_back((u8)lab[k]);
            gb_tile(tl, idx);
            gb_tile(tl, idx + 64);
        }
        if (tl.size() / 16 > 128) return err = "a weapon frame has too many tiles for the Game Boy", false;   // (16 bytes a tile)
        Bytes rec = {(u8)NC, (u8)(tl.size() / 16)};
        for (auto &c : sets) {
            std::vector<C3> cols = {{0, 0, 0}};
            for (int i : c) cols.push_back(pal[i]);
            gb_pal(rec, cols);
        }
        rec.insert(rec.end(), spr.begin(), spr.end());
        rec.insert(rec.end(), tl.begin(), tl.end());
        recs.push_back(rec);
    }
    // 18 u16 offsets (36 bytes), then the records
    Bytes head, body;
    for (auto &r : recs) {
        put_be16(head, (unsigned)(36 + body.size()));
        body.insert(body.end(), r.begin(), r.end());
    }
    out = head;
    out.insert(out.end(), body.begin(), body.end());
    return true;
}

// ------------------------------------------------------------ the maps' colours
// a colour minus its grey (luma / 100)
static C3 chroma(const C3 &c)
{
    int y = (int)(luma(c) / 100);
    return {c[0] - y, c[1] - y, c[2] - y};
}

// Splits 9 colours into 3 "families" of 3 whose chroma is closest
// (every split tried: the first family holds colour 0, the second the
// lowest one left, so each split is tried once); each family sorted dark
// to light.
static std::vector<std::vector<int>> families(const std::vector<C3> &C)
{
    std::vector<C3> ch;
    for (auto &c : C) ch.push_back(chroma(c));
    long best = -1;
    std::vector<std::vector<int>> bg;
    for (int a0 = 0; a0 < 9; a0++)
        for (int a1 = a0 + 1; a1 < 9; a1++)
            for (int a2 = a1 + 1; a2 < 9; a2++) {
                if (a0 != 0) continue;
                std::vector<int> a = {a0, a1, a2}, rest;
                for (int i = 0; i < 9; i++)
                    if (i != a0 && i != a1 && i != a2) rest.push_back(i);
                for (int b0 = 0; b0 < 6; b0++)
                    for (int b1 = b0 + 1; b1 < 6; b1++)
                        for (int b2 = b1 + 1; b2 < 6; b2++) {
                            std::vector<int> b = {rest[b0], rest[b1], rest[b2]}, c;
                            if (b[0] != rest[0]) continue;
                            for (int i : rest)
                                if (i != b[0] && i != b[1] && i != b[2]) c.push_back(i);
                            long cost = 0;
                            for (auto *g : {&a, &b, &c})
                                cost += dist(ch[(*g)[0]], ch[(*g)[1]]) + dist(ch[(*g)[0]], ch[(*g)[2]]) +
                                        dist(ch[(*g)[1]], ch[(*g)[2]]);
                            if (best < 0 || cost < best) {
                                best = cost;
                                bg = {a, b, c};
                            }
                        }
            }
    for (auto &g : bg)
        std::stable_sort(g.begin(), g.end(), [&](int x, int y) {
            long lx = luma(C[x]), ly = luma(C[y]);
            return lx != ly ? lx < ly : x < y;
        });
    return bg;
}

// 9 colours standing for a map's weighted colours (CRAM word -> weight):
// the heaviest first, then each time the colour with the largest weight x
// distance to the chosen ones, then 8 rounds of weighted k-means (integer
// averages)
static std::vector<C3> nine(const std::map<unsigned, long> &wmap)
{
    std::vector<C3> rgb;
    std::vector<long> wt;
    for (auto &p : wmap) {
        rgb.push_back(md_rgb(p.first));
        wt.push_back(p.second);
    }
    int n = (int)rgb.size();
    std::vector<C3> C;
    {
        int bi = 0;
        for (int i = 1; i < n; i++)
            if (wt[i] > wt[bi]) bi = i;
        C.push_back(rgb[bi]);
    }
    while (C.size() < 9) {
        int bi = 0;
        long bv = -1;
        for (int i = 0; i < n; i++) {
            long md = -1;
            for (auto &c : C) {
                long d = dist(rgb[i], c);
                if (md < 0 || d < md) md = d;
            }
            long v = wt[i] * md;
            if (v > bv) {
                bv = v;
                bi = i;
            }
        }
        C.push_back(rgb[bi]);
    }
    for (int round = 0; round < 8; round++) {
        std::vector<int> lab(n);
        for (int i = 0; i < n; i++) {
            int bj = 0;
            for (int j = 1; j < 9; j++)
                if (dist(rgb[i], C[j]) < dist(rgb[i], C[bj])) bj = j;
            lab[i] = bj;
        }
        for (int j = 0; j < 9; j++) {
            long tw = 0, s[3] = {0, 0, 0};
            for (int i = 0; i < n; i++)
                if (lab[i] == j) {
                    tw += wt[i];
                    for (int k = 0; k < 3; k++) s[k] += wt[i] * rgb[i][k];
                }
            if (tw) C[j] = {(int)(s[0] / tw), (int)(s[1] / tw), (int)(s[2] / tw)};
        }
    }
    return C;
}

// Per campaign map: its textures' and sprites' colours weighted by their
// histograms (+1), reduced to nine() and families(); out gets 512 bytes
// (for each 9-bit colour b2..0 g2..0 r2..0: family << 2 | shade 1..3 of
// its nearest of the nine) and the 3 families' palettes.
static bool maps(const Bytes &blk, Bytes &out, std::string &err)
{
    unsigned n_maps = be32(blk, 4) & 0xFF;   // (the low byte of the u16 map count at 6)
    for (unsigned m = 0; m < n_maps; m++) {
        u32 pk = be32(blk, 32 + 32 * m);
        if (pk + 32 + 8 * 17 > blk.size()) return err = "a map pack is damaged", false;
        unsigned n_tex = be16(blk, pk + 10), n_spr = be16(blk, pk + 12);   // from the pack's counts
        // entry e's offset (2 tex_pal, 3 tex_hist, 9 spr_pal, 10 spr_hist)
        auto ent = [&](int e) { return be32(blk, pk + 32 + 8 * e); };
        std::map<unsigned, long> wmap;
        for (unsigned t = 0; t < n_tex; t++)
            for (int k = 0; k < 32; k++) wmap[be16(blk, ent(2) + 64 * t + 2 * k)] += be16(blk, ent(3) + 64 * t + 2 * k) + 1;
        for (unsigned s = 0; s < n_spr; s++)
            for (int k = 0; k < 16; k++) wmap[be16(blk, ent(9) + 32 * s + 2 * k)] += be16(blk, ent(10) + 32 * s + 2 * k) + 1;
        std::vector<C3> C = nine(wmap);
        auto fams = families(C);
        int place[9];
        for (int f = 0; f < 3; f++)
            for (int s = 0; s < 3; s++) place[fams[f][s]] = (f << 2) | (s + 1);
        for (int i9 = 0; i9 < 512; i9++) {
            C3 p = md_rgb(((i9 & 7) << 1) | (((i9 >> 3) & 7) << 5) | (((i9 >> 6) & 7) << 9));
            int bj = 0;
            for (int j = 1; j < 9; j++)
                if (dist(p, C[j]) < dist(p, C[bj])) bj = j;
            out.push_back((u8)place[bj]);
        }
        for (auto &g : fams) {
            std::vector<C3> cols;
            for (int i : g) cols.push_back(C[i]);
            gb_pal(out, cols);
        }
    }
    return true;
}


// see gbc.h: the same as maps() for one map, for the preview (fam: the
// families' colours as the Game Boy shows them)
void view_colours(const std::map<unsigned, long> &wmap, u8 place[512], RGB fam[3][3])
{
    if (wmap.empty()) {   // no colours: three greys, everything family 0 shade 1
        for (int i = 0; i < 512; i++) place[i] = 1;
        for (int f = 0; f < 3; f++)
            for (int k = 0; k < 3; k++) fam[f][k] = {(u8)(80 * k + 60), (u8)(80 * k + 60), (u8)(80 * k + 60)};
        return;
    }
    std::vector<C3> C = nine(wmap);
    auto fams = families(C);
    int pl[9];
    for (int f = 0; f < 3; f++)
        for (int s = 0; s < 3; s++) {
            pl[fams[f][s]] = (f << 2) | (s + 1);
            unsigned v = gb_colour(C[fams[f][s]]);
            fam[f][s] = {(u8)((v & 31) * 255 / 31), (u8)(((v >> 5) & 31) * 255 / 31), (u8)(((v >> 10) & 31) * 255 / 31)};
        }
    for (int i9 = 0; i9 < 512; i9++) {
        C3 p = md_rgb(((i9 & 7) << 1) | (((i9 >> 3) & 7) << 5) | (((i9 >> 6) & 7) << 9));
        int bj = 0;
        for (int j = 1; j < 9; j++)
            if (dist(p, C[j]) < dist(p, C[bj])) bj = j;
        place[i9] = (u8)pl[bj];
    }
}

// ------------------------------------------------------------ the sound
// A song's 7-byte voice -> Game Boy channel map. The stream (after its
// first byte) is scanned: $00-$7F 1 byte, $80-$87 a note on voice c & 7
// (3 bytes, its pitch next), $88-$8F 1 byte, $90 and up 2 bytes, $FF the
// end. Of voices 0..4, the 3 used most are kept: the lowest on average
// gets channel 2, the others 0 and 1; voices 5 and 6 get 3; the rest 255
// (silent).
static Bytes song_map(const Bytes &st)
{
    std::vector<long> cnt(7, 0), sum(7, 0);
    size_t i = 1;
    while (i < st.size()) {
        u8 c = st[i];
        if (c == 0xFF) break;
        if (c < 0x80) i += 1;
        else if (c < 0x88) {
            if (i + 1 < st.size()) {
                cnt[c & 7]++;
                sum[c & 7] += st[i + 1];
            }
            i += 3;
        } else if (c < 0x90) i += 1;
        else i += 2;
    }
    std::vector<int> top;
    for (int v = 0; v < 5; v++)
        if (cnt[v]) top.push_back(v);
    std::stable_sort(top.begin(), top.end(), [&](int a, int b) { return cnt[a] != cnt[b] ? cnt[a] > cnt[b] : a < b; });
    if (top.size() > 3) top.resize(3);
    Bytes m(7, 255);
    if (!top.empty()) {
        int bass = top[0];
        long bk = sum[bass] * 1000 / cnt[bass];
        for (int v : top) {
            long k = sum[v] * 1000 / cnt[v];
            if (k < bk || (k == bk && v < bass)) {
                bk = k;
                bass = v;
            }
        }
        m[bass] = 2;
        int ch = 0;
        for (int v : top)
            if (v != bass) m[v] = (u8)ch++;
    }
    m[5] = m[6] = 3;
    return m;
}

// The SPC entry's streams (3 songs, then n effects; their addresses are
// SPC addresses from $2000) -> "GBSN": u16 offset of each song, u16 n,
// u16 offset of each effect, then the streams (each song after its
// channel map).
static bool sound(const Snes &s, Bytes &out, std::string &err)
{
    const Bytes &sp = s.ent[E_SPC];
    if (sp.size() < 8) return err = "no sound streams", false;
    unsigned n = get16(&sp[6]);
    if (sp.size() < 8 + 2 * (size_t)n) return err = "the sound streams are damaged", false;
    std::vector<size_t> starts;
    for (int k = 0; k < 3; k++) starts.push_back(get16(&sp[2 * k]) - 0x2000);
    for (unsigned k = 0; k < n; k++) starts.push_back(get16(&sp[8 + 2 * k]) - 0x2000);
    starts.push_back(sp.size());
    size_t head = 4 + 6 + 2 + 2 * (size_t)n;
    Bytes body;
    std::vector<size_t> at;
    for (size_t k = 0; k + 1 < starts.size(); k++) {
        if (starts[k] > starts[k + 1] || starts[k + 1] > sp.size()) return err = "the sound streams are damaged", false;
        Bytes st(sp.begin() + (long)starts[k], sp.begin() + (long)starts[k + 1]);
        at.push_back(head + body.size());
        if (k < 3) {
            Bytes m = song_map(st);
            body.insert(body.end(), m.begin(), m.end());
        }
        body.insert(body.end(), st.begin(), st.end());
    }
    out = {'G', 'B', 'S', 'N'};
    for (int k = 0; k < 3; k++) put_be16(out, (unsigned)at[k]);
    put_be16(out, n);
    for (unsigned k = 0; k < n; k++) put_be16(out, (unsigned)at[3 + k]);
    out.insert(out.end(), body.begin(), body.end());
    return true;
}

// The resources: "GBRES1", 0, 7 (entry count), u32 the block's offset from
// the SNES data's start, 7 u32 entry offsets (all big-endian), then the
// entries (font, face tiles, face palette, title, weapon, maps' colours,
// sound) at even offsets, the sound on a 16 KB boundary.
static bool build_res(const Snes &s, Bytes &res, std::string &err)
{
    std::vector<Bytes> ents(7);
    ents[0] = font(s);
    faces(s, ents[1], ents[2]);
    if (!title(s, ents[3], err) || !weapons(s.block, ents[4], err) || !maps(s.block, ents[5], err) ||
        !sound(s, ents[6], err))
        return false;
    Bytes head = {'G', 'B', 'R', 'E', 'S', '1', 0, 7};
    u32 blk = (u32)(s.blk_at - s.res_at);
    for (int k = 3; k >= 0; k--) head.push_back((u8)(blk >> (8 * k)));
    head.resize(12 + 4 * 7, 0);
    Bytes body;
    for (int i = 0; i < 7; i++) {
        while ((head.size() + body.size()) % (i == 6 ? 0x4000 : 2)) body.push_back(0);
        u32 o = (u32)(head.size() + body.size());
        for (int k = 0; k < 4; k++) head[12 + 4 * i + k] = (u8)(o >> (8 * (3 - k)));
        body.insert(body.end(), ents[i].begin(), ents[i].end());
    }
    res = head;
    res.insert(res.end(), body.begin(), body.end());
    return true;
}

// see gbc.h
bool resources(const Bytes &image, Bytes &res, std::string &err)
{
    Snes s;
    return parse(image, s, err) && build_res(s, res, err);
}

// ------------------------------------------------------------ the cartridge
static const size_t SIZE = 0x400000;   // the cartridge: 4 MB

// where the Game Boy resources start (a 16 KB boundary after DATA_AT), or 0
static size_t find_res(const Bytes &f)
{
    for (size_t b = DATA_AT; b + 6 <= f.size(); b += 0x4000)
        if (memcmp(&f[b], "GBRES1", 6) == 0) return b;
    return 0;
}

// see gbc.h ($143 bit 7: the Game Boy Color flag; $134: the title)
bool is_gbc(const Bytes &f)
{
    return f.size() == SIZE && (f[0x143] & 0x80) && game_title(&f[0x134]) && find_res(f) != 0;
}

// see gbc.h
bool unwrap(const Bytes &f, Bytes &w, Bytes &image, std::string &err)
{
    if (!is_gbc(f)) return err = "this is not Deimos (Doom RPG) for the Game Boy Color", false;
    size_t r = find_res(f);
    // the SNES engine's part before its resources, then the cartridge's SNES data
    engine::Engine e;
    if (!engine::load(Console::GBC, e, err)) return false;
    size_t res_at = 0;
    for (auto &rg : e.regions)
        if (rg.name == "snes_res") res_at = rg.offset;
    if (!res_at || res_at > e.rom.size()) return err = "the editor's Game Boy Color engine is damaged", false;
    w.assign(f.begin(), f.begin() + DATA_AT);
    image.assign(e.rom.begin(), e.rom.begin() + (long)res_at);
    image.insert(image.end(), f.begin() + DATA_AT, f.begin() + (long)r);
    image.resize(SIZE, 0xFF);
    // the image's own header must name the same resources bank
    if (image.size() < 0x10000 || (size_t)(image[0xFFB7] & 0x3F) * 0x10000 != res_at)
        return err = "this Game Boy Color cartridge's data are laid out differently from the editor's engine", false;
    return true;
}

// see gbc.h. (The SNES data are copied before the size check below, so
// data longer than 3 MB would run past the end of out.)
bool wrap(const Bytes &w, const Bytes &image, Bytes &out, std::string &err)
{
    // (the engine keeps the code without its last banks' $FF: up to 1 MB)
    if (w.size() < 0x150 || w.size() > DATA_AT || !game_title(&w[0x134]))
        return err = "the Game Boy Color game's code is damaged", false;
    Snes s;
    Bytes res;
    if (!parse(image, s, err) || !build_res(s, res, err)) return false;
    out.assign(SIZE, 0xFF);
    std::copy(w.begin(), w.end(), out.begin());
    std::copy(s.data.begin(), s.data.end(), out.begin() + DATA_AT);
    size_t res_at = (DATA_AT + s.data.size() + 0x3FFF) & ~(size_t)0x3FFF;
    if (res_at + res.size() > SIZE) return err = "the maps don't fit in a Game Boy Color cartridge (4 MB)", false;
    std::copy(res.begin(), res.end(), out.begin() + (long)res_at);
    out[0x148] = 0x07;   // the header's ROM size code: 4 MB
    // header checksum at $14D: x = x - byte - 1 over $134-$14C
    u8 c = 0;
    for (int i = 0x134; i < 0x14D; i++) c = (u8)(c - out[i] - 1);
    out[0x14D] = c;
    // global checksum at $14E (big-endian): the sum of every other byte
    unsigned g = 0;
    for (size_t i = 0; i < SIZE; i++)
        if (i != 0x14E && i != 0x14F) g += out[i];
    out[0x14E] = (u8)(g >> 8);
    out[0x14F] = (u8)g;
    return true;
}

}  // namespace gbc
