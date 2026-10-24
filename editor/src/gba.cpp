// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Licence: GPL-2.0-or-later.
//
// The Game Boy Advance cartridge's wrap and unwrap (see gba.h for the
// layout). Called by rom.cpp.
#include "gba.h"

#include <cstring>

namespace gba {

// true when f starts with this game's GBA header: the game title
// "DOOM RPG ADV" at $A0 and the fixed value $96 at $B2
static bool header_ok(const Bytes &f)
{
    return f.size() >= 0xC0 && (memcmp(&f[0xA0], "DEIMOS      ", 12) == 0 || memcmp(&f[0xA0], "DOOM RPG ADV", 12) == 0) && f[0xB2] == 0x96;
}

// a cartridge: the header, and something after the code
bool is_gba(const Bytes &f) { return f.size() > DATA_AT && header_ok(f); }

// see gba.h
bool unwrap(const Bytes &f, Bytes &w, Bytes &image, std::string &err)
{
    if (!is_gba(f)) {
        err = "this is not Doom RPG for the Game Boy Advance";
        return false;
    }
    w.assign(f.begin(), f.begin() + DATA_AT);
    image.assign(f.begin() + DATA_AT, f.end());
    size_t size = 0x40000;              // (as the SNES build made it: a power of two, the rest $FF)
    while (size < image.size()) size *= 2;
    if (size > 0x400000) size = 0x400000;   // (an image over 4 MB is cut)
    image.resize(size, 0xFF);
    return true;
}

// see gba.h
bool wrap(const Bytes &w, const Bytes &image, Bytes &out, std::string &err)
{
    size_t n = image.size();
    while (n && image[n - 1] == 0xFF) n--;   // n = the image without its $FF padding
    if (w.size() > DATA_AT || !header_ok(w)) {
        err = "the Game Boy Advance game's code is damaged";
        return false;
    }
    if (n > 0x400000) {
        err = "the maps don't fit (the SNES image the GBA reads is at most 4 MB)";
        return false;
    }
    out = w;
    out.resize(DATA_AT, 0xFF);   // the code, padded with $FF to 512 KB
    out.insert(out.end(), image.begin(), image.begin() + (long)n);
    out.resize((out.size() + 0xFFFF) & ~(size_t)0xFFFF, 0xFF);   // round up to 64 KB
    return true;
}

}  // namespace gba
