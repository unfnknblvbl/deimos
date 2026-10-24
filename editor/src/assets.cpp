// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Licence: GPL-2.0-or-later.
//
// The non-level graphics and sounds (see assets.h). Sections: the PNG
// decoder, image helpers, the font, the Master System's status bar and
// title, the Mega Drive's, MIDI to sound-driver streams (music and
// effects), and the SNES's resources (the Mega Drive's, repackaged).
// Everything is integer arithmetic or ordered double maths copied from
// the Python tools, so the bytes match theirs.
#include "assets.h"

#include "../third_party/stb/stb_image.h"   // its zlib decoder (implementation in projectio.cpp)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>

// Deimos's title art, embedded by CMakeLists.txt from art/*.png (generated
// art_<name>.cpp files, at global scope)
extern const unsigned char art_title_bg[], art_title_logo[], art_title_icon[];
extern const size_t art_title_bg_size, art_title_logo_size, art_title_icon_size;

namespace assets {

// ---------------------------------------------------------------- PNG (paletted)
// a big-endian u32 (PNG and MIDI are big-endian)
static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

// see assets.h. Reads the chunks (IHDR, PLTE, tRNS, IDAT; others are
// skipped; CRCs aren't checked), inflates the IDAT data and undoes each
// row's filter.
bool load_png(const Bytes &f, Indexed &out, std::string &err)
{
    static const u8 sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (f.size() < 8 || memcmp(f.data(), sig, 8) != 0) {
        err = "not a PNG";
        return false;
    }
    int depth = 0, ctype = 0, interlace = 0;
    Bytes z;
    size_t p = 8;
    // a chunk: u32 length, 4-letter type, data, u32 CRC
    while (p + 12 <= f.size()) {
        u32 n = be32(&f[p]);
        std::string t((const char *)&f[p + 4], 4);
        if (p + 12 + n > f.size()) break;
        const u8 *d = &f[p + 8];
        if (t == "IHDR") {
            out.w = (int)be32(d);
            out.h = (int)be32(d + 4);
            depth = d[8];
            ctype = d[9];
            interlace = d[12];
        } else if (t == "PLTE") {
            for (u32 i = 0; i < n / 3 && i < 256; i++)
                for (int c = 0; c < 3; c++) out.pal[i][c] = d[i * 3 + c];
        } else if (t == "tRNS") {
            for (u32 i = 0; i < n && i < 256; i++) out.alpha[i] = d[i];
        } else if (t == "IDAT")
            z.insert(z.end(), d, d + n);
        p += 12 + n;
    }
    // colour type 3 = paletted
    if (ctype != 3 || interlace || !(depth == 1 || depth == 2 || depth == 4 || depth == 8) || out.w <= 0 ||
        out.h <= 0) {
        err = "not a plain paletted PNG";
        return false;
    }
    size_t row = ((size_t)out.w * depth + 7) / 8;   // bytes per row (each row has a filter byte first)
    int len = 0;
    char *raw = stbi_zlib_decode_malloc((const char *)z.data(), (int)z.size(), &len);
    if (!raw || (size_t)len < (row + 1) * out.h) {
        if (raw) free(raw);
        err = "damaged PNG";
        return false;
    }
    std::vector<u8> cur(row), prev(row, 0);
    out.idx.assign((size_t)out.w * out.h, 0);
    for (int y = 0; y < out.h; y++) {
        const u8 *src = (const u8 *)raw + y * (row + 1);
        int filter = src[0];
        for (size_t i = 0; i < row; i++) {
            // the PNG filters on whole bytes: a left, b above, c above-left
            int a = i ? cur[i - 1] : 0, b = prev[i], c = i ? prev[i - 1] : 0, x = src[1 + i];
            switch (filter) {
            case 1: x += a; break;
            case 2: x += b; break;
            case 3: x += (a + b) / 2; break;
            case 4: {   // Paeth
                int pp = a + b - c, pa = std::abs(pp - a), pb = std::abs(pp - b), pc = std::abs(pp - c);
                x += (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
                break;
            }
            default: break;
            }
            cur[i] = (u8)x;
        }
        // unpack the pixels, leftmost in the high bits
        for (int x = 0; x < out.w; x++) {
            int bit = x * depth, v = cur[bit / 8];
            v = (v >> (8 - depth - bit % 8)) & ((1 << depth) - 1);
            out.idx[(size_t)y * out.w + x] = (u8)v;
        }
        prev = cur;
    }
    free(raw);
    return true;
}

// ---------------------------------------------------------------- Deimos's title art
// art/*.png, embedded by CMakeLists.txt (art_<name>[], art_<name>_size)
// (declared at the top of this file, outside the namespace: the generated art_*.cpp define them there)

// see assets.h
Img art_img(const char *name, int channels)
{
    const unsigned char *d = !strcmp(name, "title_bg") ? art_title_bg : !strcmp(name, "title_logo") ? art_title_logo
                                                                                                    : art_title_icon;
    size_t n = !strcmp(name, "title_bg") ? art_title_bg_size : !strcmp(name, "title_logo") ? art_title_logo_size
                                                                                           : art_title_icon_size;
    int w = 0, h = 0, comp = 0;
    unsigned char *px = stbi_load_from_memory(d, (int)n, &w, &h, &comp, channels);   // (the PNG's own pixels, as PIL reads them)
    Img o(w, h, channels);
    if (px) memcpy(o.at(0, 0), px, (size_t)w * h * channels);
    stbi_image_free(px);
    return o;
}

// ---------------------------------------------------------------- images (tools/portable_img.py)
// see assets.h
Img to_img(const Indexed &im, bool with_alpha)
{
    Img o(im.w, im.h, with_alpha ? 4 : 3);
    for (int y = 0; y < im.h; y++)
        for (int x = 0; x < im.w; x++) {
            u8 i = im.idx[(size_t)y * im.w + x];
            u8 *d = o.at(x, y);
            for (int c = 0; c < 3; c++) d[c] = im.pal[i][c];
            if (with_alpha) d[3] = im.alpha[i];
        }
    return o;
}

namespace {
// one output coordinate's two source pixels and the weight of the second (0..255)
struct Ax {
    int i0, i1, w;
};
// per output pixel: its centre mapped into the source,
// ((2 o + 1) n_in / n_out - 1) / 2, in 1/256 steps, clamped to the edges
std::vector<Ax> axis(int n_out, int n_in)
{
    std::vector<Ax> out;
    for (int o = 0; o < n_out; o++) {
        long long num = (long long)(2 * o + 1) * n_in - n_out;
        long long f = num < 0 ? 0 : num * 256 / (2 * n_out);
        f = std::min<long long>(f, (long long)(n_in - 1) * 256);
        int i0 = (int)(f >> 8);
        out.push_back({i0, std::min(i0 + 1, n_in - 1), (int)(f & 255)});
    }
    return out;
}
}  // namespace

// see assets.h (rounded: + 32768 >> 16)
Img resize_bilinear(const Img &a, int w, int h)
{
    Img o(w, h, a.c);
    auto xs = axis(w, a.w), ys = axis(h, a.h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < a.c; c++) {
                const Ax &X = xs[x], &Y = ys[y];
                long long top = (long long)a.at(X.i0, Y.i0)[c] * (256 - X.w) + (long long)a.at(X.i1, Y.i0)[c] * X.w;
                long long bot = (long long)a.at(X.i0, Y.i1)[c] * (256 - X.w) + (long long)a.at(X.i1, Y.i1)[c] * X.w;
                o.at(x, y)[c] = (u8)((top * (256 - Y.w) + bot * Y.w + 32768) >> 16);
            }
    return o;
}

// see assets.h. d + (s - d) * a / 255, with the exact integer /255 trick
// (t + (t >> 8)) >> 8; pixels off dst are skipped
void blend(Img &dst, const Img &src, int x0, int y0)
{
    for (int y = 0; y < src.h; y++)
        for (int x = 0; x < src.w; x++) {
            int a = src.at(x, y)[3];
            if (!a) continue;
            int X = x0 + x, Y = y0 + y;
            if (X < 0 || Y < 0 || X >= dst.w || Y >= dst.h) continue;
            for (int c = 0; c < 3; c++) {
                int d = dst.at(X, Y)[c], s = src.at(x, y)[c];
                int t = (s - d) * a + 128;
                dst.at(X, Y)[c] = (u8)(d + ((t + (t >> 8)) >> 8));
            }
        }
}

// see assets.h. Candidates are tried most-used first (ties: the lower
// colour); with nothing chosen yet the score is the count alone.
std::vector<int> greedy_palette(const std::vector<int> &colours, const std::vector<int> &counts, int n,
                                const std::vector<int> &fixed, int (*dist)(int, int))
{
    std::vector<int> pal(fixed.begin(), fixed.end());
    std::vector<int> order(colours.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        if (counts[a] != counts[b]) return counts[a] > counts[b];
        return colours[a] < colours[b];
    });
    std::vector<int> cands;
    std::map<int, long long> w;
    for (size_t i = 0; i < colours.size(); i++) w[colours[i]] = counts[i];
    for (int i : order)
        if (std::find(pal.begin(), pal.end(), colours[i]) == pal.end()) cands.push_back(colours[i]);
    while ((int)pal.size() < n && !cands.empty()) {
        size_t best = 0;
        long long bs = -1;
        for (size_t k = 0; k < cands.size(); k++) {
            long long sc = w[cands[k]];
            if (!pal.empty()) {
                long long m = -1;
                for (int p : pal) {
                    long long d = dist(cands[k], p);
                    if (m < 0 || d < m) m = d;
                }
                sc *= m;
            }
            if (sc > bs) {
                bs = sc;
                best = k;
            }
        }
        pal.push_back(cands[best]);
        cands.erase(cands.begin() + best);
    }
    return pal;
}

// ---------------------------------------------------------------- the font (doom_font.py)
// see assets.h (' ' is added blank)
Font load_font(const Indexed &a)
{
    Font f;
    for (int i = 0; i < 16 * 9; i++) {
        int ch = 33 + i;
        if (ch > 126) break;
        int r = i / 16, c = i % 16;
        std::vector<u8> g(12 * 9, 0);
        for (int y = 0; y < 12; y++)
            for (int x = 0; x < 9; x++) {
                int X = c * 9 + x, Y = r * 12 + y;
                if (X < a.w && Y < a.h) g[y * 9 + x] = a.idx[(size_t)Y * a.w + X] == 1;
            }
        f.glyph[ch] = g;
    }
    f.glyph[' '] = std::vector<u8>(12 * 9, 0);
    f.ok = true;
    return f;
}

namespace {
typedef std::vector<std::vector<u8>> Rows;
// 8 rows -> 7: drops one row that repeats its neighbour (rows nearest the
// middle tried first: first against the row above, then below), else row 4
Rows squeeze_rows(Rows rows)
{
    static const int order[8] = {4, 3, 5, 2, 6, 1, 7, 0};
    for (int k : order)
        if (k > 0 && rows[k] == rows[k - 1]) {
            rows.erase(rows.begin() + k);
            return rows;
        }
    for (int k : order)
        if (k < 7 && rows[k] == rows[k + 1]) {
            rows.erase(rows.begin() + k);
            return rows;
        }
    rows.erase(rows.begin() + 4);
    return rows;
}
}  // namespace

// see assets.h. The glyph's 6 middle columns (2..7) go to tile columns
// 1..6. A glyph within rows 2..9 keeps those rows (squeezed to 7 if it
// starts by row 3, else from row 3), placed from tile row 1; a glyph
// reaching outside them (descenders, tall ones) keeps its last 7 rows at
// most, bottom-aligned to tile row 7.
std::vector<u8> glyph_tile(const std::vector<u8> &g)
{
    std::vector<u8> out(64, 0);
    int top = -1, bot = -1;
    for (int y = 0; y < 12; y++)
        for (int x = 0; x < 9; x++)
            if (g[y * 9 + x]) {
                if (top < 0) top = y;
                bot = y;
            }
    if (top < 0) return out;
    // glyph row y's middle 6 pixels
    auto core = [&](int y) {
        std::vector<u8> r(6);
        for (int x = 0; x < 6; x++) r[x] = g[y * 9 + 2 + x];
        return r;
    };
    // copy rows into the tile from row y0, columns 1..6
    auto put = [&](const Rows &body, int y0) {
        for (size_t k = 0; k < body.size(); k++)
            for (int x = 0; x < 6; x++) out[(y0 + k) * 8 + 1 + x] = body[k][x];
    };
    if (top >= 2 && bot <= 9) {
        Rows body;
        for (int y = 2; y < 10; y++) body.push_back(core(y));
        if (top <= 3) put(squeeze_rows(body), 1);
        else put(Rows(body.begin() + 1, body.end()), 1);
    } else {
        Rows body;
        for (int y = std::max(1, bot - 6); y <= bot; y++) body.push_back(core(y));
        put(body, 8 - (int)body.size());
    }
    return out;
}

// a PNG from the jar, decoded; false with err when missing or unreadable
static bool png(const Jar &jar, const std::string &name, Indexed &out, std::string &err)
{
    const Bytes *b = jar.get(name);
    if (!b) {
        err = "the game has no " + name;
        return false;
    }
    if (!load_png(*b, out, err)) {
        err = name + ": " + err;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- Master System (build_ui_gfx.py)
namespace {
int q2(int v) { return (v * 6 + 255) / 510; }   // round(v / 255 * 3)
// a colour 0..255 per channel
struct RGB3 {
    int r, g, b;
    bool operator==(const RGB3 &o) const { return r == o.r && g == o.g && b == o.b; }
};
// colour RAM byte --BBGGRR
u8 cram(RGB3 c) { return (u8)((q2(c.b) << 4) | (q2(c.g) << 2) | q2(c.r)); }
// distance of two Master System colours given as r << 4 | g << 2 | b
// (levels 0..3): squared level differences
int sms_dist(int a, int b)
{
    int dr = (a >> 4) - (b >> 4), dg = ((a >> 2) & 3) - ((b >> 2) & 3), db = (a & 3) - (b & 3);
    return dr * dr + dg * dg + db * db;
}

// a list of distinct 8 x 8 tiles (64 colour indices each)
struct TileSet {
    std::vector<std::vector<u8>> tiles;
    std::map<std::vector<u8>, int> index;
    // t's number, adding it if new
    int add(const std::vector<u8> &t)
    {
        auto it = index.find(t);
        if (it != index.end()) return it->second;
        tiles.push_back(t);
        return index[t] = (int)tiles.size() - 1;
    }
    // every tile in the VDP's format: per row 4 bytes, one per bit plane
    Bytes planar() const
    {
        Bytes out;
        for (auto &t : tiles)
            for (int row = 0; row < 8; row++) {
                u8 pl[4] = {0, 0, 0, 0};
                for (int col = 0; col < 8; col++) {
                    int v = t[row * 8 + col] & 15;
                    for (int p = 0; p < 4; p++)
                        if (v & (1 << p)) pl[p] |= (u8)(1 << (7 - col));
                }
                out.insert(out.end(), pl, pl + 4);
            }
        return out;
    }
};
}  // namespace

// see assets.h. Blocks: hud_palette (32 CRAM bytes: the face's 16
// colours, then the text colours), hud_tiles (the status bar's tiles in
// VRAM slots 56..127 and 496..503), font_index (each HUD character's VRAM
// tile), hud_static (the status bar's 5 rows of name table), dlg_font
// (ASCII 32..126, one tile each), title_map and title_tiles.
bool sms_ui(const Jar &jar, const std::string &version_in, bool gg, Blocks &out, std::string &err)
{
    Indexed a, face_i;
    if (!png(jar, "a.png", a, err) || !png(jar, "doomrpg.png", face_i, err)) return false;
    Font font = load_font(a);
    Img face = to_img(face_i, false);
    std::string version;
    for (char ch : version_in) version.push_back((char)toupper((unsigned char)ch));
    // ---- palettes
    // the face's 15 colours (+ black first): greedy over its pixels'
    // console colours
    std::map<int, int> cnt;
    for (int i = 0; i < face.w * face.h; i++) {
        const u8 *p = &face.px[(size_t)i * 3];
        cnt[(q2(p[0]) << 4) | (q2(p[1]) << 2) | q2(p[2])]++;
    }
    std::vector<int> keys, counts;
    for (auto &kv : cnt) {
        keys.push_back(kv.first);
        counts.push_back(kv.second);
    }
    auto chosen = greedy_palette(keys, counts, 15, {}, sms_dist);
    std::vector<RGB3> face_pal = {{0, 0, 0}};
    for (int c : chosen) {
        RGB3 rgb{(c >> 4) * 85, ((c >> 2) & 3) * 85, (c & 3) * 85};
        if (std::find(face_pal.begin(), face_pal.end(), rgb) == face_pal.end()) face_pal.push_back(rgb);
    }
    while (face_pal.size() < 16) face_pal.push_back({0, 0, 0});
    // the second palette: fixed colours for text
    static const RGB3 text_pal[16] = {{0, 0, 0},       {255, 255, 255}, {255, 0, 0},   {170, 0, 0},
                                      {0, 255, 0},     {0, 170, 0},     {255, 255, 0}, {170, 170, 170},
                                      {85, 85, 85},    {85, 85, 255},   {255, 170, 0}, {255, 85, 85},
                                      {0, 85, 0},      {85, 0, 0},      {170, 85, 0},  {255, 255, 170}};
    Bytes hp;
    for (auto &c : face_pal) hp.push_back(cram(c));
    for (auto &c : text_pal) hp.push_back(cram(c));
    out["hud_palette"] = hp;
    // ---- status bar
    // the characters the status bar can show; slots: the VRAM tile numbers
    // its tiles may use, in order
    static const char *HUD_CHARS = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:!./-'?+,%()";
    std::vector<int> slots;
    for (int i = 56; i < 128; i++) slots.push_back(i);
    for (int i = 496; i < 504; i++) slots.push_back(i);
    TileSet ts;
    ts.add(std::vector<u8>(64, 0));
    std::vector<int> font_index(64, 0);   // per character from ' ' (32): its tile in ts
    // a character's 8 x 8 tile (unknown ones: blank)
    auto glyph = [&](int ch) { return glyph_tile(font.glyph.count(ch) ? font.glyph[ch] : font.glyph[' ']); };
    for (const char *c = HUD_CHARS; *c; c++) font_index[*c - 32] = ts.add(glyph(*c));
    // the nearest face colour to RGB pixel p
    auto nearest = [&](const u8 *p) {
        int best = 0;
        long long bd = -1;
        for (size_t i = 0; i < face_pal.size(); i++) {
            long long d = (long long)(p[0] - face_pal[i].r) * (p[0] - face_pal[i].r) +
                          (long long)(p[1] - face_pal[i].g) * (p[1] - face_pal[i].g) +
                          (long long)(p[2] - face_pal[i].b) * (p[2] - face_pal[i].b);
            if (bd < 0 || d < bd) {
                bd = d;
                best = (int)i;
            }
        }
        return best;
    };
    // the face resized to size x size, cut into tiles added to tset;
    // returns the grid of tile numbers
    auto face_tiles = [&](int size, TileSet &tset) {
        Img im = size == face.w ? face : resize_bilinear(face, size, size);
        int n = size / 8;
        std::vector<std::vector<int>> grid(n, std::vector<int>(n));
        for (int ty = 0; ty < n; ty++)
            for (int tx = 0; tx < n; tx++) {
                std::vector<u8> pix;
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) pix.push_back((u8)nearest(im.at(tx * 8 + x, ty * 8 + y)));
                grid[ty][tx] = tset.add(pix);
            }
        return grid;
    };
    auto face40 = face_tiles(gg ? 24 : 40, ts);      // (Game Gear: 24 x 24, beside the status text)
    if (ts.tiles.size() > slots.size()) {
        err = "too many status bar tiles";
        return false;
    }
    Bytes tiles = ts.planar();
    tiles.resize(slots.size() * 32, 0);
    out["hud_tiles"] = tiles;
    Bytes fi;
    for (int i : font_index) put16(fi, (unsigned)slots[i]);
    out["font_index"] = fi;
    // a name table entry: tile slots[slot], bit 8 of the tile number in
    // the high byte's bit 0, bank << 3 = the palette select bit (1: sprite palette)
    auto entry = [&](int slot, int bank) {
        int t = slots[slot];
        return Bytes{(u8)(t & 0xFF), (u8)(((t >> 8) & 1) | (bank << 3))};
    };
    // the status bar's 5 rows x 32 columns: on the Master System the face
    // (5 x 5 tiles) at columns 1..5, blank elsewhere
    Bytes rows;
    for (int r = 0; r < 5; r++) {
        Bytes b = entry(0, gg ? 1 : 0);
        if (!gg) {
            rows.insert(rows.end(), b.begin(), b.end());
            for (int i = 0; i < 5; i++) {
                Bytes e = entry(face40[r][i], 0);
                rows.insert(rows.end(), e.begin(), e.end());
            }
            for (int i = 0; i < 26; i++) rows.insert(rows.end(), b.begin(), b.end());
        } else if (r >= 1 && r <= 3) {      // the message line, then the face at columns 6..8
            for (int i = 0; i < 6; i++) rows.insert(rows.end(), b.begin(), b.end());
            for (int i = 0; i < 3; i++) {
                Bytes e = entry(face40[r - 1][i], 0);
                rows.insert(rows.end(), e.begin(), e.end());
            }
            for (int i = 0; i < 23; i++) rows.insert(rows.end(), b.begin(), b.end());
        } else
            for (int i = 0; i < 32; i++) rows.insert(rows.end(), b.begin(), b.end());
    }
    out["hud_static"] = rows;
    // ---- dialog font: ASCII 32..126
    Bytes dlg;
    for (int code = 32; code < 127; code++) {
        TileSet t;
        t.add(glyph(code));
        Bytes b = t.planar();
        dlg.insert(dlg.end(), b.begin(), b.end());
    }
    out["dlg_font"] = dlg;
    // ---- title
    // Deimos's moon (art/title_icon.png, 48 x 48) in the middle, in the text
    // palette (its greys); five centred lines of text, DEIMOS in orange;
    // tiles from VRAM 128, all in the second palette (tools/build_ui_gfx.py)
    TileSet tts;
    tts.add(std::vector<u8>(64, 0));
    Img icon = art_img("title_icon", 3);
    std::vector<std::vector<int>> face48(6, std::vector<int>(6));
    for (int ty = 0; ty < 6; ty++)
        for (int tx = 0; tx < 6; tx++) {
            std::vector<u8> pix;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    const u8 *q = icon.at(tx * 8 + x, ty * 8 + y);
                    int best = 0;
                    long long bd = -1;
                    for (int i = 0; i < 16; i++) {   // the nearest text-palette colour (first wins)
                        long long d = (long long)(q[0] - text_pal[i].r) * (q[0] - text_pal[i].r) +
                                      (long long)(q[1] - text_pal[i].g) * (q[1] - text_pal[i].g) +
                                      (long long)(q[2] - text_pal[i].b) * (q[2] - text_pal[i].b);
                        if (bd < 0 || d < bd) {
                            bd = d;
                            best = i;
                        }
                    }
                    pix.push_back((u8)best);
                }
            face48[ty][tx] = tts.add(pix);
        }
    std::map<int, int> txt, name;
    std::set<char> chars;
    std::string system = gg ? "SEGA GAME GEAR" : "SEGA MASTER SYSTEM";
    for (char ch : "DEIMOS A DOOM RPG ENGINE " + system + " PRESS BUTTON 1 PORT GPL VERSION " + version)
        chars.insert(ch);
    for (char ch : chars) txt[ch] = tts.add(glyph(ch));
    for (char ch : std::set<char>{'D', 'E', 'I', 'M', 'O', 'S'}) {   // DEIMOS: text colour 10 (orange)
        auto g = glyph(ch);
        for (auto &v : g) v = v ? 10 : 0;
        name[ch] = tts.add(g);
    }
    // name table entry for title tile i (from VRAM 128) in palette `bank`
    auto tentry = [](int i, int bank) {
        int t = 128 + i;
        return std::pair<u8, u8>{(u8)(t & 0xFF), (u8)(((t >> 8) & 1) | (bank << 3))};
    };
    Bytes tmap;
    for (int ty = 0; ty < 24; ty++)
        for (int tx = 0; tx < 32; tx++) {
            int fy = gg ? 3 : 4;      // (the Game Gear shows rows 3..20)
            auto e = (ty >= fy && ty < fy + 6 && tx >= 13 && tx <= 18) ? tentry(face48[ty - fy][tx - 13], 1)
                                                                         : tentry(0, 0);
            tmap.push_back(e.first);
            tmap.push_back(e.second);
        }
    // s centred on tile row ty
    auto put_text = [&](int ty, const std::string &s) {
        int tx0 = (32 - (int)s.size()) / 2;
        std::map<int, int> &tiles = s == "DEIMOS" ? name : txt;
        for (size_t k = 0; k < s.size(); k++) {
            size_t off = ((size_t)ty * 32 + tx0 + k) * 2;
            auto e = tentry(tiles[s[k]], 1);
            tmap[off] = e.first;
            tmap[off + 1] = e.second;
        }
    };
    static const int ROWS_SMS[5] = {12, 13, 15, 17, 19}, ROWS_GG[5] = {10, 11, 13, 15, 17};
    const int *rows_at = gg ? ROWS_GG : ROWS_SMS;
    const std::string texts[5] = {"DEIMOS", "A DOOM RPG ENGINE", system, "VERSION " + version, "PRESS BUTTON 1"};
    for (int k = 0; k < 5; k++) put_text(rows_at[k], texts[k]);
    out["title_map"] = tmap;
    out["title_tiles"] = tts.planar();
    return true;
}

// ---------------------------------------------------------------- Mega Drive (build_md_ui.py)
namespace {
const int MD_LEVELS[8] = {0, 52, 87, 116, 144, 172, 206, 255};   // the DAC's measured levels
// the nearest level to v (ties: the lower)
int md_level(int v)
{
    int best = 0;
    for (int i = 1; i < 8; i++)
        if (std::abs(MD_LEVELS[i] - v) < std::abs(MD_LEVELS[best] - v)) best = i;
    return best;
}
// RGB -> CRAM word ----BBB-GGG-RRR-
int mdc(int r, int g, int b) { return (md_level(b) << 9) | (md_level(g) << 5) | (md_level(r) << 1); }
// squared level difference of two CRAM words
int lv_dist(int a, int b)
{
    int d = 0;
    for (int s : {1, 5, 9}) {
        int x = ((a >> s) & 7) - ((b >> s) & 7);
        d += x * x;
    }
    return d;
}
void tile_bytes(Bytes &out, const std::vector<int> &px)   // 64 indices -> 32 bytes
{
    for (int i = 0; i < 64; i += 2) out.push_back((u8)(((px[i] & 15) << 4) | (px[i + 1] & 15)));
}
// quantise: (indices, palette)
// Each pixel snapped to a CRAM colour; pal = greedy_palette of those (n,
// fixed first); returns each pixel's nearest palette index.
std::vector<int> md_quantise(const Img &a, int n, const std::vector<int> &fixed, std::vector<int> &pal)
{
    std::vector<int> md((size_t)a.w * a.h);
    std::map<int, int> cnt;
    for (size_t i = 0; i < md.size(); i++) {
        const u8 *p = &a.px[i * a.c];
        md[i] = mdc(p[0], p[1], p[2]);
        cnt[md[i]]++;
    }
    std::vector<int> cols, counts;
    for (auto &kv : cnt) {
        cols.push_back(kv.first);
        counts.push_back(kv.second);
    }
    pal = greedy_palette(cols, counts, n, fixed, lv_dist);
    std::map<int, int> near;
    for (int c : cols) {
        int best = 0;
        for (size_t k = 1; k < pal.size(); k++)
            if (lv_dist(c, pal[k]) < lv_dist(c, pal[best])) best = (int)k;
        near[c] = best;
    }
    std::vector<int> idx(md.size());
    for (size_t i = 0; i < md.size(); i++) idx[i] = near[md[i]];
    return idx;
}
// appends each value as a big-endian u16
void be16v(Bytes &b, const std::vector<int> &v)
{
    for (int x : v) {
        b.push_back((u8)(x >> 8));
        b.push_back((u8)x);
    }
}
}  // namespace

// see assets.h. Blocks (big-endian words): ui_font (96 tiles), ui_faces
// (9 faces of 5 x 5 tiles, column by column), ui_icons (9 icons of 2 x 2
// tiles), ui_hud_pal, ui_dlg_pal, title_tiles, title_map (u16 per cell of
// 32 x 28), title_pal.
bool md_ui(const Jar &jar, const std::string &version, Blocks &out, std::string &err, const std::string &word)
{
    Indexed a, l, m;
    if (!png(jar, "a.png", a, err) || !png(jar, "l.png", l, err) || !png(jar, "m.png", m, err))
        return false;
    Font font = load_font(a);
    auto glyph = [&](int ch) { return glyph_tile(font.glyph.count(ch) ? font.glyph[ch] : font.glyph[' ']); };
    // ---- font: fg 1, bg 15
    Bytes fb;
    for (int code = 32; code < 128; code++) {
        auto g = glyph(code < 127 ? code : ' ');
        std::vector<int> px(64);
        for (int i = 0; i < 64; i++) px[i] = g[i] ? 1 : 15;
        tile_bytes(fb, px);
    }
    out["ui_font"] = fb;
    // ---- status bar palette
    // l.png: 9 faces of 18 x 20 stacked; m.png: 9 icons of 13 x 13 stacked.
    // Palette: 0, white, up to 13 colours of both pictures, black at 15.
    Img faces = to_img(l, false), icons = to_img(m, false);
    if (faces.w != 18 || faces.h < 180 || icons.w != 13 || icons.h < 117) {
        err = "the status bar pictures (l.png, m.png) are not the expected size";
        return false;
    }
    Img both(31, 180, 3);
    for (int y = 0; y < 180; y++) {
        for (int x = 0; x < 18; x++) memcpy(both.at(x, y), faces.at(x, y), 3);
        if (y < icons.h)
            for (int x = 0; x < 13; x++) memcpy(both.at(18 + x, y), icons.at(x, y), 3);
    }
    int black = mdc(0, 0, 0), white = mdc(255, 255, 255);
    std::vector<int> hud_pal;
    md_quantise(both, 15, {black, white}, hud_pal);
    std::vector<int> hp16 = {0, white};
    for (int cc : hud_pal)
        if (cc != black && cc != white && hp16.size() < 15) hp16.push_back(cc);
    while (hp16.size() < 15) hp16.push_back(0);
    hp16.push_back(black);
    int P[16][3];
    for (int k = 0; k < 16; k++) {
        P[k][0] = MD_LEVELS[(hp16[k] >> 1) & 7];
        P[k][1] = MD_LEVELS[(hp16[k] >> 5) & 7];
        P[k][2] = MD_LEVELS[(hp16[k] >> 9) & 7];
    }
    // a pixel's index: near-black -> 15, else the nearest of colours 1..14
    auto to_idx = [&](const Img &b, int x, int y) {
        const u8 *p = b.at(x, y);
        if (p[0] + p[1] + p[2] < 40) return 15;
        int best = 1;
        long long bd = -1;
        for (int k = 1; k < 15; k++) {
            long long d = 0;
            for (int ch = 0; ch < 3; ch++) d += (long long)(p[ch] - P[k][ch]) * (p[ch] - P[k][ch]);
            if (bd < 0 || d < bd) {
                bd = d;
                best = k;
            }
        }
        return best;
    };
    // each face doubled to 36 x 40 in a 40 x 40 block
    Bytes facesb;
    for (int k = 0; k < 9; k++) {
        Img block(40, 40, 3);
        for (int y = 0; y < 40; y++)
            for (int x = 0; x < 36; x++) memcpy(block.at(2 + x, y), faces.at(x / 2, k * 20 + y / 2), 3);
        for (int tx = 0; tx < 5; tx++)
            for (int ty = 0; ty < 5; ty++) {
                std::vector<int> px;
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) px.push_back(to_idx(block, tx * 8 + x, ty * 8 + y));
                tile_bytes(facesb, px);
            }
    }
    out["ui_faces"] = facesb;
    // each icon in a 16 x 16 block, near-white pixels as 15 too
    Bytes iconsb;
    for (int k = 0; k < 9; k++) {
        Img block(16, 16, 3);
        for (int y = 0; y < 13; y++)
            for (int x = 0; x < 13; x++) memcpy(block.at(1 + x, 1 + y), icons.at(x, k * 13 + y), 3);
        for (int tx = 0; tx < 2; tx++)
            for (int ty = 0; ty < 2; ty++) {
                std::vector<int> px;
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) {
                        const u8 *p = block.at(tx * 8 + x, ty * 8 + y);
                        bool wh = std::min({p[0], p[1], p[2]}) > 235;
                        px.push_back(wh ? 15 : to_idx(block, tx * 8 + x, ty * 8 + y));
                    }
                tile_bytes(iconsb, px);
            }
    }
    out["ui_icons"] = iconsb;
    Bytes hpb;
    be16v(hpb, hp16);
    out["ui_hud_pal"] = hpb;
    // the dialog box's palette (frame_screen in md_build.cpp reuses slots 1, 2, 3, 15)
    std::vector<int> dlg = {0, white, mdc(255, 210, 0), mdc(150, 150, 150), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            mdc(10, 20, 70)};
    Bytes dpb;
    be16v(dpb, dlg);
    out["ui_dlg_pal"] = dpb;
    // ---- title: 256 x 224
    // Deimos's background and logo (art/), the text drawn on: the logo from
    // row 36, "A Doom RPG Engine" at 94 (tools/build_md_ui.py's LOGO_Y, SUBTITLE_Y)
    const int W = 256, H = 224;
    Img title = art_img("title_bg", 3);
    Img logo = art_img("title_logo", 4);
    blend(title, logo, (W - logo.w) / 2, 36);
    auto pglyph = [&](char ch, int &w) -> std::vector<u8> {   // white core trimmed to its columns
        if (ch == ' ') {
            w = 3;
            return std::vector<u8>(12 * 3, 0);
        }
        const auto &g = font.glyph.count(ch) ? font.glyph[(int)(unsigned char)ch] : font.glyph[' '];
        int c0 = 9, c1 = -1;
        for (int y = 0; y < 12; y++)
            for (int x = 0; x < 9; x++)
                if (g[y * 9 + x]) {
                    c0 = std::min(c0, x);
                    c1 = std::max(c1, x);
                }
        if (c1 < 0) {
            w = 3;
            return std::vector<u8>(12 * 3, 0);
        }
        w = c1 - c0 + 1;
        std::vector<u8> o(12 * w);
        for (int y = 0; y < 12; y++)
            for (int x = 0; x < w; x++) o[y * w + x] = g[y * 9 + c0 + x];
        return o;
    };
    // s centred at row y in colour (r, g, b), scale x scale per font pixel,
    // with a black shadow 1 pixel down-right (drawn first); throws if too wide
    auto text = [&](const std::string &s, int y, int r, int g, int b, int scale) {
        std::vector<std::vector<u8>> gl;
        std::vector<int> ws;
        int total = -1;
        for (char ch : s) {
            int w;
            gl.push_back(pglyph(ch, w));
            ws.push_back(w);
            total += w + 1;
        }
        int x = (W - total * scale) / 2;
        if (x < 0) throw std::runtime_error("title text too wide");
        for (size_t k = 0; k < gl.size(); k++) {
            int w = ws[k];
            for (int pass = 0; pass < 2; pass++)
                for (int yy = 0; yy < 12; yy++)
                    for (int xx = 0; xx < w; xx++)
                        if (gl[k][yy * w + xx])
                            for (int sy = 0; sy < scale; sy++)
                                for (int sx = 0; sx < scale; sx++) {
                                    int X = x + xx * scale + sx + (pass ? 0 : 1), Y = y + yy * scale + sy + (pass ? 0 : 1);
                                    u8 *p = title.at(X, Y);
                                    if (pass) {
                                        p[0] = (u8)r;
                                        p[1] = (u8)g;
                                        p[2] = (u8)b;
                                    } else
                                        p[0] = p[1] = p[2] = 0;
                                }
            x += (w + 1) * scale;
        }
    };
    try {
        text("A Doom RPG Engine", 94, 225, 225, 225, 1);
        text("Version " + version, 170, 170, 170, 170, 1);
        text("PRESS START", 196, 255, 210, 0, 1);
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }
    std::vector<int> tpal;
    auto idx = md_quantise(title, 15, {}, tpal);
    std::vector<int> tpal16(1 + std::max<size_t>(tpal.size(), 15), 0);   // (colour 0, then the picture's)
    std::copy(tpal.begin(), tpal.end(), tpal16.begin() + 1);
    // the title's tiles (indices + 1: colour 0 is the backdrop), identical
    // ones stored once
    Bytes ttiles;
    std::vector<int> tmap;
    std::map<Bytes, int> seen;
    int n_tiles = 0;
    for (int ty = 0; ty < H / 8; ty++)
        for (int tx = 0; tx < W / 8; tx++) {
            std::vector<int> px;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) px.push_back(idx[(size_t)(ty * 8 + y) * W + tx * 8 + x] + 1);
            Bytes b;
            tile_bytes(b, px);
            auto it = seen.find(b);
            if (it == seen.end()) {
                it = seen.emplace(b, n_tiles++).first;
                ttiles.insert(ttiles.end(), b.begin(), b.end());
            }
            tmap.push_back(it->second);
        }
    out["title_tiles"] = ttiles;
    Bytes tm, tp;
    be16v(tm, tmap);
    be16v(tp, tpal16);
    out["title_map"] = tm;
    out["title_pal"] = tp;
    return true;
}

}  // namespace assets


// ================================================================ MIDI (build_music.py, build_sfx.py)
// The sound driver's stream (Stream::body): a loop flag byte, then
// commands in time order: $01-$7F wait that many frames (1/60 s);
// $80+v note, attenuation: key on voice v; $88+v key off; $90+v patch;
// $98+v pitch bend (signed byte); $A0+v attenuation; $A8+v pan bits;
// $B0+v note: a slide step (effects); $FF the end. Voices 0..4 melodic FM,
// 5 drums on FM, 6 the noise channel (its "note" is a drum sound number).
namespace assets {
namespace {
const int FPS = 60;   // stream time unit: frames per second

// one MIDI event: tick, its order in its track, meta or channel event,
// the meta type or status nibble, channel, and its data bytes
struct Ev {
    long tick;
    int order;
    bool meta;
    int kind;         // meta: its type; else the status high nibble
    int ch;
    Bytes data;
};

// a MIDI variable-length number at d[i] (7 bits a byte, high bit = more)
unsigned vlq(const Bytes &d, size_t &i)
{
    unsigned v = 0;
    for (;;) {
        if (i >= d.size()) throw std::runtime_error("MIDI file cut short");
        u8 b = d[i++];
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) return v;
    }
}

// Every track's meta and channel events (running status handled, sysex
// skipped), merged in time order; returns the ticks per beat. Throws on a
// bad header or track. (A meta event at a track's very end reads its type
// byte unchecked.)
int parse_midi(const Bytes &d, std::vector<Ev> &events)
{
    if (d.size() < 14 || memcmp(d.data(), "MThd", 4) != 0) throw std::runtime_error("not a MIDI file");
    int ntr = (d[10] << 8) | d[11], div = (d[12] << 8) | d[13];
    size_t i = 14;
    for (int tr = 0; tr < ntr; tr++) {
        if (i + 8 > d.size() || memcmp(&d[i], "MTrk", 4) != 0) throw std::runtime_error("damaged MIDI file");
        size_t n = be32(&d[i + 4]);
        Bytes t(d.begin() + i + 8, d.begin() + std::min(d.size(), i + 8 + n));
        i += 8 + n;
        size_t j = 0;
        long tick = 0;
        int run = 0, order = 0;
        while (j < t.size()) {
            tick += vlq(t, j);
            if (j >= t.size()) break;
            u8 b = t[j];
            if (b == 0xFF) {
                int typ = t[j + 1];
                j += 2;
                unsigned ln = vlq(t, j);
                events.push_back({tick, order, true, typ, 0, Bytes(t.begin() + j, t.begin() + std::min(t.size(), j + ln))});
                j += ln;
            } else if (b == 0xF0 || b == 0xF7) {
                j++;
                unsigned ln = vlq(t, j);
                j += ln;
            } else {
                if (b & 0x80) {
                    run = b;
                    j++;
                }
                int hi = run & 0xF0, ch = run & 15;
                int nb = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
                events.push_back({tick, order, false, hi, ch, Bytes(t.begin() + j, t.begin() + std::min(t.size(), j + nb))});
                j += nb;
            }
            order++;
        }
    }
    std::stable_sort(events.begin(), events.end(), [](const Ev &a, const Ev &b) {
        return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
    });
    return div;
}

// a tempo map: MIDI ticks -> 1/60 s frames
struct Tempo {
    int div;
    std::vector<std::pair<long, long>> changes;   // (tick, microseconds a beat)
    long frame(long tick) const
    {
        double sec = 0.0;
        long last_t = 0, last_tp = changes[0].second;
        for (size_t k = 1; k < changes.size(); k++) {
            long tt = changes[k].first, tp = changes[k].second;
            if (tt >= tick) break;
            sec += (double)((tt - last_t) * last_tp) / div / 1e6;
            last_t = tt;
            last_tp = tp;
        }
        sec += (double)((tick - last_t) * last_tp) / div / 1e6;
        return (long)std::nearbyint(sec * FPS);
    }
};

// a big-endian number of any length (the tempo meta's 3 bytes)
long be_n(const Bytes &b)
{
    long v = 0;
    for (u8 x : b) v = (v << 8) | x;
    return v;
}

// velocity x volume -> attenuation 0..127 (127 silent): the gain in dB,
// in 0.75 dB steps, x 0.6
int att_of(int vel, int vol)
{
    double g = (vel / 127.0) * (vol / 127.0);
    if (g <= 0) return 127;
    double v = std::nearbyint(-20 * std::log10(g) / 0.75 * 0.6);
    return (int)std::max(0.0, std::min(127.0, v));
}

// a stream being built: commands at frames, kept in emit order per frame
struct Stream {
    struct E {
        long fr;
        int seq;
        Bytes b;
    };
    std::vector<E> ev;
    int seq = 0;
    // add command bytes b at frame fr
    void emit(long fr, std::initializer_list<int> b)
    {
        Bytes x;
        for (int v : b) x.push_back((u8)v);
        ev.push_back({fr, seq++, x});
    }
    // the stream (see above): waits inserted between frames, end = a
    // last wait up to that frame
    Bytes body(bool loops, long end = -1)
    {
        std::stable_sort(ev.begin(), ev.end(), [](const E &a, const E &b) {
            return a.fr != b.fr ? a.fr < b.fr : a.seq < b.seq;
        });
        Bytes out = {(u8)(loops ? 1 : 0)};
        long now = 0;
        for (auto &e : ev) {
            while (e.fr > now) {
                long w = std::min(127L, e.fr - now);
                out.push_back((u8)w);
                now += w;
            }
            out.insert(out.end(), e.b.begin(), e.b.end());
        }
        if (end > now) out.push_back((u8)std::min(127L, end - now));
        out.push_back(0xFF);
        return out;
    }
};

// ---------------------------------------------------------------- music (build_music.py convert)
// patches 4 and 5 (kick, tom); 5 melodic voices; the drum FM voice; the noise voice
const int P_KICK = 4, P_TOM = 5, N_MELODIC = 5, V_DRUM_FM = 5, V_NOISE = 6;
// General MIDI program -> the driver's patch (others: 0)
int gm_patch(int prog)
{
    static const std::map<int, int> m = {{29, 1}, {30, 0}, {31, 0}, {32, 2}, {33, 2}, {34, 2}, {35, 2},
                                         {17, 3}, {18, 3}, {16, 3}, {47, 5}, {118, 5}};
    auto it = m.find(prog);
    return it == m.end() ? 0 : it->second;
}
// GM drum note -> noise sound (0 snare, 1 hat, 2 open hat, 3 crash), -1 none
int drum_map(int note)
{
    static const std::map<int, int> m = {{38, 0}, {40, 0}, {37, 1}, {39, 0}, {42, 1}, {44, 1}, {46, 2},
                                         {51, 2}, {53, 2}, {59, 2}, {49, 3}, {52, 3}, {55, 3}, {57, 3}};
    auto it = m.find(note);
    return it == m.end() ? -1 : it->second;
}
// GM tom note -> the pitch played on the FM drum voice, -1 not a tom
int tom_pitch(int note)
{
    static const std::map<int, int> m = {{41, 43}, {43, 46}, {45, 50}, {47, 53}, {48, 57}, {50, 60}};
    auto it = m.find(note);
    return it == m.end() ? -1 : it->second;
}

// A MIDI song -> a stream. Channel 10 (9 here): kicks and toms on the FM
// drum voice with a falling pitch bend, other drums on the noise voice.
// Other channels: each note-on takes a free voice (preferring one last
// used by the same channel), or steals the oldest; patch, pan and bend
// are only sent when they change.
Bytes convert_song(const Bytes &file, bool loops)
{
    std::vector<Ev> events;
    Tempo T;
    T.div = parse_midi(file, events);
    T.changes.push_back({0, 500000});
    for (auto &e : events)
        if (e.meta && e.kind == 0x51) T.changes.push_back({e.tick, be_n(e.data)});
    int chan_prog[16] = {}, chan_vol[16], chan_pan[16], chan_bend[16] = {};
    for (int c = 0; c < 16; c++) {
        chan_vol[c] = 100;
        chan_pan[c] = 0x40;
    }
    const int NONE = -99999;   // "not yet set"
    struct Voice {
        bool on = false;
        int ch = 0, note = 0, vel = 0;
        long start = 0;
    };
    Voice voice[N_MELODIC];
    int voice_patch[6], voice_pan[6], voice_bend[6], voice_last_chan[N_MELODIC];
    for (int v = 0; v < 6; v++) {
        voice_patch[v] = voice_pan[v] = NONE;
        voice_bend[v] = 0;
    }
    for (int v = 0; v < N_MELODIC; v++) voice_last_chan[v] = NONE;
    Stream S;
    // MIDI pan -> the driver's left/right bits (left $80, right $40, both $C0)
    auto pan_bits = [](int v) { return v < 0x30 ? 0x80 : v > 0x50 ? 0x40 : 0xC0; };
    // send voice v's patch, pan and bend where they differ
    auto set_voice = [&](int v, long fr, int patch, int pan, int bend) {
        if (voice_patch[v] != patch) {
            S.emit(fr, {0x90 + v, patch});
            voice_patch[v] = patch;
        }
        if (voice_pan[v] != pan) {
            S.emit(fr, {0xA8 + v, pan});
            voice_pan[v] = pan;
        }
        if (voice_bend[v] != bend) {
            S.emit(fr, {0x98 + v, bend & 0xFF});
            voice_bend[v] = bend;
        }
    };
    long end_frame = 0;
    for (auto &e : events) {
        long fr = T.frame(e.tick);
        end_frame = std::max(end_frame, fr);
        if (e.meta) continue;
        int ch = e.ch, kind = e.kind;
        auto D = [&](size_t k) { return k < e.data.size() ? (int)e.data[k] : 0; };
        if (kind == 0xC0) chan_prog[ch] = D(0);   // program change
        else if (kind == 0xB0) {                  // controller: 7 volume, 10 pan
            if (D(0) == 7) {
                chan_vol[ch] = D(1);
                for (int v = 0; v < N_MELODIC; v++)
                    if (voice[v].on && voice[v].ch == ch) S.emit(fr, {0xA0 + v, att_of(voice[v].vel, D(1))});
            } else if (D(0) == 10)
                chan_pan[ch] = D(1);
        } else if (kind == 0xE0) {   // pitch bend: +-2 semitones -> +-64 (32 a semitone)
            int b = ((D(1) << 7) | D(0)) - 8192;
            chan_bend[ch] = (int)std::nearbyint(b / 8192.0 * 2 * 32);
            for (int v = 0; v < N_MELODIC; v++)
                if (voice[v].on && voice[v].ch == ch && voice_bend[v] != chan_bend[ch]) {
                    S.emit(fr, {0x98 + v, chan_bend[ch] & 0xFF});
                    voice_bend[v] = chan_bend[ch];
                }
        } else if (kind == 0x80 || kind == 0x90) {
            int note = D(0), vel = D(1);
            bool on = kind == 0x90 && vel > 0;
            if (ch == 9) {
                if (!on) continue;
                int a = att_of(vel, chan_vol[9]);
                bool kick = note == 35 || note == 36;
                int tp = tom_pitch(note);
                if (kick || tp >= 0) {
                    set_voice(V_DRUM_FM, fr, kick ? P_KICK : P_TOM, 0xC0, 0);
                    int pitch = kick ? 40 : tp;
                    S.emit(fr, {0x88 + V_DRUM_FM});
                    S.emit(fr, {0x80 + V_DRUM_FM, pitch, a});
                    for (int k = 1; k < 7; k++) S.emit(fr + k, {0x98 + V_DRUM_FM, (-k * (kick ? 20 : 8)) & 0xFF});
                    voice_bend[V_DRUM_FM] = NONE;
                } else if (drum_map(note) >= 0)
                    S.emit(fr, {0x80 + V_NOISE, drum_map(note), a});
                continue;
            }
            if (on) {
                int prog = gm_patch(chan_prog[ch]);
                std::vector<int> free_, same;
                for (int v = 0; v < N_MELODIC; v++)
                    if (!voice[v].on) free_.push_back(v);
                for (int v : free_)
                    if (voice_last_chan[v] == ch) same.push_back(v);
                int v;
                if (!same.empty()) v = same[0];
                else if (!free_.empty()) v = free_[0];
                else {
                    v = 0;
                    for (int k = 1; k < N_MELODIC; k++)
                        if (voice[k].start < voice[v].start) v = k;
                    S.emit(fr, {0x88 + v});
                }
                set_voice(v, fr, prog, pan_bits(chan_pan[ch]), chan_bend[ch]);
                if (!voice[v].on) S.emit(fr, {0x88 + v});
                voice[v].on = true;
                voice[v].ch = ch;
                voice[v].note = note;
                voice[v].start = fr;
                voice[v].vel = vel;
                voice_last_chan[v] = ch;
                S.emit(fr, {0x80 + v, note, att_of(vel, chan_vol[ch])});
            } else
                for (int v = 0; v < N_MELODIC; v++)
                    if (voice[v].on && voice[v].ch == ch && voice[v].note == note) {
                        voice[v].on = false;
                        S.emit(fr, {0x88 + v});
                        break;
                    }
        }
    }
    for (int v = 0; v < 6; v++) S.emit(end_frame + 1, {0x88 + v});   // all keys off at the end
    return S.body(loops);
}

// ---------------------------------------------------------------- sound effects (build_sfx.py)
// the effects' patches, noise sounds and note kinds
const int P_GUITAR = 0, P_GUN = 6, P_LEAD = 7, P_TIMPANI = 8, P_SYNDRUM = 9;
enum { N_SNARE, N_HAT, N_OPEN, N_CRASH, N_SHOT, N_BLAST, N_HISS, N_TICK };
enum Kind { K_FM, K_KICK, K_GUN, K_NOISE };
// an effect's note: frames start..end, kind, patch (or noise sound),
// MIDI note and velocity; cap: the longest it may last (0 none);
// open_end: no note-off seen yet
struct Note {
    long start, end;
    Kind kind;
    int inst, note, vel;
    int cap = 0;
    bool open_end = false;
};
// patches whose pitch falls: by `semis` semitones every `every` frames
bool falling(int inst, int &semis, int &every)
{
    if (inst == P_TIMPANI) {
        semis = 2;
        every = 2;
        return true;
    }
    if (inst == P_SYNDRUM) {
        semis = every = 1;
        return true;
    }
    return false;
}

// a MIDI effect -> notes (build_sfx.py): drums to kick or noise notes,
// program 127 to gunshots, 122 to a hiss, the rest to FM notes
std::vector<Note> from_midi(const Bytes &file)
{
    std::vector<Ev> events;
    Tempo T;
    T.div = parse_midi(file, events);
    T.changes.push_back({0, 500000});
    for (auto &e : events)
        if (e.meta && e.kind == 0x51) T.changes.push_back({e.tick, be_n(e.data)});
    int prog[16] = {};
    std::map<std::pair<int, int>, size_t> held;   // (channel, note) -> its sounding Note
    std::vector<Note> out;
    for (auto &e : events) {
        if (e.meta) continue;
        long fr = T.frame(e.tick);
        auto D = [&](size_t k) { return k < e.data.size() ? (int)e.data[k] : 0; };
        if (e.kind == 0xC0) prog[e.ch] = D(0);
        else if (e.kind == 0x80 || e.kind == 0x90) {
            bool on = e.kind == 0x90 && D(1) > 0;
            std::pair<int, int> key{e.ch, D(0)};
            if (on) {
                if (e.ch == 9) {
                    int n = D(0);
                    if (n == 35 || n == 36) out.push_back({fr, fr + 8, K_KICK, 0, 60, 127});
                    else if (n == 69) continue;                            // (the cabasa: left out)
                    else {
                        int d = n == 37 || n == 38 ? N_SNARE : n == 70 ? N_TICK : n == 42 ? N_HAT : N_SNARE;
                        out.push_back({fr, fr + 8, K_NOISE, d, 60, 127});
                    }
                } else if (prog[e.ch] == 127)
                    out.push_back({fr, fr + 24, K_GUN, 0, D(0), D(1)});
                else if (prog[e.ch] == 122)
                    out.push_back({fr, fr + 60, K_NOISE, N_HISS, 60, D(1)});
                else {
                    static const std::map<int, int> GM = {{30, P_GUITAR}, {31, P_GUITAR}, {29, P_GUITAR},
                                                          {87, P_LEAD},   {47, P_TIMPANI}, {118, P_SYNDRUM}};
                    auto it = GM.find(prog[e.ch]);
                    Note n{fr, 0, K_FM, it == GM.end() ? P_GUITAR : it->second, D(0), D(1)};
                    int s1, s2;
                    n.cap = falling(n.inst, s1, s2) ? 30 : 0;
                    n.open_end = true;
                    held[key] = out.size();
                    out.push_back(n);
                }
            } else {
                auto it = held.find(key);
                if (it != held.end()) {
                    out[it->second].end = fr;
                    out[it->second].open_end = false;
                    held.erase(it);
                }
            }
        }
    }
    long end = 0;
    for (auto &e : events) end = std::max(end, T.frame(e.tick));
    for (auto &n : out) {
        if (n.open_end) n.end = end;
        if (n.cap) n.end = std::min(n.end, n.start + n.cap);
        n.end = std::max(n.end, n.start + 2);
    }
    return out;
}

// Notes -> an effect stream: noise notes on voice 6; the others on FM
// voices 4 and 5 (a free one, else the one started earlier is cut), each
// with its patch, its slides, and a key-off where the next note doesn't
// follow straight on.
Bytes sfx_stream(const std::vector<Note> &notes)
{
    Stream S;
    const int VF[2] = {4, 5};   // the effects' FM voices, both centre-panned
    for (int v : VF) S.emit(0, {0xA8 + v, 0xC0});
    std::vector<size_t> order(notes.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return notes[a].start < notes[b].start; });
    std::map<int, long> busy_end, busy_start;
    std::map<int, bool> busy;
    std::map<int, std::vector<size_t>> on_voice;
    for (size_t oi : order) {
        const Note &n = notes[oi];
        if (n.kind == K_NOISE) {
            int a = att_of(n.vel, 127);
            S.emit(n.start, {0x80 + V_NOISE, n.inst, (int)std::min(15.0, std::nearbyint(a / 2.67))});
            continue;
        }
        int v = -1;
        for (int c : VF)
            if (!busy[c] || busy_end[c] <= n.start) {
                v = c;
                break;
            }
        if (v < 0) v = busy_start[4] <= busy_start[5] ? 4 : 5;
        busy[v] = true;
        busy_end[v] = n.end;
        busy_start[v] = n.start;
        on_voice[v].push_back(oi);
    }
    for (int v : VF) {
        int patch = -1;
        auto &lst = on_voice[v];
        for (size_t i = 0; i < lst.size(); i++) {
            const Note &n = notes[lst[i]];
            long stop = i + 1 < lst.size() ? std::min(n.end, notes[lst[i + 1]].start) : n.end;
            int a = att_of(n.vel, 127);
            int inst = n.kind == K_KICK ? P_KICK : n.kind == K_GUN ? P_GUN : n.inst;
            if (patch != inst) {
                S.emit(n.start, {0x90 + v, inst});
                patch = inst;
            }
            std::vector<std::array<long, 3>> slide;
            if (n.kind == K_GUN) {
                S.emit(n.start, {0x80 + V_NOISE, n.note >= 50 ? N_SHOT : N_BLAST, 0});
                int pitch = n.note - 12;
                S.emit(n.start, {0x98 + v, 0});
                S.emit(n.start, {0x80 + v, pitch, a});
                for (int k = 1; k < 10; k++) slide.push_back({n.start + k, 0xB0 + v, pitch - k * 2});
            } else if (n.kind == K_KICK) {
                S.emit(n.start, {0x98 + v, 0});
                S.emit(n.start, {0x80 + v, 40, a});
                for (int k = 1; k < 7; k++) slide.push_back({n.start + k, 0x98 + v, (-k * 20) & 0xFF});
            } else {
                S.emit(n.start, {0x98 + v, 0});
                S.emit(n.start, {0x80 + v, n.note, a});
                int semis, every;
                if (falling(n.inst, semis, every)) {
                    long steps = std::min<long>(12 / semis, (n.end - n.start) / every);
                    for (long k = 1; k <= steps; k++)
                        slide.push_back({n.start + k * every, 0xB0 + v, std::max<long>(24, n.note - k * semis)});
                }
            }
            for (auto &sl : slide)
                if (sl[0] < stop) S.emit(sl[0], {(int)sl[1], (int)(sl[2] & 0xFF)});
            if (i + 1 == lst.size() || stop < notes[lst[i + 1]].start) S.emit(stop, {0x88 + v});
        }
    }
    long end = 0;
    for (auto &n : notes) end = std::max(end, n.end);
    return S.body(false, end);
}

// helpers to write effects by hand: an FM note, a noise hit, a run of
// notes `step` frames apart, and joining two lists
Note fm(long s, long frames, int inst, int note, int vel = 127) { return {s, s + frames, K_FM, inst, note, vel}; }
Note noise(long s, int typ, int vel = 127) { return {s, s + 8, K_NOISE, typ, 60, vel}; }
std::vector<Note> seq(long s, long step, int inst, std::initializer_list<int> notes, long frames = 0, int vel = 127)
{
    std::vector<Note> o;
    int k = 0;
    for (int n : notes) o.push_back(fm(s + k++ * step, frames ? frames : step, inst, n, vel));
    return o;
}
std::vector<Note> operator+(std::vector<Note> a, const std::vector<Note> &b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}
}  // namespace

// see assets.h: blocks song_MUS_TITLE (looping), song_MUS_INTRO, song_MUS_END
bool md_music(const Jar &jar, Blocks &out, std::string &err)
{
    static const struct {
        const char *name, *file;
        bool loops;
    } songs[] = {{"MUS_TITLE", "t.mid", true}, {"MUS_INTRO", "i.mid", false}, {"MUS_END", "e.mid", false}};
    try {
        for (auto &s : songs) {
            const Bytes *f = jar.get(s.file);
            if (!f) throw std::runtime_error(std::string("the game has no ") + s.file);
            out[std::string("song_") + s.name] = convert_song(*f, s.loops);
        }
    } catch (const std::exception &e) {
        err = std::string("the music: ") + e.what();
        return false;
    }
    return true;
}

// see assets.h: a block sfx_<name> for each effect below, in the
// driver's order
bool md_sfx(const Jar &jar, Blocks &out, std::string &err)
{
    try {
        // an effect from one of the jar's MIDI files
        auto clip = [&](const char *f) {
            const Bytes *b = jar.get(f);
            if (!b) throw std::runtime_error(std::string("the game has no ") + f);
            return from_midi(*b);
        };
        const int G = P_GUITAR, L = P_LEAD, T = P_TIMPANI, D = P_SYNDRUM;
        std::vector<std::pair<std::string, std::vector<Note>>> eff = {
            {"none", {}},
            {"axe", clip("0.mid")},
            {"extinguisher", clip("1.mid")},
            {"pistol", clip("2.mid")},
            {"shotgun", clip("3.mid")},
            {"chaingun", clip("4.mid")},
            {"plasma", clip("6.mid")},
            {"rocket", clip("7.mid")},
            {"bfg", clip("8.mid")},
            {"mon_attack", {fm(0, 9, G, 40, 120), fm(0, 9, G, 47, 110)}},
            {"fireball", {noise(0, N_HISS, 110), fm(0, 18, D, 64, 120)}},
            {"raise", seq(0, 4, G, {36, 39, 42, 45, 48, 51}, 0, 110)},
            {"mon_hurt", {fm(0, 6, G, 45, 115), noise(0, N_TICK, 100)}},
            {"mon_die", seq(0, 5, G, {42, 41, 40, 39, 38, 37}, 0, 115)},
            {"hurt", {Note{0, 6, K_KICK, 0, 60, 127}, fm(0, 10, D, 52, 115)}},
            {"dodge", {noise(0, N_OPEN, 100)}},
            {"player_die", clip("n.mid")},
            {"step", {noise(0, N_TICK, 70)}},
            {"bump", {Note{0, 6, K_KICK, 0, 60, 100}}},
            {"pickup", seq(0, 4, L, {76, 83}, 0, 110)},
            {"credits", seq(0, 3, L, {84, 91}, 0, 105)},
            {"weapon", seq(0, 4, L, {60, 64, 67, 72}, 6, 115)},
            {"key", seq(0, 5, L, {67, 72, 76}, 8, 115)},
            {"heal", clip("p.mid")},
            {"door", {fm(0, 16, T, 40, 120), noise(0, N_HISS, 90)}},
            {"denied", seq(0, 6, L, {50, 47}, 7, 115)},
            {"explode", clip("7.mid")},
            {"break", {noise(0, N_SNARE, 120), fm(0, 10, D, 48, 120)}},
            {"level_up", seq(0, 3, L, {60, 64, 67, 72, 76, 79}, 4, 115) + std::vector<Note>{fm(18, 20, L, 84, 115)}},
            {"blip", clip("m.mid")},
            {"select", seq(0, 3, L, {72, 79}, 4, 100)},
            {"dialog", seq(0, 3, L, {69, 81}, 4, 90)},
            {"switch", {noise(0, N_TICK, 120), fm(0, 5, D, 72, 110)}},
            {"complete", {}},
            {"dog", clip("9.mid")},
            {"secret", clip("p.mid")},
            {"item", clip("p.mid")},
        };
        for (auto &e : eff) out["sfx_" + e.first] = sfx_stream(e.second);
    } catch (const std::exception &e) {
        err = std::string("the sound effects: ") + e.what();
        return false;
    }
    return true;
}

// ================================================================ the SNES (Deimos)
// see assets.h: one block, snes_res, holding the Mega Drive's graphics
// (title word `word`) and every sound stream for the SPC
bool snes_res(const Jar &jar, const std::string &version, Blocks &out, std::string &err, const char *word)
{
    Blocks b;
    if (!md_ui(jar, version, b, err, word) || !md_music(jar, b, err) || !md_sfx(jar, b, err)) return false;
    // the SPC's streams (tools/snes_sound.py streams): for SPC memory $2000:
    // u16 the three songs, u16 the number of effects, u16 each effect, then them
    static const char *SONGS[] = {"song_MUS_TITLE", "song_MUS_INTRO", "song_MUS_END"};
    static const char *SFX[] = {"none", "axe", "extinguisher", "pistol", "shotgun", "chaingun", "plasma", "rocket",
                                "bfg", "mon_attack", "fireball", "raise", "mon_hurt", "mon_die", "hurt", "dodge",
                                "player_die", "step", "bump", "pickup", "credits", "weapon", "key", "heal", "door",
                                "denied", "explode", "break", "level_up", "blip", "select", "dialog", "switch",
                                "complete", "dog", "secret", "item"};
    const int NS = (int)(sizeof SFX / sizeof SFX[0]);
    std::vector<const Bytes *> parts;
    for (auto n : SONGS) {
        auto it = b.find(n);
        if (it == b.end()) {
            err = std::string("the music: no ") + n;
            return false;
        }
        parts.push_back(&it->second);
    }
    for (auto n : SFX) {
        auto it = b.find(std::string("sfx_") + n);
        if (it == b.end()) {
            err = std::string("the sound effects: no ") + n;
            return false;
        }
        parts.push_back(&it->second);
    }
    const u32 AT = 0x2000;   // where the SPC loads the streams
    u32 head = 6 + 2 + 2 * NS;
    Bytes spc(head, 0), body;
    for (size_t i = 0; i < parts.size(); i++) {
        u32 a = AT + head + (u32)body.size();
        size_t at = i < 3 ? 2 * i : 8 + 2 * (i - 3);
        spc[at] = (u8)a;
        spc[at + 1] = (u8)(a >> 8);
        body.insert(body.end(), parts[i]->begin(), parts[i]->end());
    }
    spc[6] = (u8)NS;
    spc[7] = 0;
    spc.insert(spc.end(), body.begin(), body.end());
    if (AT + spc.size() > 0xFF00) {   // (the streams must end below SPC address $FF00)
        err = "the music and sounds don't fit the SNES's sound memory";
        return false;
    }
    // the resources (tools/snes_ui_res.py): "DRPGRES1", u16 count, u16 0,
    // per entry u32 offset, u32 size (little-endian), the entries 2-aligned
    static const char *NAMES[] = {"ui_font", "ui_faces", "ui_icons", "ui_hud_pal", "ui_dlg_pal", "title_tiles",
                                  "title_map", "title_pal", "spc_streams"};
    const int NN = 9;
    b["spc_streams"] = spc;
    Bytes res = {'D', 'R', 'P', 'G', 'R', 'E', 'S', '1', (u8)NN, 0, 0, 0};
    res.resize(12 + 8 * NN, 0);
    for (int i = 0; i < NN; i++) {
        const Bytes &e = b[NAMES[i]];
        if (res.size() % 2) res.push_back(0);
        u32 off = (u32)res.size(), n = (u32)e.size();
        for (int k = 0; k < 4; k++) {
            res[12 + 8 * i + k] = (u8)(off >> (8 * k));
            res[16 + 8 * i + k] = (u8)(n >> (8 * k));
        }
        res.insert(res.end(), e.begin(), e.end());
    }
    out["snes_res"] = res;
    return true;
}

}  // namespace assets
