#!/usr/bin/env bash
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

# Doom RPG for the Virtual Boy: out/deimos_vb.vb. The game: the portable
# core, the Game Boy Color's menus and boxes (gbc/src/ui.c), the SNES
# port's glue, campaign and saves, the 3D view (view/view.c), on the
# Virtual Boy's machine layer (src/) - the GBA port's route; the data: the
# SNES build's cartridge image (its resources made with this version),
# read as it is.
#   ./build.sh            the game
#   ./build.sh test       the script mode (SCRIPT= a 32x/tools/make_script.py script)
#   ./build.sh viewtest   the view test (SCRIPT= a tools/vb_view_test.py view list)
#   ./build.sh diag       the game with start-up markers (plat.h's DIAG), for
#                         finding where an emulator stops
# The compiler: v810-gcc 4.9.4 (VUEngine's patches; ../tools/deps.sh builds it).
#
# The steps: assemble the start-up and the vectors (src/crt0.s); compile
# the shared game code and the Virtual Boy layer; link with src/vb.ld into
# obj/vb.elf and flatten it to obj/code.bin and obj/vectors.bin; make the
# sound's tables (tools/vb_sound.py) and the SNES game's data;
# tools/vb_rom.py puts the code, the data (and a test script) and the
# vectors into the cartridge.
# Environment: V810 (the compiler prefix, default v810), MEGADRIVE_DIR,
# XDEFS (more -D flags), SCRIPT (the test script or view list).
set -euo pipefail
cd "$(dirname "$0")"

# the tools this build needs: ../tools/deps.sh finds them, and offers to
# install what is missing (with your package manager, or built from source)
source ../tools/deps.sh
deps_require python3 pillow numpy v810-gcc
V=${V810:-v810}
MD=${MEGADRIVE_DIR:-$(cd ../megadrive 2>/dev/null && pwd)}      # (megadrive/: its build makes the maps' block)
VERSION=$(cat VERSION)
EXTRA=""                          # (-D flags for the test builds)
OUT=out/deimos_vb.vb              # (the cartridge to write)
case "${1:-}" in
    test) EXTRA="-DPLAT_SCRIPT"; OUT=out/deimos_vb_test.vb ;;
    viewtest) EXTRA="-DPLAT_VIEWTEST"; OUT=out/deimos_vb_view.vb ;;
    diag) EXTRA="-DPLAT_DIAG"; OUT=out/deimos_vb_diag.vb ;;
esac
mkdir -p obj out
# -ffreestanding: no C library (gba/src/libc.c has the few functions the
# game uses); -fno-common: every variable in its object's .bss (so a whole
# object's can move to the VIP's memory below)
CF="-O2 -ffreestanding -fno-common -fomit-frame-pointer -Wall -Wno-unused-function -Wno-pointer-sign \
    -Wno-unknown-pragmas -Wno-char-subscripts -fno-strict-aliasing $EXTRA ${XDEFS:-} -Isrc -I../gba/src"
OBJS=""
# build SOURCE NAME [FLAGS...]: compile SOURCE to obj/NAME.o with any extra
# FLAGS, and add that object to the link list OBJS.
build() { $V-gcc $CF "${@:3}" -c "$1" -o "obj/$2.o"; OBJS="$OBJS obj/$2.o"; }
$V-gcc -c src/crt0.s -o obj/crt0.o
OBJS="obj/crt0.o"
# the Game Boy Color's ui.c with the Virtual Boy's button names and its
# wider status bar (tools/vb_ui.py: a copy, the source unchanged)
python3 tools/vb_ui.py ../gbc/src/ui.c obj/ui.c
build obj/ui.c ui -DVIEW_FX
build ../core/rules.c rules
build ../core/gen/data.c data
build ../snes/src/glue.c glue
# (glue.c's variables: the level's data and strings, 24 KB, into the VIP's memory)
$V-objcopy --rename-section .bss=.vbss obj/glue.o
build ../snes/src/campaign.c campaign
build ../snes/src/save.c save
build ../snes/src/vdata.c vdata -Dvd_words=snes_vd_words     # (plat.c's own)
build ../view/view.c view -DVIEW_FX -DVIEW_FX_FAST -Wno-unused-variable -Wno-unused-const-variable
build ../gba/src/libc.c libc
python3 tools/vb_sound.py $MD/build/gen obj/snd_tab.c
build obj/snd_tab.c snd_tab
for c in plat view_draw sound; do build src/$c.c $c; done
build src/viewtest.c viewtest -DVIEW_FX
build src/gfx.c gfx -DVIEW_FX "-DVERSION_STR=\"$VERSION\""
# libgcc for the few helpers the compiler calls; the map file for sizes
LIBGCC=$($V-gcc -print-libgcc-file-name)
$V-ld -T src/vb.ld -Map obj/vb.map -o obj/vb.elf $OBJS $LIBGCC
$V-objcopy -O binary -R .vectors -R .vbss -R .bss obj/vb.elf obj/code.bin
$V-objcopy -O binary -j .vectors obj/vb.elf obj/vectors.bin
$V-nm obj/vb.elf > obj/vb.syms
$V-size -A obj/vb.elf | grep -E '^\.(text|data|bss|vbss) '     # (print the sizes)
# the game's data: the SNES game's (snes/build.sh's parts), with this version
python3 ../tools/snes_ui_res.py $MD obj/res_vb.bin ../snes/out/spc_streams.bin "$VERSION" "VIRTUAL BOY" > /dev/null
python3 ../tools/snes_rom.py ../snes/out/deimos.bin ../snes/out/tables.bin obj/res_vb.bin \
    $MD/build/gen/campaign.bin obj/deimos_vb.sfc > /dev/null
python3 tools/vb_rom.py obj/code.bin obj/vectors.bin obj/deimos_vb.sfc $OUT ${SCRIPT:-}
