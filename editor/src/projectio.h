/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// A project as plain files: project.json + textures/<id>.png + sprites/<id>.png
// (for using other paint programs, or keeping a level in git).
// Licence: GPL-2.0-or-later.
//
// The folder also holds border.png, weapons/<n>.png and <n>_firing.png,
// and drawings/<key>.png (each drawing as 8-bit grey: grey level = palette
// index, 255 = transparent). Used by the command-line tool's --export and
// --import, and by the Art tab's PNG import, export and paste.
#pragma once
#include "project.h"

#include <string>

// writes p into dir (creating its subfolders); false with err on failure
bool export_project(const Project &p, const std::string &dir, std::string &err);
// reads such a folder into p; a picture PNG that differs from its drawing
// in its colours gets a new drawing of its own
bool import_project(const std::string &dir, Project &p, std::string &err);

// PNG <-> Art. Textures: opaque, at most 16 colours (more are reduced).
// Sprites: transparent pixels allowed, at most 16 colours.
bool art_to_png(const Art &a, bool sprite, const std::string &file, std::string &err);
bool art_from_png(const std::string &file, bool sprite, Art &a, std::string &err);
// reduce any RGBA picture to an Art (used by import and paste)
void art_from_rgba(const u8 *rgba, int w, int h, bool sprite, Art &a);
