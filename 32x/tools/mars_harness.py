#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Headless PicoDrive (libretro, built with tools' memory patch: ids 5 SDRAM,
6 frame buffers, 7 the 32X's registers) for the 32X tests: the Mega Drive game's
md_harness driver with the 32X's memories.

Used by the 32X test runs to read the SH-2's SDRAM (the game state, the
script mode's results) and the COMM registers. The paths of the Mega Drive game's
tools and of the PicoDrive core are fixed to the build machine's.
Licence: GPL-2.0-or-later.
"""
import ctypes
import os
import sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]   # the repository
sys.path.insert(0, str(ROOT / "megadrive" / "tools"))   # (the Mega Drive's harness, which this one extends)
import md_harness as H  # noqa: E402

CORE = os.environ.get("MARS_CORE", str(ROOT / "emu" / "picodrive/picodrive_libretro.so"))   # the libretro core: $MARS_CORE, else emu/ in the repository
# the core's options: a 6-button pad, a US console, no audio filter, the SH-2 recompiler on
H.VARIABLES.update({b"picodrive_input1": b"6 button pad", b"picodrive_region": b"US",
                    b"picodrive_audio_filter": b"disabled", b"picodrive_drc": b"enabled"})


class Mars(H.MegaDrive):
    """A PicoDrive instance running a 32X cartridge, with access to the 32X's memories."""

    def __init__(self, rom):
        super().__init__(rom, CORE)

    def _mem(self, kind, n):
        """The first n bytes of the core's memory area kind (5, 6 or 7 above), or None."""
        p = self.lib.retro_get_memory_data(kind)
        return bytearray(ctypes.string_at(p, n)) if p else None

    def sdram(self, addr, n):
        """SDRAM bytes as the SH-2 sees them (big-endian), addr from 0x06000000"""
        a = addr & 0x3FFFF          # (the cache-through and cached views are the same memory)
        lo = a & ~1
        raw = self._mem(5, 0x40000)
        if raw is None:
            return b""
        b = bytearray()
        # PicoDrive keeps SDRAM as little-endian 16-bit words: swap each pair
        for i in range(lo, min(0x40000, a + n + 1), 2):
            b += bytes([raw[i + 1], raw[i]])
        return bytes(b[a - lo:a - lo + n])

    def comm(self):
        """The 8 COMM words (offsets $20..$2F of the 32X's registers), or None."""
        r = self._mem(7, 0x40)
        return [r[0x20 + 2 * i] | (r[0x21 + 2 * i] << 8) for i in range(8)] if r else None
