/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// The level blob and the strings blob the game's rules read: byte-for-byte
// the same as tools/gen_cdata.py level_blob/strings_blob of each port
// (Master System: little-endian; the Mega Drive campaign's, which every
// other console reads: big-endian, tables at even addresses).
// Licence: GPL-2.0-or-later.
//
// The "level blob" is the rules' view of one map: the thing types, things,
// doors, scripts, the solid-cell bit map and a few lookup tables, as a
// header followed by a list of u16 offsets to tables (see levelblob.cpp).
// The "strings blob" holds the texts: a u16 offset per text, then the
// texts in Latin-1, each ending in 0. The builders (sms_build, md_build)
// place both in the ROM where the game code expects them.
#pragma once
#include "project.h"

#include <string>
#include <vector>

// the Master System's (and Game Gear's) level blob ("LVL1", version 2);
// throws std::runtime_error with a readable reason when a limit is exceeded
Bytes make_level_blob(const Project &p);
// its texts (the same limits), the phone's keys they name written as the
// console's buttons (keytext.h): gg = the Game Gear's, else the Master System's
Bytes make_strings_blob(const Project &p, bool gg = false);
// checks a project against the Master System's limits; returns "" when fine
std::string check_limits(const Project &p);

// the Mega Drive campaign's (LVB2): maps = the campaign's map names in ROM
// order (CHANGEMAP's targets), map_id = the original game's id of the map
// (both throw std::runtime_error when a limit is exceeded)
Bytes make_level_blob_md(const Project &p, const std::vector<std::string> &maps, int map_id);
// (the phone's keys the texts name made into button tokens: keytext::md_text)
Bytes make_strings_blob_md(const Project &p);
// "" when p fits a campaign map's limits, else the reason
std::string check_limits_md(const Project &p);
