// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Licence: GPL-2.0-or-later.
//
// The 3DS .3dsx's wrap (see n3ds.h). The parts of the formats
// used here (all little-endian):
//   .3dsx header: u16 header size at 4 (44 with the extended header),
//                 u32 romfs offset at 40;
//   romfs (level 3) header, 0x28 bytes: u32 header size (0x28) at 0,
//                 file metadata table offset at 28, its size at 32, file
//                 data offset at 36 (all from the romfs start);
//   file entry:   u64 data offset at 8, u64 data size at 16, u32 name
//                 length in bytes at 28, the UTF-16 name at 32, padded to
//                 4 bytes.
#include "n3ds.h"

#include <cstring>

namespace n3ds {

// read / write a little-endian u32 in place
static u32 le32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static void put32(u8 *p, u32 v)
{
    for (int i = 0; i < 4; i++) p[i] = (u8)(v >> (8 * i));
}

// true when f starts with the "3DSX" magic and has room for the extended
// header (44 bytes)
static bool is_3dsx(const Bytes &f) { return f.size() >= 44 && memcmp(f.data(), "3DSX", 4) == 0; }

// the romfs's deimos.sfc: where its entry is (file offset) and where its data starts
// Walks the romfs's file metadata table for an entry named deimos.sfc.
// Fails (with err) unless the file's data ends exactly at the end of f,
// since the editor only knows how to replace a trailing file.
static bool find(const Bytes &f, size_t &entry, size_t &data, std::string &err)
{
    err = "this .3dsx is not Doom RPG for the 3DS (no deimos.sfc in its romfs)";
    if (!is_3dsx(f) || f[4] < 44) return false;   // needs the extended header
    size_t fs = le32(&f[40]);                       // the romfs's offset
    if (fs == 0 || fs + 40 > f.size() || le32(&f[fs]) != 0x28) return false;
    // ft: the file table, ftn: its size, fd: where the files' data start
    size_t ft = fs + le32(&f[fs + 28]), ftn = le32(&f[fs + 32]), fd = fs + le32(&f[fs + 36]);
    static const char name[] = "deimos.sfc";
    for (size_t p = ft; p + 32 <= ft + ftn && p + 32 <= f.size();) {
        u32 nl = le32(&f[p + 28]);                  // name length in bytes
        if (p + 32 + nl > f.size()) return false;
        // compare the UTF-16LE name with the ASCII one
        bool same = nl == 2 * (sizeof name - 1);
        for (size_t i = 0; same && i < sizeof name - 1; i++)
            same = f[p + 32 + 2 * i] == (u8)name[i] && f[p + 33 + 2 * i] == 0;
        if (same) {
            // the 64-bit offset and size (their high halves must be 0)
            u32 off_hi = le32(&f[p + 12]), size_hi = le32(&f[p + 20]);
            size_t off = le32(&f[p + 8]), size = le32(&f[p + 16]);
            if (off_hi || size_hi || fd + off + size != f.size()) {
                err = "this .3dsx's deimos.sfc is not at its end (made by another tool?)";
                return false;
            }
            entry = p;
            data = fd + off;
            return true;
        }
        p += 32 + ((nl + 3) & ~3u);   // next entry: the name padded to 4 bytes
    }
    return false;
}

// see n3ds.h. Writes the size into the file table's first entry: the
// romfs is expected to hold deimos.sfc only, and find() then checks that
// this entry is the one whose data ends the file.
bool wrap(const Bytes &w, const Bytes &image, Bytes &out, std::string &err)
{
    Bytes f = w;
    f.insert(f.end(), image.begin(), image.end());
    size_t e, d;
    // (find checks the size, so the entry's is written first: the wrap's own end is where the image starts)
    size_t fs = w.size() >= 44 ? le32(&w[40]) : 0;
    if (!is_3dsx(w) || fs + 40 > w.size()) {
        err = "the 3DS game's .3dsx is damaged";
        return false;
    }
    {
        size_t ft = fs + le32(&w[fs + 28]);
        if (ft + 32 > f.size()) {
            err = "the 3DS game's .3dsx is damaged";
            return false;
        }
        put32(&f[ft + 16], (u32)image.size());   // the entry's data size, low half
        put32(&f[ft + 20], 0);                    // and high half
    }
    // check the result: the file must be found and start where the wrap ends
    if (!find(f, e, d, err) || d != w.size()) {
        if (err.empty()) err = "the 3DS game's .3dsx is damaged";
        return false;
    }
    out = std::move(f);
    return true;
}

}  // namespace n3ds
