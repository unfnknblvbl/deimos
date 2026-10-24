/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// A ROM the editor has made, for any of the nine consoles, and what the
// editor knows about the consoles (their names, file extensions, families).
// Licence: GPL-2.0-or-later.
//
// The two ROM families: the Master System family (SMS, Game Gear) is built
// by sms_build.*; every other console by md_build.*, because they all carry
// the Mega Drive game's "campaign block" of maps: the Mega Drive in its own
// cartridge, the others inside a SNES-layout cartridge image. For the 3DS,
// 32X, Game Boy Color and Game Boy Advance, `data` is that SNES image and
// `wrap` the console's own code around it, put back on save by n3ds.*,
// mars.*, gbc.* and gba.*. engine.h makes Roms; the App (app.cpp) and the
// command-line tool (cli.cpp) save them.
#pragma once
#include "build.h"
#include "project.h"

#include <string>

// the consoles the editor makes ROMs for (MARS = the 32X, SNESFX = the SNES with the SuperFX)
enum class Console { SMS, GG, MD, SNES, SNESFX, N3DS, MARS, GBC, GBA, VB };
// (N_CONSOLES, in project.h, is how many there are: the Console order is
// also CONSOLE_KEYS' and engine::file_name's)
// the Master System and the Game Gear share the engine's code and ROM layout
inline bool sms_family(Console c) { return c == Console::SMS || c == Console::GG; }
// the Mega Drive and the SNES share the campaign's block of maps (the Mega Drive game's format)
// (the 3DS game reads a SNES-layout cartridge image from its romfs: n3ds.h)
inline bool md_family(Console c)
{
    return c == Console::MD || c == Console::SNES || c == Console::SNESFX || c == Console::N3DS || c == Console::MARS ||
           c == Console::GBC || c == Console::GBA || c == Console::VB;
}
// the console's name for the user ("Master System", "SNES (SuperFX)"...)
inline const char *console_name(Console c)
{
    return c == Console::SMS ? "Master System" : c == Console::GG ? "Game Gear" : c == Console::MD ? "Mega Drive"
                               : c == Console::SNES ? "SNES" : c == Console::SNESFX ? "SNES (SuperFX)" : c == Console::N3DS ? "3DS" : c == Console::MARS ? "32X" : c == Console::GBA ? "Game Boy Advance" : c == Console::VB ? "Virtual Boy" : "Game Boy Color";
}
// the usual file extension of the console's ROMs (both SNES kinds: .sfc)
inline const char *console_ext(Console c)
{
    return c == Console::SMS ? ".sms" : c == Console::GG ? ".gg" : c == Console::MD ? ".md" : c == Console::N3DS ? ".3dsx" : c == Console::MARS ? ".32x" : c == Console::GBC ? ".gbc" : c == Console::GBA ? ".gba" : c == Console::VB ? ".vb" : ".sfc";
}

// a ROM made by the editor: its bytes and what console it is for
class Rom {
public:
    Bytes data;                     // (the 3DS: the cartridge image in the .3dsx's romfs)
    Bytes wrap;                     // (the 3DS: the .3dsx before it; the 32X, the Game Boy Color, the Game Boy Advance: its code)
    Console console = Console::SMS;

    // the ROM file's bytes (wrapped for the 3DS, 32X, GBC, GBA); false with
    // err when the wrap is damaged or the maps don't fit the cartridge
    bool file_bytes(Bytes &out, std::string &err) const;
    // writes file_bytes to file; false with err when it can't
    bool save(const std::string &file, std::string &err) const;
    const char *console_name() const { return ::console_name(console); }
};

// a whole file into out; false with err when it can't be opened
bool read_file(const std::string &file, Bytes &out, std::string &err);
// writes data to file + ".tmp", then replaces file with it
bool write_file(const std::string &file, const Bytes &data, std::string &err);
