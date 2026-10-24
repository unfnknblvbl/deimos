#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Headless driver for the Gearsystem libretro core (drhelius/Gearsystem,
GPL-3.0) via ctypes - a real, accurate SMS emulator with proper VDP timing
and line interrupts, replacing the toy z80_min/sms_machine pair that hid
real defects (it ignored display-enable, sampled the screen at arbitrary
instruction counts, and never ran a real frame).

Usage as a library:
    emu = Gearsystem(".../gearsystem_libretro.so", "rom.sms")
    emu.run_frames(60)                       # ~1 second
    emu.run_frames(2, buttons={"a"})         # hold button for 2 frames
    img = emu.screenshot()                   # PIL RGB image 256x192
    ram = emu.ram()                          # bytes, 8KB at $C000

Button names: up down left right a b (a = button 1 / fire).
Also "1" and "fire" (button 1), "2" and "b" (button 2) and "start" (the
core's Start: the Master System pause button / the Game Gear's Start).

How it works: the core is a shared library with the libretro C API. This
class registers Python callbacks for video, audio, input and environment
queries, loads the ROM from memory and steps the core one frame per
retro_run(). The video callback keeps the last frame; the input callback
reports the buttons in self.pressed. regs(), pc_trace() and the watch
functions call retro_dbg_* entry points that only exist in a core built
with tools/gearsystem-debug.patch (README).

Used by test_lockstep.py (its Rom class) and through it by every tool that
plays the ROM. Run as a script: gearsystem_harness.py ROM OUT.png [frames]
saves a screenshot after that many frames (default 120).
"""
import ctypes
import sys
from pathlib import Path

from PIL import Image

# libretro API constants (libretro.h)
RETRO_ENVIRONMENT_SET_PIXEL_FORMAT = 10
RETRO_ENVIRONMENT_GET_VARIABLE = 15
RETRO_ENVIRONMENT_GET_LOG_INTERFACE = 27
RETRO_PIXEL_FORMAT_XRGB8888 = 1
RETRO_DEVICE_JOYPAD = 1
RETRO_MEMORY_SYSTEM_RAM = 2

# libretro joypad button ids (RETRO_DEVICE_ID_JOYPAD_*)
JOYPAD_IDS = {"b": 0, "y": 1, "select": 2, "start": 3, "up": 4, "down": 5,
              "left": 6, "right": 7, "a": 8, "x": 9}
# Gearsystem maps libretro A/B to SMS buttons; on SMS "button 1" is libretro B
# and "button 2" is libretro A (RETRO_DEVICE_ID_JOYPAD_B = 0, _A = 8). We
# accept both and let callers use 'a' meaning SMS button 1 for readability.
BUTTON_ALIASES = {"a": "b", "fire": "b", "1": "b", "2": "a", "b": "a"}


class retro_game_info(ctypes.Structure):
    """libretro's struct retro_game_info: the ROM handed to retro_load_game."""

    _fields_ = [("path", ctypes.c_char_p), ("data", ctypes.c_void_p),
                ("size", ctypes.c_size_t), ("meta", ctypes.c_char_p)]


class retro_system_av_info(ctypes.Structure):
    """libretro's struct retro_system_av_info (geometry and timing flattened into one)."""

    _fields_ = [("base_width", ctypes.c_uint), ("base_height", ctypes.c_uint),
                ("max_width", ctypes.c_uint), ("max_height", ctypes.c_uint),
                ("aspect_ratio", ctypes.c_float), ("fps", ctypes.c_double),
                ("sample_rate", ctypes.c_double)]


# C function pointer types of the libretro callbacks
ENV_CB = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_uint, ctypes.c_void_p)
VIDEO_CB = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_size_t)
AUDIO_SAMPLE_CB = ctypes.CFUNCTYPE(None, ctypes.c_int16, ctypes.c_int16)
AUDIO_BATCH_CB = ctypes.CFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t)
INPUT_POLL_CB = ctypes.CFUNCTYPE(None)
INPUT_STATE_CB = ctypes.CFUNCTYPE(ctypes.c_int16, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint)


class Gearsystem:
    """One emulated console running one ROM in the Gearsystem libretro core."""

    def __init__(self, core_path, rom_path):
        """Load the core library at core_path, register the callbacks and load the ROM at rom_path.

        Raises RuntimeError if the core refuses the ROM. The callback objects
        are kept on self so Python does not free them while the core holds
        their pointers.
        """
        self.lib = ctypes.CDLL(str(core_path))
        self.frame = None
        self.pixel_format = 2              # libretro RGB565 until the core says otherwise
        self.frame_w = self.frame_h = 0
        self.pressed = set()
        self._rom = Path(rom_path).read_bytes()

        self._env_cb = ENV_CB(self._env)
        self._video_cb = VIDEO_CB(self._video)
        self._audio_cb = AUDIO_SAMPLE_CB(lambda l, r: None)
        self.audio = None                  # set to a bytearray to record (s16 stereo)
        self._audio_batch_cb = AUDIO_BATCH_CB(self._audio_batch)
        self._poll_cb = INPUT_POLL_CB(lambda: None)
        self._input_cb = INPUT_STATE_CB(self._input)

        lib = self.lib
        # hand the callbacks to the core, then initialise it
        lib.retro_set_environment(self._env_cb)
        lib.retro_set_video_refresh(self._video_cb)
        lib.retro_set_audio_sample(self._audio_cb)
        lib.retro_set_audio_sample_batch(self._audio_batch_cb)
        lib.retro_set_input_poll(self._poll_cb)
        lib.retro_set_input_state(self._input_cb)
        lib.retro_init()

        info = retro_game_info()
        self._rom_buf = ctypes.create_string_buffer(self._rom, len(self._rom))
        info.path = str(rom_path).encode()
        info.data = ctypes.cast(self._rom_buf, ctypes.c_void_p)
        info.size = len(self._rom)
        info.meta = None
        lib.retro_load_game.restype = ctypes.c_bool
        if not lib.retro_load_game(ctypes.byref(info)):
            raise RuntimeError("retro_load_game failed")
        # pointer and size results must not be truncated to C int
        lib.retro_get_memory_data.restype = ctypes.c_void_p
        lib.retro_get_memory_size.restype = ctypes.c_size_t
        self.frames_run = 0

    # ---- libretro callbacks ----
    def _env(self, cmd, data):
        """Environment callback: accept a pixel format (RGB565 or XRGB8888); refuse everything else."""
        if cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            fmt = ctypes.cast(data, ctypes.POINTER(ctypes.c_int)).contents.value
            self.pixel_format = fmt          # Gearsystem asks for RGB565 (2)
            return fmt in (RETRO_PIXEL_FORMAT_XRGB8888, 2)
        if cmd == RETRO_ENVIRONMENT_GET_VARIABLE:
            return False
        return False

    def _audio_batch(self, data, frames):
        """Audio callback: append frames stereo s16 samples (4 bytes each) to self.audio if recording."""
        if self.audio is not None and data:
            self.audio += ctypes.string_at(data, frames * 4)
        return frames

    def sample_rate(self):
        """The core's audio sample rate in Hz."""
        av = retro_system_av_info()
        self.lib.retro_get_system_av_info(ctypes.byref(av))
        return av.sample_rate

    def _video(self, data, width, height, pitch):
        """Video callback: copy the frame (data is NULL when the core repeats the last one)."""
        if data:
            n = pitch * height
            self.frame = ctypes.string_at(data, n)
            self.frame_w, self.frame_h, self.frame_pitch = width, height, pitch

    def _input(self, port, device, index, id_):
        """Input callback: 1 if button id_ of joypad 0 is in self.pressed."""
        if port != 0 or device != RETRO_DEVICE_JOYPAD:
            return 0
        for name in self.pressed:
            if JOYPAD_IDS.get(name) == id_:
                return 1
        return 0

    # ---- public API ----
    def run_frames(self, n, buttons=()):
        """Run n frames with buttons (names or aliases) held, then release them."""
        self.pressed = {BUTTON_ALIASES.get(b, b) for b in buttons}
        for _ in range(n):
            self.lib.retro_run()
            self.frames_run += 1
        self.pressed = set()

    def press(self, button, hold=2, release=2):
        """Edge-triggered press: hold then release (frames)."""
        self.run_frames(hold, {button})
        self.run_frames(release)

    def screenshot(self) -> Image.Image:
        """The last frame as an RGB image (raises RuntimeError before the first frame)."""
        if self.frame is None:
            raise RuntimeError("no frame yet")
        import numpy as np
        w, h, pitch = self.frame_w, self.frame_h, self.frame_pitch
        if getattr(self, "pixel_format", 2) == 2:
            # RGB565 decoded explicitly: PIL's raw-mode names for 16-bit
            # formats were getting this wrong (one gave purple walls, the
            # other olive) - verified against the C harness / core source.
            buf = np.frombuffer(self.frame, dtype="<u2").reshape(h, pitch // 2)[:, :w]
            r = (((buf >> 11) & 31) * 255 // 31).astype("uint8")
            g = (((buf >> 5) & 63) * 255 // 63).astype("uint8")
            b = ((buf & 31) * 255 // 31).astype("uint8")
            return Image.fromarray(np.stack([r, g, b], -1), "RGB")
        img = Image.frombuffer("RGBX", (w, h), self.frame, "raw", "BGRX", pitch, 1).convert("RGB")
        return img

    def ram(self) -> bytes:
        """A copy of the console's 8 KB work RAM ($C000-$DFFF)."""
        ptr = self.lib.retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM)
        size = self.lib.retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM)
        return ctypes.string_at(ptr, size)

    def regs(self):
        """PC, SP, AF, BC, DE, HL, IX, IY (needs the patched core, see README)."""
        out = (ctypes.c_uint16 * 8)()
        self.lib.retro_dbg_regs(out)
        return dict(zip(("pc", "sp", "af", "bc", "de", "hl", "ix", "iy"), list(out)))

    def pc_trace(self):
        """The last 256 instruction addresses, oldest first."""
        out = (ctypes.c_uint16 * 256)()
        pos = ctypes.c_uint8()
        self.lib.retro_dbg_pc_ring(out, ctypes.byref(pos))
        p = pos.value
        return [out[(p + i) & 255] for i in range(256)]

    def watch(self, lo, hi):
        """Records writes to RAM lo..hi (debugging; needs the patched core)."""
        self.lib.retro_dbg_watch(ctypes.c_uint16(lo), ctypes.c_uint16(hi))

    def watch_log(self):
        """The last 64 watched writes, oldest first: (pc, address, value)."""
        out = (ctypes.c_uint32 * 64)()
        pos = ctypes.c_uint8()
        self.lib.retro_dbg_watch_ring(out, ctypes.byref(pos))
        p = pos.value
        n = min(p, 64)
        vals = [out[(p - n + i) & 63] for i in range(n)]
        lo = getattr(self, "_watch_lo", 0)      # (not used)
        return [(v >> 16, (v >> 8 & 255), v & 255) for v in vals]   # u32 records: pc in bits 16-31, an 8-bit address field, the value

    def watch_trace(self):
        """The 256 instruction addresses before the first watched write."""
        out = (ctypes.c_uint16 * 256)()
        pos = ctypes.c_uint8()
        self.lib.retro_dbg_watch_snap(out, ctypes.byref(pos))
        p = pos.value
        return [out[(p + i) & 255] for i in range(256)]

    def poke(self, addr, value):
        """Writes one byte of work RAM (test set-ups only)."""
        ptr = self.lib.retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM)
        # (addr - $C000) & $1FFF: also accepts the RAM mirror at $E000-$FFFF
        ctypes.cast(ptr, ctypes.POINTER(ctypes.c_uint8))[(addr - 0xC000) & 0x1FFF] = value & 0xFF

    def ram_byte(self, addr) -> int:
        """One byte of work RAM at CPU address addr."""
        return self.ram()[(addr - 0xC000) & 0x1FFF]


# where the patched core is built: emu/Gearsystem next to the repository
DEFAULT_CORE = Path(__file__).resolve().parent.parent.parent / "emu" / "Gearsystem" / "platforms" / "libretro" / "gearsystem_libretro.so"

if __name__ == "__main__":
    rom = sys.argv[1]
    out = sys.argv[2]
    frames = int(sys.argv[3]) if len(sys.argv) > 3 else 120
    emu = Gearsystem(DEFAULT_CORE, rom)
    emu.run_frames(frames)
    emu.screenshot().save(out)
    print("saved", out, emu.frame_w, emu.frame_h)
