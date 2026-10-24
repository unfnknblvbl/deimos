// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Licence: GPL-2.0-or-later.
//
// The Master System's level blob (tools/gen_cdata.py level_blob):
//   16-byte header: "LVL1", version 2, then counts: defs, things,
//     monsters, lines, events, commands (low byte), strings, spawn x, z,
//     facing, commands' high byte
//   u16 offset of each table (from the blob's start, little-endian), in
//     TABLES order, then "start" and "title"
//   the tables
// The campaign's blob (LVB2) is laid out the same way with a 24-byte
// header and the TABLES2 list (see make_level_blob_md).
// Names ending in 0 (ent_def0, ev_state0...) are a table's starting values,
// which the game copies into RAM and changes while playing.
#include "levelblob.h"
#include "keytext.h"

#include <cctype>
#include <stdexcept>

// the single-level blob's tables in order (the names are for reference;
// only their count is used: t[i] below is TABLES[i])
static const char *TABLES[] = {"def_etype", "def_esub", "def_parm", "def_name", "ent_def0", "ent_x0", "ent_z0",
                               "ent_hidden0", "ent_mon", "line_x", "line_z", "line_etype", "line_locked0",
                               "ev_x", "ev_z", "ev_state0", "ev_flags", "ev_cmd0", "ev_ncmd", "ev_at",
                               "cmd_op", "cmd_a1", "cmd_a2", "wall_bits", "static_block", "line_cells",
                               "drop_def"};
static const int N_TABLES = sizeof(TABLES) / sizeof(TABLES[0]);
// (eType, eSub) of the 16 things a monster or crate can drop, in the
// games' DD_* order: health, armour, 1 and 5 credits, small and large
// medkit, then ammo types 0..4 twice (types 6 and 16). drop_def holds each
// one's index in defs (255 = not in this map).
static const int DROP_TYPES[16][2] = {{3, 20}, {3, 21}, {3, 22}, {3, 23}, {4, 25}, {4, 26}, {6, 0}, {6, 1},
                                      {6, 2},  {6, 3},  {6, 4},  {16, 0}, {16, 1}, {16, 2}, {16, 3}, {16, 4}};

namespace {
// the most of each a ROM's tables hold
struct Limits {
    int defs, map_ents, mons, lines, events, cmds, strings;
};
// the Master System's (tools/gen_cdata.py of that port, 0.2.14 on)
const Limits SMS_LIMITS = {64, 192, 56, 40, 96, 320, 255};
// the most bytes for the level blob ($A000-$BFFF of bank 2) and the
// strings blob (bank 4 from $8000)
const int SMS_LEVEL_MAX = 0x2000, SMS_STRINGS_MAX = 0x2000;
// the Mega Drive campaign's (tools/gen_cdata.py)
const Limits MD_LIMITS = {64, 280, 100, 64, 160, 448, 255};
// blob byte limits, and the most Pain Elementals (things with spawns)
const int MD_LEVEL_MAX = 0x8000, MD_STRINGS_MAX = 0x4000, MD_MAX_SPAWNS = 8;
}  // namespace

// "" when p fits L, else the first reason it doesn't: a count over its
// limit, a thing of an unknown type, or two scripts on one cell
// (monsters = things whose type has eType 1)
static std::string check_limits(const Project &p, const Limits &L)
{
    int n_mon = 0, n_cmd = 0;
    for (auto &e : p.entities)
        if (e.def >= 0 && e.def < (int)p.defs.size() && p.defs[e.def].eType == 1) n_mon++;
    for (auto &e : p.events) n_cmd += (int)e.cmds.size();
    struct {
        const char *what;
        int n, mx;
    } checks[] = {{"entity types", (int)p.defs.size(), L.defs},
                  {"things on the map", (int)p.entities.size(), L.map_ents},
                  {"monsters", n_mon, L.mons},
                  {"doors / sliding walls", (int)p.lines.size(), L.lines},
                  {"scripted cells", (int)p.events.size(), L.events},
                  {"script commands", n_cmd, L.cmds},
                  {"strings", (int)p.strings.size(), L.strings}};
    for (auto &c : checks)
        if (c.n > c.mx)
            return std::string("too many ") + c.what + ": " + std::to_string(c.n) + " (the ROM allows " +
                   std::to_string(c.mx) + ")";
    for (auto &e : p.entities)
        if (e.def < 0 || e.def >= (int)p.defs.size()) return "a thing on the map has an unknown type";
    for (auto &e : p.events)
        for (auto &e2 : p.events)
            if (&e != &e2 && e.x == e2.x && e.z == e2.z) return "two scripts on the same cell";
    return "";
}

// the public checks (see levelblob.h)
std::string check_limits(const Project &p) { return check_limits(p, SMS_LIMITS); }
std::string check_limits_md(const Project &p) { return check_limits(p, MD_LIMITS); }

namespace {
// appends 16- and 32-bit values in the chosen byte order
struct Out {
    bool be;   // true: big-endian (68000), false: little-endian (Z80)
    void p16(Bytes &b, unsigned v) const
    {
        if (be) {
            b.push_back((v >> 8) & 0xFF);
            b.push_back(v & 0xFF);
        } else
            put16(b, v);
    }
    void p32(Bytes &b, u32 v) const
    {
        if (be)
            for (int i = 3; i >= 0; i--) b.push_back((v >> (8 * i)) & 0xFF);
        else
            put32(b, v);
    }
};
}  // namespace

// see levelblob.h
Bytes make_level_blob(const Project &p)
{
    Out o{false};   // little-endian
    std::string err = check_limits(p, SMS_LIMITS);
    if (!err.empty()) throw std::runtime_error(err);
    const auto &defs = p.defs;
    // ent_mon: each thing's monster slot (monsters numbered in map order), 255 = not a monster
    std::vector<int> mon_slot;
    int n_mon = 0;
    for (auto &e : p.entities) mon_slot.push_back(defs[e.def].eType == 1 ? n_mon++ : 255);
    // all events' commands in one list; starts[i] = event i's first
    std::vector<const Cmd *> cmds;
    std::vector<int> starts;
    for (auto &e : p.events) {
        starts.push_back((int)cmds.size());
        for (auto &c : e.cmds) cmds.push_back(&c);
    }
    std::vector<Bytes> t(N_TABLES);
    for (auto &d : defs) {
        t[0].push_back(d.eType);
        t[1].push_back(d.eSub);
        o.p32(t[2], (u32)d.parm);
    }
    // def_name: a u16 offset per name (from the table's start), then the
    // names, upper-cased (ASCII letters only), each ending in 0
    Bytes names;
    for (auto &d : defs) {
        o.p16(t[3], 2 * defs.size() + names.size());
        for (char ch : utf8_to_latin1(d.name)) names.push_back((u8)((ch >= 'a' && ch <= 'z') ? ch - 32 : ch));
        names.push_back(0);
    }
    t[3].insert(t[3].end(), names.begin(), names.end());
    for (size_t i = 0; i < p.entities.size(); i++) {
        auto &e = p.entities[i];
        t[4].push_back(e.def);
        t[5].push_back(e.x);
        t[6].push_back(e.z);
        t[7].push_back(e.hidden ? 1 : 0);
        t[8].push_back(mon_slot[i]);
    }
    for (auto &l : p.lines) {
        t[9].push_back(l.x);
        t[10].push_back(l.z);
        t[11].push_back(l.eType);
        t[12].push_back(l.locked ? 1 : 0);
    }
    // ev_at: per cell, its event's index (255 = none)
    Bytes ev_at(1024, 255);
    for (size_t i = 0; i < p.events.size(); i++) {
        auto &e = p.events[i];
        t[13].push_back(e.x);
        t[14].push_back(e.z);
        t[15].push_back(e.state);
        t[16].push_back(e.flags);
        o.p16(t[17], starts[i]);   // (words: up to 320 commands)
        t[18].push_back((u8)e.cmds.size());
        ev_at[e.z * 32 + e.x] = (u8)i;
    }
    t[19] = ev_at;
    for (auto *c : cmds) {
        t[20].push_back(c->op);
        o.p32(t[21], c->a1);
        o.p32(t[22], c->a2);
    }
    // bit maps of the 1024 cells (cell k: byte k >> 3, bit k & 7): the
    // solid cells, and the cells a door or sliding wall blocks
    Bytes wall_bits(128, 0), line_cells(128, 0);
    for (int i = 0; i < 1024; i++)
        if (p.wall[i]) wall_bits[i >> 3] |= 1 << (i & 7);
    for (auto &l : p.lines) {
        int k = l.z * 32 + l.x;
        line_cells[k >> 3] |= 1 << (k & 7);
    }
    t[23] = wall_bits;
    t[24] = Bytes(p.static_block.begin(), p.static_block.end());
    t[25] = line_cells;
    // drop_def: each drop kind's first matching thing type (255 = none)
    for (auto &dt : DROP_TYPES) {
        int found = 255;
        for (size_t i = 0; i < defs.size(); i++)
            if (defs[i].eType == dt[0] && defs[i].eSub == dt[1]) {
                found = (int)i;
                break;
            }
        t[26].push_back(found);
    }
    int n_tables = N_TABLES + 2;
    t.resize(n_tables);
    t[N_TABLES].push_back(p.new_game ? 1 : 0);          // "start": a new game's kit
    std::string title;                                     // "title": shown at the start and the end
    for (unsigned char ch : utf8_to_latin1(p.name)) title.push_back((char)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch));
    if (title.size() > 16) title.resize(16);   // at most 16 characters, then a 0
    t[N_TABLES + 1].assign(title.begin(), title.end());
    t[N_TABLES + 1].push_back(0);
    // the 16-byte header (see the top of this file)
    Bytes blob = {'L', 'V', 'L', '1', 2, (u8)defs.size(), (u8)p.entities.size(), (u8)n_mon, (u8)p.lines.size(),
                  (u8)p.events.size(), (u8)cmds.size(), (u8)p.strings.size(), (u8)p.spawn_x, (u8)p.spawn_z,
                  (u8)p.spawn_f, (u8)(cmds.size() >> 8)};
    // the offset table, then the tables themselves
    int header = 16 + 2 * n_tables;
    Bytes body;
    for (int i = 0; i < n_tables; i++) {
        o.p16(blob, header + body.size());
        body.insert(body.end(), t[i].begin(), t[i].end());
    }
    blob.insert(blob.end(), body.begin(), body.end());
    if ((int)blob.size() > SMS_LEVEL_MAX)
        throw std::runtime_error("the level data is too big: " + std::to_string(blob.size()) + " bytes (max " +
                                 std::to_string(SMS_LEVEL_MAX) + ")");
    return blob;
}

// see levelblob.h: a u16 offset per text (from the blob's start), then
// the texts in Latin-1, each ending in 0, with the phone's keys a text
// names written as the console's buttons (keytext.h: the Master System's
// or the Game Gear's, as tools/gen_cdata.py writes them)
Bytes make_strings_blob(const Project &p, bool gg)
{
    Bytes offs, body;
    unsigned base = 2 * p.strings.size();
    for (auto &s : p.strings) {
        put16(offs, base + body.size());
        std::string l1 = keytext::sms_text(utf8_to_latin1(s), gg);
        body.insert(body.end(), l1.begin(), l1.end());
        body.push_back(0);
    }
    offs.insert(offs.end(), body.begin(), body.end());
    if ((int)offs.size() > SMS_STRINGS_MAX)
        throw std::runtime_error("the strings are too long: " + std::to_string(offs.size()) + " bytes (max " +
                                 std::to_string(SMS_STRINGS_MAX) + ")");
    return offs;
}

// ---- the Mega Drive campaign's blob (LVB2, tools/gen_cdata.py level_blob)
// its tables in order (as TABLES, plus line_kind, thin and spawns)
static const char *TABLES2[] = {"def_etype", "def_esub", "def_parm", "def_name", "ent_def0", "ent_x0", "ent_z0",
                                "ent_hidden0", "ent_mon", "line_x", "line_z", "line_etype", "line_locked0",
                                "line_kind", "ev_x", "ev_z", "ev_state0", "ev_flags", "ev_cmd0", "ev_ncmd", "ev_at",
                                "cmd_op", "cmd_a1", "cmd_a2", "wall_bits", "static_block", "thin", "line_cells",
                                "drop_def", "spawns"};
static const int N_TABLES2 = sizeof(TABLES2) / sizeof(TABLES2[0]);

// a thin wall's bit in the "thin" table's byte for its cell: bits 0..3 =
// W, E, N, S edge of a type-14 blocker, bits 4..7 the same for type 15
static int thin_bit(int type, const std::string &edge)
{
    int k = edge == "W" ? 0 : edge == "E" ? 1 : edge == "N" ? 2 : 3;
    return (1 << k) << (type == 15 ? 4 : 0);
}

// CHANGEMAP's string ("/level02.bsp") -> the campaign's map index (255: none, the end)
static int map_index_of(const Project &p, u32 a1, const std::vector<std::string> &maps)
{
    unsigned sid = a1 & 0xFF;   // the string id naming the target map
    if (sid >= p.strings.size()) return 255;
    std::string n = p.strings[sid];
    // strip the slashes around the name and its extension: "/level02.bsp" -> "level02"
    size_t a = n.find_first_not_of('/'), b = n.find_last_not_of('/');
    n = a == std::string::npos ? "" : n.substr(a, b - a + 1);
    size_t dot = n.rfind('.');
    if (dot != std::string::npos) n = n.substr(0, dot);
    for (size_t i = 0; i < maps.size(); i++)
        if (maps[i] == n) return (int)i;
    return 255;
}

// see levelblob.h. The same tables as make_level_blob plus line_kind (a
// door's exit and secret bits), thin (thin walls per cell) and spawns (the
// Pain Elementals' lost souls); CHANGEMAP's target becomes a map index.
// 24-byte header: "LVB2", version 2, defs, u16 things, monsters, lines,
// events, strings, u16 commands, spawn x, z, facing, the map id, the
// floor's and the ceiling's RGB; then a u16 offset per table.
Bytes make_level_blob_md(const Project &p, const std::vector<std::string> &maps, int map_id)
{
    Out o{true};
    std::string err = check_limits(p, MD_LIMITS);
    if (!err.empty()) throw std::runtime_error(err);
    const auto &defs = p.defs;
    std::vector<int> mon_slot;
    int n_mon = 0;
    for (auto &e : p.entities) mon_slot.push_back(defs[e.def].eType == 1 ? n_mon++ : 255);
    std::vector<const Cmd *> cmds;
    std::vector<int> starts;
    for (auto &e : p.events) {
        starts.push_back((int)cmds.size());
        for (auto &c : e.cmds) cmds.push_back(&c);
    }
    std::vector<Bytes> t(N_TABLES2);
    for (auto &d : defs) {
        t[0].push_back(d.eType);
        t[1].push_back(d.eSub);
        o.p32(t[2], (u32)d.parm);
    }
    Bytes names;
    for (auto &d : defs) {
        o.p16(t[3], 2 * defs.size() + names.size());
        for (char ch : utf8_to_latin1(d.name)) names.push_back((u8)((ch >= 'a' && ch <= 'z') ? ch - 32 : ch));
        names.push_back(0);
    }
    t[3].insert(t[3].end(), names.begin(), names.end());
    for (size_t i = 0; i < p.entities.size(); i++) {
        auto &e = p.entities[i];
        t[4].push_back(e.def);
        t[5].push_back(e.x);
        t[6].push_back(e.z);
        t[7].push_back(e.hidden ? 1 : 0);
        t[8].push_back(mon_slot[i]);
    }
    for (auto &l : p.lines) {
        t[9].push_back(l.x);
        t[10].push_back(l.z);
        t[11].push_back(l.eType);
        t[12].push_back(l.locked ? 1 : 0);
        // line_kind: bit 0 = an exit door (its wall has texture 7), bit 1
        // = secret (its wall lacks flag 4); no wall: neither
        int tex = 0, flags = 4;
        if (l.wall >= 0 && l.wall < (int)p.walls.size()) {
            tex = p.walls[l.wall].tex;
            flags = p.walls[l.wall].flags;
        }
        t[13].push_back((u8)((tex == 7 ? 1 : 0) | ((flags & 4) ? 0 : 2)));   // exit, secret
    }
    Bytes ev_at(1024, 255);
    for (size_t i = 0; i < p.events.size(); i++) {
        auto &e = p.events[i];
        t[14].push_back(e.x);
        t[15].push_back(e.z);
        t[16].push_back(e.state);
        t[17].push_back(e.flags);
        o.p16(t[18], starts[i]);
        t[19].push_back((u8)e.cmds.size());
        ev_at[e.z * 32 + e.x] = (u8)i;
    }
    t[20] = ev_at;
    for (auto *c : cmds) {
        t[21].push_back(c->op);
        u32 a1 = c->a1;
        // CHANGEMAP: replace the string id with the target's index in maps
        if (c->op == 2) a1 = (a1 & ~0xFFu) | (u32)map_index_of(p, a1, maps);
        o.p32(t[22], a1);
        o.p32(t[23], c->a2);
    }
    // bit maps as in make_level_blob, plus a byte per cell of thin-wall bits
    Bytes wall_bits(128, 0), line_cells(128, 0), thin(1024, 0);
    for (int i = 0; i < 1024; i++)
        if (p.wall[i]) wall_bits[i >> 3] |= 1 << (i & 7);
    for (auto &l : p.lines) {
        int k = l.z * 32 + l.x;
        line_cells[k >> 3] |= 1 << (k & 7);
    }
    for (auto &th : p.thin)
        if (th.x >= 0 && th.x < 32 && th.z >= 0 && th.z < 32) thin[th.z * 32 + th.x] |= thin_bit(th.type, th.edge);
    t[24] = wall_bits;
    t[25] = Bytes(p.static_block.begin(), p.static_block.end());
    t[26] = thin;
    t[27] = line_cells;
    for (auto &dt : DROP_TYPES) {
        int found = 255;
        for (size_t i = 0; i < defs.size(); i++)
            if (defs[i].eType == dt[0] && defs[i].eSub == dt[1]) {
                found = (int)i;
                break;
            }
        t[28].push_back(found);
    }
    // spawns: u16 count, then per Pain Elemental its thing index and the
    // two things it lets out ($FFFF = none)
    int n_sp = 0;
    Bytes sp;
    for (size_t i = 0; i < p.entities.size(); i++)
        if (!p.entities[i].spawns.empty()) {
            n_sp++;
            o.p16(sp, (unsigned)i);
            for (int k = 0; k < 2; k++)
                o.p16(sp, k < (int)p.entities[i].spawns.size() ? (unsigned)p.entities[i].spawns[k] & 0xFFFF : 0xFFFF);
        }
    if (n_sp > MD_MAX_SPAWNS)
        throw std::runtime_error("too many Pain Elementals: " + std::to_string(n_sp) + " (the ROM allows " +
                                 std::to_string(MD_MAX_SPAWNS) + ")");
    o.p16(t[29], n_sp);
    t[29].insert(t[29].end(), sp.begin(), sp.end());
    // the 24-byte header (see above)
    Bytes blob = {'L', 'V', 'B', '2', 2, (u8)defs.size()};
    o.p16(blob, (unsigned)p.entities.size());
    for (int v : {n_mon, (int)p.lines.size(), (int)p.events.size(), (int)p.strings.size()}) blob.push_back((u8)v);
    o.p16(blob, (unsigned)cmds.size());
    for (int v : {p.spawn_x, p.spawn_z, p.spawn_f, map_id & 0xFF}) blob.push_back((u8)v);
    for (RGB c : {p.floor, p.ceiling}) blob.insert(blob.end(), {c.r, c.g, c.b});
    int header = 24 + 2 * N_TABLES2;
    Bytes body;
    for (int i = 0; i < N_TABLES2; i++) {
        if (body.size() & 1) body.push_back(0);   // tables at even offsets (68000 word reads)
        o.p16(blob, header + body.size());
        body.insert(body.end(), t[i].begin(), t[i].end());
    }
    blob.insert(blob.end(), body.begin(), body.end());
    if ((int)blob.size() > MD_LEVEL_MAX)
        throw std::runtime_error("the level data is too big: " + std::to_string(blob.size()) + " bytes (max " +
                                 std::to_string(MD_LEVEL_MAX) + ")");
    return blob;
}

// see levelblob.h: as make_strings_blob, big-endian, up to 16 KB, the
// phone's keys a text names made into button tokens (keytext::md_text, as
// megadrive/tools/gen_cdata.py does: the games show the buttons' names)
Bytes make_strings_blob_md(const Project &p)
{
    Out o{true};
    Bytes offs, body;
    unsigned base = 2 * p.strings.size();
    for (auto &s : p.strings) {
        o.p16(offs, base + body.size());
        std::string l1 = keytext::md_text(utf8_to_latin1(s));
        body.insert(body.end(), l1.begin(), l1.end());
        body.push_back(0);
    }
    offs.insert(offs.end(), body.begin(), body.end());
    if ((int)offs.size() > MD_STRINGS_MAX)
        throw std::runtime_error("the strings are too long: " + std::to_string(offs.size()) + " bytes (max " +
                                 std::to_string(MD_STRINGS_MAX) + ")");
    return offs;
}
