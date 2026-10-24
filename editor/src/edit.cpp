// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Licence: GPL-2.0-or-later.
//
// Map editing operations (declared in edit.h): digging floor and filling in
// wall with the walls kept around the open cells, doors and the scripts that
// work them, and the index fix-ups needed when a door, thing or thing type is
// deleted (scripts name doors and things by their index).
//
// Called by the Map tab (ui_map.cpp), App::new_map (app.cpp) and the Thing
// types tab (ui_data.cpp). Works only on the Project passed in; it calls
// nothing outside this file. See edit.h for the coordinate system.
#include "edit.h"

#include <algorithm>

namespace edit {

// the step to the neighbouring cell across each edge (EDGE_N, E, S, W)
static const int DX[4] = {0, 1, 0, -1}, DZ[4] = {-1, 0, 1, 0};
// flag bit saying which side of a wall is open (tools/gamedata.py line_cell)
// Indexed by edge: a wall made on a cell's north edge gets 0x800, east
// 0x2000, south 0x1000, west 0x4000 (stored in Wall::flags).
static const int OPEN_FLAG[4] = {0x800, 0x2000, 0x1000, 0x4000};

bool in_map(int x, int z) { return x >= 0 && x < 32 && z >= 0 && z < 32; }

bool solid(const Project &p, int x, int z) { return !in_map(x, z) || p.wall[z * 32 + x]; }

// edge endpoints, in the direction the games draw them: a one-sided wall
// shows only its front, and the inside of the cell must be in front
// ((A - eye) x (B - A) > 0, as in Sector 1)
// Inputs: cell (x, z) and an Edge. Outputs: the end points A (ax, az) and
// B (bx, bz) in world units. Going round the cell N, E, S, W the points run
// clockwise (as seen on the map with z pointing down).
static void edge_points(int x, int z, int edge, int &ax, int &az, int &bx, int &bz)
{
    int x0 = x * 64, z0 = z * 64, x1 = x0 + 64, z1 = z0 + 64;   // the cell's corners, world units
    switch (edge) {
    case EDGE_N: ax = x0; az = z0; bx = x1; bz = z0; break;
    case EDGE_E: ax = x1; az = z0; bx = x1; bz = z1; break;
    case EDGE_S: ax = x1; az = z1; bx = x0; bz = z1; break;
    default: ax = x0; az = z1; bx = x0; bz = z0; break;
    }
}

std::vector<int> walls_on_edge(const Project &p, int x, int z, int edge)
{
    int ax, az, bx, bz;
    edge_points(x, z, edge, ax, az, bx, bz);   // the edge's end points
    std::vector<int> r;
    for (size_t i = 0; i < p.walls.size(); i++) {
        const Wall &w = p.walls[i];
        // the same segment, drawn either way round
        if ((w.ax == ax && w.az == az && w.bx == bx && w.bz == bz) || (w.ax == bx && w.az == bz && w.bx == ax && w.bz == az))
            r.push_back((int)i);
    }
    return r;
}

std::vector<int> walls_touching(const Project &p, int x, int z)
{
    std::vector<int> r;
    int x0 = x * 64, z0 = z * 64, x1 = x0 + 64, z1 = z0 + 64;
    for (size_t i = 0; i < p.walls.size(); i++) {
        const Wall &w = p.walls[i];
        // the wall's bounding box; skip it if the box misses the cell
        int wx0 = std::min(w.ax, w.bx), wx1 = std::max(w.ax, w.bx), wz0 = std::min(w.az, w.bz), wz1 = std::max(w.az, w.bz);
        if (wx1 < x0 || wx0 > x1 || wz1 < z0 || wz0 > z1) continue;
        // ignore walls that only touch at a corner
        bool along_x = wz0 == wz1, along_z = wx0 == wx1;
        if (along_x && (wx1 <= x0 || wx0 >= x1)) continue;
        if (along_z && (wz1 <= z0 || wz0 >= z1)) continue;
        r.push_back((int)i);
    }
    return r;
}

// the face of cell (x,z) on `edge`, seen from inside the cell
// Adds a wall there with texture tex and the open-side flag for that edge,
// unless the edge already has a wall.
static void set_face(Project &p, int x, int z, int edge, int tex)
{
    auto have = walls_on_edge(p, x, z, edge);
    if (!have.empty()) return;   // already walled: keep it as it is
    Wall w;
    edge_points(x, z, edge, w.ax, w.az, w.bx, w.bz);
    w.tex = tex;
    w.flags = OPEN_FLAG[edge];
    p.walls.push_back(w);
}

// Removes every wall on one edge of cell (x, z).
static void clear_edge(Project &p, int x, int z, int edge)
{
    auto have = walls_on_edge(p, x, z, edge);
    // highest index first, so deleting one does not shift the others
    std::sort(have.rbegin(), have.rend());
    for (int w : have) delete_wall(p, w);
}

void make_solid(Project &p, int x, int z, int tex)
{
    if (!in_map(x, z) || p.wall[z * 32 + x]) return;   // off the map, or already solid
    // whatever stood here goes
    int l = line_at(p, x, z);
    if (l >= 0) delete_line(p, l);   // the door here, its wall and its script commands
    // walls inside the cell (not on its edges) go
    // Deleting renumbers the walls, so start the search again after each one.
    for (;;) {
        bool removed = false;
        for (int wi : walls_touching(p, x, z)) {
            bool on_edge = false;
            for (int e = 0; e < 4; e++)
                for (int k : walls_on_edge(p, x, z, e))
                    if (k == wi) on_edge = true;
            if (!on_edge) {
                delete_wall(p, wi);
                removed = true;
                break;
            }
        }
        if (!removed) break;
    }
    p.wall[z * 32 + x] = 1;          // now solid
    p.static_block[z * 32 + x] = 0;  // a solid cell has no blocking prop
    // each edge: the walls that faced into this cell go; an open neighbour
    // gets a wall facing into it instead (the old wall's texture if there was one)
    for (int e = 0; e < 4; e++) {
        int nx = x + DX[e], nz = z + DZ[e];
        int t = tex;
        auto old = walls_on_edge(p, x, z, e);
        if (!old.empty()) t = p.walls[old[0]].tex;
        clear_edge(p, x, z, e);
        // (e + 2) % 4: the same edge seen from the neighbour (N <-> S, E <-> W)
        if (!solid(p, nx, nz)) set_face(p, nx, nz, (e + 2) % 4, old.empty() ? tex : t);
    }
    // the things and props in the cell go (highest index first, so the
    // indices still to delete stay valid)
    auto ents = entities_at(p, x, z);
    std::sort(ents.rbegin(), ents.rend());
    for (int i : ents) delete_entity(p, i);
    auto sc = scenery_at(p, x, z);
    std::sort(sc.rbegin(), sc.rend());
    for (int i : sc) delete_scenery(p, i);
    // the player's start was here: move it to the first open neighbour (N, E,
    // S, W); with none it stays in the now solid cell
    // (The cell's script, if any, is kept.)
    if (p.spawn_x == x && p.spawn_z == z) {
        for (int e = 0; e < 4; e++)
            if (!solid(p, x + DX[e], z + DZ[e])) {
                p.spawn_x = x + DX[e];
                p.spawn_z = z + DZ[e];
                break;
            }
    }
}

void make_floor(Project &p, int x, int z, int tex)
{
    if (!in_map(x, z) || x == 0 || z == 0 || x == 31 || z == 31) return;   // keep the border solid
    if (!p.wall[z * 32 + x]) return;   // already floor
    p.wall[z * 32 + x] = 0;            // now open
    // each edge: the old wall goes (it faced the neighbour, which may be
    // open); a solid neighbour gets a wall facing into this cell
    for (int e = 0; e < 4; e++) {
        int nx = x + DX[e], nz = z + DZ[e];
        // keep the texture the neighbour had on this edge, if any
        int t = tex;
        auto old = walls_on_edge(p, x, z, e);
        if (!old.empty()) t = p.walls[old[0]].tex;
        clear_edge(p, x, z, e);
        if (solid(p, nx, nz)) set_face(p, x, z, e, t);
    }
}

void set_edge_texture(Project &p, int x, int z, int edge, int tex)
{
    for (int w : walls_on_edge(p, x, z, edge)) p.walls[w].tex = tex;
}

// The first door / sliding wall whose cell is (x, z), or -1.
int line_at(const Project &p, int x, int z)
{
    for (size_t i = 0; i < p.lines.size(); i++)
        if (p.lines[i].x == x && p.lines[i].z == z) return (int)i;
    return -1;
}

// The script attached to cell (x, z), or -1.
int event_at(const Project &p, int x, int z)
{
    for (size_t i = 0; i < p.events.size(); i++)
        if (p.events[i].x == x && p.events[i].z == z) return (int)i;
    return -1;
}

int ensure_event(Project &p, int x, int z)
{
    int e = event_at(p, x, z);
    if (e >= 0) return e;   // the cell has one already
    Event ev;               // else a new script with no commands (state 0, flags 0)
    ev.x = x;
    ev.z = z;
    p.events.push_back(ev);
    return (int)p.events.size() - 1;
}

void delete_event(Project &p, int i)
{
    if (i >= 0 && i < (int)p.events.size()) p.events.erase(p.events.begin() + i);
}

// Opcodes whose first argument (low byte of a1) is a line slot:
// 6 MOVELINE, 12 LOCK, 13 UNLOCK, 14 TOGGLELOCK, 15 OPENLINE, 16 CLOSELINE,
// 17 MOVELINE2.
bool op_uses_line(int op) { return op == 6 || op == 12 || op == 13 || op == 14 || op == 15 || op == 16 || op == 17; }
// Opcodes whose a1 is a thing (entity) index: 7 SHOW.
bool op_uses_entity(int op) { return op == 7; }

std::string add_door(Project &p, int x, int z, DoorKind kind, int tex)
{
    if (!in_map(x, z) || solid(p, x, z)) return "doors go on floor cells";
    if (line_at(p, x, z) >= 0) return "there is already a door here";
    // 64: the most any console allows; the Master System's 40 is checked at build time
    if ((int)p.lines.size() >= 64) return "the games allow 64 doors / sliding walls (the Master System 40)";
    bool ns = solid(p, x - 1, z) && solid(p, x + 1, z);   // corridor runs north-south
    bool we = solid(p, x, z - 1) && solid(p, x, z + 1);   // corridor runs west-east
    if (!ns && !we) return "a door needs walls on both sides (a corridor cell)";
    // the door's wall runs across the corridor through the cell's centre
    Wall w;
    int cx = x * 64 + 32, cz = z * 64 + 32;   // the cell's centre, world units
    if (ns) {
        // west to east across a north-south corridor
        w.ax = x * 64; w.az = cz; w.bx = x * 64 + 64; w.bz = cz;
        w.flags = 0x105;   // the original game's flag value for such a door (kept as-is by the builders)
    } else {
        // south to north across a west-east corridor
        w.ax = cx; w.az = z * 64 + 64; w.bx = cx; w.bz = z * 64;
        w.flags = 0x205;   // likewise, for a door the other way round
    }
    w.tex = tex;
    if (kind == DOOR_LOCKED) w.flags |= 0x400;   // the locked-door flag bit
    p.walls.push_back(w);
    // the line: which wall, the cell it blocks, its kind (eType 9 = sliding wall)
    Line l;
    l.wall = (int)p.walls.size() - 1;
    l.x = x;
    l.z = z;
    l.eType = kind == DOOR_SECRET ? 9 : 0;
    l.eSub = kind == DOOR_LOCKED ? 1 : 0;
    l.name = kind == DOOR_SECRET ? "Wall" : kind == DOOR_LOCKED ? "Locked Door" : "Door";
    l.locked = kind == DOOR_LOCKED;
    p.lines.push_back(l);
    int slot = (int)p.lines.size() - 1;
    if (kind == DOOR_SECRET) return "";   // opened by a script you write (OPENLINE)
    // use the door cell -> open; walk away from it on either side -> close
    // {15 OPENLINE, slot, a2 0x100}: the door cell's script opens it
    p.events[ensure_event(p, x, z)].cmds.push_back({15, (u32)slot, 0x100});
    // leave flags: N 16, E 32, S 64, W 128
    // {16 CLOSELINE, slot, flags}: the cells on both sides close the door when
    // the player walks away from it, i.e. leaves in any direction but back
    // towards the door. Each is added only if that cell is open.
    if (ns) {
        if (!solid(p, x, z - 1)) p.events[ensure_event(p, x, z - 1)].cmds.push_back({16, (u32)slot, 16 | 32 | 128});
        if (!solid(p, x, z + 1)) p.events[ensure_event(p, x, z + 1)].cmds.push_back({16, (u32)slot, 32 | 64 | 128});
    } else {
        if (!solid(p, x - 1, z)) p.events[ensure_event(p, x - 1, z)].cmds.push_back({16, (u32)slot, 16 | 64 | 128});
        if (!solid(p, x + 1, z)) p.events[ensure_event(p, x + 1, z)].cmds.push_back({16, (u32)slot, 16 | 32 | 64});
    }
    return "";
}

void delete_wall(Project &p, int wi)
{
    if (wi < 0 || wi >= (int)p.walls.size()) return;
    for (size_t s = 0; s < p.lines.size(); s++)
        if (p.lines[s].wall == wi) {
            delete_line(p, (int)s);
            return;   // delete_line removes the wall too
        }
    p.walls.erase(p.walls.begin() + wi);
    // the lines point at walls by index: those after the gap move down one
    for (auto &l : p.lines)
        if (l.wall > wi) l.wall--;
}

void delete_line(Project &p, int slot)
{
    if (slot < 0 || slot >= (int)p.lines.size()) return;
    int wi = p.lines[slot].wall;
    p.lines.erase(p.lines.begin() + slot);
    // fix the scripts: the slot is in a1's low byte
    for (auto &ev : p.events) {
        std::vector<Cmd> keep;
        for (auto c : ev.cmds) {
            if (op_uses_line(c.op)) {
                int a = (int)(c.a1 & 0xFF);
                if (a == slot) continue;   // a command on the deleted line: dropped
                if (a > slot) c.a1 = (c.a1 & ~0xFFu) | (u32)(a - 1);   // a later line: one slot down
            }
            keep.push_back(c);
        }
        ev.cmds = keep;
    }
    // drop now-empty scripts
    // (this also drops any script that was empty before)
    for (int i = (int)p.events.size() - 1; i >= 0; i--)
        if (p.events[i].cmds.empty()) delete_event(p, i);
    // the line's wall goes too; the other lines' wall indices are fixed up
    if (wi >= 0 && wi < (int)p.walls.size()) {
        p.walls.erase(p.walls.begin() + wi);
        for (auto &l : p.lines)
            if (l.wall > wi) l.wall--;
    }
}

// Appends the thing (shown from the start: hidden = false).
void add_entity(Project &p, int def, int x, int z)
{
    Entity e;
    e.def = def;
    e.x = x;
    e.z = z;
    p.entities.push_back(e);
}

std::vector<int> entities_at(const Project &p, int x, int z)
{
    std::vector<int> r;
    for (size_t i = 0; i < p.entities.size(); i++)
        if (p.entities[i].x == x && p.entities[i].z == z) r.push_back((int)i);
    return r;
}

void delete_entity(Project &p, int i)
{
    if (i < 0 || i >= (int)p.entities.size()) return;
    // SHOW's thing: the Master System's a byte ($FF: none), the Mega Drive
    // campaign's a word ($FFFF: none)
    bool wide = p.campaign();
    u32 mask = wide ? 0xFFFF : 0xFF;
    p.entities.erase(p.entities.begin() + i);
    // fix the scripts' SHOW commands: the thing number is in a1 & mask
    for (auto &ev : p.events) {
        std::vector<Cmd> keep;
        for (auto c : ev.cmds) {
            if (op_uses_entity(c.op)) {
                int a = (int)(c.a1 & mask);
                if (a == i) continue;   // showed the deleted thing: dropped
                // a later thing moves down one ("none" stays "none")
                if (a > i && a != (int)mask) c.a1 = (c.a1 & ~mask) | (u32)(a - 1);
            }
            keep.push_back(c);
        }
        ev.cmds = keep;
    }
    // Pain Elementals' spawn links (thing numbers; $FFFF = none)
    for (auto &e : p.entities)
        for (auto &s : e.spawns)
            if (s == i) s = 0xFFFF;
            else if (s > i && s != 0xFFFF) s--;
}

void delete_def(Project &p, int d)
{
    // its things first (from the end, so the indices stay valid)
    for (int i = (int)p.entities.size() - 1; i >= 0; i--)
        if (p.entities[i].def == d) delete_entity(p, i);
    p.defs.erase(p.defs.begin() + d);
    // the remaining things' type numbers above d move down one
    for (auto &e : p.entities)
        if (e.def > d) e.def--;
}

void add_scenery(Project &p, int tile, int x, int z, bool blocks)
{
    Scenery s;
    s.tile = tile;
    s.x = x * 64 + 32;   // the cell's centre, world units
    s.z = z * 64 + 32;
    p.scenery.push_back(s);
    // 13: the blocking-prop type the editor uses (static_block holds 7, 13, 14 or 15)
    if (blocks && in_map(x, z)) p.static_block[z * 32 + x] = 13;
}

std::vector<int> scenery_at(const Project &p, int x, int z)
{
    std::vector<int> r;
    // >> 6: world units to cell (64 units a cell)
    for (size_t i = 0; i < p.scenery.size(); i++)
        if (p.scenery[i].x >> 6 == x && p.scenery[i].z >> 6 == z) r.push_back((int)i);
    return r;
}

void delete_scenery(Project &p, int i)
{
    if (i < 0 || i >= (int)p.scenery.size()) return;
    int x = p.scenery[i].x >> 6, z = p.scenery[i].z >> 6;   // its cell
    p.scenery.erase(p.scenery.begin() + i);
    // the last prop in the cell gone: the cell no longer blocks
    if (in_map(x, z) && scenery_at(p, x, z).empty()) p.static_block[z * 32 + x] = 0;
}

}  // namespace edit
