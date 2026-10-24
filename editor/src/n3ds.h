/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// The 3DS game (the Deimos repository's 3ds/): a .3dsx (the code,
// the icon) whose romfs holds one file, deimos.sfc, a cartridge image in
// the SNES game's layout (the campaign block the editor already writes for
// the SNES). The image is the romfs's last bytes and the .3dsx's end: the
// engine (engine.h) keeps what comes before it (the "wrap"), and the
// editor puts a new image after it, its size written into the romfs's
// file entry. rom.cpp calls wrap when it saves a 3DS ROM.
// Licence: GPL-2.0-or-later.
#pragma once
#include "build.h"

#include <string>

namespace n3ds {
// appends image to wrap and writes its size into the file entry; false
// with err when the wrap is damaged
bool wrap(const Bytes &wrap, const Bytes &image, Bytes &out, std::string &err);
}  // namespace n3ds
