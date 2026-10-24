/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// Doom RPG SMS editor - shared small types.
// Licence: GPL-2.0-or-later.
//
// Included by nearly every editor source file. It holds:
// - short integer type names (u8, u16, u32, s32) and Bytes, the byte
//   buffer every builder reads and writes (ROM images, blobs, files);
// - RGB, an 8-bit-per-channel colour;
// - little-endian put/get helpers for Bytes;
// - utf8_to_latin1, which turns the editor's text into the consoles' text.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// fixed-size integers, named as in the consoles' C code
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;

// a growable byte buffer: a whole ROM, a blob, a file read into memory
typedef std::vector<u8> Bytes;

// one colour, 0..255 per channel (pictures' palettes, previews)

struct RGB {
    u8 r = 0, g = 0, b = 0;
    bool operator==(const RGB &o) const { return r == o.r && g == o.g && b == o.b; }
    bool operator!=(const RGB &o) const { return !(*this == o); }
};

// Little-endian helpers. put16/put32 append the low 16 or 32 bits of v to
// b, low byte first. get16/get32 read the same back from p (which must
// have 2 or 4 readable bytes).
inline void put16(Bytes &b, unsigned v) { b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF); }
inline void put32(Bytes &b, u32 v) { for (int i = 0; i < 4; i++) b.push_back((v >> (8 * i)) & 0xFF); }
inline unsigned get16(const u8 *p) { return p[0] | (p[1] << 8); }
inline u32 get32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

// UTF-8 (the editor's text) -> Latin-1 (what the consoles' fonts and the
// original game use); other characters become '?'
// Two-byte UTF-8 sequences (lead byte 110xxxxx) decode to U+0080..U+07FF;
// those below 256 are Latin-1 and are kept. Longer sequences, stray
// continuation bytes and a lead byte at the very end each become one '?',
// and the continuation bytes (10xxxxxx) that follow are skipped.
inline std::string utf8_to_latin1(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) out.push_back((char)c);   // ASCII as it is
        else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            unsigned v = ((c & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F);
            out.push_back(v < 256 ? (char)v : '?');
            i++;
        } else {
            out.push_back('?');
            while (i + 1 < s.size() && ((unsigned char)s[i + 1] & 0xC0) == 0x80) i++;
        }
    }
    return out;
}

// true when the cartridge header's game title at p is one of ours: "DEIMOS"
// (from 2026-10, every console) or "DOOM RPG" (the ROMs made before the
// project was called Deimos)
inline bool game_title(const u8 *p)
{
    return memcmp(p, "DEIMOS", 6) == 0 || memcmp(p, "DOOM RPG", 8) == 0;
}
