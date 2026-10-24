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
// The editor keeps the code (the "wrap") and puts it back before a new
// image; the cartridge is padded to a power of two (at most 4 MB) and its
// Mega Drive header's size and checksum are set.
// rom.cpp calls these when it opens or saves a .32x.
// Licence: GPL-2.0-or-later.
#pragma once
#include "build.h"

#include <string>

namespace mars {
const unsigned DATA_AT = 0x80000;   // where the SNES-layout image starts (512 KB)
// true when f is a 32X cartridge ("SEGA 32X" at $100, the 32X security
// code's "MARS CHECK MODE" at $3C0) longer than its code
bool is_32x(const Bytes &f);
// splits a cartridge into its code (wrap, DATA_AT bytes) and the image,
// padded with $FF to a power of two from 256 KB
bool unwrap(const Bytes &f, Bytes &wrap, Bytes &image, std::string &err);
// joins code and image (its trailing $FF bytes dropped) into a cartridge
// in out, sets the header's ROM end and checksum; false with err when the
// code is damaged or the result is over 4 MB
bool wrap(const Bytes &wrap, const Bytes &image, Bytes &out, std::string &err);
}  // namespace mars
