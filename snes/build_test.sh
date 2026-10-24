#!/usr/bin/env bash
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

# The smallest test ROM (out/test.sfc): src/test_main.c on crt0.s and
# hw.s only (the backdrop fades through the greys). No header checksum
# or padding (snes_rom.py is not run). The tool paths are the development
# machine's.
set -euo pipefail
cd "$(dirname "$0")"
# the tools (../tools/deps.sh finds them, as build.sh does)
source ../tools/deps.sh
deps_require python3 pillow numpy cc65 wla-spc700 wla-superfx
if [ -z "${CC65_LIB:-}" ]; then                       # (cc65's none.lib: installed, or built in place)
    P=$(dirname "$(dirname "$(command -v cc65 || echo /usr/bin/cc65)")")
    CC65_LIB=$P/share/cc65/lib; [ -f "$CC65_LIB/none.lib" ] || CC65_LIB=$P/lib
fi
mkdir -p obj out
cc65 -O --cpu 65816 -t none -I ../core -o obj/test_main.s src/test_main.c
for f in obj/test_main.s src/crt0.s src/hw.s; do ca65 --cpu 65816 -o obj/$(basename ${f%.s}).o $f; done
ld65 -C snes.cfg -m obj/test.map -o out/test.sfc obj/crt0.o obj/hw.o obj/test_main.o "$CC65_LIB/none.lib"
ls -la out/test.sfc
