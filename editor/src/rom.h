/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// A game ROM the editor can open: Doom RPG for the Master System (0.2.8
// or later), the Mega Drive game for the Mega Drive (0.0.4 or later), or Doom RPG
// for the SNES, with or without the SuperFX, the 3DS, the 32X or the Game Boy Color (the
// Deimos repository's, 0.1.0 or later). Both keep
// the editable level inside the ROM, and the game code reads all the level
// data at run time, so rebuilding a ROM never touches its code.
// Licence: GPL-2.0-or-later.
//
// ("Both": the two ROM families. The Master System family (SMS, Game Gear)
// is handled by sms_build.*; every other console by md_build.*, because
// they all carry the Mega Drive game's "campaign block" of maps: the Mega Drive in its
// own cartridge, the others inside a SNES-layout cartridge image. For the
// 3DS, 32X, Game Boy Color and Game Boy Advance, `data` is that SNES image
// and `wrap` the console's own code around it, split off on load and put
// back on save by n3ds.*, mars.*, gbc.* and gba.*.)
// The GUI (app.cpp) and the command-line tool (cli.cpp) hold one Rom per
// open file.
#pragma once
#include "build.h"
#include "md_build.h"
#include "project.h"

#include <string>

// the consoles the editor makes ROMs for (MARS = the 32X, SNESFX = the SNES with the SuperFX)
enum class Console { SMS, GG, MD, SNES, SNESFX, N3DS, MARS, GBC, GBA };
// the Master System and the Game Gear share the engine's code and ROM layout
inline bool sms_family(Console c) { return c == Console::SMS || c == Console::GG; }
// the Mega Drive and the SNES share the campaign's block of maps (the Mega Drive game's format)
// (the 3DS game reads a SNES-layout cartridge image from its romfs: n3ds.h)
inline bool md_family(Console c)
{
    return c == Console::MD || c == Console::SNES || c == Console::SNESFX || c == Console::N3DS || c == Console::MARS ||
           c == Console::GBC || c == Console::GBA;
}
// The consoles whose ROMs the editor opens and edits: the Mega Drive, the
// 3DS, the Game Boy Color and the Game Boy Advance. The others (Master
// System, Game Gear, SNES, SNES with the SuperFX, 32X) are made locked,
// without the editable copy of the level, to save room; the editor makes
// them new (from the game, or from the level or campaign open in the
// editor) but does not open them.
inline bool editable(Console c)
{
    return c == Console::MD || c == Console::N3DS || c == Console::GBC || c == Console::GBA;
}
// the console's name for the user ("Master System", "SNES (SuperFX)"...)
inline const char *console_name(Console c)
{
    return c == Console::SMS ? "Master System" : c == Console::GG ? "Game Gear" : c == Console::MD ? "Mega Drive"
                               : c == Console::SNES ? "SNES" : c == Console::SNESFX ? "SNES (SuperFX)" : c == Console::N3DS ? "3DS" : c == Console::MARS ? "32X" : c == Console::GBA ? "Game Boy Advance" : "Game Boy Color";
}
// the usual file extension of the console's ROMs (both SNES kinds: .sfc)
inline const char *console_ext(Console c)
{
    return c == Console::SMS ? ".sms" : c == Console::GG ? ".gg" : c == Console::MD ? ".md" : c == Console::N3DS ? ".3dsx" : c == Console::MARS ? ".32x" : c == Console::GBC ? ".gbc" : c == Console::GBA ? ".gba" : ".sfc";
}

// one open ROM: its bytes, what console it is for, and (campaign ROMs)
// which map is being edited
class Rom {
public:
    Bytes data;                     // (the 3DS: the cartridge image in the .3dsx's romfs)
    Bytes wrap;                     // (the 3DS: the .3dsx before it; the 32X, the Game Boy Color, the Game Boy Advance: its code)
    Console console = Console::SMS;
    std::string path;               // the file it was loaded from ("" for a new ROM)
    // the Mega Drive campaign (0.0.6 and later): its maps, and the one being edited
    std::vector<md::MapInfo> maps;
    int map = 0;                    // index into maps
    bool is_campaign() const { return !maps.empty(); }

    // reads file and works out its console (unwrapping the 3DS, 32X, GBC
    // and GBA formats); false with err when it isn't a ROM the editor
    // knows. On failure *this is unchanged except possibly `console`.
    bool load(const std::string &file, std::string &err);
    // writes the ROM to file (wrapping it again for the 3DS, 32X, GBC, GBA)
    bool save(const std::string &file, std::string &err) const;
    // the level (or the current campaign map) stored in the ROM
    bool read_project(Project &p, std::string &err) const;
    // rewrites the level-dependent parts (the game code stays as it is)
    bool build(const Project &p, const BuildOptions &opt, BuildStats &stats, std::string &err);
    const char *console_name() const { return ::console_name(console); }
};

// a whole file into out; false with err when it can't be opened
bool read_file(const std::string &file, Bytes &out, std::string &err);
// writes data to file + ".tmp", then replaces file with it
bool write_file(const std::string &file, const Bytes &data, std::string &err);
