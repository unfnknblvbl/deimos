/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// Level geometry shared by both consoles' builders: wall pieces (doors
// shifted like the original, walls split every 64 units, wall decals baked
// into copies of the textures they cover) and the walls visible from each
// cell. Port of build_walls / visible_walls in tools/live_data.py (SMS)
// and tools/build_render_data.py (Mega Drive), which are the same code.
// Licence: GPL-2.0-or-later.
//
// Used by sms_build.cpp and md_build.cpp (the visibility lists in ROM),
// sms_model.cpp and the editor's previews. A "wall piece" (Seg) is at most
// 64 units long, so a texture's 64 columns cover it once. The "view" is
// the 3D picture the player sees from a cell facing one of 4 directions:
// a 90-degree field of view, the eye 16 units behind the cell's centre.
#pragma once
#include "project.h"

#include <string>
#include <vector>

// one wall piece, as the renderers draw it
struct Seg {
    int ax, az, bx, bz;   // its ends, world units (after the door nudge)
    int tex;        // index into Geometry::tex_imgs
    int line;       // door/line slot, 255 = a plain wall
    int ua, ub;     // texture column at A and B
    int two;        // two-sided
    int wall;       // index into Project::walls
};

// everything build_geometry makes from a Project
struct Geometry {
    std::vector<Seg> segs;
    std::vector<std::vector<RGB>> tex_imgs;   // 64x64 [v][u]
    std::vector<int> tex_src;                 // texture id each image comes from
    std::vector<char> tex_plain;              // ... with no decals baked in
    std::vector<Scenery> scenery;             // props + decals not baked into a wall
    int n_baked = 0;                          // decals baked into walls
};

const int NEAR = 20;   // props nearer than this (world units, after the -16) aren't drawn
// a facing's unit step: FX[f], FZ[f] for f = 0 north, 1 east, 2 south, 3 west
extern const int FX[4], FZ[4];

// throws std::runtime_error with a readable reason
Geometry build_geometry(const Project &p);

// Rotates a world offset (rx, rz) from the eye into view space for
// facing f: d = depth ahead, l = sideways (positive to the right).
inline void view_xf(int f, int rx, int rz, int &d, int &l)
{
    switch (f & 3) {
    case 0: d = -rz; l = rx; break;
    case 1: d = rx; l = rz; break;
    case 2: d = rz; l = -rx; break;
    default: d = -rx; l = -rz; break;
    }
}
// the eye's world position for cell (gx, gz) facing f: the cell's centre,
// 16 units back
inline void camera(int gx, int gz, int f, int &vx, int &vz)
{
    vx = gx * 64 + 32 - 16 * FX[f];
    vz = gz * 64 + 32 - 16 * FZ[f];
}

// walls (indices into segs, ascending) that win at least one of `cols`
// columns; colz = the nearest depth per column (may be null)
// removed[i] != 0 leaves piece i out (open doors). The columns span the
// 90-degree view evenly; a column with no wall gets depth infinity.
void visible_walls(const std::vector<Seg> &segs, const std::vector<char> &removed, int gx, int gz, int f,
                   std::vector<int> &out, std::vector<double> *colz, int cols = 512);
// the scenery that can show in front of the walls (SMS), and the cells
// whose things can show (Mega Drive, 128-byte bit map)
// Both use the colz of visible_walls for the same spot: something shows
// if at least one column it covers has its nearest wall beyond it.
std::vector<int> visible_scenery(const std::vector<Scenery> &scen, const std::vector<double> &colz, int gx, int gz,
                                 int f, int cols = 512);
Bytes visible_cells(const std::vector<double> &colz, int gx, int gz, int f, int cols = 512);
