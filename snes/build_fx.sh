#!/usr/bin/env bash
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

# Doom RPG for the SNES with the SuperFX (GSU-2): the game ROM
# (out/deimos_fx.sfc) from the Mega Drive game's build. The same game as build.sh's;
# the 3D view is drawn by the GSU (gsu/view.s), the campaign's block
# trimmed to fit the GSU's 2 MB (tools/snes_fx_block.py).
#
# The C is built with -D SUPERFX (the SuperFX machine layer: src/fx.c,
# src/gsu_cpu.s, src/fx_list.s) and -D VIEW_FX (view/view.c hands the
# drawing to the GSU instead of src/draw.s and vcore.s), and with the
# SNES game's view size (VIEW: as build.sh's, given to cc65, ca65 and the
# GSU's assembler alike). The linker config
# is snes_fx.cfg (the GSU cartridge's memory map). The GSU program
# (gsu/view.s) is assembled by WLA-DX's wla-superfx, not ca65.
#
# Environment: MEGADRIVE_DIR, CC65_LIB, CFLAGS_EXTRA (as build.sh), WLA_SUPERFX
# and WLALINK (the WLA-DX assembler and linker).
set -euo pipefail
cd "$(dirname "$0")"

# the tools this build needs: ../tools/deps.sh finds them, and offers to install
# what is missing (with your package manager, or built from source)
source ../tools/deps.sh
deps_require python3 pillow numpy cc65 wla-spc700 wla-superfx
MD=${MEGADRIVE_DIR:-$(cd ../megadrive 2>/dev/null && pwd)}
WLA_SUPERFX=${WLA_SUPERFX:-wla-superfx}
WLALINK=${WLALINK:-wlalink}
if [ -z "${CC65_LIB:-}" ]; then                       # (cc65's none.lib: installed, or built in place)
    P=$(dirname "$(dirname "$(command -v cc65 || echo /usr/bin/cc65)")")
    CC65_LIB=$P/share/cc65/lib; [ -f "$CC65_LIB/none.lib" ] || CC65_LIB=$P/lib
fi
mkdir -p objfx out
# the 3D view's size (view/view.h, src/view.inc, gsu/view.s): 104 x 72, as
# build.sh's
VIEW_W=104
VIEW_H=72
VIEW="-D VIEW_W=$VIEW_W -D VIEW_H=$VIEW_H"
# the same compile as build.sh's, with the SuperFX defines
CC="cc65 -O --cpu 65816 -t none -I ../core -D SUPERFX -D VIEW_FX $VIEW ${CFLAGS_EXTRA:-}"
$CC --code-name RULES -o objfx/rules.s ../core/rules.c
$CC --code-name RULES -o objfx/data.s ../core/gen/data.c
$CC -o objfx/view.s ../view/view.c
for c in glue campaign vdata video save fx ui; do $CC -o objfx/$c.s src/$c.c; done
# assemble the compiled C
OBJS=""
for c in rules data view glue campaign vdata video save fx ui; do
    ca65 --cpu 65816 -o objfx/$c.o objfx/$c.s
    OBJS="$OBJS objfx/$c.o"
done
# the hand-written assembler (-I src: banks.inc; no draw.s or vcore.s,
# the GSU draws the view)
for f in crt0 hw far video vdata gfx save sound gsu_cpu fx_list; do
    ca65 --cpu 65816 -I src -D SUPERFX $VIEW -o objfx/${f}_a.o src/$f.s
    OBJS="objfx/${f}_a.o $OBJS"
done
# link with the GSU cartridge's map
ld65 -C snes_fx.cfg -m objfx/deimos_fx.map -Ln objfx/deimos_fx.lbl -o out/deimos_fx.bin $OBJS "$CC65_LIB/none.lib"
# the GSU's program: out/gsu_view.bin (a bank of the ROM)
(cd gsu && $WLA_SUPERFX -DVIEW_W=$VIEW_W -DVIEW_H=$VIEW_H -o ../objfx/gsu_view.o view.s && printf "[objects]\n../objfx/gsu_view.o\n" > ../objfx/gsuv.link && $WLALINK -b ../objfx/gsuv.link ../out/gsu_view.bin)
# sound, tables and resources as in build.sh (the title says "SUPER FX")
python3 ../tools/snes_sound.py $MD out > /dev/null
python3 ../tools/snes_tables.py out/tables_game.bin out/spc_engine.bin --view $VIEW_W $VIEW_H > /dev/null
python3 ../tools/snes_ui_res.py $MD out/res_fx.bin out/spc_streams.bin "$(cat VERSION)" "SUPER FX" > /dev/null
# the campaign block, trimmed to fit the 2 MB ROM
python3 ../tools/snes_fx_block.py $MD/build/gen/campaign.bin out/fx_block.bin > /dev/null
# the cartridge in the GSU's layout (header at $7FB0)
python3 ../tools/snes_rom.py --fx out/deimos_fx.bin out/tables_game.bin out/gsu_view.bin out/res_fx.bin out/fx_block.bin out/deimos_fx.sfc
