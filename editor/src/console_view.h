/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// The 3D view as each console shows it (the Map tab's preview): the size of
// its picture, its focal length, its colours (the Mega Drive's 9 bits and
// 16 a view, the 15 bits of the SNES family, the Game Boy Color's
// families of colours, a palette per tile), its floor and ceiling (shaded
// line by line, dithered on the Mega Drive, by distance on the 3DS, flat
// on the Game Boy Color), at the size the console puts it on the screen.
// The Master System and the Game Gear have their own exact renderer
// (sms_build.h). Licence: GPL-2.0-or-later.
//
// Called by App::draw_preview (ui_map.cpp). It draws the view with the
// true-colour renderer (render.h) and then recolours it for the console
// (console_view.cpp). It is a close imitation, not a bit-exact copy, of what
// each console draws.
#pragma once
#include "geometry.h"
#include "project.h"
#include "rom.h"

#include <vector>

// The finished preview picture.
struct ConsoleView {
    int w = 0, h = 0;               // the picture as the screen shows it
                                    // (w = 0: nothing drawn)
    std::vector<RGB> px;            // w * h pixels, row by row from the top
    const char *note = "";          // a line on how the console draws it
};

// Draws the view from cell (gx, gz) facing `facing` (0 N, 1 E, 2 S, 3 W) as
// console c shows it. geo = build_geometry(p); line_open: per door slot,
// nonzero = drawn open. Any console not handled specially (the Mega Drive,
// and also the Master System / Game Gear if passed) gets the Mega Drive's look.
void console_view(Console c, const Project &p, const Geometry &geo, int gx, int gz, int facing,
                  const std::vector<u8> &line_open, ConsoleView &out);
