/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// The consoles' engines the editor reads (engines/*.eng beside the program,
// made by each console's tools/make_engine.py and ../tools/snes_make_engine.py): the games' code with nothing of the
// original game in it. A new ROM is an engine plus what the editor makes
// from the user's DoomRPG.jar: the status bar, fonts, title screen, music
// and sounds (src/assets.*), and the maps.
// Licence: GPL-2.0-or-later.
//
// An engine file ("DEIMENG1"): a byte for the console and 3 zero bytes,
// u32 template size, u32 region count; per region a length-prefixed name,
// u32 ROM offset, u32 size; a length-prefixed version string; u32 maps
// address, u32 maps limit; u32 packed size and the ZX0-packed template;
// for the wrapped consoles (3DS, 32X, GBC, GBA) then u32 size, u32 packed
// size and the ZX0-packed wrap. All little-endian. The files are named for
// the consoles' makers and kinds (file_name): sg8 (Master System), sg8hh
// (Game Gear), sg16 (Mega Drive), sg32 (32X), ntnd8hh (Game Boy Color),
// ntnd16 (SNES), ntnd16fx (SNES with the Super FX), ntnd32hh (Game Boy
// Advance) and ntnd3Dhh (3DS).
#pragma once
#include "build.h"
#include "jar.h"
#include "projectfile.h"
#include "rom.h"

#include <string>
#include <vector>

namespace engine {

// a part of the template blanked out by make_engine.py, to be filled with
// the assets::Blocks entry of the same name
struct Region {
    std::string name;
    u32 offset = 0, size = 0;   // where in the template, and how many bytes
};
// one console's engine, unpacked
struct Engine {
    Console console = Console::SMS;
    Bytes rom;                       // the template
    std::vector<Region> regions;     // the parts made from the jar
    std::string version;             // the port's version, shown on the title screen
    u32 maps_addr = 0, maps_limit = 0;   // (Mega Drive) where the maps' block goes
    Bytes wrap;                      // (3DS) the .3dsx before the cartridge image
                                     // (32X, GBC, GBA: the console's code)
};
// console c's engine file name (sg16.eng ...)
const char *file_name(Console c);
// the folder the engine files are read from: $DEIMOS_ENGINES, else engines/
// beside the program (or the source's editor/engines/ when the editor runs
// from its build folder)
std::string folder();
// whether console c's engine file is there (the editor offers only those)
bool available(Console c);
// reads and unpacks console c's engine; false with err if it's missing or damaged
bool load(Console c, Engine &e, std::string &err);
// the engine's version string ("?" when it can't be read)
std::string version(Console c);

// the game's maps that the jar has, in the order they are played (the
// Master System and Game Gear hold one at a time, the others any of them)
std::vector<std::string> campaign_maps(const Jar &jar);

// A new ROM of the game's own maps, straight from the jar: the Master
// System and Game Gear get maps[0] (as a single level); every other
// console all of them, in order, as the whole game. notes collects what
// didn't come across from the jar.
bool new_rom(Console c, const Jar &jar, const std::vector<std::string> &maps, Rom &out, const BuildOptions &opt,
             BuildStats &stats, std::string &err, std::vector<std::string> *notes = nullptr);
// A new project of the game's maps (in play order), each in the whole
// game's form, with its art; notes as for new_rom.
bool new_project(const Jar &jar, const std::vector<std::string> &maps, ProjectFile &out, const BuildOptions &opt,
                 std::string &err, std::vector<std::string> *notes = nullptr);
// A ROM for console c of a project: every map, in order, each as console
// c has it (its own pictures and texts); the Master System and Game Gear
// take map `map` alone, in its single-level form (Project::single_level).
// The graphics and sounds come from the jar.
bool project_rom(Console c, const Jar &jar, const ProjectFile &pf, int map, Rom &out, const BuildOptions &opt,
                 BuildStats &stats, std::string &err);
// "" when console c can take map `map` of the project (the Master System
// and Game Gear: within their limits), else why not
std::string why_not(Console c, const ProjectFile &pf, int map);

}  // namespace engine
