// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// ZX0 (Einar Saukas' format): decompression, written from the format's
// description in the reference decoder.
// Licence of this file: GPL-2.0-or-later.
//
// The ZX0 stream, as read by zx0_decompress below: a sequence of blocks,
// each chosen by one control bit:
//   literals     Elias gamma length, then that many raw bytes;
//   last offset  gamma length, copy from the same offset as before (only
//                allowed right after literals);
//   new offset   gamma of the offset's high part (bits inverted), then a
//                byte with its low 7 bits in bits 7..1; that byte's bit 0
//                is the first bit of the following length (the "backtrack"
//                below); gamma length + 1. A high part of 256 ends the
//                stream.
// Bits are read MSB first from bytes taken as they are needed, interleaved
// with the literal and offset bytes.
#include "zx0.h"

#include <stdexcept>

namespace {
// Reads a ZX0 stream: bytes, single bits and Elias gamma numbers.
struct Reader {
    const u8 *p, *end;        // next input byte, end of the input
    // bit_mask: the bit of bit_value to read next (0 = fetch a new byte);
    // last: the last byte read by byte(); backtrack: the next bit() is
    // bit 0 of `last` (ZX0 hides a length's first bit in an offset byte)
    int bit_mask = 0, bit_value = 0, last = 0;
    bool backtrack = false;
    // the next input byte; throws when the input runs out
    int byte()
    {
        if (p >= end) throw std::runtime_error("zx0: truncated data");
        return last = *p++;
    }
    // the next control bit, MSB first
    int bit()
    {
        if (backtrack) {
            backtrack = false;
            return last & 1;
        }
        bit_mask >>= 1;
        if (!bit_mask) {
            bit_mask = 128;
            bit_value = byte();
        }
        return (bit_value & bit_mask) ? 1 : 0;
    }
    // an interlaced Elias gamma number (>= 1): while a 0 "continue" bit
    // is read, one more data bit follows (XORed with inverted, which is 1
    // for an offset's high part)
    int gamma(int inverted)
    {
        int v = 1;
        while (!bit()) v = (v << 1) | (bit() ^ inverted);
        return v;
    }
};
}  // namespace

// Decodes one ZX0 stream (see zx0.h). A direct transcription of the
// reference decoder's three states, with the gotos kept: literals, a copy
// from the last offset, a copy from a new offset.
Bytes zx0_decompress(const u8 *data, size_t size, size_t *consumed)
{
    Reader r{data, data + size};
    Bytes out;
    int last_offset = 1, length;   // the format starts with offset 1
    // copy len bytes from off bytes back in the output (they may overlap)
    auto copy = [&](int off, int len) {
        if (off > (int)out.size()) throw std::runtime_error("zx0: bad offset");
        while (len-- > 0) out.push_back(out[out.size() - off]);
    };
literals:   // a run of raw bytes
    length = r.gamma(0);
    for (int i = 0; i < length; i++) out.push_back((u8)r.byte());
    if (r.bit()) goto new_offset;   // 1: a copy from a new offset follows
    // 0: a copy from the last offset
    length = r.gamma(0);
    copy(last_offset, length);
    if (!r.bit()) goto literals;    // then 0: literals, 1: new offset
new_offset:
    last_offset = r.gamma(1);       // the offset's high part, + 1
    if (last_offset == 256) {       // the end marker
        if (consumed) *consumed = r.p - data;
        return out;
    }
    // offset = high * 128 - (low byte's bits 7..1); the low byte's bit 0
    // is the first bit of the length
    last_offset = last_offset * 128 - (r.byte() >> 1);
    r.backtrack = true;
    length = r.gamma(0) + 1;        // a new-offset copy is at least 2 bytes
    copy(last_offset, length);
    if (r.bit()) goto new_offset;   // 1: another new offset, 0: literals
    goto literals;
}
