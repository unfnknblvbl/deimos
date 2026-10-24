/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// The editable level ("project"), the same for the Master System and the
// Mega Drive: what the ROM's source container holds (see
// tools/source_container.py, which writes exactly the same JSON).
// Licence: GPL-2.0-or-later.
//
// A Project is one map with everything needed to build it: the 32 x 32
// cell grid, the walls, props, things and their types, doors, scripts,
// texts, and the art (drawings plus palettes). Almost every part of the
// editor reads or changes it: jar.cpp makes one from the original game,
// container.cpp reads and writes it inside a ROM, levelblob.cpp turns it
// into the game rules' tables, geometry.cpp into wall pieces, and the
// sms_build / md_build builders into a ROM.
//
// Coordinates: cells are (x, z), 0..31, x to the east, z to the south;
// cell index = z * 32 + x. World units are 64 per cell. Facings are
// 0 north (-z), 1 east, 2 south, 3 west (geometry.cpp's FX/FZ).
// "Campaign map": a map of the Mega Drive's whole-game ROM (map_id >= 0),
// which keeps a few things the single-level ROMs drop.
#pragma once
#include "common.h"

#include <map>
#include <string>
#include <vector>

struct Wall {          // a wall segment in world units (64 per cell)
    int ax = 0, az = 0, bx = 0, bz = 0;   // its ends A and B
    int tex = 0;       // wall texture id
    int flags = 0;     // original game flags (door bits etc.), kept as-is
                       // (geometry.cpp reads: 1 two-sided, 8/16 nudge +3/-3,
                       // 256/512 nudge along z/x, 0x400 locked, 0x8000
                       // texture mirrored; jar.cpp reads 0x800..0x4000)
};

struct Scenery {       // a fixed prop or wall decal
    int tile = 0;      // sprite id (305 and up: wall texture tile - 305)
    int x = 0, z = 0;  // world units
    u32 info = 0;      // original flags: 0x780000 = wall decal orientation, 0x2000000 = mirrored
    int cell = -1;     // (campaign maps) the cell whose HIDE takes it away, -1: always there
};

struct EntityDef {     // a kind of thing (monster, item, person...)
    int tile = 0;      // sprite id
    // the original game's kind (eType: 1 monster, 3/4/6/16 pickups,
    // 7/13/14/15 blocking props...) and sub-kind within it
    int eType = 0, eSub = 0;
    s32 parm = 0;      // the original's parameter (amount, monster stats...), kept as-is
    std::string name;  // UTF-8; the blob stores it upper-cased in Latin-1
};

struct Entity {        // a thing on the map
    int def = 0;       // index into Project::defs
    int x = 0, z = 0;  // cell
    bool hidden = false;  // appears only when an event SHOWs it
    std::vector<int> spawns;  // (campaign maps) the two things a dying Pain Elemental lets out ($FFFF: none)
};

struct Thin {          // (campaign maps) an invisible blocker: a thin wall on one edge of a cell
    int x = 0, z = 0;  // cell
    int type = 14;     // 14 or 15 (which shots it stops)
    std::string edge = "W";   // W E N S
};

struct Line {          // a door or sliding wall: one of the walls, switchable
    int wall = 0;      // index into walls
    int x = 0, z = 0;  // the cell it blocks while closed
    int eType = 0, eSub = 0;   // its kind, from the thing type drawn on it (9/0: a plain wall)
    std::string name;
    bool locked = false;       // starts locked (wall flag 0x400)
};

struct Cmd {           // one script command
    int op = 0;        // byte code, 0..42 (cmd_name())
    // its two arguments, as in the original game, except: a door command's
    // a1 is a slot in Project::lines, SHOW's a1 an index in entities
    // (255, or $FFFF on campaign maps: none), CHANGEMAP's low byte a
    // string id naming the map
    u32 a1 = 0, a2 = 0;
};

struct Event {         // a script attached to a cell
    int x = 0, z = 0;  // cell
    // state: the script's starting state (CHANGESTATE, NEXTSTATE and
    // PREVSTATE change it while playing; commands can depend on it);
    // flags: the original trigger's 3 flag bits (bit 0: the script does
    // not run when the player just walks onto the cell)
    int state = 0, flags = 0;
    std::vector<Cmd> cmds;
};

// A drawing: 64x64 palette indices. Textures are [v][u] (row = height,
// column = position along the wall); sprites and weapons [row][col].
// 255 = transparent (sprites and weapons only).
const u8 CLEAR = 255;
struct Drawing {
    u8 px[64 * 64] = {};
};

// A picture = a drawing + its own 16 colours. Several pictures may share
// one drawing with different colours, as in the original game.
struct ArtRef {
    std::string shape;   // key into Project::shapes
    RGB pal[16];         // colour of each palette index
};

// flattened picture (drawing + colours), for the builders
struct Art {
    u8 px[64 * 64] = {};
    RGB pal[16];
    RGB at(int i) const { return pal[px[i] & 15]; }      // pixel i's colour (CLEAR reads as 15)
    bool clear(int i) const { return px[i] == CLEAR; }   // pixel i is transparent
};

const int N_WEAPON_FRAMES = 18;   // 9 weapons x (idle, firing)

// One console's own versions of pictures and texts (the Art and Text tabs'
// "Edit for" setting): only those that differ from the shared ones, by id.
// A console without its own version of something uses the shared one. The
// Master System and Game Gear can also have a fixed 16-colour level palette
// (has_palette): the builder uses it instead of choosing the 16 colours.
// Kept in project.json under "per_console", keyed by console_key(), and
// only when a console has something of its own (so ROMs made before this
// existed rebuild byte for byte).
struct ConsoleOwn {
    std::map<int, ArtRef> textures, sprites;   // by texture id / sprite tile
    std::map<int, std::string> strings;        // by text id (UTF-8)
    bool has_palette = false;
    RGB palette[16];
    bool empty() const { return textures.empty() && sprites.empty() && strings.empty() && !has_palette; }
};
// the key a console's own versions are kept under ("sms", "gg", "md", "snes",
// "snesfx", "3ds", "32x", "gbc", "gba")
extern const char *const CONSOLE_KEYS[9];

struct Project {
    std::string name = "Sector 1";   // the map's title as the game shows it
    bool new_game = false;  // (Master System) the game starts on this map: a new game's kit
    RGB floor, ceiling;     // the original map's floor and ceiling colours
    // the Master System's own ceiling and floor (its floor fades into the
    // ceiling colour towards the horizon); without them its usual ones: a
    // black ceiling, a dark red floor (as the Master System port chose) -
    // except a level from a Master System ROM that had none (sms_plain:
    // those used the above, and keep them)
    bool has_sms_colours = false, sms_plain = false;
    RGB sms_ceiling, sms_floor;
    RGB sms_ceil() const { return has_sms_colours ? sms_ceiling : sms_plain ? ceiling : RGB{0, 0, 0}; }
    RGB sms_flo() const { return has_sms_colours ? sms_floor : sms_plain ? floor : RGB{85, 0, 0}; }
    int spawn_x = 1, spawn_z = 1, spawn_f = 0;   // the player's start: cell and facing
    // a map of the Mega Drive campaign: its original map id (-1: a single level)
    int map_id = -1;
    bool campaign() const { return map_id >= 0; }
    std::vector<Thin> thin;   // (campaign maps only; written to JSON only for them)
    std::vector<u8> wall = std::vector<u8>(1024, 1);       // 1 = solid cell
    std::vector<u8> static_block = std::vector<u8>(1024);  // blocking prop type (7, 13, 14, 15) or 0
    std::vector<Wall> walls;
    std::vector<Scenery> scenery;      // props and wall decals
    std::vector<EntityDef> defs;       // the thing types this map uses
    std::vector<Entity> entities;      // things on the map
    std::vector<Line> lines;           // doors and sliding walls ("line slots")
    std::vector<Event> events;         // scripted cells, at most one per cell
    std::vector<std::string> strings;  // texts (UTF-8), referenced by id from scripts

    // the drawings by key ("t<offset>", "s<offset>" from the jar, or
    // "<prefix>new<n>" for ones made in the editor)
    std::map<std::string, Drawing> shapes;
    std::map<int, ArtRef> textures;    // by texture id
    std::map<int, ArtRef> sprites;     // by sprite id (tile)
    ArtRef border;                     // the rock around the Master System's view
    std::vector<ArtRef> weapons;       // N_WEAPON_FRAMES: the player's weapon (Mega Drive)
    // each console's own pictures, texts and palette (see ConsoleOwn), by
    // CONSOLE_KEYS name; consoles with nothing of their own are left out
    std::map<std::string, ConsoleOwn> per_console;
    // (set by for_console from a console's fixed palette: the Master System
    // and Game Gear builder then uses these 16 colours as the level's)
    bool has_level_palette = false;
    RGB level_palette[16];
    // this level as console `key` builds it: its own pictures and texts in
    // place of the shared ones, its fixed palette if any (per_console is
    // kept, so the ROM's editable copy still has every console's versions)
    Project for_console(const std::string &key) const;

    // replaces *this with the level in project.json's text; false with err
    // when it can't be read or is an old format (version < 2)
    bool from_json(const std::string &text, std::string &err);   // the art's drawings come separately
    // project.json's text, byte for byte as tools/source_container.py writes it
    std::string to_json() const;

    // a picture's drawing and colours in one (a zero drawing when its shape is missing)
    Art flat(const ArtRef &r) const;
    // the drawing a picture uses, or nullptr when its shape is missing
    const Drawing *drawing(const ArtRef &r) const;
    Drawing *drawing(ArtRef &r);
    // pictures using a drawing (for "also used by")
    std::vector<std::string> users(const std::string &shape) const;
    // a shapes key not yet used: prefix + "new" + the first free number from 1
    std::string new_shape_key(const std::string &prefix) const;
    // removes the drawings no picture refers to
    void drop_unused_shapes();
};

// command names (Doom RPG event byte-code)
// "?" for 0 and for anything out of 0..42
const char *cmd_name(int op);
