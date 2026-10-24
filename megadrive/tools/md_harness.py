#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Headless driver for the Genesis Plus GX libretro core (via ctypes), used to
test the Mega Drive game: run frames, press buttons, read work RAM, take
screenshots, record sound.

    emu = MegaDrive("out/deimos.md")    # (ROM first; core_path second, optional)
    emu.run_frames(60)
    emu.run_frames(2, {"a"})              # hold A for 2 frames
    emu.screenshot().save("x.png")
    emu.read(0xFF0000, 16)                # work RAM, 68000 byte order

Buttons: up down left right a b c start x y z mode (a 6-button pad is
plugged in; emu.set_pad(3) swaps it for a 3-button one).
Licence: GPL-2.0-or-later.
"""
import ctypes
import os
import sys
from pathlib import Path

import numpy as np
from PIL import Image

# libretro API constants (libretro.h)
RETRO_ENVIRONMENT_SET_PIXEL_FORMAT = 10
RETRO_ENVIRONMENT_GET_VARIABLE = 15
RETRO_DEVICE_JOYPAD = 1
RETRO_DEVICE_ID_JOYPAD_MASK = 256  # input query for all buttons at once, as a bit mask
RETRO_MEMORY_SAVE_RAM = 0
RETRO_MEMORY_SYSTEM_RAM = 2
RETRO_DEVICE_MDPAD_3B = 0x101  # Genesis Plus GX's pad subclasses of RETRO_DEVICE_JOYPAD
RETRO_DEVICE_MDPAD_6B = 0x201

# MD button -> libretro joypad id (Genesis Plus GX: A=Y, B=B, C=A, X=L, Y=X, Z=R, Mode=Select)
IDS = {"b": 0, "a": 1, "mode": 2, "start": 3, "up": 4, "down": 5, "left": 6, "right": 7, "c": 8,
       "y": 9, "x": 10, "z": 11}

# the core: $MD_CORE, else emu/gpgx/ beside the repository
DEFAULT_CORE = Path(os.environ.get("MD_CORE", Path(__file__).resolve().parents[2] / "emu" / "gpgx"
                                   / "genesis_plus_gx_libretro.so"))


class retro_game_info(ctypes.Structure):
    """libretro's struct retro_game_info: the ROM handed to retro_load_game."""
    _fields_ = [("path", ctypes.c_char_p), ("data", ctypes.c_void_p),
                ("size", ctypes.c_size_t), ("meta", ctypes.c_char_p)]


class retro_system_av_info(ctypes.Structure):
    """libretro's struct retro_system_av_info, flattened (geometry, then timing): screen size, frame rate and sample rate."""
    _fields_ = [("base_width", ctypes.c_uint), ("base_height", ctypes.c_uint),
                ("max_width", ctypes.c_uint), ("max_height", ctypes.c_uint),
                ("aspect_ratio", ctypes.c_float), ("fps", ctypes.c_double),
                ("sample_rate", ctypes.c_double)]


class retro_variable(ctypes.Structure):
    """libretro's struct retro_variable: a core option's key and its value."""
    _fields_ = [("key", ctypes.c_char_p), ("value", ctypes.c_char_p)]


# the C signatures of the callbacks the core calls
ENV_CB = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_uint, ctypes.c_void_p)
VIDEO_CB = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_size_t)
AUDIO_SAMPLE_CB = ctypes.CFUNCTYPE(None, ctypes.c_int16, ctypes.c_int16)
AUDIO_BATCH_CB = ctypes.CFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t)
INPUT_POLL_CB = ctypes.CFUNCTYPE(None)
INPUT_STATE_CB = ctypes.CFUNCTYPE(ctypes.c_int16, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint)

# core options the harness answers: NTSC, 68000 address errors trapped (to
# catch odd-address word reads), no overscan, stereo
VARIABLES = {
    b"genesis_plus_gx_bram": b"per game",
    b"genesis_plus_gx_region_detect": b"ntsc-u",
    b"genesis_plus_gx_force_dtack": b"disabled",
    b"genesis_plus_gx_addr_error": b"enabled",
    b"genesis_plus_gx_overscan": b"disabled",
    b"genesis_plus_gx_render": b"single field",
    b"genesis_plus_gx_sound_output": b"stereo",
}


class MegaDrive:
    """One emulated Mega Drive running a ROM, in this process (the core is a
    shared library loaded with ctypes). The callback objects are kept as
    attributes so Python does not free them while the core holds them.
    `frame` holds the last video frame, `audio` (a bytes object, when set
    by the caller) collects 16-bit stereo samples, `pressed` the buttons
    held during run_frames.
    """
    def __init__(self, rom_path, core_path=DEFAULT_CORE):
        """Loads the core from core_path, registers the callbacks, loads the ROM
        at rom_path and plugs in a 6-button pad. Raises RuntimeError if the
        core refuses the ROM.
        """
        self.lib = ctypes.CDLL(str(core_path))
        self.frame = None
        self.pixel_format = 0
        self.pressed = set()
        self.audio = None
        self._keep = []
        self._rom = Path(rom_path).read_bytes()
        self._env_cb = ENV_CB(self._env)
        self._video_cb = VIDEO_CB(self._video)
        self._audio_cb = AUDIO_SAMPLE_CB(lambda l, r: None)
        self._audio_batch_cb = AUDIO_BATCH_CB(self._audio_batch)
        self._poll_cb = INPUT_POLL_CB(lambda: None)
        self._input_cb = INPUT_STATE_CB(self._input)
        lib = self.lib
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
        lib.retro_get_memory_data.restype = ctypes.c_void_p
        lib.retro_get_memory_size.restype = ctypes.c_size_t
        self.frames_run = 0
        self.set_pad(6)

    def set_pad(self, buttons):
        """plug a 3- or 6-button pad into port 1

        buttons: 6 for a 6-button pad, anything else for a 3-button one.
        """
        self.lib.retro_set_controller_port_device(0, RETRO_DEVICE_MDPAD_6B if buttons == 6 else RETRO_DEVICE_MDPAD_3B)

    def sram(self, n=0x4000) -> bytes:
        """the battery-backed RAM (the core's copy: byte i of the cartridge's
        odd-byte SRAM is at 2*i+1)

        Returns the first n bytes (empty if the core has none).
        """
        p = self.lib.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM)
        return ctypes.string_at(p, n) if p else b""

    def set_sram(self, data):
        """Overwrites the start of the core's battery RAM with `data` (to restore a save after a restart)."""
        p = self.lib.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM)
        ctypes.memmove(p, bytes(data), len(data))

    # ---- callbacks ----
    def _env(self, cmd, data):
        """The environment callback: records the pixel format the core picks and
        answers option queries from VARIABLES (the buffers are kept alive in
        _keep). Returns False for every other request.
        """
        if cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            self.pixel_format = ctypes.cast(data, ctypes.POINTER(ctypes.c_int)).contents.value
            return True
        if cmd == RETRO_ENVIRONMENT_GET_VARIABLE:
            var = ctypes.cast(data, ctypes.POINTER(retro_variable)).contents
            v = VARIABLES.get(var.key)
            if v is None:
                var.value = None
                return False
            buf = ctypes.create_string_buffer(v)
            self._keep.append(buf)
            var.value = ctypes.cast(buf, ctypes.c_char_p)
            return True
        return False

    def _video(self, data, width, height, pitch):
        """The video callback: keeps a copy of the frame and its size (data is NULL for a repeated frame)."""
        if data:
            self.frame = ctypes.string_at(data, pitch * height)
            self.frame_w, self.frame_h, self.frame_pitch = width, height, pitch

    def _audio_batch(self, data, frames):
        """The audio callback: appends the samples (4 bytes per stereo frame) to `audio` if recording. Returns frames consumed."""
        if self.audio is not None and data:
            self.audio += ctypes.string_at(data, frames * 4)
        return frames

    def _input(self, port, device, index, id_):
        """The input callback: the state of the buttons in `pressed`, for pad 1
        only. id_ RETRO_DEVICE_ID_JOYPAD_MASK asks for all of them as a bit mask.
        """
        if port != 0 or device != RETRO_DEVICE_JOYPAD:
            return 0
        if id_ == RETRO_DEVICE_ID_JOYPAD_MASK:
            m = 0
            for b in self.pressed:
                m |= 1 << IDS[b]
            return m
        return int(any(IDS[b] == id_ for b in self.pressed))

    # ---- API ----
    def run_frames(self, n, buttons=()):
        """Runs n frames holding `buttons` (names from IDS), then releases them."""
        self.pressed = set(buttons)
        for _ in range(n):
            self.lib.retro_run()
            self.frames_run += 1
        self.pressed = set()

    def sample_rate(self):
        """The core's audio sample rate in Hz."""
        av = retro_system_av_info()
        self.lib.retro_get_system_av_info(ctypes.byref(av))
        return av.sample_rate

    def screenshot(self) -> Image.Image:
        """The last frame as an RGB Pillow image, converted from the core's pixel format (0 = 0RGB1555, 1 = XRGB8888, 2 = RGB565)."""
        w, h, pitch = self.frame_w, self.frame_h, self.frame_pitch
        if self.pixel_format == 2:                     # RGB565
            buf = np.frombuffer(self.frame, dtype="<u2").reshape(h, pitch // 2)[:, :w]
            r = (((buf >> 11) & 31) * 255 // 31).astype("uint8")
            g = (((buf >> 5) & 63) * 255 // 63).astype("uint8")
            b = ((buf & 31) * 255 // 31).astype("uint8")
            return Image.fromarray(np.stack([r, g, b], -1), "RGB")
        if self.pixel_format == 1:                     # XRGB8888
            return Image.frombuffer("RGBX", (w, h), self.frame, "raw", "BGRX", pitch, 1).convert("RGB")
        buf = np.frombuffer(self.frame, dtype="<u2").reshape(h, pitch // 2)[:, :w]   # 0RGB1555
        r = (((buf >> 10) & 31) * 255 // 31).astype("uint8")
        g = (((buf >> 5) & 31) * 255 // 31).astype("uint8")
        b = ((buf & 31) * 255 // 31).astype("uint8")
        return Image.fromarray(np.stack([r, g, b], -1), "RGB")

    def _ram_ptr(self):
        """A byte pointer to the core's 64 KB of work RAM (68000 $FF0000..$FFFFFF)."""
        return ctypes.cast(self.lib.retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM),
                           ctypes.POINTER(ctypes.c_uint8))

    def read(self, addr, n) -> bytes:
        """Work RAM bytes as the 68000 sees them (the core keeps 16-bit
        words in host order).

        addr: a 68000 address (only the low 16 bits are used); the ^ 1 swaps
        the two bytes of each word on a little-endian host.
        """
        p = self._ram_ptr()
        base = addr & 0xFFFF
        return bytes(p[(base + i) ^ 1] for i in range(n))

    def read_block(self, addr, n) -> bytes:
        """The same as read(), but for a large block: copies all 64 KB once and
        swaps every byte pair with numpy.
        """
        p = self._ram_ptr()
        raw = ctypes.string_at(p, 0x10000)
        a = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 2)[:, ::-1].reshape(-1)
        base = addr & 0xFFFF
        return a[base:base + n].tobytes()

    def poke(self, addr, value):
        """Writes one byte of work RAM at 68000 address addr (the low 16 bits)."""
        self._ram_ptr()[(addr & 0xFFFF) ^ 1] = value & 0xFF


if __name__ == "__main__":
    emu = MegaDrive(sys.argv[1])
    emu.run_frames(int(sys.argv[3]) if len(sys.argv) > 3 else 120)
    emu.screenshot().save(sys.argv[2])
    print("saved", sys.argv[2], emu.frame_w, emu.frame_h)
