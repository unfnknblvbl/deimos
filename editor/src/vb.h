/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// The Virtual Boy game (the Deimos repository's vb/, its tools/vb_rom.py):
// an 8 MB cartridge with the code from $000000 (at most 512 KB), the SNES
// game's cartridge image from $080000 (as far as its last byte that isn't
// $FF, at most 4 MB), the header from $7FFDE0 and the interrupt vectors
// from $7FFE00; the rest $FF. The engine (engine.h) keeps the code and the
// header and vectors as its "wrap": the last $220 bytes of the cartridge
// first, then the code without its trailing $FF bytes. rom.cpp calls wrap
// when it saves a Virtual Boy ROM; the SNES image itself is made by the
// SNES code paths.
// Licence: GPL-2.0-or-later.
#pragma once
#include "build.h"

#include <string>

namespace vb {
const unsigned SIZE = 0x800000;     // the cartridge: 8 MB
const unsigned DATA_AT = 0x80000;   // where the SNES image starts (512 KB)
const unsigned TAIL = 0x220;        // the header and the vectors at the cartridge's end
// joins the wrap (tail, then code) and a SNES image (its trailing $FF
// bytes dropped) into a cartridge in out; false with err when the wrap is
// damaged or the image is over 4 MB
bool wrap(const Bytes &wrap, const Bytes &image, Bytes &out, std::string &err);
}  // namespace vb
