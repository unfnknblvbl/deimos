/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// The Mega Drive game's maps: the "campaign block", made from the maps'
// projects. Port of megadrive/tools/build_render_data.py, build_weapons.py,
// gen_cdata.py and md_campaign.py, producing byte-identical output.
// Licence: GPL-2.0-or-later.
//
// Called by engine.cpp for every console except the Master System family:
// the block (MDCP) holds every map's pack, the textures and sprites shared
// between maps and the weapon. The Mega Drive finds it through its
// header's directory ($1C8 "DEIMOSMD", $1D0 address, $1D4 limit); the
// SNES-image consoles carry the same block (its bank in the SNES header at
// $FFB6, or $7FB6 for the SuperFX).
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

// A whole new block of maps after rom[0, addr) (the engine), which must
// end by limit: the maps in play order (their names, titles and ids) with
// their projects, each built as console opt.console has it. snes: in a
// SNES-layout cartridge (HiROM); snes_fx: the SuperFX's (its header at
// $7FB0, the visibility tables left out). rom gets the block, its header
// and checksum; false with err when a map breaks a limit or the maps
// don't fit (rom is then cut to the engine).
bool new_campaign(Bytes &rom, u32 addr, u32 limit, const std::vector<MapInfo> &maps,
                  const std::vector<Project> &projects, const BuildOptions &opt, BuildStats &stats, std::string &err,
                  bool snes = false, bool snes_fx = false);

// what one weapon frame looks like on screen (256x160 view, 0 = clear,
// else 1 + palette index) and its 16 colours
void weapon_frame(const Project &p, int frame, std::vector<u8> &idx, u16 pal[16]);

}  // namespace md
