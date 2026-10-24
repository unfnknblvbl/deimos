/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// The level blob and the strings blob the game's rules read: byte-for-byte
// the same as tools/gen_cdata.py level_blob/strings_blob of each port
// (Master System: little-endian; Mega Drive: big-endian, tables at even
// addresses).
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

// throws std::runtime_error with a readable reason when a limit is exceeded
// big_endian = false: the Master System's blob ("LVL1", version 2);
// true: the Mega Drive game's single-level blob ("LVB1", version 1)
Bytes make_level_blob(const Project &p, bool big_endian);
// the texts (the same choice of byte order and limits); the phone's keys
// they name written as the console's buttons (keytext.h): gg = the Game
// Gear's (else the Master System's, or for big_endian the old Mega Drive
// ROMs' default buttons)
Bytes make_strings_blob(const Project &p, bool big_endian, bool gg = false);
// checks a project against the ROM limits; returns "" when fine
std::string check_limits(const Project &p);          // the Master System's
std::string check_limits_mdv1(const Project &p);     // the Mega Drive game's single-level ROMs (0.0.4, 0.0.5)

// the Mega Drive campaign's (LVB2): maps = the campaign's map names in ROM
// order (CHANGEMAP's targets), map_id = the original game's id of the map
// (both throw std::runtime_error when a limit is exceeded)
Bytes make_level_blob_md(const Project &p, const std::vector<std::string> &maps, int map_id);
// (the phone's keys the texts name made into button tokens: keytext::md_text)
Bytes make_strings_blob_md(const Project &p);
// "" when p fits a campaign map's limits, else the reason
std::string check_limits_md(const Project &p);
