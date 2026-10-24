// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Licence: GPL-2.0-or-later.
//
// The Mega Drive builder (see md_build.h), in three parts:
//   the weapon     frame_screen and make_weapons: the 18 weapon frames as
//                  hardware sprites (shared by every map);
//   one map        make_tables: wall pieces, textures, visibility,
//                  sprites, props;
//   the campaign   make_cmap, assemble and new_campaign: the block of
//                  every map.
// A map's pack ("MDPK"): 32-byte header (u16 version at 4, u16 entry count
// at 6, six u16 counts at 8: walls, textures, sprites, props, floor and
// ceiling colour, then the portal's texture swaps at 20..30), then per
// entry a u32 offset and u32 size, then the entries, each at a multiple
// of 4.
#include "md_build.h"
#include "md_areas.h"
#include "colour.h"
#include "geometry.h"
#include "levelblob.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

namespace md {
namespace {

// the 3D view's size in pixels; the most weapon tiles per frame; SPLIT:
// the tile number where the weapon's VRAM area is interrupted (a sprite's
// tiles may not straddle it)
const int VIEW_W = 256, VIEW_H = 160, MAX_TILES = 256, SPLIT = 192;
// Combat.c wpinfo: shots, hold, idle x, idle y, attack x, attack y
const int WPINFO[9][6] = {{1, 30, 20, 25, 20, 0}, {1, 50, 0, 15, 0, 0}, {1, 35, 0, 15, 0, 0},
                          {1, 35, 0, 12, 0, 0},   {3, 15, 0, 15, 0, 0}, {1, 40, 0, 15, 0, 0},
                          {3, 10, 0, 15, 0, 0},   {1, 50, 0, 15, 0, 0}, {1, 50, 0, 13, 0, 5}};

// big-endian append (be16, be32) and read (get_be32)
void be16(Bytes &b, unsigned v)
{
    b.push_back((v >> 8) & 0xFF);
    b.push_back(v & 0xFF);
}
void be32(Bytes &b, u32 v)
{
    for (int i = 3; i >= 0; i--) b.push_back((v >> (8 * i)) & 0xFF);
}
u32 get_be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
// RGB <-> 0xRRGGBB
u32 key_of(RGB c) { return ((u32)c.r << 16) | ((u32)c.g << 8) | c.b; }
RGB rgb_of(u32 k) { return {(u8)(k >> 16), (u8)(k >> 8), (u8)k}; }

// round(x / total * 255) of each count, 0 stays 0, used colours at least 1
int weight(double x, double total) { return std::max(1, (int)py_round(x / total * 255)); }

// a Mega Drive tile: 4 bits per pixel, left pixel in the high nibble
Bytes tile_bytes(const u8 *px)   // 64 indices, row-major -> 32 bytes
{
    Bytes out;
    for (int i = 0; i < 64; i += 2) out.push_back((u8)(((px[i] & 15) << 4) | (px[i + 1] & 15)));
    return out;
}

// the four weapon tables (the wp_* entries), big-endian:
//   frames   per frame 5 u16: first tile, tile count, palette number,
//            first sprite, sprite count
//   sprites  per sprite 4 u16: x, y in the view, size ((w-1) << 2 | (h-1),
//            in tiles), first tile (within the frame)
//   pal      16 colours per frame
//   tiles    every frame's tiles, 32 bytes each, column-major per sprite
struct Weapons {
    Bytes frames, sprites, pal, tiles;
};

// one frame: 256x160 screen of 0 / 1+colour, and the 16 CRAM colours
// Frame fi = weapon fi / 2, still (even) or firing (odd). The 64 x 64
// picture is drawn at double size, placed by WPINFO's offsets (x centred
// on 128, y from the view's bottom). alpha marks the opaque screen pixels.
void frame_screen(const Project &p, int fi, std::vector<u8> &idx, u16 pal[16], std::vector<char> &alpha)
{
    int w = fi / 2, f = fi & 1;
    Art cv = p.flat(p.weapons.at(fi));
    int px = f ? WPINFO[w][4] : WPINFO[w][2], py = f ? WPINFO[w][5] : WPINFO[w][3];
    int x0 = 128 + (px - 32) * 2, y0 = VIEW_H - (64 - py) * 2;
    std::vector<RGB> scr(VIEW_W * VIEW_H);
    alpha.assign(VIEW_W * VIEW_H, 0);
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x++) {
            int sx = x0 + x, sy = y0 + y;
            if (sx < 0 || sy < 0 || sx >= VIEW_W || sy >= VIEW_H) continue;
            int i = (y / 2) * 64 + x / 2;
            if (cv.clear(i)) continue;
            scr[sy * VIEW_W + sx] = cv.at(i);
            alpha[sy * VIEW_W + sx] = 1;
        }
    // palette line 3 (tools/build_weapons.py frame_colours): the dialog's
    // colours in slots 1, 2, 3, 15 and up to 9 of the frame's own in 4..12,
    // the worst served first (pixels x distance); each pixel the nearest
    static const int LEVS[8] = {0, 52, 87, 116, 144, 172, 206, 255};   // the DAC's 8 levels
    // distance of two CRAM colours: weighted squared level differences
    // (red x3, green x4, blue x2)
    auto cdist = [](u16 a, u16 b) {
        int d[3];
        for (int k = 0; k < 3; k++) d[k] = LEVS[(a >> (1 + 4 * k)) & 7] - LEVS[(b >> (1 + 4 * k)) & 7];
        return 3 * d[0] * d[0] + 4 * d[1] * d[1] + 2 * d[2] * d[2];
    };
    std::map<u16, long long> weight;   // CRAM colour -> pixel count
    for (int i = 0; i < VIEW_W * VIEW_H; i++)
        if (alpha[i]) weight[md_colour(scr[i])]++;
    std::map<int, u16> slots = {{1, md_colour(RGB{255, 255, 255})}, {2, md_colour(RGB{255, 210, 0})},
                                {3, md_colour(RGB{150, 150, 150})}, {15, md_colour(RGB{10, 20, 70})}};
    std::map<u16, long long> mind;     // each colour's distance to its nearest slot
    for (auto &kv : weight) {
        long long m = -1;
        for (auto &f : slots) {
            long long d = cdist(kv.first, f.second);
            if (m < 0 || d < m) m = d;
        }
        mind[kv.first] = m;
    }
    // slots 4..12: each time the colour with the largest count x distance
    for (int s = 4; s <= 12; s++) {
        int best = -1;
        long long bv = 0;
        for (auto &kv : weight) {
            long long v = kv.second * mind[kv.first];
            if (v > bv) bv = v, best = kv.first;
        }
        if (best < 0) break;   // every colour is matched exactly
        slots[s] = (u16)best;
        for (auto &kv : mind) kv.second = std::min<long long>(kv.second, cdist(kv.first, (u16)best));
    }
    std::map<u16, int> slot_of;
    for (auto &kv : weight) {
        int bs = 0;
        long long bd = -1;
        for (auto &sl : slots) {     // (ascending slots: ties to the lower)
            long long d = cdist(kv.first, sl.second);
            if (bd < 0 || d < bd) bd = d, bs = sl.first;
        }
        slot_of[kv.first] = bs;
    }
    for (int i = 0; i < 16; i++) pal[i] = slots.count(i) ? slots[i] : 0;
    idx.assign(VIEW_W * VIEW_H, 0);
    for (int i = 0; i < VIEW_W * VIEW_H; i++)
        if (alpha[i]) idx[i] = (u8)slot_of[md_colour(scr[i])];
}

// tools/build_weapons.py: each frame's opaque area is cut into sprites of
// up to 4 x 4 tiles on a 32-pixel grid from its top-left (8-aligned);
// empty sprites are skipped. Throws when a frame is empty or needs more
// than MAX_TILES tiles.
Weapons make_weapons(const Project &p)
{
    Weapons out;
    std::vector<u16> frames, sprites, pals;
    std::vector<Bytes> tiles;
    for (int fi = 0; fi < N_WEAPON_FRAMES; fi++) {
        std::vector<u8> idx;
        std::vector<char> alpha;
        u16 pal[16];
        frame_screen(p, fi, idx, pal, alpha);
        int bx0 = VIEW_W, bx1 = 0, by0 = VIEW_H, by1 = 0;
        for (int y = 0; y < VIEW_H; y++)
            for (int x = 0; x < VIEW_W; x++)
                if (alpha[y * VIEW_W + x]) {
                    bx0 = std::min(bx0, x);
                    bx1 = std::max(bx1, x + 1);
                    by0 = std::min(by0, y);
                    by1 = std::max(by1, y + 1);
                }
        if (bx1 == 0) throw std::runtime_error("weapon picture " + std::to_string(fi) + " is empty");
        bx0 -= bx0 % 8;   // the box's corner on the tile grid
        by0 -= by0 % 8;
        size_t first_spr = sprites.size(), first_tile = tiles.size();
        for (int sy = by0; sy < by1; sy += 32)
            for (int sx = bx0; sx < bx1; sx += 32) {
                // this sprite's size in tiles (up to 4 x 4, less at the box's edge)
                int wt = std::min(4, (std::min(bx1, VIEW_W) - sx + 7) / 8);
                int ht = std::min(4, (std::min(by1, VIEW_H) - sy + 7) / 8);
                std::vector<u8> block(ht * 8 * wt * 8, 0);
                bool any = false;
                for (int y = 0; y < ht * 8; y++)
                    for (int x = 0; x < wt * 8; x++) {
                        int X = sx + x, Y = sy + y;
                        if (X < VIEW_W && Y < VIEW_H) {
                            block[y * wt * 8 + x] = idx[Y * VIEW_W + X];
                            if (block[y * wt * 8 + x]) any = true;
                        }
                    }
                if (!any) continue;
                int t0 = (int)(tiles.size() - first_tile);
                if (t0 < SPLIT && SPLIT < t0 + wt * ht) {   // a sprite's tiles must be contiguous in VRAM
                    for (int k = t0; k < SPLIT; k++) tiles.push_back(Bytes(32, 0));
                    t0 = SPLIT;
                }
                // the VDP wants a sprite's tiles column by column
                for (int tx = 0; tx < wt; tx++)
                    for (int ty = 0; ty < ht; ty++) {
                        u8 t[64];
                        for (int y = 0; y < 8; y++)
                            for (int x = 0; x < 8; x++) t[y * 8 + x] = block[(ty * 8 + y) * wt * 8 + tx * 8 + x];
                        tiles.push_back(tile_bytes(t));
                    }
                sprites.insert(sprites.end(), {(u16)sx, (u16)sy, (u16)(((wt - 1) << 2) | (ht - 1)), (u16)t0});
            }
        int nt = (int)(tiles.size() - first_tile);
        if (nt > MAX_TILES)
            throw std::runtime_error("weapon picture " + std::to_string(fi) + " needs " + std::to_string(nt) +
                                     " tiles (the Mega Drive version has room for 256)");
        frames.insert(frames.end(), {(u16)first_tile, (u16)nt, (u16)(pals.size() / 16), (u16)(first_spr / 4),
                                     (u16)((sprites.size() - first_spr) / 4)});
        pals.insert(pals.end(), pal, pal + 16);
    }
    for (u16 v : frames) be16(out.frames, v);
    for (u16 v : sprites) be16(out.sprites, v);
    for (u16 v : pals) be16(out.pal, v);
    for (auto &t : tiles) out.tiles.insert(out.tiles.end(), t.begin(), t.end());
    return out;
}

}  // namespace

// see md_build.h (the Art tab's preview)
void weapon_frame(const Project &p, int frame, std::vector<u8> &idx, u16 pal[16])
{
    std::vector<char> alpha;
    frame_screen(p, frame, idx, pal, alpha);
}

namespace {
// what make_tables made: counts for the pack header
struct Counts {
    int walls = 0, tex = 0, spr = 0, scenery = 0;
    std::vector<std::pair<int, int>> swaps;   // the portal's texture slots: open -> closed
};

// the Reactor's portal: its textures (the walls of its line entities, eType 7
// eSub 3 and 4) -> theirs once both power couplings are destroyed
// (Entity_powerCouplingDied); fixed in the original's code. As
// tools/build_render_data.py's PORTAL_CLOSED_TEX.
const std::map<int, int> PORTAL_CLOSED_TEX = {{34, 92}, {33, 91}};
const int MAX_SWAPS = 5;   // room in the pack header (bytes 21..30)

// pack header bytes 20..30: the number of swaps, then (open, closed) slot pairs
void put_swaps(Bytes &head, const std::vector<std::pair<int, int>> &sw)
{
    head[20] = (u8)sw.size();
    for (size_t k = 0; k < sw.size(); k++) {
        head[21 + 2 * k] = (u8)sw[k].first;
        head[22 + 2 * k] = (u8)sw[k].second;
    }
}

// One map's render tables (tools/build_render_data.py tables()), in the
// campaign's form: the props hidden at the start left out, each prop with
// its cell.
bool make_tables(const Project &p0, const BuildOptions &opt, std::map<std::string, Bytes> &blobs, Counts &cnt,
                 std::string &err)
{
    Project pf = p0;   // drop the props hidden at the start (sprite flag 0x10000)
    pf.scenery.clear();
    for (auto &sc : p0.scenery)
        if (!(sc.info & 0x10000)) pf.scenery.push_back(sc);
    const Project &p = pf;
    Geometry geo;
    try {
        geo = build_geometry(p);
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }
    auto &walls = geo.segs;
    // ---- walls
    // 8 u16 per piece: ax, az, bx, bz, texture image, door slot (255
    // none), ua | two-sided << 8, ub
    Bytes &bw = blobs["walls"];
    for (auto &s : walls)
        for (int v : {s.ax, s.az, s.bx, s.bz, s.tex, s.line, s.ua | (s.two << 8), s.ub}) be16(bw, (u16)v);
    // ---- the portal's closed textures, after the walls' own
    // each plain portal texture gets its closed texture as an extra image,
    // and a swap (its slot -> the extra one's) for the game to apply
    size_t n_wall_tex = geo.tex_imgs.size();
    for (size_t t = 0; t < n_wall_tex; t++) {
        auto c = PORTAL_CLOSED_TEX.find(geo.tex_src[t]);
        if (!geo.tex_plain[t] || c == PORTAL_CLOSED_TEX.end()) continue;
        auto tx = p.textures.find(c->second);
        if (tx == p.textures.end()) continue;
        Art a = p.flat(tx->second);
        std::vector<RGB> img(4096);
        for (int i = 0; i < 4096; i++) img[i] = a.at(i);
        if ((int)cnt.swaps.size() < MAX_SWAPS) cnt.swaps.push_back({(int)t, (int)geo.tex_imgs.size()});
        geo.tex_imgs.push_back(std::move(img));
        geo.tex_src.push_back(c->second);
        geo.tex_plain.push_back(1);
    }
    // ---- textures: 64x64 indices (u*64 + v) into their own (up to 32) colours
    if ((int)geo.tex_imgs.size() > R_MAX_TEX) {
        err = std::to_string(geo.tex_imgs.size()) + " different wall textures (with wall decals): the Mega Drive "
              "version allows " + std::to_string(R_MAX_TEX);
        return false;
    }
    // tex: 4096 bytes per image, column by column; tex_pal: its colours
    // (in RGB order, 32 CRAM words); tex_hist: each colour's share of the
    // pixels, 1..255 (0 = unused), for the game's per-area palettes
    for (size_t t = 0; t < geo.tex_imgs.size(); t++) {
        auto &img = geo.tex_imgs[t];
        std::map<u32, int> idx_of;
        for (auto &c : img) idx_of[key_of(c)] = 0;
        if (idx_of.size() > 32) {
            err = "a wall texture has " + std::to_string(idx_of.size()) + " colours (with its decals); at most 32";
            return false;
        }
        int n = 0;
        for (auto &kv : idx_of) kv.second = n++;
        std::vector<int> h(32, 0);
        for (int u = 0; u < 64; u++)
            for (int v = 0; v < 64; v++) blobs["tex"].push_back((u8)idx_of[key_of(img[v * 64 + u])]);
        for (auto &c : img) h[idx_of[key_of(c)]]++;
        for (auto &kv : idx_of) be16(blobs["tex_pal"], md_colour(rgb_of(kv.first)));
        for (int k = n; k < 32; k++) be16(blobs["tex_pal"], 0);
        for (int k = 0; k < 32; k++) be16(blobs["tex_hist"], h[k] ? weight(h[k], 4096.0) : 0);
    }
    // ---- visibility: walls per (cell, facing), doors open and shut; cells whose things can show
    opt.say("working out what can be seen from each cell...");
    std::vector<char> doors(walls.size()), none(walls.size(), 0);
    for (size_t i = 0; i < walls.size(); i++) doors[i] = walls[i].line != 255;
    std::vector<int> cells;
    for (int i = 0; i < 1024; i++)
        if (!p.wall[i]) cells.push_back(i);
    std::vector<std::vector<int>> vis(cells.size() * 4);
    std::vector<Bytes> maps(cells.size() * 4);
    std::atomic<int> done(0);
    parallel_for((int)cells.size(), opt.n_threads(), [&](int ci) {
        if (opt.cancelled()) return;
        int gx = cells[ci] % 32, gz = cells[ci] / 32;
        for (int f = 0; f < 4; f++) {
            std::vector<int> vo, vc;
            std::vector<double> colz;
            visible_walls(walls, doors, gx, gz, f, vo, &colz);
            visible_walls(walls, none, gx, gz, f, vc, nullptr);
            std::set<int> u(vo.begin(), vo.end());
            u.insert(vc.begin(), vc.end());
            vis[ci * 4 + f].assign(u.begin(), u.end());
            maps[ci * 4 + f] = visible_cells(colz, gx, gz, f);
        }
        int k = ++done;
        if (k % 16 == 0) opt.at(0.05f + 0.75f * k / cells.size());
    });
    if (opt.cancelled()) {
        err = "cancelled";
        return false;
    }
    // pvs: u16 words, per view a count then the wall ids; pvs_index[cell *
    // 4 + f] = the view's word index (0: the empty list at word 0).
    // vis: 128-byte cell bit maps; vis_index = the view's map number (map
    // 0, all cells set, for views that don't exist)
    std::vector<int> pvs_index(4096, 0), vis_index(4096, 0);
    std::vector<int> pvs = {0};
    Bytes vis_maps(128, 255);
    for (size_t ci = 0; ci < cells.size(); ci++)
        for (int f = 0; f < 4; f++) {
            int k = cells[ci] * 4 + f;
            vis_index[k] = (int)(vis_maps.size() / 128);
            vis_maps.insert(vis_maps.end(), maps[ci * 4 + f].begin(), maps[ci * 4 + f].end());
            pvs_index[k] = (int)pvs.size();
            pvs.push_back((int)vis[ci * 4 + f].size());
            pvs.insert(pvs.end(), vis[ci * 4 + f].begin(), vis[ci * 4 + f].end());
        }
    if (pvs.size() > 65535 || vis_maps.size() / 128 > 65535) {
        err = "the visibility table is too big for the Mega Drive version";
        return false;
    }
    for (int v : pvs_index) be16(blobs["pvs_index"], v);
    for (int v : pvs) be16(blobs["pvs"], v);
    for (int v : vis_index) be16(blobs["vis_index"], v);
    blobs["vis"] = vis_maps;
    // ---- sprites: every tile the level can show
    // per sprite (slot = order of tile number): spr_pal its 16 colours,
    // spr_hist their shares (1..255), spr_off the offset of its data,
    // spr_cols per column (first row, row count), spr_coff per column the
    // u16 offset of its rows from spr_off; spr_data the rows' colour
    // indices (255 = transparent)
    std::set<int> ts;
    for (auto &d : p.defs) ts.insert(d.tile);
    for (auto &s : geo.scenery) ts.insert(s.tile);
    std::vector<int> tiles(ts.begin(), ts.end());
    if ((int)tiles.size() > R_MAX_SPR) {
        err = std::to_string(tiles.size()) + " different sprites: the Mega Drive version allows " +
              std::to_string(R_MAX_SPR);
        return false;
    }
    std::map<int, int> slot;
    Bytes &spr_data = blobs["spr_data"];
    for (size_t si = 0; si < tiles.size(); si++) {
        int t = tiles[si];
        slot[t] = (int)si;
        auto it = p.sprites.find(t);
        if (it == p.sprites.end()) {
            err = "sprite " + std::to_string(t) + " is used but has no picture";
            return false;
        }
        Art cv = p.flat(it->second);
        std::map<u32, int> cols;
        for (int i = 0; i < 4096; i++)
            if (!cv.clear(i)) cols[key_of(cv.at(i))] = 0;
        if (cols.size() > 16) {
            err = "sprite " + std::to_string(t) + " has " + std::to_string(cols.size()) + " colours; at most 16";
            return false;
        }
        std::vector<RGB> cl;
        for (auto &kv : cols) {
            kv.second = (int)cl.size();
            cl.push_back(rgb_of(kv.first));
        }
        // every pixel -> the closest of its colours (transparent ones are black)
        std::vector<u8> idx(4096, 0);
        if (!cl.empty())
            for (int i = 0; i < 4096; i++) {
                RGB c = cv.clear(i) ? RGB{0, 0, 0} : cv.at(i);
                int best = 0;
                long long bd = -1;
                for (size_t k = 0; k < cl.size(); k++) {
                    long long d = (long long)(c.r - cl[k].r) * (c.r - cl[k].r) +
                                  (long long)(c.g - cl[k].g) * (c.g - cl[k].g) +
                                  (long long)(c.b - cl[k].b) * (c.b - cl[k].b);
                    if (bd < 0 || d < bd) {
                        bd = d;
                        best = (int)k;
                    }
                }
                idx[i] = (u8)best;
            }
        for (size_t k = 0; k < 16; k++) be16(blobs["spr_pal"], k < cl.size() ? md_colour(cl[k]) : 0);
        std::vector<int> h(16, 0);
        int tot = 0;
        for (int i = 0; i < 4096; i++)
            if (!cv.clear(i)) {
                h[idx[i]]++;
                tot++;
            }
        for (int k = 0; k < 16; k++) be16(blobs["spr_hist"], (tot && h[k]) ? weight(h[k], (double)tot) : 0);
        u32 off = (u32)spr_data.size();
        be32(blobs["spr_off"], off);
        for (int c = 0; c < 64; c++) {
            be16(blobs["spr_coff"], (u16)(spr_data.size() - off));
            int top = -1, bot = -1;
            for (int r = 0; r < 64; r++)
                if (!cv.clear(r * 64 + c)) {
                    if (top < 0) top = r;
                    bot = r + 1;
                }
            if (top < 0) {
                blobs["spr_cols"].push_back(0);
                blobs["spr_cols"].push_back(0);
                continue;
            }
            blobs["spr_cols"].push_back((u8)top);
            blobs["spr_cols"].push_back((u8)(bot - top));
            for (int r = top; r < bot; r++) spr_data.push_back(cv.clear(r * 64 + c) ? 255 : idx[r * 64 + c]);
        }
    }
    if (spr_data.size() > 0xFFFFFF) {
        err = "the sprites are too big";
        return false;
    }
    // ---- scenery + entity types
    // scenery: per prop x, z, sprite slot, the info's high 16 bits and its
    // cell; def_spr: each thing type's sprite slot
    for (auto &s : geo.scenery)
        for (int v : {s.x, s.z, slot[s.tile], (int)((s.info >> 16) & 0xFFFF), s.cell}) be16(blobs["scenery"], (u16)v);
    for (auto &d : p.defs) blobs["def_spr"].push_back((u8)slot[d.tile]);
    cnt.walls = (int)walls.size();
    cnt.tex = (int)geo.tex_imgs.size();
    cnt.spr = (int)tiles.size();
    cnt.scenery = (int)geo.scenery.size();
    return true;
}
}  // namespace

// the SNES cartridge (the Deimos repository's tools/snes_rom.py
// finish): a power of two from 256 KB, the header's size code and checksum
// (the header at hdr: $FFB0, or the SuperFX's $7FB0)
static void finish_snes(Bytes &out, size_t hdr = 0xFFB0)
{
    size_t size = 0x40000;
    while (size < out.size()) size *= 2;
    out.resize(size, 0xFF);
    // ROM size code at $xFD7: size = 1 KB << code
    int code = 0;
    while ((size_t)1024 << (code + 1) <= size) code++;
    out[hdr + 0x27] = (u8)code;
    // the checksum is summed with complement $FFFF and checksum $0000 in
    // place, then both are written ($xFDC complement, $xFDE checksum)
    out[hdr + 0x2C] = 0xFF;
    out[hdr + 0x2D] = 0xFF;
    out[hdr + 0x2E] = 0;
    out[hdr + 0x2F] = 0;
    u32 sum = 0;
    for (size_t i = 0; i < size; i++) sum += out[i];
    sum &= 0xFFFF;
    out[hdr + 0x2C] = (u8)((sum ^ 0xFFFF) & 0xFF);
    out[hdr + 0x2D] = (u8)((sum ^ 0xFFFF) >> 8);
    out[hdr + 0x2E] = (u8)(sum & 0xFF);
    out[hdr + 0x2F] = (u8)(sum >> 8);
}

// tools/fix_header.py: padded with $FF to a power of two (at least 128 KB),
// ROM end, checksum
static void finish_rom(Bytes &out)
{
    size_t size = 0x20000;
    while (size < out.size()) size *= 2;
    out.resize(size, 0xFF);
    // $1A4: the ROM's last address; $18E: the sum of the words from $200
    u32 end = (u32)size - 1;
    for (int i = 0; i < 4; i++) out[0x1A4 + i] = (end >> (24 - 8 * i)) & 0xFF;
    u32 sum = 0;
    for (size_t i = 0x200; i + 1 < size; i += 2) sum += (out[i] << 8) | out[i + 1];
    out[0x18E] = (sum >> 8) & 0xFF;
    out[0x18F] = sum & 0xFF;
}

// ======================================================================
// The campaign: every map in one block (tools/md_campaign.py), the
// textures and sprites in shared banks.
//
// The block ("MDCP"), from its start: u16 version (2) at 4, u16 map count
// at 6, u32 offsets of the shared textures (8), shared sprites (12), the
// weapon directory (16), the block's size (20), then two zero u32s (24,
// 28). Then a 32-byte record per map: u32 offset of its pack, its map id
// at 4, its title (upper case, up to 25 characters) at 6.
// The weapon directory: 4 x (u32 offset, u32 size) of the wp_* tables.
// Each map's pack is as above (version 2, the CENTRIES entries); its
// tex_off and spr_off give offsets into the shared textures and sprites,
// so a texture or sprite used by several maps is stored once. Offsets
// are from the block's start.

namespace {
// a campaign map pack's entries, in order (vis_index and vis are word
// lists here, see vis_lists)
const char *CENTRIES[] = {"walls", "tex_off", "tex_pal", "tex_hist", "spr_cols", "spr_off", "spr_coff", "pvs_index",
                          "pvs", "spr_pal", "spr_hist", "scenery", "def_spr", "vis_index", "vis", "level",
                          "strings"};
const int N_CENTRIES = 17, C_TEX_OFF = 1, C_SPR_COLS = 4, C_SPR_OFF = 5, C_SPR_COFF = 6;
const int CAMP_HEAD = 32, MAP_REC = 32;   // the block header and a map record, in bytes

// big-endian read; big-endian write at a position
u16 get_be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
void put_be32(Bytes &b, size_t at, u32 v)
{
    for (int i = 0; i < 4; i++) b[at + i] = (v >> (24 - 8 * i)) & 0xFF;
}

struct CMap {                  // one map as the block holds it
    MapInfo info;
    int counts[6] = {};        // walls, textures, sprites, scenery, floor, ceiling colour
    std::vector<std::pair<int, int>> swaps;   // the portal's texture swaps
    Bytes tabs[N_CENTRIES];    // (tex_off and spr_off: rebuilt from the lists below)
    std::vector<Bytes> tex;    // its textures (4096 each)
    std::vector<Bytes> spr;    // its sprites' column data
};

// a whole campaign block, unpacked
struct CBlock {
    u32 addr = 0, limit = 0;   // where it starts in the ROM, and where it must end
    bool snes = false;         // in a SNES cartridge
    bool fx = false;           // the SuperFX one (no visibility tables)
    std::vector<CMap> maps;
    Bytes weapons[4];          // wp_frames, wp_sprites, wp_pal, wp_tiles
};

// md_campaign.py vis_lists: 128-byte cell maps -> word lists ($FFFF ends one; map 0, every cell, is $FFFF)
// idx_out: per view the word offset of its list in list_out ($FFFF for
// map 0); list_out: the visible cells' numbers, each list ending in $FFFF
bool vis_lists(const Bytes &maps, const Bytes &index, Bytes &idx_out, Bytes &list_out, std::string &err)
{
    std::vector<int> starts, out;
    for (size_t k = 0; k * 128 < maps.size(); k++) {
        if (k == 0) {
            starts.push_back(0xFFFF);
            continue;
        }
        starts.push_back((int)out.size());
        for (int c = 0; c < 1024; c++)
            if (maps[k * 128 + (c >> 3)] >> (c & 7) & 1) out.push_back(c);
        out.push_back(0xFFFF);
    }
    if (out.size() >= 0xFFFF) {
        err = "the visibility lists are too long for the Mega Drive version";
        return false;
    }
    for (size_t i = 0; i + 1 < index.size(); i += 2) be16(idx_out, (u16)starts[get_be16(&index[i])]);
    for (int v : out) be16(list_out, (u16)v);
    return true;
}


}  // namespace

// one map's part of the block: its tables, textures and sprites (nm.info is the caller's)
static bool make_cmap(const Project &p, const std::vector<std::string> &names, CMap &nm, const BuildOptions &opt,
                      std::string &err)
{
    // ---- its tables
    std::map<std::string, Bytes> blobs;
    Counts cnt;
    Bytes level, strings;
    try {
        level = make_level_blob_md(p, names, p.map_id);
        strings = make_strings_blob_md(p);
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }
    if (!make_tables(p, opt, blobs, cnt, err)) return false;
    int cv[6] = {cnt.walls, cnt.tex, cnt.spr, cnt.scenery, (int)md_colour(p.floor), (int)md_colour(p.ceiling)};
    memcpy(nm.counts, cv, sizeof cv);
    nm.swaps = cnt.swaps;
    for (int k = 0; k < cnt.tex; k++)
        nm.tex.emplace_back(blobs["tex"].begin() + 4096 * k, blobs["tex"].begin() + 4096 * (k + 1));
    const Bytes &so = blobs["spr_off"], &sd = blobs["spr_data"];
    // split the sprite data per sprite (each ends where the next starts)
    for (int k = 0; k < cnt.spr; k++) {
        u32 a = get_be32(&so[4 * k]), e = k + 1 < cnt.spr ? get_be32(&so[4 * k + 4]) : (u32)sd.size();
        nm.spr.emplace_back(sd.begin() + a, sd.begin() + e);
    }
    // the tables as they are (the offsets, visibility, level and strings
    // are filled separately)
    for (int k = 0; k < N_CENTRIES; k++) {
        std::string name = CENTRIES[k];
        if (name != "tex_off" && name != "spr_off" && name != "vis_index" && name != "vis" && name != "level" &&
            name != "strings")
            nm.tabs[k] = blobs[name];
    }
    if (!vis_lists(blobs["vis"], blobs["vis_index"], nm.tabs[13], nm.tabs[14], err)) return false;   // bit maps -> lists
    nm.tabs[15] = level;
    nm.tabs[16] = strings;
    return true;
}

// the block (tools/md_campaign.py) after rom[0, cb.addr), then (the Mega
// Drive) its colours per area; the cartridge finished as its console's
static bool assemble(Bytes &rom, CBlock &cb, const BuildOptions &opt, BuildStats &stats, std::string &err)
{
    int n = (int)cb.maps.size();
    // ---- the block (tools/md_campaign.py)
    Bytes blk(CAMP_HEAD + MAP_REC * n, 0);
    memcpy(blk.data(), "MDCP", 4);
    blk[5] = 2;
    blk[6] = (u8)(n >> 8);
    blk[7] = (u8)n;
    // append b at the next multiple of 4; returns its offset
    auto add = [&](const Bytes &b) {
        while (blk.size() % 4) blk.push_back(0);
        u32 off = (u32)blk.size();
        blk.insert(blk.end(), b.begin(), b.end());
        return off;
    };
    u32 wp = add(Bytes(32, 0));   // the weapon directory, then its tables
    for (int i = 0; i < 4; i++) {
        u32 o = add(cb.weapons[i]);
        put_be32(blk, wp + 8 * i, o);
        put_be32(blk, wp + 8 * i + 4, (u32)cb.weapons[i].size());
    }
    // the shared textures and sprites: identical ones stored once
    // (sprites at even offsets); each map's tex_off / spr_off rebuilt
    Bytes tex_bank, spr_bank;
    std::map<Bytes, u32> tex_at, spr_at;
    for (auto &m : cb.maps) {
        m.tabs[C_TEX_OFF].clear();
        for (auto &key : m.tex) {
            auto it = tex_at.find(key);
            if (it == tex_at.end()) {
                it = tex_at.emplace(key, (u32)tex_bank.size()).first;
                tex_bank.insert(tex_bank.end(), key.begin(), key.end());
            }
            be32(m.tabs[C_TEX_OFF], it->second);
        }
        m.tabs[C_SPR_OFF].clear();
        for (auto &key : m.spr) {
            auto it = spr_at.find(key);
            if (it == spr_at.end()) {
                if (spr_bank.size() & 1) spr_bank.push_back(0);
                it = spr_at.emplace(key, (u32)spr_bank.size()).first;
                spr_bank.insert(spr_bank.end(), key.begin(), key.end());
            }
            be32(m.tabs[C_SPR_OFF], it->second);
        }
    }
    u32 tb = add(tex_bank), sb = add(spr_bank);
    put_be32(blk, 8, tb);
    put_be32(blk, 12, sb);
    put_be32(blk, 16, wp);
    // each map's pack, and its record
    for (int i = 0; i < n; i++) {
        auto &m = cb.maps[i];
        Bytes head(32 + 8 * N_CENTRIES, 0);
        memcpy(head.data(), "MDPK", 4);
        head[5] = 2;
        head[7] = N_CENTRIES;
        for (int k = 0; k < 6; k++) {
            head[8 + 2 * k] = (u8)(m.counts[k] >> 8);
            head[9 + 2 * k] = (u8)m.counts[k];
        }
        put_swaps(head, m.swaps);
        u32 po = add(head);
        if (cb.fx)          // (the GSU looks at every wall and thing: tools/snes_fx_block.py)
            for (int k : {7, 8, 13, 14}) m.tabs[k].clear();
        for (int k = 0; k < N_CENTRIES; k++) {
            u32 o = add(m.tabs[k]);
            put_be32(blk, po + 32 + 8 * k, o);
            put_be32(blk, po + 36 + 8 * k, (u32)m.tabs[k].size());
        }
        size_t r = CAMP_HEAD + MAP_REC * i;
        put_be32(blk, r, po);
        blk[r + 4] = (u8)m.info.id;
        std::string title = m.info.title;
        for (auto &ch : title)
            if (ch >= 'a' && ch <= 'z') ch -= 32;
        for (size_t k = 0; k < 25 && k < title.size(); k++) blk[r + 6 + k] = (u8)title[k];
    }
    while (blk.size() % 4) blk.push_back(0);
    put_be32(blk, 20, (u32)blk.size());
    // the Mega Drive's colours per area (tools/md_areas.c), right after the
    // block; its address at $1D8 of the header
    Bytes areas;
    if (!cb.snes && !cb.fx) {
        unsigned char *t = nullptr;
        size_t n = md_areas(blk.data(), blk.size(), &t);   // malloc'ed table in t, its size returned
        if (!n) {
            err = "could not work out the areas' colours";
            return false;
        }
        areas.assign(t, t + n);
        free(t);
    }
    size_t need = blk.size() + areas.size();
    if (cb.addr + need > cb.limit) {
        err = "the maps are too big: they need " + std::to_string(need / 1024) + " KB and there is room for " +
              std::to_string((cb.limit - cb.addr) / 1024) + " KB";
        return false;
    }
    Bytes out(rom.begin(), rom.begin() + cb.addr);
    out.insert(out.end(), blk.begin(), blk.end());
    if (!areas.empty()) {
        put_be32(out, 0x1D8, (u32)out.size());
        out.insert(out.end(), areas.begin(), areas.end());
    }
    if (cb.fx) finish_snes(out, 0x7FB0);
    else if (cb.snes) finish_snes(out);
    else finish_rom(out);
    stats.rom_kb = (int)(out.size() / 1024);
    stats.used_kb = (int)(need / 1024);
    stats.free_kb = (int)((cb.limit - cb.addr - need) / 1024);
    rom = std::move(out);
    opt.at(1.0f);
    return true;
}

// see md_build.h. The weapon comes from the first map's project.
// Progress: 0..80% for the maps.
bool new_campaign(Bytes &rom, u32 addr, u32 limit, const std::vector<MapInfo> &maps,
                  const std::vector<Project> &projects, const BuildOptions &opt, BuildStats &stats, std::string &err,
                  bool snes, bool snes_fx)
{
    if (maps.empty() || maps.size() != projects.size()) {
        err = "no maps to build";
        return false;
    }
    CBlock cb;
    cb.addr = addr;
    cb.limit = limit;
    cb.snes = snes || snes_fx;
    cb.fx = snes_fx;
    rom.resize(addr, 0);
    std::vector<std::string> names;
    for (auto &m : maps) names.push_back(m.name);
    for (size_t i = 0; i < maps.size(); i++) {
        opt.say("map " + std::to_string(i + 1) + " of " + std::to_string(maps.size()) + ": " + maps[i].title);
        CMap nm;
        nm.info = maps[i];
        // the game's data as this console has the map (Project::for_console);
        // a single level (one from an older level folder, say) takes the
        // place's map id, its props are always there and SHOW's "none" is
        // the campaign's $FFFF
        Project q = projects[i].for_console(opt.console);
        if (!q.campaign()) {
            q.map_id = maps[i].id;
            for (auto &sc : q.scenery) sc.cell = -1;
            for (auto &ev : q.events)
                for (auto &c : ev.cmds)
                    if (c.op == 7 && c.a1 == 0xFF) c.a1 = 0xFFFF;
        }
        if (!make_cmap(q, names, nm, opt, err)) {
            err = maps[i].title + ": " + err;
            return false;
        }
        cb.maps.push_back(std::move(nm));
        opt.at(0.8f * (i + 1) / maps.size());
    }
    try {
        Weapons w = make_weapons(projects[0].for_console(opt.console));
        cb.weapons[0] = w.frames;
        cb.weapons[1] = w.sprites;
        cb.weapons[2] = w.pal;
        cb.weapons[3] = w.tiles;
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }
    return assemble(rom, cb, opt, stats, err);
}

}  // namespace md
