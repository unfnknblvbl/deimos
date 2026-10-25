#!/usr/bin/env bash
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

# A first SuperFX test (out/fx_test.sfc): src/test_fx.c with the GSU
# program gsu/test.s, which fills the picture with a pattern; no game.
# It reuses out/tables.bin and out/res.bin from an earlier build_fx.sh
# or build.sh run.
set -euo pipefail
cd "$(dirname "$0")"
# the tools (../tools/deps.sh finds them, as build.sh does)
source ../tools/deps.sh
deps_require python3 pillow numpy cc65 wla-spc700 wla-superfx
# cc65's install: its library (none.lib) and ca65's macro packages
# (longbranch.mac: newer cc65 keeps them as files in asminc/, and finds
# them through CA65_INC when it was installed somewhere else than it was
# built for), both beside the cc65 found on PATH unless set
P=$(dirname "$(dirname "$(command -v cc65 || echo /usr/bin/cc65)")")
if [ -z "${CC65_LIB:-}" ]; then
    CC65_LIB=$P/share/cc65/lib; [ -f "$CC65_LIB/none.lib" ] || CC65_LIB=$P/lib
fi
if [ -z "${CA65_INC:-}" ]; then
    for d in "$P/share/cc65/asminc" "$P/asminc"; do [ -d "$d" ] && { export CA65_INC=$d; break; }; done
fi
mkdir -p objfx out
CC="cc65 -O --cpu 65816 -t none -I ../core -D SUPERFX"
$CC -o objfx/video.s src/video.c
$CC -o objfx/test_fx.s src/test_fx.c
OBJS=""
for f in objfx/video.s objfx/test_fx.s; do
    ca65 --cpu 65816 -I src -o ${f%.s}.o $f; OBJS="$OBJS ${f%.s}.o"; done
for f in crt0 hw video gfx gsu_cpu sound vdata; do
    ca65 --cpu 65816 -I src -D SUPERFX -o objfx/${f}_a.o src/$f.s; OBJS="objfx/${f}_a.o $OBJS"; done
# link with the GSU cartridge's map; then the GSU program and the ROM
ld65 -C snes_fx.cfg -m objfx/test.map -Ln objfx/test.lbl -o out/fx_test.bin $OBJS "$CC65_LIB/none.lib"
(cd gsu && ${WLA_SUPERFX:-wla-superfx} -o ../objfx/gsu_test.o test.s && printf "[objects]\n../objfx/gsu_test.o\n" > ../objfx/gsu.link && ${WLALINK:-wlalink} -b ../objfx/gsu.link ../out/gsu_test.bin)
python3 ../tools/snes_fx_block.py ${MEGADRIVE_DIR:-../megadrive}/build/gen/campaign.bin out/fx_block.bin > /dev/null
python3 ../tools/snes_rom.py --fx out/fx_test.bin out/tables.bin out/gsu_test.bin out/res.bin out/fx_block.bin out/fx_test.sfc
