#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Headless Gambatte (libretro) for the Game Boy Color tests: run frames,
press buttons, read work RAM (all 8 banks) and the cartridge RAM, take
screenshots, record the sound - the Mega Drive game's md_harness driver with
Gambatte's buttons and memories.

Buttons: up down left right a b select start.

It reuses megadrive/tools/md_harness.py (class MegaDrive: the libretro
loading, frames, screenshots, sound) with Gambatte's core, its options and
the Game Boy's buttons; the paths are the build machine's.
Licence: GPL-2.0-or-later.
"""
import ctypes
import os
import sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]   # the repository
sys.path.insert(0, str(ROOT / "megadrive" / "tools"))   # (the Mega Drive's harness, which this one extends)
import md_harness as H  # noqa: E402

CORE = os.environ.get("GB_CORE", str(ROOT / "emu" / "gambatte/gambatte_libretro.so"))   # the libretro core: $GB_CORE, else emu/ in the repository
# Gambatte's options: a Game Boy Color, its colours unchanged, no boot ROM,
# no frame blending
H.VARIABLES.clear()
H.VARIABLES.update({b"gambatte_gb_hwmode": b"GBC", b"gambatte_gbc_color_correction": b"disabled",
                    b"gambatte_gb_bootloader": b"disabled", b"gambatte_mix_frames": b"disabled"})
# libretro joypad ids: B=0, Y=1, SELECT=2, START=3, UP=4, DOWN=5, LEFT=6, RIGHT=7, A=8
H.IDS = {"b": 0, "select": 2, "start": 3, "up": 4, "down": 5, "left": 6, "right": 7, "a": 8}


class GBC(H.MegaDrive):
    """Gambatte running rom: md_harness's driver with the Game Boy's memories."""

    def __init__(self, rom):
        self._dir = ctypes.create_string_buffer(b"/tmp")  # (the system and save directory the core asks for)
        super().__init__(rom, CORE)

    def _env(self, cmd, data):
        """libretro environment calls: frame duping allowed, directories given."""
        if cmd == 3:                                    # CAN_DUPE
            ctypes.cast(data, ctypes.POINTER(ctypes.c_bool))[0] = True
            return True
        if cmd in (9, 31):                              # the system / save directory
            ctypes.cast(data, ctypes.POINTER(ctypes.c_char_p))[0] = ctypes.cast(self._dir, ctypes.c_char_p).value
            return True
        return super()._env(cmd, data)

    def set_pad(self, buttons):
        """Nothing: the buttons go through md_harness's input callback."""
        pass

    def wram(self, addr, n):
        """work RAM: addr C000-CFFF (bank 0) or bank * 0x1000 + offset (bank 1..7: D000 + ...)"""
        p = self.lib.retro_get_memory_data(2)
        return ctypes.string_at(p + addr, n)

    def wram_at(self, gbaddr, n, bank=1):
        """n bytes of work RAM at Game Boy address gbaddr ($D000.. in bank bank)."""
        if gbaddr < 0xD000:
            return self.wram(gbaddr - 0xC000, n)
        return self.wram(bank * 0x1000 + gbaddr - 0xD000, n)

    def sram_bytes(self, off, n):
        """n bytes of the cartridge RAM (all banks, 8 KB each) from off."""
        p = self.lib.retro_get_memory_data(0)
        return ctypes.string_at(p + off, n)


def _vram(self, bank, addr, n):
    """video RAM (both banks: Gambatte's libretro memory 3 when it has it)"""
    p = self.lib.retro_get_memory_data(3)
    size = self.lib.retro_get_memory_size(3)
    if not p or size < 0x4000:
        raise RuntimeError(f"no video RAM from the core ({size})")
    return ctypes.string_at(p + bank * 0x2000 + addr - 0x8000, n)


GBC.vram = _vram                                        # (vram(bank, address, n))
