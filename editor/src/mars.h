/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// The 32X game (the Deimos repository's 32x/): the 68000's and the
// SH-2s' code in the cartridge's first 512 KB, then the game's data, a
// cartridge image in the SNES game's layout (the campaign block the editor
// already writes for the SNES), as far as its last byte that isn't $FF.
// The engine (engine.h) keeps the code (the "wrap"), and the editor puts a
// new image after it; the cartridge is padded to a power of two (at most
// 4 MB) and its Mega Drive header's size and checksum are set.
// rom.cpp calls wrap when it saves a 32X ROM.
// Licence: GPL-2.0-or-later.
#pragma once
#include "build.h"

#include <string>

namespace mars {
const unsigned DATA_AT = 0x80000;   // where the SNES-layout image starts (512 KB)
// joins code and image (its trailing $FF bytes dropped) into a cartridge
// in out, sets the header's ROM end and checksum; false with err when the
// code is damaged or the result is over 4 MB
bool wrap(const Bytes &wrap, const Bytes &image, Bytes &out, std::string &err);
}  // namespace mars
