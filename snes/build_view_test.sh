#!/usr/bin/env bash
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

# the 3D view on the SNES with a test mailbox (tools/snes_view_test.py)
# out/view_test.sfc: the rules and the CPU-drawn view with
# src/test_view.c as the main program (no ui.c or save). The view's size
# is the game's (build.sh: 104 x 72) unless VIEW_W / VIEW_H say otherwise
# (VIEW_W=128 VIEW_H=88: the original); test/view_host_snes (or view_host
# at 128 x 88) is the reference to compare with. VIEWDEF passes extra
# cc65 defines. Its intermediate files go in obj/view_test, its tables
# and resources in out/*_view_test.bin (the game's are left alone).
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
MD=${MEGADRIVE_DIR:-../megadrive}
VIEW_W=${VIEW_W:-104}
VIEW_H=${VIEW_H:-72}
VIEW="-D VIEW_W=$VIEW_W -D VIEW_H=$VIEW_H"
D=obj/view_test
mkdir -p $D out
CC="cc65 -O --cpu 65816 -t none -I ../core $VIEW ${VIEWDEF:-}"
$CC --code-name RULES -o $D/rules.s ../core/rules.c
$CC --code-name RULES -o $D/data.s ../core/gen/data.c
$CC -o $D/view.s ../view/view.c
for c in glue campaign vdata video test_view; do $CC -o $D/$c.s src/$c.c; done
# assemble: compiled C as <name>.o, hand-written assembler as <name>_a.o
# (video and vdata have both)
for f in $D/rules.s $D/data.s $D/view.s $D/glue.s $D/campaign.s $D/vdata.s $D/video.s $D/test_view.s \
         src/crt0.s src/hw.s src/far.s src/draw.s src/video.s src/vdata.s src/vcore.s src/gfx.s src/sound.s; do
    o=$D/$(basename ${f%.s}).o
    [[ $f == src/* ]] && o=$D/$(basename ${f%.s})_a.o
    ca65 --cpu 65816 $VIEW -o $o $f
done
ld65 -C snes.cfg -m obj/view_test.map -Ln obj/view_test.lbl -o out/view_test.bin \
    $D/crt0_a.o $D/hw_a.o $D/far_a.o $D/draw_a.o $D/video_a.o $D/vdata_a.o $D/vcore_a.o $D/gfx_a.o $D/sound_a.o \
    $D/rules.o $D/data.o $D/view.o $D/glue.o $D/campaign.o $D/vdata.o $D/video.o $D/test_view.o \
    "$CC65_LIB/none.lib"
# the view's tables (for its size), the resources, the ROM
python3 ../tools/snes_sound.py $MD out > /dev/null
python3 ../tools/snes_tables.py out/tables_view_test.bin out/spc_engine.bin --view $VIEW_W $VIEW_H > /dev/null
python3 ../tools/snes_ui_res.py $MD out/res_view_test.bin out/spc_streams.bin "$(cat VERSION)" > /dev/null
python3 ../tools/snes_rom.py out/view_test.bin out/tables_view_test.bin out/res_view_test.bin $MD/build/gen/campaign.bin out/view_test.sfc
