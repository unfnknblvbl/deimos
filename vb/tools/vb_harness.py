#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Headless Mednafen Virtual Boy (Beetle VB, libretro) for the Virtual Boy
tests: run frames, press buttons, read and write the work RAM, take
screenshots of both eyes - the Mega Drive game's md_harness driver with
the Virtual Boy's buttons, memories and 3D output.

    vb = VB("out/deimos_vb.vb", "obj/vb.syms")
    vb.run_frames(50, {"a"})            # hold A for 50 frames (one second)
    vb.u32("script_states")             # a variable of the game, by name
    left, right = vb.eyes()             # the last frame, each eye 384 x 224

Buttons: up down left right (the left D-pad), a b l r select start, and
the right D-pad: rup rdown rleft rright.

The core: $VB_CORE, else emu/beetle-vb/mednafen_vb_libretro.so in the
repository (git-ignored). Its options are set to show the two eyes side by
side with no gap (so a screenshot holds both: the left eye's 384 columns,
then the right's), in black and red, with the accurate CPU emulation
(timings as the hardware's; VB_CPU=fast for the faster one). The work
RAM ($05000000, 64 KB) is the core's SYSTEM_RAM; the battery RAM its SAVE_RAM (64 KB: the game's byte i
at 2 i). Symbols come from an nm listing of the game (v810-gcc's names
start with an underscore, dropped here).
Licence: GPL-2.0-or-later.
"""
import ctypes
import os
import struct
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]   # the repository
sys.path.insert(0, str(ROOT / "megadrive" / "tools"))   # (the Mega Drive's harness, which this one extends)
import md_harness as H  # noqa: E402

CORE = os.environ.get("VB_CORE", str(ROOT / "emu" / "beetle-vb" / "mednafen_vb_libretro.so"))
# the core's options: the two eyes side by side, no gap, red on black, the
# accurate CPU (VB_CPU=fast: the faster emulation, for long runs), no VR presentation
H.VARIABLES.clear()
H.VARIABLES.update({b"vb_3dmode": b"side-by-side", b"vb_sidebyside_separation": b"0",
                    b"vb_anaglyph_preset": b"disabled", b"vb_color_mode": b"black & red",
                    b"vb_cpu_emulation": os.environ.get("VB_CPU", "accurate").encode(), b"vb_vr": b"disabled",
                    b"vb_right_analog_to_digital": b"disabled", b"vb_opposite_directions": b"disabled"})
# button name -> libretro joypad id (the core's mapping: the right D-pad on L2 / R2 / L3 / R3)
H.IDS = {"b": 0, "select": 2, "start": 3, "up": 4, "down": 5, "left": 6, "right": 7, "a": 8,
         "l": 10, "r": 11, "rup": 12, "rleft": 13, "rdown": 14, "rright": 15}
WRAM = 0x05000000                       # the work RAM's address
EYE_W, EYE_H = 384, 224                 # one eye's picture
# the shades' red level in the screenshots (BRTA 32, BRTB 64, BRTC 32: 1/4, 1/2 and all of
# the light, which the core shows through its gamma as these)
SHADE_RED = (0, 135, 185, 254)
# libretro's log callback, void log(level, fmt, ...): the arguments after fmt are not read
LOG_CB = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_char_p)


class VB(H.MegaDrive):
    """A Beetle VB instance running rom; syms (an nm listing) names the game's variables."""

    def __init__(self, rom, syms=None):
        """Load the core and the ROM (md_harness's), then read the symbols."""
        self._dir = ctypes.create_string_buffer(b"/tmp")   # the system and save directory
        self._log_cb = LOG_CB(lambda level, fmt: None)   # (the core's log lines: ignored; it needs a logger)
        super().__init__(rom, CORE)
        self.sym = {}
        if syms:
            for line in Path(syms).read_text().splitlines():
                p = line.split()
                if len(p) >= 3:
                    self.sym[p[2][1:] if p[2].startswith("_") else p[2]] = int(p[0], 16)

    def _env(self, cmd, data):
        """The libretro environment callback: the calls the core needs, the rest to md_harness."""
        if cmd == 3:                                    # CAN_DUPE
            ctypes.cast(data, ctypes.POINTER(ctypes.c_bool))[0] = True
            return True
        if cmd in (9, 31):                              # the system / save directory
            ctypes.cast(data, ctypes.POINTER(ctypes.c_char_p))[0] = ctypes.cast(self._dir, ctypes.c_char_p).value
            return True
        if cmd & 0xFFFF == 51:                          # GET_INPUT_BITMASKS: yes (md_harness answers masks)
            return True
        if cmd == 27:                                   # GET_LOG_INTERFACE: a logger that drops the lines
            ctypes.cast(data, ctypes.POINTER(ctypes.c_void_p))[0] = ctypes.cast(self._log_cb, ctypes.c_void_p).value
            return True
        return super()._env(cmd, data)

    def set_pad(self, buttons):
        """Does nothing: an override of md_harness's Mega Drive version."""
        pass

    def wram(self):
        """A ctypes pointer to the core's 64 KB of work RAM."""
        return self.lib.retro_get_memory_data(H.RETRO_MEMORY_SYSTEM_RAM)

    def mem(self, addr, n):
        """n bytes of the work RAM at Virtual Boy address addr."""
        o = addr - WRAM
        if o < 0 or o + n > 0x10000:
            raise KeyError(f"not in the work RAM: {addr:08X}")
        return ctypes.string_at(self.wram() + o, n)

    def poke_mem(self, addr, data):
        """Write data into the work RAM at Virtual Boy address addr."""
        o = addr - WRAM
        if o < 0 or o + len(data) > 0x10000:
            raise KeyError(f"not in the work RAM: {addr:08X}")
        ctypes.memmove(self.wram() + o, bytes(data), len(data))

    def u32(self, name):
        """The little-endian u32 variable called name."""
        return struct.unpack("<I", self.mem(self.sym[name], 4))[0]

    def u16(self, name):
        """The little-endian u16 variable called name."""
        return struct.unpack("<H", self.mem(self.sym[name], 2))[0]

    def u8(self, name):
        """The byte variable called name."""
        return self.mem(self.sym[name], 1)[0]

    def eyes(self):
        """The last frame as two arrays of shades (0..3), (224, 384) each:
        the left eye, the right eye (the side-by-side picture cut in two;
        each pixel's red level mapped back to its shade)."""
        img = np.array(self.screenshot())[:, :, 0].astype(int)
        near = np.abs(np.arange(256)[:, None] - np.array(SHADE_RED)[None, :]).argmin(1)
        sh = near[img]
        return sh[:EYE_H, :EYE_W], sh[:EYE_H, EYE_W:2 * EYE_W]
