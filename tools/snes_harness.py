#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Headless driver for the snes9x libretro core (via ctypes): run frames,
press buttons, read work RAM (128 KB: $7E0000..$7FFFFF), take
screenshots, record sound. Used by the SNES port's tests and videos.

    emu = Snes("snes/out/deimos.sfc")
    emu.run_frames(60, {"a"})
    emu.screenshot().save("x.png")
    emu.wram(0x2000, 16)                 # $7E2000..

Buttons: b y select start up down left right a x l r.

libretro is the emulator-core plugin API: the core is a shared library;
this class registers the callbacks it calls (environment queries, video
frames, audio, input), loads the ROM from memory and steps it a frame at a
time with retro_run. The core's path comes from SNES_CORE.
Licence: GPL-2.0-or-later.
"""
import ctypes
import os
from pathlib import Path

import numpy as np
from PIL import Image

# libretro's constants (libretro.h)
RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY = 9
RETRO_ENVIRONMENT_SET_PIXEL_FORMAT = 10
RETRO_ENVIRONMENT_GET_VARIABLE = 15
RETRO_DEVICE_JOYPAD = 1
RETRO_DEVICE_ID_JOYPAD_MASK = 256
RETRO_MEMORY_SAVE_RAM = 0
RETRO_MEMORY_SYSTEM_RAM = 2
RETRO_MEMORY_VIDEO_RAM = 3
# button name -> libretro joypad id (and bit in the joypad mask)
IDS = {"b": 0, "y": 1, "select": 2, "start": 3, "up": 4, "down": 5, "left": 6, "right": 7, "a": 8, "x": 9,
       "l": 10, "r": 11}

DEFAULT_CORE = Path(os.environ.get("SNES_CORE", Path(__file__).resolve().parents[1] / "emu" / "snes9x" / "snes9x_libretro.so"))


def labels(path):
    """a VICE label file (ld65 -Ln) -> {name: address}"""
    out = {}
    for line in Path(path).read_text().splitlines():
        p = line.split()
        if len(p) == 3 and p[0] == "al":
            out[p[2].lstrip(".")] = int(p[1], 16)
    return out


# the libretro structures and callback types used
class retro_game_info(ctypes.Structure):
    _fields_ = [("path", ctypes.c_char_p), ("data", ctypes.c_void_p),
                ("size", ctypes.c_size_t), ("meta", ctypes.c_char_p)]


class retro_system_av_info(ctypes.Structure):
    _fields_ = [("base_width", ctypes.c_uint), ("base_height", ctypes.c_uint),
                ("max_width", ctypes.c_uint), ("max_height", ctypes.c_uint),
                ("aspect_ratio", ctypes.c_float), ("fps", ctypes.c_double),
                ("sample_rate", ctypes.c_double)]


class retro_variable(ctypes.Structure):
    _fields_ = [("key", ctypes.c_char_p), ("value", ctypes.c_char_p)]


ENV_CB = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_uint, ctypes.c_void_p)
VIDEO_CB = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_size_t)
AUDIO_SAMPLE_CB = ctypes.CFUNCTYPE(None, ctypes.c_int16, ctypes.c_int16)
AUDIO_BATCH_CB = ctypes.CFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t)
INPUT_POLL_CB = ctypes.CFUNCTYPE(None)
INPUT_STATE_CB = ctypes.CFUNCTYPE(ctypes.c_int16, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint)


class Snes:
    """snes9x (or any libretro core) running the ROM at rom_path. frame: the
    last video frame; pressed: the buttons held; audio: a bytearray to
    collect the sound in (None: not recorded)."""

    def __init__(self, rom_path, core_path=DEFAULT_CORE):
        self.lib = ctypes.CDLL(str(core_path))
        self.frame = None
        self.pixel_format = 0
        self.pressed = set()
        self.audio = None
        self._keep = []                                 # (not used)
        # (the callbacks are kept in self._*_cb: ctypes would free them otherwise)
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
        self._rom_buf = ctypes.create_string_buffer(self._rom, len(self._rom))   # (the ROM given in memory)
        info.path = str(rom_path).encode()
        info.data = ctypes.cast(self._rom_buf, ctypes.c_void_p)
        info.size = len(self._rom)
        info.meta = None
        lib.retro_load_game.restype = ctypes.c_bool
        if not lib.retro_load_game(ctypes.byref(info)):
            raise RuntimeError("retro_load_game failed")
        lib.retro_get_memory_data.restype = ctypes.c_void_p
        lib.retro_get_memory_size.restype = ctypes.c_size_t
        lib.retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD)
        self.frames_run = 0

    def _env(self, cmd, data):
        """environment calls: the pixel format noted; no variables; the rest refused"""
        if cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            self.pixel_format = ctypes.cast(data, ctypes.POINTER(ctypes.c_int)).contents.value
            return True
        if cmd == RETRO_ENVIRONMENT_GET_VARIABLE:
            var = ctypes.cast(data, ctypes.POINTER(retro_variable)).contents
            var.value = None
            return False
        return False

    def _video(self, data, width, height, pitch):
        """a frame: kept (a duplicate frame, data NULL, keeps the last)"""
        if data:
            self.frame = ctypes.string_at(data, pitch * height)
            self.frame_w, self.frame_h, self.frame_pitch = width, height, pitch

    def _audio_batch(self, data, frames):
        """audio: stereo 16-bit frames, appended when recording"""
        if self.audio is not None and data:
            self.audio += ctypes.string_at(data, frames * 4)
        return frames

    def _input(self, port, device, index, id_):
        """the joypad on port 0: the pressed buttons, one id or the whole mask"""
        if port != 0 or device != RETRO_DEVICE_JOYPAD:
            return 0
        if id_ == RETRO_DEVICE_ID_JOYPAD_MASK:
            m = 0
            for b in self.pressed:
                m |= 1 << IDS[b]
            return m
        return int(any(IDS[b] == id_ for b in self.pressed))

    def run_frames(self, n, buttons=()):
        """n frames with buttons held, then released"""
        self.pressed = set(buttons)
        for _ in range(n):
            self.lib.retro_run()
            self.frames_run += 1
        self.pressed = set()

    def sample_rate(self):
        """the core's audio sample rate (Hz)"""
        av = retro_system_av_info()
        self.lib.retro_get_system_av_info(ctypes.byref(av))
        return av.sample_rate

    def screenshot(self) -> Image.Image:
        """the last frame as an RGB image (from RGB565, XRGB8888 or 0RGB1555)"""
        w, h, pitch = self.frame_w, self.frame_h, self.frame_pitch
        if self.pixel_format == 2:                     # RGB565
            buf = np.frombuffer(self.frame, dtype="<u2").reshape(h, pitch // 2)[:, :w]
            r = (((buf >> 11) & 31) * 255 // 31).astype("uint8")
            g = (((buf >> 5) & 63) * 255 // 63).astype("uint8")
            b = ((buf & 31) * 255 // 31).astype("uint8")
            return Image.fromarray(np.stack([r, g, b], -1), "RGB")
        if self.pixel_format == 1:                     # XRGB8888
            return Image.frombuffer("RGBX", (w, h), self.frame, "raw", "BGRX", pitch, 1).convert("RGB")
        buf = np.frombuffer(self.frame, dtype="<u2").reshape(h, pitch // 2)[:, :w]
        r = (((buf >> 10) & 31) * 255 // 31).astype("uint8")
        g = (((buf >> 5) & 31) * 255 // 31).astype("uint8")
        b = ((buf & 31) * 255 // 31).astype("uint8")
        return Image.fromarray(np.stack([r, g, b], -1), "RGB")

    def _mem(self, kind):
        """one of the core's memories: (byte pointer, size)"""
        p = self.lib.retro_get_memory_data(kind)
        n = self.lib.retro_get_memory_size(kind)
        return ctypes.cast(p, ctypes.POINTER(ctypes.c_uint8)), n

    def wram(self, addr, n) -> bytes:
        """work RAM from $7E0000 + addr (addr 0..$1FFFF)"""
        p, size = self._mem(RETRO_MEMORY_SYSTEM_RAM)
        return bytes(p[addr:addr + n])

    def poke(self, addr, value):
        """a byte of work RAM ($7E0000 + addr) set"""
        p, size = self._mem(RETRO_MEMORY_SYSTEM_RAM)
        p[addr] = value & 0xFF

    def vram(self) -> bytes:
        """the whole video RAM"""
        p, size = self._mem(RETRO_MEMORY_VIDEO_RAM)
        return bytes(p[:size]) if size else b""

    def sram(self) -> bytes:
        """the whole cartridge RAM (the SuperFX build's frame buffer is there)"""
        p, size = self._mem(RETRO_MEMORY_SAVE_RAM)
        return bytes(p[:size]) if size else b""
