#!/usr/bin/env bash
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

# the rules on the SNES with a test mailbox (tools/snes_lockstep.py)
# out/rules_test.sfc: core/rules.c with glue.c, campaign.c and
# src/test_rules.c (no screen, no sound).
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
CC="cc65 -O --cpu 65816 -t none -I ../core"
$CC --code-name RULES -o obj/rules.s ../core/rules.c
$CC --code-name RULES -o obj/data.s ../core/gen/data.c
for c in glue campaign test_rules; do $CC -o obj/$c.s src/$c.c; done
# assemble everything, then link (the rules' ROM: code and the block only)
for f in obj/rules.s obj/data.s obj/glue.s obj/campaign.s obj/test_rules.s src/crt0.s src/hw.s src/far.s; do
    ca65 --cpu 65816 -o obj/$(basename ${f%.s}).o $f
done
ld65 -C snes.cfg -m obj/rules_test.map -Ln obj/rules_test.lbl -o out/rules_test.bin \
    obj/crt0.o obj/hw.o obj/far.o obj/rules.o obj/data.o obj/glue.o obj/campaign.o obj/test_rules.o \
    "$CC65_LIB/none.lib"
python3 ../tools/snes_rom.py out/rules_test.bin ${MEGADRIVE_DIR:-../megadrive}/build/gen/campaign.bin out/rules_test.sfc
