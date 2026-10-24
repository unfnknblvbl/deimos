/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// ZX0 compression (Einar Saukas' format, as decoded by the ROM's dzx0_standard).
// Used for the source container's entries (container.cpp) and the other
// packed data the builders write. Implemented in zx0.cpp on top of the
// reference compressor in third_party/zx0.
#pragma once
#include "common.h"

// quick = smaller search window (much faster, slightly bigger output)
// Returns one ZX0 stream, end marker included. An empty input is
// compressed as one zero byte.
Bytes zx0_compress(const Bytes &in, bool quick = false);
// decompresses one stream starting at data; *consumed = bytes of input used
// size bounds the input. Throws std::runtime_error on truncated data or an
// offset that points before the start of the output.
Bytes zx0_decompress(const u8 *data, size_t size, size_t *consumed = nullptr);
