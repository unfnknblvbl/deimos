/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// The Game Boy Color game (the Deimos repository's gbc/): a 4 MB
// MBC5 cartridge whose first 1 MB is the code (the "wrap"), then the SNES
// game's data as they are (its cartridge image from the resources' bank
// on: the resources made from the jar, then the campaign's block), then
// the Game Boy's own resources, made from those data (gbc/tools/gb_res.py:
// the same bytes). The editor makes the SNES image, then the cartridge
// from it.
// Licence: GPL-2.0-or-later.
//
// Cartridge layout: $000000 the Game Boy code (up to 1 MB), $100000 the
// SNES image from its resources' bank on ("DEIMRES1" ...), then on the
// next 16 KB boundary the Game Boy resources ("GBRES1"), the rest $FF.
// rom.cpp calls wrap; the Map tab's Game Boy Color preview calls
// view_colours.
#pragma once
#include "build.h"

#include <map>
#include <string>

namespace gbc {
const unsigned DATA_AT = 0x100000;   // where the SNES data start in the cartridge (1 MB)
// code + SNES image -> a 4 MB cartridge, with the Game Boy resources made
// anew from the image and the header's checksums set
bool wrap(const Bytes &wrap, const Bytes &image, Bytes &out, std::string &err);
// the Game Boy's resources from a SNES image (gb_res.py's build)
bool resources(const Bytes &image, Bytes &res, std::string &err);
// the 3D view's colours for a map whose 9-bit Mega Drive colours are used
// wmap[colour] times (resources()' way): per colour (b2..0 g2..0 r2..0) its
// family << 2 | shade (1..3), the families' shades (dark first)
void view_colours(const std::map<unsigned, long> &wmap, u8 place[512], RGB fam[3][3]);
}  // namespace gbc
