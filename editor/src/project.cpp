// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Project <-> JSON (each map of a project file, projectfile.h, and a level
// folder's project.json): the same keys in the same order every time, no
// spaces, so an unchanged level is written back byte for byte.
// Licence: GPL-2.0-or-later.
//
// Also the Project's small helpers for the art (flat, drawing, users,
// new_shape_key, drop_unused_shapes, rename_shape), its Master System
// form (single_level) and the script command names.
// Reading uses nlohmann::json (key order doesn't matter); writing uses
// ordered_json so the keys come out in insertion order, as Python's
// json.dumps does.
#include "project.h"

#include "../third_party/json/json.hpp"

#include <cstring>
#include <set>

using nlohmann::json;
using ojson = nlohmann::ordered_json;

// [r, g, b] -> RGB (black when j isn't an array of at least 3)
static RGB rgb_of(const json &j)
{
    RGB c;
    if (j.is_array() && j.size() >= 3) {
        c.r = j[0].get<int>();
        c.g = j[1].get<int>();
        c.b = j[2].get<int>();
    }
    return c;
}

// RGB -> [r, g, b]
static ojson json_of(RGB c) { return ojson::array({c.r, c.g, c.b}); }

// {"shape": key, "pal": [[r,g,b] x16]} -> ArtRef; throws when "shape" or "pal" is missing
static ArtRef ref_of(const json &j)
{
    ArtRef r;
    r.shape = j.at("shape").get<std::string>();
    auto &p = j.at("pal");
    for (int i = 0; i < 16 && i < (int)p.size(); i++) r.pal[i] = rgb_of(p[i]);
    return r;
}

// ArtRef -> {"shape": ..., "pal": [...]}
static ojson json_of(const ArtRef &r)
{
    ojson pal = ojson::array();
    for (auto &c : r.pal) pal.push_back(json_of(c));
    ojson o;
    o["shape"] = r.shape;
    o["pal"] = pal;
    return o;
}

// see project.h. Missing required keys and wrong types throw inside
// nlohmann::json; they are caught below and reported in err.
bool Project::from_json(const std::string &text, std::string &err)
{
    try {
        json j = json::parse(text);
        *this = Project();   // start from the defaults
        name = j.value("name", std::string("Sector 1"));
        new_game = j.value("new_game", false);
        floor = rgb_of(j["floor_rgb"]);
        ceiling = rgb_of(j["ceiling_rgb"]);
        if (j.contains("sms_ceiling_rgb") && j.contains("sms_floor_rgb")) {
            has_sms_colours = true;
            sms_ceiling = rgb_of(j["sms_ceiling_rgb"]);
            sms_floor = rgb_of(j["sms_floor_rgb"]);
        }
        map_id = j.value("map_id", -1);
        if (j.contains("thin"))
            for (auto &t : j["thin"]) thin.push_back({t["x"], t["z"], t["type"], t["edge"].get<std::string>()});
        spawn_x = j["spawn"][0];   // [x, z, facing]
        spawn_z = j["spawn"][1];
        spawn_f = j["spawn"][2];
        wall = j["wall"].get<std::vector<u8>>();
        static_block = j["static_block"].get<std::vector<u8>>();
        if (wall.size() != 1024 || static_block.size() != 1024) {
            err = "map grid is not 32x32";
            return false;
        }
        for (auto &w : j["walls"])
            walls.push_back({w["ax"], w["az"], w["bx"], w["bz"], w["tex"], w["flags"]});
        for (auto &s : j["scenery"])
            scenery.push_back({s["tile"], s["x"], s["z"], s["info"].get<u32>(), s.value("cell", -1)});
        for (auto &d : j["defs"]) defs.push_back({d["tile"], d["eType"], d["eSub"], d["parm"].get<s32>(), d["name"]});
        for (auto &e : j["entities"]) {
            entities.push_back({e["def"], e["x"], e["z"], e["hidden"].get<bool>(), {}});
            if (e.contains("spawns")) entities.back().spawns = e["spawns"].get<std::vector<int>>();
        }
        // (older files named a line's wall "line")
        for (auto &l : j["lines"])
            lines.push_back({l.contains("wall") ? l["wall"] : l["line"], l["x"], l["z"], l["eType"], l["eSub"],
                             l["name"], l["locked"].get<bool>()});
        for (auto &e : j["events"]) {
            Event ev{e["x"], e["z"], e["state"], e["flags"], {}};
            for (auto &c : e["cmds"]) ev.cmds.push_back({c["op"], c["a1"].get<u32>(), c["a2"].get<u32>()});
            events.push_back(ev);
        }
        strings = j["strings"].get<std::vector<std::string>>();
        // version 1 had no per-picture art
        if (j.value("version", 1) < 2 || !j.contains("textures")) {
            err = "this level is in an old format the editor no longer reads";
            return false;
        }
        // textures and sprites are objects keyed by the id as a string
        for (auto &kv : j["textures"].items()) textures[std::stoi(kv.key())] = ref_of(kv.value());
        for (auto &kv : j["sprites"].items()) sprites[std::stoi(kv.key())] = ref_of(kv.value());
        border = ref_of(j["border"]);
        for (auto &w : j["weapons"]) weapons.push_back(ref_of(w));
        // each console's own pictures, texts and palette (optional)
        if (j.contains("per_console"))
            for (auto &kv : j["per_console"].items()) {
                ConsoleOwn &o = per_console[kv.key()];
                const json &c = kv.value();
                if (c.contains("textures"))
                    for (auto &t : c["textures"].items()) o.textures[std::stoi(t.key())] = ref_of(t.value());
                if (c.contains("sprites"))
                    for (auto &t : c["sprites"].items()) o.sprites[std::stoi(t.key())] = ref_of(t.value());
                if (c.contains("strings"))
                    for (auto &t : c["strings"].items()) o.strings[std::stoi(t.key())] = t.value().get<std::string>();
                if (c.contains("palette")) {
                    o.has_palette = true;
                    for (int i = 0; i < 16 && i < (int)c["palette"].size(); i++) o.palette[i] = rgb_of(c["palette"][i]);
                }
            }
        if (weapons.size() != (size_t)N_WEAPON_FRAMES) {
            err = "the level should have 18 weapon pictures";
            return false;
        }
    } catch (const std::exception &e) {
        err = std::string("the level's JSON: ") + e.what();
        return false;
    }
    return true;
}

// see project.h. The optional keys (new_game, map_id, sms_*_rgb, thin, a
// prop's cell, a thing's spawns, per_console) are written only when used.
std::string Project::to_json() const
{
    ojson j;
    j["version"] = 2;
    j["name"] = name;
    if (new_game) j["new_game"] = true;
    if (campaign()) j["map_id"] = map_id;
    j["floor_rgb"] = json_of(floor);
    j["ceiling_rgb"] = json_of(ceiling);
    if (has_sms_colours) {
        j["sms_ceiling_rgb"] = json_of(sms_ceiling);
        j["sms_floor_rgb"] = json_of(sms_floor);
    }
    j["spawn"] = ojson::array({spawn_x, spawn_z, spawn_f});
    j["wall"] = wall;
    j["static_block"] = static_block;
    ojson a = ojson::array();   // reused for each array below
    if (campaign()) {
        for (auto &t : thin) {
            ojson o;
            o["x"] = t.x;
            o["z"] = t.z;
            o["type"] = t.type;
            o["edge"] = t.edge;
            a.push_back(o);
        }
        j["thin"] = a;
        a = ojson::array();
    }
    for (auto &w : walls) {
        ojson o;
        o["ax"] = w.ax;
        o["az"] = w.az;
        o["bx"] = w.bx;
        o["bz"] = w.bz;
        o["tex"] = w.tex;
        o["flags"] = w.flags;
        a.push_back(o);
    }
    j["walls"] = a;
    a = ojson::array();
    for (auto &s : scenery) {
        ojson o;
        o["tile"] = s.tile;
        o["x"] = s.x;
        o["z"] = s.z;
        o["info"] = s.info;
        if (campaign()) o["cell"] = s.cell;
        a.push_back(o);
    }
    j["scenery"] = a;
    a = ojson::array();
    for (auto &d : defs) {
        ojson o;
        o["tile"] = d.tile;
        o["eType"] = d.eType;
        o["eSub"] = d.eSub;
        o["parm"] = d.parm;
        o["name"] = d.name;
        a.push_back(o);
    }
    j["defs"] = a;
    a = ojson::array();
    for (auto &e : entities) {
        ojson o;
        o["def"] = e.def;
        o["x"] = e.x;
        o["z"] = e.z;
        o["hidden"] = e.hidden;
        if (!e.spawns.empty()) o["spawns"] = e.spawns;
        a.push_back(o);
    }
    j["entities"] = a;
    a = ojson::array();
    for (auto &l : lines) {
        ojson o;
        o["wall"] = l.wall;
        o["x"] = l.x;
        o["z"] = l.z;
        o["eType"] = l.eType;
        o["eSub"] = l.eSub;
        o["name"] = l.name;
        o["locked"] = l.locked;
        a.push_back(o);
    }
    j["lines"] = a;
    a = ojson::array();
    for (auto &e : events) {
        ojson c = ojson::array();
        for (auto &k : e.cmds) {
            ojson o;
            o["op"] = k.op;
            o["a1"] = k.a1;
            o["a2"] = k.a2;
            c.push_back(o);
        }
        ojson o;
        o["x"] = e.x;
        o["z"] = e.z;
        o["state"] = e.state;
        o["flags"] = e.flags;
        o["cmds"] = c;
        a.push_back(o);
    }
    j["events"] = a;
    j["strings"] = strings;
    ojson t = ojson::object();
    for (auto &kv : textures) t[std::to_string(kv.first)] = json_of(kv.second);
    j["textures"] = t;
    t = ojson::object();
    for (auto &kv : sprites) t[std::to_string(kv.first)] = json_of(kv.second);
    j["sprites"] = t;
    j["border"] = json_of(border);
    a = ojson::array();
    for (auto &w : weapons) a.push_back(json_of(w));
    j["weapons"] = a;
    // each console's own versions, only for consoles that have any
    ojson pc = ojson::object();
    for (auto &kv : per_console) {
        if (kv.second.empty()) continue;
        ojson c = ojson::object();
        if (!kv.second.textures.empty()) {
            ojson o = ojson::object();
            for (auto &t : kv.second.textures) o[std::to_string(t.first)] = json_of(t.second);
            c["textures"] = o;
        }
        if (!kv.second.sprites.empty()) {
            ojson o = ojson::object();
            for (auto &t : kv.second.sprites) o[std::to_string(t.first)] = json_of(t.second);
            c["sprites"] = o;
        }
        if (!kv.second.strings.empty()) {
            ojson o = ojson::object();
            for (auto &t : kv.second.strings) o[std::to_string(t.first)] = t.second;
            c["strings"] = o;
        }
        if (kv.second.has_palette) {
            ojson o = ojson::array();
            for (auto &col : kv.second.palette) o.push_back(json_of(col));
            c["palette"] = o;
        }
        pc[kv.first] = c;
    }
    if (!pc.empty()) j["per_console"] = pc;
    return j.dump(-1, ' ', true);   // compact, non-ASCII characters escaped (like Python's default)
}

const char *const CONSOLE_KEYS[9] = {"sms", "gg", "md", "snes", "snesfx", "3ds", "32x", "gbc", "gba"};

// see project.h
Project Project::for_console(const std::string &key) const
{
    Project p = *this;
    auto it = per_console.find(key);
    if (it == per_console.end()) return p;
    const ConsoleOwn &o = it->second;
    for (auto &kv : o.textures) p.textures[kv.first] = kv.second;
    for (auto &kv : o.sprites) p.sprites[kv.first] = kv.second;
    for (auto &kv : o.strings)
        if (kv.first >= 0 && kv.first < (int)p.strings.size()) p.strings[kv.first] = kv.second;
    if (o.has_palette) {
        p.has_level_palette = true;
        for (int i = 0; i < 16; i++) p.level_palette[i] = o.palette[i];
    }
    return p;
}

// see project.h
const Drawing *Project::drawing(const ArtRef &r) const
{
    auto it = shapes.find(r.shape);
    return it == shapes.end() ? nullptr : &it->second;
}

Drawing *Project::drawing(ArtRef &r)
{
    auto it = shapes.find(r.shape);
    return it == shapes.end() ? nullptr : &it->second;
}

// see project.h
Art Project::flat(const ArtRef &r) const
{
    Art a;
    if (const Drawing *d = drawing(r)) memcpy(a.px, d->px, sizeof a.px);
    for (int i = 0; i < 16; i++) a.pal[i] = r.pal[i];
    return a;
}

// readable names of every picture whose drawing is shape ("texture 3",
// "sprite 12", "border", "weapon 2 firing"), for the Art tab
std::vector<std::string> Project::users(const std::string &shape) const
{
    std::vector<std::string> out;
    for (auto &kv : textures)
        if (kv.second.shape == shape) out.push_back("texture " + std::to_string(kv.first));
    for (auto &kv : sprites)
        if (kv.second.shape == shape) out.push_back("sprite " + std::to_string(kv.first));
    if (border.shape == shape) out.push_back("border");
    for (size_t i = 0; i < weapons.size(); i++)
        // weapons come in pairs: even = still, odd = firing
        if (weapons[i].shape == shape) out.push_back("weapon " + std::to_string(i / 2) + (i & 1 ? " firing" : ""));
    for (auto &pc : per_console) {   // the consoles' own versions
        for (auto &kv : pc.second.textures)
            if (kv.second.shape == shape) out.push_back("texture " + std::to_string(kv.first) + " (" + pc.first + ")");
        for (auto &kv : pc.second.sprites)
            if (kv.second.shape == shape) out.push_back("sprite " + std::to_string(kv.first) + " (" + pc.first + ")");
    }
    return out;
}

// see project.h
std::string Project::new_shape_key(const std::string &prefix) const
{
    for (int i = 1;; i++) {
        std::string k = prefix + "new" + std::to_string(i);
        if (!shapes.count(k)) return k;
    }
}

// see project.h (called before writing a project file, so only drawings in use are stored)
void Project::drop_unused_shapes()
{
    std::set<std::string> used;
    for (auto &kv : textures) used.insert(kv.second.shape);
    for (auto &kv : sprites) used.insert(kv.second.shape);
    used.insert(border.shape);
    for (auto &w : weapons) used.insert(w.shape);
    for (auto &pc : per_console) {   // the consoles' own versions' drawings too
        for (auto &kv : pc.second.textures) used.insert(kv.second.shape);
        for (auto &kv : pc.second.sprites) used.insert(kv.second.shape);
    }
    for (auto it = shapes.begin(); it != shapes.end();)
        if (used.count(it->first)) ++it;
        else it = shapes.erase(it);
}

// see project.h. The drawing is replaced if `to` already names one.
void Project::rename_shape(const std::string &from, const std::string &to)
{
    auto it = shapes.find(from);
    if (it == shapes.end() || from == to) return;
    Drawing d = it->second;
    shapes.erase(it);
    shapes[to] = d;
    // every reference: the shared pictures, the border, the weapon and the consoles' own versions
    auto re = [&](ArtRef &r) {
        if (r.shape == from) r.shape = to;
    };
    for (auto &kv : textures) re(kv.second);
    for (auto &kv : sprites) re(kv.second);
    re(border);
    for (auto &w : weapons) re(w);
    for (auto &pc : per_console) {
        for (auto &kv : pc.second.textures) re(kv.second);
        for (auto &kv : pc.second.sprites) re(kv.second);
    }
}

// see project.h. The steps undo, one by one, what import_map (jar.cpp)
// does differently for a campaign map; the result is the level import_map
// gives in the single-level form.
Project Project::single_level() const
{
    if (!campaign()) return *this;
    Project p = *this;
    p.new_game = map_id == 1;   // the UAC Base: the game starts there
    p.map_id = -1;
    // a thin blocker on a cell's edge: the Master System's rules have no
    // thin walls, so it blocks the whole cell (with its type, 14 or 15)
    for (auto &t : thin)
        if (t.x >= 0 && t.x < 32 && t.z >= 0 && t.z < 32) p.static_block[t.z * 32 + t.x] = (u8)t.type;
    p.thin.clear();
    for (auto &sc : p.scenery) sc.cell = -1;   // (no HIDE takes a prop away)
    for (auto &e : p.entities) e.spawns.clear();
    // SHOW (op 7) of no thing: $FFFF on a campaign map, 255 here
    for (auto &ev : p.events)
        for (auto &c : ev.cmds)
            if (c.op == 7 && c.a1 == 0xFFFF) c.a1 = 0xFF;
    return p;
}

// see project.h. The names follow the original game's script byte codes
// (game/events.asm carries them out on the Master System).
const char *cmd_name(int op)
{
    static const char *names[] = {"?", "GOTO", "CHANGEMAP", "TRIGGER", "MESSAGE", "PAIN", "MOVELINE", "SHOW",
                                  "DIALOG", "GIVEMAP", "PASSWORD", "CHANGESTATE", "LOCK", "UNLOCK", "TOGGLELOCK",
                                  "OPENLINE", "CLOSELINE", "MOVELINE2", "HIDE", "NEXTSTATE", "PREVSTATE",
                                  "INCSTAT", "DECSTAT", "REQSTAT", "FORCEMESSAGE", "ANIM", "DIALOGNOBACK",
                                  "SAVEGAME", "ABORTMOVE", "SHAKE", "FLOORCOL", "CEILCOL", "ENABLEWEAPONS",
                                  "OPENSTORE", "CHANGESPRITE", "PARTICLES", "REFRESHVIEW", "WAIT", "PORTAL",
                                  "CHKLEVEL", "NOTE", "CHECKKEY", "SOUND"};
    if (op < 0 || op > 42) return "?";
    return names[op];
}
