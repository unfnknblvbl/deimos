/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// The game's own texts that the consoles' code shows (the rules' messages,
// the menus' words, the vendor's items, the weapons' names) and the maps'
// titles, made from the user's DoomRPG.jar: the same recipes, in the same
// order, as tools/game_text.py, so the editor's ROMs are byte for byte the
// builds'. Nothing of the game's text is in the editor itself.
// Licence: GPL-2.0-or-later.
//
// The "texts table" goes into every ROM: in the campaign block (every
// console but the Master System: md_blob, at the offset in the block
// header's bytes 24..27) and in the Master System's and Game Gear's bank 2
// at $8000 (sms_blob). The code finds text TX_n through the table's n-th
// entry. Used by engine.cpp, md_build.cpp, sms_build.cpp and jar.cpp.
#pragma once
#include "build.h"

#include <string>
#include <vector>

class Jar;

namespace gametext {

// the texts table as the consoles show it, before tokenising: Latin-1, in
// tools/game_text.py's TEXTS order (a string the jar lacks is "")
std::vector<std::string> texts(const Jar &jar);
// the table as the campaign block holds it: a big-endian u16 offset per
// text (from the table's start), then the texts, each ended by a 0; the
// phone's keys a text names made into tokens (keytext::md_text)
Bytes md_blob(const Jar &jar);
// the Master System's (gg: the Game Gear's) table: a little-endian u16
// CPU address per text ($8000 + its offset), then the texts, the keys
// named as that console's buttons (keytext::sms_text)
Bytes sms_blob(const Jar &jar, bool gg);
// a map's title as the game names it (tools/game_text.py MAP_TITLE: from
// the jar's base.str, Deimos's own for the maps the game does not name),
// UTF-8; "" for a map it does not know
std::string map_title(const Jar &jar, const std::string &map);

}  // namespace gametext
