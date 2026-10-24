#!/usr/bin/env bash
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

# Doom RPG for the Game Boy Advance: out/deimos_gba.gba. The game: the
# portable core, the Game Boy Color's menus and boxes (gbc/src/ui.c), the
# SNES port's glue, campaign and saves, the 3D view (view/view.c), on the
# GBA's machine layer (src/); the data: the SNES build's cartridge image
# (its resources made with the title's word ADVANCE), read as it is.
#   ./build.sh          the game
#   ./build.sh test     the script mode (SCRIPT= a 32x/tools/make_script.py script)
# The compiler: arm-none-eabi GCC 13 (README.md).
#
# The steps: assemble the start-up (src/crt0.s) and the divisions
# (src/div.s); compile the shared game code and the GBA layer (Thumb code
# in the cartridge, except the 3D view's view.c, built as ARM code and
# moved to the fast RAM); link with src/gba.ld into obj/gba.elf and flatten
# it to obj/code.bin; make the SNES game's data; tools/gba_rom.py puts the
# code and the data (and a test script) into the cartridge.
# Environment: ARM (the compiler prefix), MEGADRIVE_DIR, XDEFS (more -D flags),
# SCRIPT (the test script).
set -euo pipefail
cd "$(dirname "$0")"

# the tools this build needs: ../tools/deps.sh finds them, and offers to
# install what is missing (with your package manager, or built from source)
source ../tools/deps.sh
deps_require python3 pillow numpy arm-none-eabi
A=${ARM:-arm-none-eabi}
MD=${MEGADRIVE_DIR:-$(cd ../megadrive 2>/dev/null && pwd)}      # (megadrive/: its build makes the maps' block)
VERSION=$(cat VERSION)
EXTRA=""                          # (-D flags for the test build)
OUT=out/deimos_gba.gba           # (the cartridge to write)
if [ "${1:-}" = test ]; then EXTRA="-DPLAT_SCRIPT"; OUT=out/deimos_gba_test.gba; fi
mkdir -p obj out
CF="-mcpu=arm7tdmi -mthumb -mthumb-interwork -O2 -ffreestanding -fomit-frame-pointer -Wall -Wno-unused-function \
    -Wno-pointer-sign -Wno-unknown-pragmas -Wno-char-subscripts -fno-strict-aliasing $EXTRA ${XDEFS:-} -Isrc"
OBJS=""
# build SOURCE NAME [FLAGS...]: compile SOURCE to obj/NAME.o with any extra
# FLAGS, and add that object to the link list OBJS.
build() { $A-gcc $CF "${@:3}" -c "$1" -o "obj/$2.o"; OBJS="$OBJS obj/$2.o"; }
$A-gcc $CF -c src/crt0.s -o obj/crt0.o
$A-gcc $CF -c src/div.s -o obj/div.o
OBJS="obj/crt0.o obj/div.o"
# the Game Boy Color's ui.c, with the GBA's headers and the buttons' names
sed -e 's|"SEL+LEFT", "SEL+RIGHT"|"L", "R"|' ../gbc/src/ui.c > obj/ui.c
build obj/ui.c ui -DVIEW_FX       # (VIEW_FX: the 3D view's free camera, as the SNES SuperFX build)
build ../core/rules.c rules
build ../core/gen/data.c data
build ../snes/src/glue.c glue
build ../snes/src/campaign.c campaign
build ../snes/src/save.c save
build ../snes/src/vdata.c vdata -Dvd_words=snes_vd_words     # (plat.c's own)
# the 3D view's C: ARM code, in the fast RAM
build ../view/view.c view -DVIEW_FX -DVIEW_FX_FAST -marm -mlong-calls -Wno-unused-variable -Wno-unused-const-variable
$A-objcopy --rename-section .text=.iwram obj/view.o    # (its code into .iwram: gba.ld copies it to the fast RAM)
for c in plat view_draw sound libc; do build src/$c.c $c; done
build src/gfx.c gfx -DVIEW_FX -mlong-calls "-DVERSION_STR=\"$VERSION\""
# libgcc for the helpers div.s does not replace; the map file for sizes
LIBGCC=$($A-gcc -mcpu=arm7tdmi -mthumb -print-libgcc-file-name)
$A-ld -T src/gba.ld --no-warn-rwx-segments -Map obj/gba.map -o obj/gba.elf $OBJS $LIBGCC
$A-objcopy -O binary obj/gba.elf obj/code.bin
$A-nm obj/gba.elf > obj/gba.syms
$A-size obj/gba.elf | tail -1     # (print the sizes)
# the game's data: the SNES game's (snes/build.sh's parts), the title's word ADVANCE
python3 ../tools/snes_ui_res.py $MD obj/res_gba.bin ../snes/out/spc_streams.bin "$VERSION" "ADVANCE" > /dev/null
python3 ../tools/snes_rom.py ../snes/out/deimos.bin ../snes/out/tables.bin obj/res_gba.bin \
    $MD/build/gen/campaign.bin out/deimos_gba.sfc > /dev/null
python3 tools/gba_rom.py obj/code.bin out/deimos_gba.sfc $OUT ${SCRIPT:-}
