/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// The 3DS game (the Deimos repository's 3ds/): a .3dsx (the code,
// the icon) whose romfs holds one file, deimos.sfc, a cartridge image in
// the SNES game's layout (the campaign block the editor already writes for
// the SNES). The image is the romfs's last bytes and the .3dsx's end, so
// the editor keeps what comes before it (the "wrap") and puts it back
// with a new image, its size written into the romfs's file entry.
// rom.cpp calls these when it opens or saves a .3dsx.
// Licence: GPL-2.0-or-later.
#pragma once
#include "build.h"

#include <string>

namespace n3ds {
// true when f starts with the "3DSX" magic (any .3dsx, not only this game)
bool is_3dsx(const Bytes &f);
// splits a .3dsx into everything before deimos.sfc's data (wrap) and the
// image; false with err when it has no such file at its end
bool unwrap(const Bytes &f, Bytes &wrap, Bytes &image, std::string &err);
// appends image to wrap and writes its size into the file entry; false
// with err when the wrap is damaged
bool wrap(const Bytes &wrap, const Bytes &image, Bytes &out, std::string &err);
}  // namespace n3ds
