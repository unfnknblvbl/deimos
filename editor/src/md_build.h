/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// Mega Drive (the Mega Drive game): the level's data pack, rebuilt from a project.
// Port of tools/build_render_data.py, tools/build_weapons.py,
// tools/gen_cdata.py and tools/md_pack.py of the Mega Drive game repository,
// producing byte-identical output.
// Licence: GPL-2.0-or-later.
//
// Called by rom.cpp and engine.cpp for every console except the Master
// System family. Two ROM forms:
// - single level (Mega Drive version 0.0.4, 0.0.5): one "pack" (MDPK) at the
//   address in the Mega Drive header's directory ($1C8 "MDRPGDIR", $1D0
//   address, $1D4 limit);
// - the "campaign block" (MDCP, 0.0.6 on): every map's pack, the
//   textures and sprites shared between maps, the weapon, and one source
//   container with all maps. The SNES-image consoles carry the same block
//   (its bank in the SNES header at $FFB6, or $7FB6 for the SuperFX).
// All values in packs and blocks are big-endian (the 68000's order).
#pragma once
#include "build.h"
#include "project.h"

#include <string>
#include <vector>

namespace md {

// per map: the most texture images (decals and the portal's closed
// textures included) and different sprites
const int R_MAX_TEX = 48, R_MAX_SPR = 64;
const u32 PACK_LIMIT = 0x200000;   // a single-level pack must end below 2 MB

// a Mega Drive game ROM: "SEGA" at $100 and the "MDRPGDIR" directory at $1C8
bool is_rom(const Bytes &rom);
// the SNES port's ROMs (Deimos): the same block of maps, in a HiROM
// cartridge (its bank at $FFB6)
bool is_snes(const Bytes &rom);
// the SuperFX build (the GSU's LoROM-style map: the header at $7FB0, the
// block from bank $40 on, the visibility tables left out)
bool is_snes_fx(const Bytes &rom);

// The campaign ROMs (0.0.6 and later) hold every map of the game; the
// single-level ROMs (0.0.4, 0.0.5) one. `map` picks which (ignored for a
// single level).
struct MapInfo {
    std::string name;     // "level01"
    std::string title;    // "Sector 1"
    int id = 0;           // the original game's map id
};
// the campaign's maps in ROM order; empty for a single-level ROM
bool campaign_maps(const Bytes &rom, std::vector<MapInfo> &out, std::string &err);
// the level (single-level ROM) or campaign map `map` stored in the ROM
bool read_project(const Bytes &rom, int map, Project &p, std::string &err);
// rebuilds the ROM with p (a campaign ROM: p replaces map `map`, the
// other maps are kept); rom is replaced only on success
bool build(Bytes &rom, int map, const Project &p, const BuildOptions &opt, BuildStats &stats, std::string &err);
// a whole new block of maps after rom[0, addr) (the engine): the maps in
// order with their projects (the campaign's form); rom gets the block and its header
bool new_campaign(Bytes &rom, u32 addr, u32 limit, const std::vector<MapInfo> &maps, std::vector<Project> &projects,
                  const BuildOptions &opt, BuildStats &stats, std::string &err, bool snes = false,
                  bool snes_fx = false);
// the pack alone (for tests)
bool make_pack(const Project &p, const BuildOptions &opt, Bytes &pack, std::string &err);

// what one weapon frame looks like on screen (256x160 view, 0 = clear,
// else 1 + palette index) and its 16 colours
void weapon_frame(const Project &p, int frame, std::vector<u8> &idx, u16 pal[16]);

}  // namespace md
