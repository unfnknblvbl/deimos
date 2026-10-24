#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""Records every sound effect from the ROM (the emulator's own YM2612 and
PSG) into one WAV, with a short silence between them, and the title music
under one of them (the effect borrows FM voices 4-5 and must give them back).

Usage: md_sfx_test.py OUT.wav [ids...]      Licence: GPL-2.0-or-later."""
import re
import sys
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from md_lockstep import Rom, sym  # noqa: E402

# How it works: src/sound.c checks the RAM byte sfx_test every frame; a
# non-zero value plays that effect (0xFF stops the music) and is cleared.
# The script pokes each effect's id there (the ids are build_sfx.py's
# SFX_* numbers from build/gen/sfx.h), lets 110 frames go by, and prints
# when each one starts. --alone stops the title music first. It runs at
# import (no functions); it needs a build (out/deimos.md and its map).
ROOT = Path(__file__).resolve().parent.parent
names = re.findall(r"#define SFX_(\w+) (\d+)", (ROOT / "build/gen/sfx.h").read_text())
names = {int(i): n for n, i in names if n != "COUNT"}  # id -> effect name
r = Rom(str(ROOT / "out/deimos.md"))
r.run(240)                           # the title, its music playing
audio = bytearray()
r.e.audio = audio
rate = r.e.sample_rate()
ids = [int(a) for a in sys.argv[2:] if a.isdigit()] or [i for i in sorted(names) if i]  # the ids asked for, else every effect (id 0 is none)
r.run(150)                           # the music alone
if "--alone" in sys.argv:
    r.e.poke(sym("sfx_test"), 0xFF)  # (music off)
    r.run(5)
mark = []
for i in ids:
    mark.append((len(audio) // 4 / rate, names[i]))  # start time in seconds (4 bytes per stereo sample)
    r.e.poke(sym("sfx_test"), i)
    r.run(110)
with wave.open(sys.argv[1], "wb") as f:
    f.setnchannels(2)
    f.setsampwidth(2)
    f.setframerate(int(round(rate)))
    f.writeframes(bytes(audio))
for t, n in mark:
    print(f"{t:6.2f}s  {n}")
