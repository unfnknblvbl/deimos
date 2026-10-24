/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// The Game Boy Advance game (the Deimos repository's gba/): its code
// in the cartridge's first 512 KB, then the SNES game's cartridge image as
// it is (as far as its last byte that isn't $FF), which the GBA reads at its
// SNES addresses. The editor keeps the code (the "wrap") and puts it back in
// front of a new image; the cartridge is padded to a multiple of 64 KB
// (the header, its logo and checksum, are in the code and don't change).
// rom.cpp calls these when it opens or saves a .gba; the SNES image itself
// is made and read by the SNES code paths.
// Licence: GPL-2.0-or-later.
#pragma once
#include "build.h"

#include <string>

namespace gba {
const unsigned DATA_AT = 0x80000;   // where the SNES image starts in the cartridge (512 KB)
// true when f is this game's cartridge (its header's title and fixed byte)
bool is_gba(const Bytes &f);
// splits a cartridge into its code (wrap, DATA_AT bytes) and the SNES
// image, padded back with $FF to a power of two from 256 KB (at most 4 MB);
// false with err when f is not this game
bool unwrap(const Bytes &f, Bytes &wrap, Bytes &image, std::string &err);
// joins code and a SNES image (its trailing $FF bytes dropped) into a
// cartridge in out; false with err when the code is damaged or the image
// is over 4 MB
bool wrap(const Bytes &wrap, const Bytes &image, Bytes &out, std::string &err);
}  // namespace gba
