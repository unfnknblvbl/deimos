/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// A first-person view in true colour, the way the original game draws it
// (walls from the level's wall pieces, props and things as billboards).
// The editor uses it for the Mega Drive preview; the Master System
// preview is the console's exact renderer (sms_build.h).
// Licence: GPL-2.0-or-later.
//
// Called by console_view() (console_view.cpp), which draws the Map tab's
// preview for every console but the Master System and Game Gear, and then
// recolours the picture the way that console would.
// It is a simple software renderer: one depth per screen column for the
// walls (no floor or ceiling texturing: flat ceiling and floor colours),
// then sprites drawn back to front, column by column.
#pragma once
#include "geometry.h"
#include "project.h"

#include <vector>

// What to draw and how big.
struct ViewOptions {
    int w = 256, h = 160;              // the picture's size in pixels
    bool things = true;                // draw the things on the map (not hidden ones)
    std::vector<u8> line_open;         // per door/line slot: drawn open
    double focal = 0;                  // the focal length in pixels (0: half the width, a 90 degree view)
    std::vector<u8> *covered = nullptr;   // (out) w * h: 1 where a wall or a sprite is drawn
};

// geo = build_geometry(p); out = w * h colours
// Draws the view from cell (gx, gz) facing `facing` (0 north, 1 east,
// 2 south, 3 west). The camera stands 16 world units behind the cell's
// centre (geometry.h camera()), eye 36 units above the floor, walls 64 high.
// out is row by row, top first; the top half starts as p.ceiling, the
// bottom half as p.floor.
void render_view(const Project &p, const Geometry &geo, int gx, int gz, int facing, const ViewOptions &o,
                 std::vector<RGB> &out);
