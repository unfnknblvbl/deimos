#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Headless mGBA (libretro) for the Game Boy Advance tests: run frames, press
buttons, read the memories at their GBA addresses (the core's memory map),
take screenshots, record the sound - the Mega Drive game's md_harness driver with
mGBA's buttons and memories.

Buttons: up down left right a b l r select start.

Used by gba_lockstep.py. The paths of megadrive/tools and of the mGBA
core are fixed to the build machine's. The core reports its memory map
through the libretro SET_MEMORY_MAPS call; GBA.mem and GBA.poke_mem look
addresses up in it.
Licence: GPL-2.0-or-later.
"""
import ctypes
import struct
import os
import sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]   # the repository
sys.path.insert(0, str(ROOT / "megadrive" / "tools"))   # (the Mega Drive's harness, which this one extends)
import md_harness as H  # noqa: E402

CORE = os.environ.get("GBA_CORE", str(ROOT / "emu" / "mgba/mgba_libretro.so"))   # the libretro core: $GBA_CORE, else emu/ in the repository
# the core's options: no BIOS (its start skipped), no frame skipping, the
# raw colours and sound, idle loops left alone (so timing matches)
H.VARIABLES.clear()
H.VARIABLES.update({b"mgba_skip_bios": b"ON", b"mgba_use_bios": b"OFF", b"mgba_frameskip": b"0",
                    b"mgba_color_correction": b"OFF", b"mgba_interframe_blending": b"OFF",
                    b"mgba_audio_low_pass_filter": b"disabled", b"mgba_idle_optimization": b"Don't Remove Known"})
# button name -> libretro joypad id
H.IDS = {"b": 0, "select": 2, "start": 3, "up": 4, "down": 5, "left": 6, "right": 7, "a": 8, "l": 10, "r": 11}


class Desc(ctypes.Structure):
    """libretro's retro_memory_descriptor: one region of the core's memory map."""
    _fields_ = [("flags", ctypes.c_uint64), ("ptr", ctypes.c_void_p), ("offset", ctypes.c_size_t),
                ("start", ctypes.c_size_t), ("select", ctypes.c_size_t), ("disconnect", ctypes.c_size_t),
                ("len", ctypes.c_size_t), ("addrspace", ctypes.c_char_p)]


class MemMap(ctypes.Structure):
    """libretro's retro_memory_map: the list of descriptors."""
    _fields_ = [("descriptors", ctypes.POINTER(Desc)), ("num_descriptors", ctypes.c_uint)]


class GBA(H.MegaDrive):
    """An mGBA instance running rom; syms (an nm listing) names the game's variables."""

    def __init__(self, rom, syms=None):
        self._dir = ctypes.create_string_buffer(b"/tmp")   # the system and save directory
        self.regions = []           # (GBA address, length, host pointer) from the memory map
        super().__init__(rom, CORE)
        self.sym = {}
        if syms:
            for line in Path(syms).read_text().splitlines():
                p = line.split()
                if len(p) >= 3:
                    self.sym[p[2]] = int(p[0], 16)

    def _env(self, cmd, data):
        """The libretro environment callback: the calls mGBA needs, the rest to md_harness."""
        if cmd == 3:                                    # CAN_DUPE
            ctypes.cast(data, ctypes.POINTER(ctypes.c_bool))[0] = True
            return True
        if cmd in (9, 31):                              # the system / save directory
            ctypes.cast(data, ctypes.POINTER(ctypes.c_char_p))[0] = ctypes.cast(self._dir, ctypes.c_char_p).value
            return True
        if cmd & 0xFFFF == 36:                          # SET_MEMORY_MAPS (experimental)
            m = ctypes.cast(data, ctypes.POINTER(MemMap)).contents
            for i in range(m.num_descriptors):
                d = m.descriptors[i]
                if d.ptr and d.len:
                    self.regions.append((d.start, d.len, d.ptr + d.offset))
            return True
        return super()._env(cmd, data)

    def set_pad(self, buttons):
        """Does nothing: an override of md_harness's Mega Drive version."""
        pass

    def mem(self, addr, n):
        """n bytes at GBA address addr (within one region of the memory map)."""
        for start, ln, ptr in self.regions:
            if start <= addr and addr + n <= start + ln:
                return ctypes.string_at(ptr + addr - start, n)
        raise KeyError(f"no memory at {addr:08X}")

    def poke_mem(self, addr, data):
        """Write data at GBA address addr."""
        for start, ln, ptr in self.regions:
            if start <= addr and addr + len(data) <= start + ln:
                ctypes.memmove(ptr + addr - start, bytes(data), len(data))
                return
        raise KeyError(f"no memory at {addr:08X}")

    def u32(self, name):
        """The little-endian u32 variable called name."""
        return struct.unpack("<I", self.mem(self.sym[name], 4))[0]

    def u8(self, name):
        """The byte variable called name."""
        return self.mem(self.sym[name], 1)[0]
