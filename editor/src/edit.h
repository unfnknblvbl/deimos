/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// Map editing operations that keep the level consistent (walls around
// solid cells, door scripts, indices used by scripts after deletions).
// Licence: GPL-2.0-or-later.
//
// Callers: the Map tab (ui_map.cpp), App::new_map (app.cpp), the Thing types
// tab (ui_data.cpp) and the script editor. All functions work on a Project
// (project.h) in place and do not touch the undo stack: the caller pushes
// undo first and calls App::changed() after.
//
// Coordinates: the map is 32 x 32 cells; cell (x, z) has index z * 32 + x in
// Project::wall and Project::static_block. x grows east, z grows south.
// Walls and props are in world units, 64 per cell, so cell (x, z) spans
// x*64..x*64+64 and z*64..z*64+64.
//
// A "line" is a door or sliding wall: an entry in Project::lines that points
// at one wall. Scripts name a line by its slot (its index in p.lines) and a
// thing by its index in p.entities, so deleting one renumbers the scripts.
#pragma once
#include "project.h"

// The four edges of a cell, clockwise from north (also used as facings:
// 0 north = -z, 1 east = +x, 2 south = +z, 3 west = -x).
enum Edge { EDGE_N = 0, EDGE_E = 1, EDGE_S = 2, EDGE_W = 3 };

namespace edit {
// true if (x, z) is a cell of the 32 x 32 map
bool in_map(int x, int z);
// true if cell (x, z) is solid wall; cells outside the map count as solid
bool solid(const Project &p, int x, int z);

// walls exactly on a cell edge (indices into p.walls)
// A wall matches in either direction (A to B or B to A).
std::vector<int> walls_on_edge(const Project &p, int x, int z, int edge);
// all walls touching a cell (for the inspector)
// Includes walls on its edges and inside it (doors, diagonals); walls that
// only meet the cell at a corner are left out.
std::vector<int> walls_touching(const Project &p, int x, int z);

// Turns a floor cell into solid wall: removes its door, walls inside it,
// things and props, and puts wall faces on the open neighbours' sides
// (texture tex, or the one that edge already had). Moves the player's start
// to a neighbour if it was here. Does nothing to a cell already solid.
void make_solid(Project &p, int x, int z, int tex);
// Digs out a solid cell into floor and puts walls (texture tex, or the
// texture the edge already had) on each edge that faces a solid cell.
// The map's outer ring of cells always stays solid.
void make_floor(Project &p, int x, int z, int tex);
// Gives every wall on one edge of cell (x, z) the texture tex.
void set_edge_texture(Project &p, int x, int z, int edge, int tex);

// What the Door tool places: a plain door (opens when used), a locked door
// (a script must UNLOCK it), or a secret sliding wall (a script must OPENLINE it).
enum DoorKind { DOOR_NORMAL, DOOR_LOCKED, DOOR_SECRET };
// returns "" or why it can't
// Puts a door across corridor cell (x, z): adds the wall, the line and,
// except for a secret wall, the scripts that open and close it.
std::string add_door(Project &p, int x, int z, DoorKind kind, int tex);
// Removes line `slot` and its wall, and fixes the scripts: commands on that
// line go, higher slots shift down by one, scripts left empty go.
void delete_line(Project &p, int slot);
// Removes a wall; a door's wall takes its whole line with it (delete_line).
void delete_wall(Project &p, int wall);

int line_at(const Project &p, int x, int z);          // -1 if none
int event_at(const Project &p, int x, int z);         // -1 if none
// The index of cell (x, z)'s script, adding an empty one if it has none.
int ensure_event(Project &p, int x, int z);
// Removes script i (no other fix-ups are needed: nothing refers to scripts by index).
void delete_event(Project &p, int i);

// Adds a thing of type `def` (index into p.defs) in cell (x, z).
void add_entity(Project &p, int def, int x, int z);
// Removes thing i: SHOW commands naming it go, higher numbers shift down,
// and Pain Elemental spawn links to it become "none" ($FFFF).
void delete_entity(Project &p, int i);
// The things in cell (x, z), as indices into p.entities, in list order.
std::vector<int> entities_at(const Project &p, int x, int z);
// Removes thing type d and every thing of that type; later types shift down.
// d must be a valid index (it is not checked).
void delete_def(Project &p, int d);

// Adds prop sprite `tile` in the centre of cell (x, z); if `blocks`, the cell
// is marked as blocked by a prop (static_block 13).
void add_scenery(Project &p, int tile, int x, int z, bool blocks);
// The props (and wall decals) whose position is inside cell (x, z), as indices into p.scenery.
std::vector<int> scenery_at(const Project &p, int x, int z);
// Removes prop i; its cell stops blocking once it has no props left.
void delete_scenery(Project &p, int i);

// which ops use a1 as a line slot / entity index / string id
// (script opcodes: see cmd_name in project.cpp; only the line and entity
// tests exist here)
bool op_uses_line(int op);
bool op_uses_entity(int op);
}  // namespace edit
