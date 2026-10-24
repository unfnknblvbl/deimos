// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// The Virtual Boy cartridge's wrap (see vb.h for the layout). Called by
// rom.cpp when a Virtual Boy ROM is saved.
// Licence: GPL-2.0-or-later.
#include "vb.h"

namespace vb {

// see vb.h
bool wrap(const Bytes &w, const Bytes &image, Bytes &out, std::string &err)
{
    if (w.size() < TAIL || w.size() - TAIL > DATA_AT) {   // (the tail, then at most 512 KB of code)
        err = "the Virtual Boy game's code is damaged";
        return false;
    }
    size_t n = image.size();
    while (n && image[n - 1] == 0xFF) n--;   // n = the image without its $FF padding
    if (n > 0x400000) {
        err = "the maps don't fit (the SNES image the Virtual Boy reads is at most 4 MB)";
        return false;
    }
    out.assign(SIZE, 0xFF);
    std::copy(w.begin() + TAIL, w.end(), out.begin());                       // the code at $000000
    std::copy(image.begin(), image.begin() + (long)n, out.begin() + DATA_AT);  // the SNES image at $080000
    std::copy(w.begin(), w.begin() + TAIL, out.end() - TAIL);                  // the header and vectors at the end
    return true;
}

}  // namespace vb
