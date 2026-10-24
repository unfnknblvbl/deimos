#!/usr/bin/env bash
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

# Doom RPG for the SNES: the game ROM (out/deimos.sfc) from the Mega Drive game's
# build (its campaign block and the graphics made from the jar).
#
# Steps:
#   1. cc65 compiles the C to 65816 assembler: the portable rules
#      (core/rules.c and its generated tables core/gen/data.c, both put in
#      the RULES segment, bank $C0's lower half), the 3D view (view/view.c)
#      and the SNES port's own C (snes/src/*.c).
#   2. ca65 assembles those and the hand-written assembler (snes/src/*.s).
#   3. ld65 links it with snes.cfg into out/deimos_game.bin (banks
#      $C0-$C1: the code, then the data copied into RAM at start-up).
#      Steps 1-3 are done twice: for the game, with its smaller 3D view
#      (VIEW below), and at the original view size into out/deimos.bin,
#      which only goes into the other ports' copies of the SNES image.
#   4. Python tools make the rest of the cartridge: the sound driver and
#      its streams (snes_sound.py), the generated banks of the 3D view
#      (snes_tables.py: column table, compiled scalers; the game's and the
#      original size's for the other ports), the resources
#      made from the jar (snes_ui_res.py: font, faces, weapons, pictures),
#      and snes_rom.py glues it all together with the Mega Drive game's "campaign
#      block" (every map, texture and string of the game in one blob,
#      made by the Mega Drive game's build) and fixes the header's checksum.
#
# Environment: MEGADRIVE_DIR (the megadrive/ folder, default ../megadrive),
# CC65_LIB (where none.lib is), CFLAGS_EXTRA (extra cc65 options).
set -euo pipefail
cd "$(dirname "$0")"

# the tools this build needs: ../tools/deps.sh finds them, and offers to install
# what is missing (with your package manager, or built from source)
source ../tools/deps.sh
deps_require python3 pillow numpy cc65 wla-spc700
MD=${MEGADRIVE_DIR:-$(cd ../megadrive 2>/dev/null && pwd)}      # (megadrive/: its build makes the maps' block)
if [ -z "${CC65_LIB:-}" ]; then                       # (cc65's none.lib: installed, or built in place)
    P=$(dirname "$(dirname "$(command -v cc65 || echo /usr/bin/cc65)")")
    CC65_LIB=$P/share/cc65/lib; [ -f "$CC65_LIB/none.lib" ] || CC65_LIB=$P/lib
fi
mkdir -p obj out obj/shared
# the 3D view's size for the SNES game (view/view.h, src/view.inc): 104 x 72,
# 80% of the original 128 x 88 (rounded to whole 8-pixel strips), the same
# field of view; build_fx.sh has the same. VIEW: as cc65 and ca65 take it
VIEW_W=104
VIEW_H=72
VIEW="-D VIEW_W=$VIEW_W -D VIEW_H=$VIEW_H"
# build_code DIR DEFINES OUT: the whole program compiled, assembled and
# linked in DIR with the view size DEFINES into OUT (and DIR/deimos.map,
# DIR/deimos.lbl)
build_code() {
    local d=$1 defs=$2 out=$3
    # -O: optimise; -t none: no target library beyond none.lib (the runtime)
    local CC="cc65 -O --cpu 65816 -t none -I ../core $defs ${CFLAGS_EXTRA:-}"
    $CC --code-name RULES -o $d/rules.s ../core/rules.c
    $CC --code-name RULES -o $d/data.s ../core/gen/data.c
    $CC -o $d/view.s ../view/view.c
    for c in glue campaign vdata video save ui; do $CC -o $d/$c.s src/$c.c; done
    # assemble the compiled C
    local OBJS="" f o
    for f in rules data view glue campaign vdata video save ui; do
        ca65 --cpu 65816 -o $d/$f.o $d/$f.s
        OBJS="$OBJS $d/$f.o"
    done
    # assemble the hand-written assembler; its objects go first on the link
    # line (crt0 first of all: the start-up code)
    for f in crt0 hw far draw video vdata vcore gfx save sound; do
        ca65 --cpu 65816 $defs -o $d/${f}_a.o src/$f.s
        OBJS="$d/${f}_a.o $OBJS"
    done
    # link: DIR/deimos.map is the map file, DIR/deimos.lbl the labels (for
    # debuggers and the test tools)
    ld65 -C snes.cfg -m $d/deimos.map -Ln $d/deimos.lbl -o $out $OBJS "$CC65_LIB/none.lib"
}
# the game's code: out/deimos_game.bin (labels obj/deimos.lbl)
build_code obj "$VIEW" out/deimos_game.bin
# the code at the original view size, out/deimos.bin: only for the image
# of the SNES cartridge that the 32X, 3DS and Game Boy Advance builds carry
# (their tools/snes_rom.py image has the SNES's code banks, which they
# never run), so that their ROMs stay as they were
build_code obj/shared "" out/deimos.bin
# the SPC700 sound driver and the music and effects (out/spc_*.bin)
python3 ../tools/snes_sound.py $MD out > /dev/null
# the 3D view's generated banks from $C2 (with the sound driver's engine):
# the game's for its view's height (out/tables_game.bin, also the SuperFX
# build's), and the original's for the other ports' images (out/tables.bin)
python3 ../tools/snes_tables.py out/tables_game.bin out/spc_engine.bin --view $VIEW_W $VIEW_H > /dev/null
python3 ../tools/snes_tables.py out/tables.bin out/spc_engine.bin > /dev/null
# the resources made from the jar (with the sound streams and the version)
python3 ../tools/snes_ui_res.py $MD out/res.bin out/spc_streams.bin "$(cat VERSION)" > /dev/null
# the cartridge: code, tables, resources, campaign block; size and checksum
python3 ../tools/snes_rom.py out/deimos_game.bin out/tables_game.bin out/res.bin $MD/build/gen/campaign.bin out/deimos.sfc
