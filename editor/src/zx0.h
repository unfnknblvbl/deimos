/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// ZX0 decompression (Einar Saukas' format, as decoded by the consoles'
// dzx0_standard): the engine files' template and wrap are packed with it
// (engine.cpp). Implemented in zx0.cpp from the format's description.
#pragma once
#include "common.h"

// decompresses one stream starting at data; *consumed = bytes of input used
// size bounds the input. Throws std::runtime_error on truncated data or an
// offset that points before the start of the output.
Bytes zx0_decompress(const u8 *data, size_t size, size_t *consumed = nullptr);
