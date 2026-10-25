// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Licence: GPL-2.0-or-later.
//
// Three parts: a minimal zip reader (Jar::open), the art decoder (JarArt:
// palettes.bin, mappings.bin, wtexels.bin, bitshapes.bin, stexels.bin),
// and the map converter (import_map: the .bsp map, entities.db and the
// .str text tables). The original files are little-endian. The bit
// meanings used below were found by reading the phone game's loader (the
// function names in comments, such as Game_loadMapEntities, are those of
// the decompiled game).
#include "jar.h"
#include "gametext.h"

#include "../third_party/stb/stb_image.h"   // its zlib decoder (implementation in projectio.cpp)

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>

// ---------------------------------------------------------------- the zip
// Reads the whole file, finds the zip's central directory and unpacks
// every file (stored, or deflated via stb_image's zlib decoder). Files
// that can't be unpacked are skipped; then the game's data files must be
// there.
bool Jar::open(const std::string &path, std::string &err)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "can't open " + path;
        return false;
    }
    Bytes z((std::istreambuf_iterator<char>(f)), {});
    // end of central directory: its signature "PK" 5 6, searched backwards
    // over the last 22 + 65535 bytes (22 = the record, 65535 = the longest
    // zip comment)
    long e = -1;
    for (long i = (long)z.size() - 22; i >= 0 && i >= (long)z.size() - 22 - 65535; i--)
        if (get32(&z[i]) == 0x06054b50) {
            e = i;
            break;
        }
    if (e < 0) {
        err = path + " is not a .jar / .zip file";
        return false;
    }
    unsigned n = get16(&z[e + 10]);   // total number of entries
    size_t pos = get32(&z[e + 16]);   // the central directory's offset
    files_.clear();
    for (unsigned k = 0; k < n; k++) {
        // a central directory entry: 46 bytes (signature "PK" 1 2), then the
        // name, extra field and comment
        if (pos + 46 > z.size() || get32(&z[pos]) != 0x02014b50) {
            err = path + ": damaged zip directory";
            return false;
        }
        int method = get16(&z[pos + 10]);   // 0 stored, 8 deflate
        u32 csize = get32(&z[pos + 20]), usize = get32(&z[pos + 24]);
        int nlen = get16(&z[pos + 28]), xlen = get16(&z[pos + 30]), clen = get16(&z[pos + 32]);
        u32 loc = get32(&z[pos + 42]);      // the local header's offset
        std::string name((const char *)&z[pos + 46], nlen);
        pos += 46 + nlen + xlen + clen;
        if (loc + 30 > z.size()) continue;
        // data = after the 30-byte local header, its name and its extra field
        size_t data = loc + 30 + get16(&z[loc + 26]) + get16(&z[loc + 28]);
        if (data + csize > z.size() || name.empty() || name.back() == '/') continue;   // (folders)
        Bytes out(usize);
        if (method == 0) memcpy(out.data(), &z[data], usize);
        else if (method == 8) {
            // raw deflate, no zlib header
            int got = stbi_zlib_decode_noheader_buffer((char *)out.data(), (int)usize, (const char *)&z[data], (int)csize);
            if (got != (int)usize) continue;
        } else
            continue;   // other methods: not used by the game's jar
        files_[name] = std::move(out);
    }
    for (const char *need : {"entities.db", "entities.str", "mappings.bin", "palettes.bin", "wtexels.bin",
                             "bitshapes.bin", "stexels.bin"})
        if (!files_.count(need)) {
            err = path + " doesn't look like Doom RPG (no " + need + ")";
            return false;
        }
    return true;
}

// see jar.h
const Bytes *Jar::get(const std::string &name) const
{
    auto it = files_.find(name);
    return it == files_.end() ? nullptr : &it->second;
}

// every "<name>.bsp" that has a "<name>.str" beside it, as <name>, in
// alphabetical order
std::vector<std::string> Jar::maps() const
{
    std::vector<std::string> out;
    for (auto &kv : files_) {
        const std::string &n = kv.first;
        if (n.size() > 4 && n.substr(n.size() - 4) == ".bsp" && files_.count(n.substr(0, n.size() - 4) + ".str"))
            out.push_back(n.substr(0, n.size() - 4));
    }
    return out;
}

// see jar.h: the game's name for the map (tools/game_text.py MAP_TITLE),
// else its file name
std::string Jar::short_title(const std::string &m) const
{
    std::string t = gametext::map_title(*this, m);
    return t.empty() ? m : t;
}

// see jar.h: the short title, with what the editor's lists add
std::string Jar::map_title(const std::string &m) const
{
    std::string t = short_title(m);
    if (m == "intro") return t + " (the start of the game)";
    if (m == "junction_destroyed") return t + " (destroyed)";
    if (m == "items") return t + " (test map)";
    return t;
}

// ---------------------------------------------------------------- the art (JarArt)
namespace {
// reads little-endian values from a file in order; every read throws
// std::runtime_error when the file ends too early
struct Rd {
    const Bytes &d;
    size_t p = 0;   // the next byte to read
    explicit Rd(const Bytes &b) : d(b) {}
    void need(size_t n) const
    {
        if (p + n > d.size()) throw std::runtime_error("the file ends too early");
    }
    u8 u8_()
    {
        need(1);
        return d[p++];
    }
    unsigned u16()
    {
        need(2);
        unsigned v = get16(&d[p]);
        p += 2;
        return v;
    }
    int s16() { return (int16_t)u16(); }
    u32 u32_()
    {
        need(4);
        u32 v = get32(&d[p]);
        p += 4;
        return v;
    }
    void skip(size_t n)
    {
        need(n);
        p += n;
    }
};

// The original game's art, decoded as the ports' tools/extract_textures.py
// and extract_sprites.py decode it:
//   palettes.bin   u32 byte count, then RGB565 colours;
//   mappings.bin   four u32 counts, then per wall texture (offset into
//                  wtexels in texels, palette index) pairs, the same per
//                  sprite (record offset into bitshapes, palette index),
//                  then two s16 "resolve" tables (id -> the slot whose art
//                  it uses);
//   wtexels.bin    u32 count, then 64x64 textures at 4 bits per texel,
//                  column-major;
//   bitshapes.bin  u32 size, then per sprite a record: u32 offset of its
//                  texels, 4 bytes, top, bottom, left, right, then a
//                  1-bit-per-pixel mask, one row of bytes per mask row;
//   stexels.bin    the sprites' 4-bit texels, the mask's set pixels in order.
// A sprite's mask "rows" are the picture's columns (sprites are stored on
// their side).
struct JarArt {
    std::vector<RGB> pal;                  // every colour of palettes.bin
    std::vector<u32> wall_table, sprite_table;    // the (offset, palette) pairs
    std::vector<int> wall_resolve, sprite_resolve;
    const Bytes *wtex, *stexels;
    Bytes shapes;                          // bitshapes.bin after its count
    long boundary = 0;                     // where stexels' offsets start: wtexels' count * 2

    explicit JarArt(const Jar &j)
    {
        const Bytes &pb = *j.get("palettes.bin");
        u32 n2 = get32(&pb[0]);
        for (u32 i = 0; i < n2 / 2 && 4 + 2 * i + 1 < pb.size(); i++) {
            unsigned s = get16(&pb[4 + 2 * i]);
            // RGB565 with red in the low bits; widened to 8 bits by
            // repeating the top bits
            int r5 = s & 31, g6 = (s >> 5) & 63, b5 = (s >> 11) & 31;
            pal.push_back({(u8)((r5 << 3) | (r5 >> 2)), (u8)((g6 << 2) | (g6 >> 4)), (u8)((b5 << 3) | (b5 >> 2))});
        }
        const Bytes &m = *j.get("mappings.bin");
        u32 n5 = get32(&m[0]), n6 = get32(&m[4]), n7 = get32(&m[8]), n8 = get32(&m[12]);
        size_t off = 16;
        for (u32 i = 0; i < n5 * 2; i++, off += 4) wall_table.push_back(get32(&m[off]));
        for (u32 i = 0; i < n6 * 2; i++, off += 4) sprite_table.push_back(get32(&m[off]));
        for (u32 i = 0; i < n7; i++, off += 2) wall_resolve.push_back((int16_t)get16(&m[off]));
        for (u32 i = 0; i < n8; i++, off += 2) sprite_resolve.push_back((int16_t)get16(&m[off]));
        wtex = j.get("wtexels.bin");
        stexels = j.get("stexels.bin");
        const Bytes &bs = *j.get("bitshapes.bin");
        u32 cnt = get32(&bs[0]);
        shapes.assign(bs.begin() + 4, bs.begin() + std::min<size_t>(bs.size(), 4 + (size_t)cnt));
        boundary = (long)get32(&(*wtex)[0]) * 2;
    }
    // an id through a resolve table (out of range: the id itself)
    static int resolve(const std::vector<int> &t, int raw) { return raw >= 0 && raw < (int)t.size() ? t[raw] : raw; }
    // the 16 colours from palette index poff (magenta past the end)
    void colours(u32 poff, RGB out[16]) const
    {
        for (int k = 0; k < 16; k++) out[k] = poff + k < pal.size() ? pal[poff + k] : RGB{255, 0, 255};
    }
    // wall texture slot tid -> its drawing in d ([v][u]) and colours in
    // out; returns the shape key "t<offset>", shared by every texture that
    // uses the same texels. Throws for an unknown slot.
    std::string wall_slot(int tid, Drawing &d, RGB out[16]) const
    {
        if (tid < 0 || (size_t)tid * 2 + 1 >= wall_table.size()) throw std::runtime_error("unknown wall texture " + std::to_string(tid));
        u32 off = wall_table[tid * 2], poff = wall_table[tid * 2 + 1];
        size_t base = 4 + off / 2;   // two texels per byte, after the u32 count
        u8 nib[4096] = {};           // the texels, low nibble first
        for (int i = 0; i < 2048; i++)
            if (base + i < wtex->size()) {
                u8 b = (*wtex)[base + i];
                nib[i * 2] = b & 15;
                nib[i * 2 + 1] = b >> 4;
            }
        // stored column by column: transpose into [row][col]
        for (int col = 0; col < 64; col++)
            for (int row = 0; row < 64; row++) d.px[row * 64 + col] = nib[col * 64 + row];
        colours(poff, out);
        return "t" + std::to_string(off);
    }
    // sprite slot sid -> its drawing in d (CLEAR where the mask is 0) and
    // colours in out; returns the shape key "s<record offset>". Throws for
    // an unknown slot.
    std::string sprite_slot(int sid, Drawing &d, RGB out[16]) const
    {
        if (sid < 0 || (size_t)sid * 2 + 1 >= sprite_table.size()) throw std::runtime_error("unknown sprite " + std::to_string(sid));
        u32 rec = sprite_table[sid * 2], poff = sprite_table[sid * 2 + 1];
        // a byte of bitshapes (0 past the end)
        auto at = [&](size_t i) -> u8 { return i < shapes.size() ? shapes[i] : 0; };
        u32 file_off = at(rec) | (at(rec + 1) << 8) | (at(rec + 2) << 16) | ((u32)at(rec + 3) << 24);
        int top = at(rec + 8), bottom = at(rec + 9), left = at(rec + 10), right = at(rec + 11);
        // the mask's size: height rows (picture columns top..bottom) of
        // width bits (picture rows left..right)
        int height = bottom - top + 1, width = right - left + 1, row_bytes = (width + 7) / 8;
        // a run of set mask bits: mask row (named col), first bit, length,
        // and the index of its first texel in stexels
        struct Run {
            int col, start, n, p0;
        };
        std::vector<Run> runs;
        int count = 0;
        size_t b0 = rec + 12;
        // collect the runs of set bits, LSB first within each byte
        for (int row = 0; row < height; row++) {
            size_t ro = b0 + (size_t)row * row_bytes;
            int col = 0;
            while (col < width) {
                while (col < width && !(at(ro + col / 8) & (1 << (col % 8)))) col++;
                if (col == width) break;
                int start = col;
                while (col < width && (at(ro + col / 8) & (1 << (col % 8)))) col++;
                runs.push_back({row, start, col - start, count});
                count += col - start;
            }
        }
        // the texels: count nibbles from the sprite's offset, which counts
        // from the start of wtexels' texels (hence - boundary)
        long base = 4 + ((long)file_off - boundary) / 2;
        std::vector<u8> nib;
        for (long i = 0; i < (count + 1) / 2; i++) {
            long k = base + i;
            if (k < 0 || k >= (long)stexels->size()) break;
            u8 b = (*stexels)[k];
            nib.push_back(b & 15);
            nib.push_back(b >> 4);
        }
        std::fill(d.px, d.px + 4096, CLEAR);
        // place each run: mask row -> picture column x, bit -> picture row y
        for (auto &r : runs)
            for (int i = 0; i < r.n; i++) {
                int y = left + r.start + i, x = top + r.col;
                if (y < 64 && x < 64) d.px[y * 64 + x] = r.p0 + i < (int)nib.size() ? nib[r.p0 + i] : 0;
            }
        colours(poff, out);
        return "s" + std::to_string(rec);
    }
};

// a .str text table: u16 count, then per text a u16 length and its
// Latin-1 bytes; returned as UTF-8. Throws when the file is cut short.
std::vector<std::string> str_table(const Bytes &s)
{
    Rd r(s);
    unsigned n = r.u16();
    std::vector<std::string> out;
    for (unsigned i = 0; i < n; i++) {
        unsigned L = r.u16();
        r.need(L);
        std::string t;
        for (unsigned k = 0; k < L; k++) {   // latin-1 -> UTF-8
            u8 c = r.d[r.p + k];
            if (c < 0x80) t.push_back((char)c);
            else {
                t.push_back((char)(0xC0 | (c >> 6)));
                t.push_back((char)(0x80 | (c & 63)));
            }
        }
        r.p += L;
        out.push_back(t);
    }
    return out;
}

// entity types (eType) that matter here
const int ET_ENEMY = 1, ET_SPECIAL = 7, ET_DECOR = 13, ET_BLOCK14 = 14, ET_BLOCK15 = 15;
// eTypes that become things (Entity: they move, are picked up or talk);
// every other sprite becomes a prop (Scenery)
const std::set<int> DYNAMIC = {1, 2, 3, 4, 5, 6, 16, 10, 11, 12};
// eTypes of props that block movement (Project::static_block)
const std::set<int> STATIC_BLOCK = {ET_SPECIAL, ET_DECOR, ET_BLOCK14, ET_BLOCK15};
// script commands whose a1 is a wall number (MOVELINE, LOCK, UNLOCK,
// TOGGLELOCK, OPENLINE, CLOSELINE, MOVELINE2): those walls become doors
const std::set<int> LINE_OPS = {6, 12, 13, 14, 15, 16, 17};
// (eType, eSub) of the drop kinds whose thing types are always kept, even
// when no thing on the map uses them (cf. levelblob.cpp's DROP_TYPES,
// which also has (6, 0) and (16, 0))
const int DROP_DEFS[14][2] = {{3, 20}, {3, 21}, {3, 22}, {3, 23}, {4, 25}, {4, 26}, {6, 1},
                              {6, 2},  {6, 3},  {6, 4},  {16, 1}, {16, 2}, {16, 3}, {16, 4}};
// script commands the consoles' rules carry out (game/events.asm)
const std::set<int> SUPPORTED_OPS = {2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 26, 40, 41};
// ...the Mega Drive's (megadrive/src/rules.s: GOTO, TRIGGER, the floor and
// ceiling colours, weapons put away, vendors, CHANGESPRITE, the Reactor's
// portal, CHECK_COMPLETED_LEVEL too)
const std::set<int> MD_SUPPORTED_OPS = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
                                        20, 21, 22, 23, 24, 26, 30, 31, 32, 33, 34, 38, 39, 40, 41};
// ...and the ones that only change how things look or sound (harmless to skip)
const std::set<int> COSMETIC_OPS = {27, 29, 35, 36, 37, 42};
}  // namespace

// see jar.h. The pictures needed: every texture a wall uses (and the
// portal's closed textures), every sprite a thing type or prop uses, the
// border (wall texture 11) and the 18 weapon frames.
bool import_art(const Jar &jar, Project &p, std::string &err)
{
    try {
        JarArt art(jar);
        std::map<std::string, Drawing> shapes;
        // store a drawing (pictures sharing texels share the key)
        auto put = [&](const std::string &key, const Drawing &d) { shapes[key] = d; };
        std::set<int> tex, spr;
        for (auto &w : p.walls) tex.insert(w.tex);
        // (+ the textures the Reactor's portal takes once it is closed:
        // megadrive/tools/build_render_data.py's PORTAL_CLOSED_TEX)
        if (tex.count(34)) tex.insert(92);
        if (tex.count(33)) tex.insert(91);
        for (auto &d : p.defs) spr.insert(d.tile);
        for (auto &s : p.scenery) spr.insert(s.tile);
        p.textures.clear();
        p.sprites.clear();
        for (int t : tex) {
            Drawing d;
            ArtRef r;
            r.shape = art.wall_slot(JarArt::resolve(art.wall_resolve, t), d, r.pal);   // decode texture t
            put(r.shape, d);
            p.textures[t] = r;
        }
        for (int t : spr) {
            Drawing d;
            ArtRef r;
            // tiles from 305 up are wall textures used as props or wall decals (flag 0x40000)
            if (t >= 305) r.shape = art.wall_slot(JarArt::resolve(art.wall_resolve, t - 305), d, r.pal);
            else if (t >= 82 && t <= 90 && t % 2 == 0) {
                // an ammo pack (halon, bullets, ...): no picture of
                // its own (its sprite id is the axe's); the original draws
                // the single item, t - 1, four times round the spot, so here:
                // three copies side by side, 16 columns apart, the middle
                // one last (tools/sprite_art.py's PLURAL_TILES); the
                // drawing's key is the single item's + "x3")
                Drawing one;
                r.shape = art.sprite_slot(JarArt::resolve(art.sprite_resolve, t - 1), one, r.pal) + "x3";
                for (int i = 0; i < 64 * 64; i++) d.px[i] = CLEAR;
                for (int dx : {-16, 16, 0})
                    for (int y = 0; y < 64; y++)
                        for (int x = 0; x < 64; x++) {
                            int sx = x - dx;
                            if (sx >= 0 && sx < 64 && one.px[y * 64 + sx] != CLEAR) d.px[y * 64 + x] = one.px[y * 64 + sx];
                        }
            } else r.shape = art.sprite_slot(JarArt::resolve(art.sprite_resolve, t), d, r.pal);
            put(r.shape, d);
            p.sprites[t] = r;
        }
        {
            Drawing d;
            p.border.shape = art.wall_slot(11, d, p.border.pal);   // the brown rock
            put(p.border.shape, d);
        }
        p.weapons.clear();
        // weapons: sprite slots 240..248 (resolved), each followed by its
        // firing frame
        for (int w = 0; w < 9; w++)
            for (int f = 0; f < 2; f++) {
                Drawing d;
                ArtRef r;
                r.shape = art.sprite_slot(JarArt::resolve(art.sprite_resolve, 240 + w) + f, d, r.pal);
                put(r.shape, d);
                p.weapons.push_back(r);
            }
        p.shapes = shapes;
    } catch (const std::exception &e) {
        err = std::string("the game's art: ") + e.what();
        return false;
    }
    return true;
}

// see jar.h. Steps: read the .bsp (header, walls, sprites, triggers,
// commands, grid), read the thing types, sort the sprites into things and
// props, make doors of the walls scripts switch, turn triggers into
// scripted cells, then fetch the texts and the art.
bool import_map(const Jar &jar, const std::string &map, Project &p, std::string &err, std::vector<std::string> *notes,
                bool campaign)
{
    const Bytes *bsp = jar.get(map + ".bsp"), *strs = jar.get(map + ".str");
    if (!bsp || !strs) {
        err = "there is no map called " + map + " in the game";
        return false;
    }
    try {
        // ---- the map (tools/bsp_parser.py)
        Rd r(*bsp);
        // 13-byte header: floor RGB, ceiling RGB, 3 unknown bytes, the map
        // id (h[9]), the spawn cell (u16 h[10..11]) and angle (h[12])
        u8 h[13];
        for (u8 &b : h) b = r.u8_();
        r.skip((size_t)r.u16() * 10);   // BSP nodes
        std::vector<Wall> walls;
        // walls: 8 bytes each, coordinates in units of 8 (<< 3 = world units)
        unsigned nw = r.u16();
        for (unsigned i = 0; i < nw; i++) {
            Wall w;
            w.ax = r.u8_() << 3;
            w.az = r.u8_() << 3;
            w.bx = r.u8_() << 3;
            w.bz = r.u8_() << 3;
            w.tex = r.s16();
            w.flags = (int)r.u16();
            walls.push_back(w);
        }
        // sprites: x, z (units of 8), then 24 bits of info: bits 0..8 the
        // tile, 16 hidden, 17 an invisible blocker, 18 a wall texture
        // (tile + 305), 19..23 wall-mounted orientation, 24 render only,
        // 25 mirrored
        struct Spr {
            int x, z;
            u32 c;
        };
        std::vector<Spr> sprites;
        unsigned ns = r.u16();
        for (unsigned i = 0; i < ns; i++) {
            int x = r.u8_() << 3, z = r.u8_() << 3;
            u32 lo = r.u8_(), hi = r.u16();
            sprites.push_back({x, z, lo | (hi << 16)});
        }
        // triggers (scripted cells): u32 each, unpacked below
        std::vector<u32> triggers;
        unsigned nt = r.u16();
        for (unsigned i = 0; i < nt; i++) triggers.push_back(r.u32_());
        struct Act {
            int type;
            u32 f1, f2;
        };
        // commands: op byte, two u32 arguments
        std::vector<Act> acts;
        unsigned na = r.u16();
        for (unsigned i = 0; i < na; i++) {
            int t = r.u8_();
            u32 f1 = r.u32_(), f2 = r.u32_();
            acts.push_back({t, f1, f2});
        }
        // the grid: 2 bits per cell, 4 cells per byte, lowest bits first
        // (1 = solid)
        std::vector<int> grid(1024);
        for (int bi = 0; bi < 256; bi++) {
            u8 b = r.u8_();
            for (int sub = 0; sub < 4; sub++) grid[bi * 4 + sub] = (b >> (sub * 2)) & 3;
        }
        // ---- the game data (tools/gamedata.py)
        const Bytes &db = *jar.get("entities.db");
        auto names = str_table(*jar.get("entities.str"));
        // entities.db: u16 count, then 8 bytes per type: u16 tile, eType,
        // eSub, s32 parm; their names are entities.str's texts
        unsigned nd = get16(&db[0]);
        std::vector<EntityDef> all;
        for (unsigned i = 0; i < nd; i++) {
            if (2 + i * 8 + 8 > db.size()) throw std::runtime_error("entities.db is cut short");
            const u8 *q = &db[2 + i * 8];
            EntityDef d;
            d.tile = get16(q);
            d.eType = q[2];
            d.eSub = q[3];
            d.parm = (s32)get32(q + 4);
            d.name = i < names.size() ? names[i] : "def" + std::to_string(i);
            all.push_back(d);
        }
        // look-ups into all: by sprite tile, and by (eType, eSub); for
        // duplicates the last one wins
        std::map<int, int> by_tile;
        std::map<std::pair<int, int>, int> by_type;
        for (size_t i = 0; i < all.size(); i++) {
            by_tile[all[i].tile] = (int)i;
            by_type[{all[i].eType, all[i].eSub}] = (int)i;
        }
        Project q;
        q.name = jar.short_title(map);
        q.floor = {h[0], h[1], h[2]};
        q.ceiling = {h[3], h[4], h[5]};
        int map_id = h[9];   // 1 = the UAC Base
        if (campaign) q.map_id = map_id;
        else q.new_game = map_id == 1;    // the UAC Base: the game starts there
        int spawn_index = h[10] | (h[11] << 8), angle = h[12];
        // the angle (256 to a turn) -> facing 0..3 (N E S W); others: north
        int facing = angle == 64 ? 0 : angle == 0 ? 1 : angle == 192 ? 2 : angle == 128 ? 3 : 0;
        q.spawn_x = spawn_index % 32;
        q.spawn_z = spawn_index / 32;
        q.spawn_f = facing;
        for (int i = 0; i < 1024; i++) q.wall[i] = (grid[i] & 3) == 1 ? 1 : 0;
        q.static_block.assign(1024, 0);
        std::map<int, int> used;   // def index in all -> compact id
        std::vector<int> order;    // compact id -> def index in all
        // the map keeps only the thing types it uses, numbered in order of
        // first use; returns type i's compact id
        auto use_def = [&](int i) {
            auto it = used.find(i);
            if (it != used.end()) return it->second;
            int c = (int)used.size();
            used[i] = c;
            order.push_back(i);
            return c;
        };
        std::map<int, int> sprite_to_entity;   // sprite index -> thing index
        std::vector<std::pair<int, int>> es7;   // (entity, its sprite): an eSub-7 monster
        // mark cell (cx, cz) as blocked by a prop of eType t
        auto block = [&](int cx, int cz, int t) {
            if (cx >= 0 && cx < 32 && cz >= 0 && cz < 32) q.static_block[cz * 32 + cx] = t;
        };
        for (size_t si = 0; si < sprites.size(); si++) {
            u32 info = sprites[si].c;
            int t = info & 511;                 // the tile
            if (info & 0x40000) t += 305;       // a wall texture used as a sprite
            int x = sprites[si].x, z = sprites[si].z;
            auto dit = by_tile.find(t);
            int di = dit == by_tile.end() ? -1 : dit->second;
            bool hidden = (info & 0x10000) != 0;
            // the cell the thing is linked to (Render_beginLoadMapData's 1-unit
            // nudge, then Game_loadMapEntities' offsets for wall-mounted things)
            int px = x, pz = z;
            if (info & 0x800000) {
                if (info & 0x100000) pz += 1;
                else if (info & 0x80000) pz -= 1;
                else if (info & 0x200000) px += 1;
                else if (info & 0x400000) px -= 1;
            }
            int lx = px, lz = pz;
            if (info & 0x200000) lx -= 64;
            else if (info & 0x100000) lz -= 64;
            else if (info & 0x80000) lz += 32;
            else if (info & 0x400000) lx += 32;
            int cx = lx >> 6, cz = lz >> 6;
            if ((info & 0x1000000) && di >= 0 && DYNAMIC.count(all[di].eType)) di = -1;   // render only
            // a thing: placed on its linked cell
            if (di >= 0 && DYNAMIC.count(all[di].eType)) {
                sprite_to_entity[(int)si] = (int)q.entities.size();
                if (all[di].eType == ET_ENEMY && all[di].eSub == 7) es7.push_back({(int)q.entities.size(), (int)si});
                q.entities.push_back({use_def(di), cx, cz, hidden, {}});
                continue;
            }
            // otherwise a prop; a typed prop or a blocker gets its cell
            // (campaign maps: the cell whose HIDE removes it)
            Scenery sc{t, x, z, info, (di >= 0 || (info & 0x20000)) ? cz * 32 + cx : -1};
            if (!campaign) sc.cell = -1;
            q.scenery.push_back(sc);
            // does it block? a blocking type, or an invisible blocker
            // (tile 216 counts as type 15, others as 14)
            int btype = 0;
            if (di >= 0 && STATIC_BLOCK.count(all[di].eType)) btype = all[di].eType;
            else if (info & 0x20000)
                btype = t == 216 ? ET_BLOCK15 : ET_BLOCK14;
            if (btype && !hidden && !(info & 0x1000000)) {
                if (btype == ET_BLOCK14 || btype == ET_BLOCK15) {
                    // Game_trace: a thin wall at the sprite's x (or z); without its
                    // orientation bits it stops nothing. The Master System's rules
                    // have no thin walls: there it blocks its whole cell.
                    // orientation bits 0x180000 (along x): the N or S
                    // edge; 0x600000 (along z): the W or E edge
                    const char *edge = nullptr;
                    if ((info & 0x20000) && (info & 0x180000)) edge = pz == cz * 64 ? "N" : "S";
                    else if ((info & 0x20000) && (info & 0x600000)) edge = px == cx * 64 ? "W" : "E";
                    if (edge) {
                        if (campaign) q.thin.push_back({cx, cz, btype, edge});
                        else block(cx, cz, btype);
                    }
                } else
                    block(cx, cz, btype);
            }
        }
        if (campaign)                // Entity_died: an eSub-7 monster's next two sprites appear
            for (auto &e7 : es7)
                for (int k = 1; k <= 2; k++) {
                    auto it = sprite_to_entity.find(e7.second + k);
                    q.entities[e7.first].spawns.push_back(it == sprite_to_entity.end() ? 0xFFFF : it->second);
                }
        // keep the drop kinds' thing types (after the map's own)
        for (auto &dd : DROP_DEFS) {
            auto it = by_type.find({dd[0], dd[1]});
            if (it != by_type.end()) use_def(it->second);
        }
        // switchable lines: every line a script opens, closes, locks or unlocks
        std::set<u32> refs;
        for (auto &a : acts)
            if (LINE_OPS.count(a.type)) refs.insert(a.f1);
        std::map<u32, int> line_slot;
        for (u32 li : refs) {
            if (li >= walls.size()) throw std::runtime_error("a script refers to a wall that isn't there");
            const Wall &w = walls[li];
            // after the loader's 3-unit nudge of one-sided walls (both halves of a
            // sliding secret wall end up on the cell between them)
            int f = w.flags, ax = w.ax, az = w.az, bx = w.bx, bz = w.bz;
            int nudge = (f & 8) ? 3 : (f & 16) ? -3 : 0;
            if (f & 512) {
                ax += nudge;
                bx += nudge;
            } else if (f & 256) {
                az += nudge;
                bz += nudge;
            }
            int x = ax + (bx - ax) / 2, y = az + (bz - az) / 2;   // (Python int(v / 2): towards 0)
            // step one unit off the wall's middle to the side flags
            // 0x800/0x2000/0x1000/0x4000 name, into the cell it closes
            if (f & 0x800) y -= 1;
            else if (f & 0x2000) x += 1;
            else if (f & 0x1000) y += 1;
            else if (f & 0x4000) x -= 1;
            line_slot[li] = (int)q.lines.size();
            Line l;
            l.wall = (int)li;
            l.x = x >> 6;
            l.z = y >> 6;
            // the door's kind: the thing type whose tile is this wall's
            // texture (305 + tex); none: a plain wall (eType 9)
            auto ld = by_tile.find(305 + w.tex);
            if (ld != by_tile.end()) {
                l.eType = all[ld->second].eType;
                l.eSub = all[ld->second].eSub;
                l.name = all[ld->second].name;
            } else {
                l.eType = 9;
                l.eSub = 0;
                l.name = "Wall";
            }
            l.locked = (w.flags & 0x400) != 0;
            q.lines.push_back(l);
        }
        std::map<int, int> skipped;   // op -> how many were dropped
        // a trigger: bits 0..9 cell, 10..18 first command, 19..24 command
        // count, 25..28 state, 29..31 flags
        for (u32 ev : triggers) {
            int cell = ev & 1023, ci = (ev >> 10) & 511, cc = (ev >> 19) & 63;
            Event e;
            e.x = cell % 32;
            e.z = cell / 32;
            e.state = (ev >> 25) & 15;
            e.flags = (ev >> 29) & 7;
            for (int k = ci; k < ci + cc; k++) {
                if (k >= (int)acts.size()) throw std::runtime_error("a script runs past the end of the commands");
                const Act &a = acts[k];
                u32 a1 = a.f1;
                if (LINE_OPS.count(a.type)) a1 = (u32)line_slot[a1];   // wall number -> door slot
                else if (a.type == 7) {   // SHOW: sprite index -> thing index
                    auto it = sprite_to_entity.find((int)(a1 & 0xFFFF));
                    a1 = it == sprite_to_entity.end() ? (campaign ? 0xFFFF : 0xFF) : (u32)it->second;
                }
                // the command is kept anyway; the game ignores what it can't run
                if (!(campaign ? MD_SUPPORTED_OPS : SUPPORTED_OPS).count(a.type) && !COSMETIC_OPS.count(a.type))
                    skipped[a.type]++;
                e.cmds.push_back({a.type, a1, a.f2});
            }
            q.events.push_back(e);
        }
        // the used thing types, in compact-id order
        q.defs.resize(order.size());
        for (size_t c = 0; c < order.size(); c++) q.defs[c] = all[order[c]];
        q.walls = walls;
        q.strings = str_table(*strs);
        if (!import_art(jar, q, err)) return false;   // the pictures the map needs
        if (notes)
            for (auto &kv : skipped)
                notes->push_back(std::string(cmd_name(kv.first)) + " x" + std::to_string(kv.second) +
                                 ": a script command the consoles don't carry out");
        p = std::move(q);
    } catch (const std::exception &e) {
        err = map + ": " + e.what();
        return false;
    }
    return true;
}
