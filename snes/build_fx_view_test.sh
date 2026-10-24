#!/usr/bin/env bash
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

# the SuperFX build's 3D view with the test mailbox (tools/snes_view_test.py --fx)
# out/fx_view_test.sfc: the game's rules and view with src/test_view.c as
# the main program (no ui.c, no save), the GSU drawing. The view's size is
# the game's (build_fx.sh: 104 x 72) unless VIEW_W / VIEW_H say otherwise
# (VIEW_W=128 VIEW_H=88: the original); test/view_host_snes_fx (or
# view_host_fx at 128 x 88) is the reference to compare with. VIEWDEF
# passes extra cc65 defines. Its intermediate files go in objfx/view_test,
# its GSU program, tables and resources in out/*_view_test.bin (the
# game's are left alone)..
set -euo pipefail
cd "$(dirname "$0")"
# the tools (../tools/deps.sh finds them, as build.sh does)
source ../tools/deps.sh
deps_require python3 pillow numpy cc65 wla-spc700 wla-superfx
if [ -z "${CC65_LIB:-}" ]; then                       # (cc65's none.lib: installed, or built in place)
    P=$(dirname "$(dirname "$(command -v cc65 || echo /usr/bin/cc65)")")
    CC65_LIB=$P/share/cc65/lib; [ -f "$CC65_LIB/none.lib" ] || CC65_LIB=$P/lib
fi
MD=${MEGADRIVE_DIR:-../megadrive}
VIEW_W=${VIEW_W:-104}
VIEW_H=${VIEW_H:-72}
VIEW="-D VIEW_W=$VIEW_W -D VIEW_H=$VIEW_H"
D=objfx/view_test
mkdir -p $D out
CC="cc65 -O --cpu 65816 -t none -I ../core -D SUPERFX -D VIEW_FX $VIEW ${VIEWDEF:-}"
$CC --code-name RULES -o $D/rules.s ../core/rules.c
$CC --code-name RULES -o $D/data.s ../core/gen/data.c
$CC -o $D/view.s ../view/view.c
for c in glue campaign vdata video fx test_view; do $CC -o $D/$c.s src/$c.c; done
# assemble the compiled C, then the hand-written assembler
OBJS=""
for c in rules data view glue campaign vdata video fx test_view; do
    ca65 --cpu 65816 -o $D/$c.o $D/$c.s; OBJS="$OBJS $D/$c.o"; done
for f in crt0 hw far video vdata gsu_cpu gfx sound fx_list; do
    ca65 --cpu 65816 -I src -D SUPERFX $VIEW -o $D/${f}_a.o src/$f.s; OBJS="$D/${f}_a.o $OBJS"; done
# link; the GSU program (-S: with its symbols); the trimmed block; the
# tables for the view's size; the resources; the ROM
ld65 -C snes_fx.cfg -m objfx/view_test.map -Ln objfx/view_test.lbl -o out/fx_view_test.bin $OBJS "$CC65_LIB/none.lib"
(cd gsu && ${WLA_SUPERFX:-wla-superfx} -DVIEW_W=$VIEW_W -DVIEW_H=$VIEW_H -o ../$D/gsu_view.o view.s && printf "[objects]\n../$D/gsu_view.o\n" > ../$D/gsuv.link && ${WLALINK:-wlalink} -S -b ../$D/gsuv.link ../out/gsu_view_test.bin)
python3 ../tools/snes_fx_block.py $MD/build/gen/campaign.bin out/fx_block_view_test.bin > /dev/null
python3 ../tools/snes_sound.py $MD out > /dev/null
python3 ../tools/snes_tables.py out/tables_view_test.bin out/spc_engine.bin --view $VIEW_W $VIEW_H > /dev/null
python3 ../tools/snes_ui_res.py $MD out/res_view_test.bin out/spc_streams.bin "$(cat VERSION)" "SUPER FX" > /dev/null
python3 ../tools/snes_rom.py --fx out/fx_view_test.bin out/tables_view_test.bin out/gsu_view_test.bin out/res_view_test.bin out/fx_block_view_test.bin out/fx_view_test.sfc
