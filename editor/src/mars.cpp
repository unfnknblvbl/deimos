// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Licence: GPL-2.0-or-later.
//
// The 32X cartridge's wrap and unwrap (see mars.h for the layout). Called
// by rom.cpp.
#include "mars.h"

#include <cstring>

namespace mars {

// see mars.h
bool is_32x(const Bytes &f)
{
    return f.size() > DATA_AT && memcmp(&f[0x100], "SEGA 32X", 8) == 0 && memcmp(&f[0x3C0], "MARS CHECK MODE", 15) == 0;
}

// see mars.h
bool unwrap(const Bytes &f, Bytes &w, Bytes &image, std::string &err)
{
    if (!is_32x(f)) {
        err = "this is not a 32X cartridge";
        return false;
    }
    w.assign(f.begin(), f.begin() + DATA_AT);
    image.assign(f.begin() + DATA_AT, f.end());
    size_t size = 0x40000;              // (as the SNES build made it: a power of two, the rest $FF)
    while (size < image.size()) size *= 2;
    image.resize(size, 0xFF);
    return true;
}

// see mars.h
bool wrap(const Bytes &w, const Bytes &image, Bytes &out, std::string &err)
{
    size_t n = image.size(), size = 0x80000;
    while (n && image[n - 1] == 0xFF) n--;   // n = the image without its $FF padding
    // (w: the code, up to 512 KB; the engine keeps it without its $FF
    // padding, which goes back here)
    if (w.size() > DATA_AT || w.size() < 0x3D0 || memcmp(&w[0x3C0], "MARS CHECK MODE", 15) != 0) {
        err = "the 32X game's code is damaged";
        return false;
    }
    while (size < DATA_AT + n) size *= 2;    // the cartridge: a power of two
    if (size > 0x400000) {
        err = "the maps don't fit in a 32X cartridge (4 MB)";
        return false;
    }
    out = w;
    out.resize(DATA_AT, 0xFF);   // the code, padded with $FF to 512 KB
    out.insert(out.end(), image.begin(), image.begin() + (long)n);
    out.resize(size, 0xFF);
    // Mega Drive header: $1A4-$1A7 = the ROM's last address (big-endian)
    out[0x1A4] = (u8)((size - 1) >> 24);
    out[0x1A5] = (u8)((size - 1) >> 16);
    out[0x1A6] = (u8)((size - 1) >> 8);
    out[0x1A7] = (u8)(size - 1);
    // $18E-$18F = the checksum: the 16-bit sum of the big-endian words
    // from $200 to the end
    unsigned sum = 0;
    for (size_t i = 0x200; i + 1 < size; i += 2) sum += (unsigned)(out[i] << 8 | out[i + 1]);
    out[0x18E] = (u8)(sum >> 8);
    out[0x18F] = (u8)sum;
    return true;
}

}  // namespace mars
