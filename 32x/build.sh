#!/usr/bin/env bash
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

# Doom RPG for the 32X: out/deimos32x.32x. The game runs on the SH-2s
# (the SNES port's game code on sh2/plat.c's machine layer, the 3D view
# r32x.c, the sound sound32x.c); the 68000 starts them and reads the pad.
# The game's data: the SNES build's cartridge layout with the resources
# made for the 32X (the title's word: 32X), less what only the SNES runs
# (tools/mars_image.py).
#   ./build.sh          the game
#   ./build.sh test     the script mode (tools/make_script.py's script at $3E0000)
# The compilers: sh-elf and m68k-elf GCC 13 (TOOLCHAIN.md).
#   ./build.sh bench    a benchmark build of the 3D view (R32X_BENCH), out/bench.32x
#
# The steps, in order:
#   1. the 68000's code (m68k/boot.s + m68k/main.c), linked at $880000 by
#      m68k/m68k.ld and flattened to obj/m68k.bin;
#   2. the SH-2s' code: sh2/crt0.s, the portable rules (core/), the SNES
#      port's UI and game glue (snes/src/), and the 32X machine layer
#      (sh2/*.c), linked by sh2/sh2.ld into obj/sh2.elf, then flattened to
#      obj/sh2.bin (without .bss) and its symbol table dumped to obj/sh2.syms
#      (make_rom.py reads the entry points from it);
#   3. the game's data: the SNES build's resources and cartridge image, then
#      tools/mars_image.py drops the SNES-only parts;
#   4. tools/make_rom.py puts the three parts (and an optional test script)
#      into one cartridge and fixes its headers and checksum.
# Environment: M68K, SH2 (compiler prefixes), MEGADRIVE_DIR (the Mega Drive game
# checkout, whose build made the campaign block), SCRIPT (test script).
set -euo pipefail
cd "$(dirname "$0")"

# the tools this build needs: ../tools/deps.sh finds them, and offers to
# install what is missing (with your package manager, or built from source)
source ../tools/deps.sh
deps_require python3 pillow numpy m68k-elf sh-elf
# without DoomRPG.jar (DEIMOS_NOJAR=1, for the editor's engine): the resources as
# zeros of the size the engine's region has (../tools/snes_ui_res.py --no-jar)
NOJAR_RES=()
[ -z "${DEIMOS_NOJAR:-}" ] || NOJAR_RES=(--no-jar ../editor/engines/sg32.eng)
M=${M68K:-m68k-elf}               # (the compilers' prefixes: on PATH, or set these)
S=${SH2:-sh-elf}
MD=${MEGADRIVE_DIR:-$(cd ../megadrive 2>/dev/null && pwd)}      # (megadrive/: its build makes the maps' block)
EXTRA=""                          # (extra -D flags for the SH-2 C files)
OUT=out/deimos32x.32x            # (the cartridge to write)
if [ "${1:-}" = bench ]; then EXTRA="-DR32X_BENCH"; OUT=out/bench.32x; fi
if [ "${1:-}" = test ]; then EXTRA="-DPLAT_SCRIPT"; OUT=out/deimos32x_test.32x; fi
mkdir -p obj out
# the 68000
$M-gcc -m68000 -O2 -fomit-frame-pointer -ffreestanding -nostdlib -c m68k/boot.s -o obj/boot.o
$M-gcc -m68000 -O2 -fomit-frame-pointer -ffreestanding -nostdlib -c m68k/main.c -o obj/main68.o
$M-ld -T m68k/m68k.ld -o obj/m68k.elf obj/boot.o obj/main68.o
$M-objcopy -O binary obj/m68k.elf obj/m68k.bin
# the SH-2s (-m2: plain SH-2; -mb: big-endian, as the 32X is; -D__MARS__
# selects the 32X paths in the shared SNES sources; -Isrc names a directory 32x/ does not have, so it adds nothing)
CF="-m2 -mb -O2 -ffreestanding -fno-builtin -fomit-frame-pointer -Wall -Wno-unused-function -Wno-pointer-sign \
    -D__MARS__ $EXTRA -Isrc"
OBJS="obj/crt0.o"
$S-gcc $CF -c sh2/crt0.s -o obj/crt0.o
# build SOURCE NAME [FLAGS...]: compile SOURCE for the SH-2 to obj/NAME.o
# with any extra FLAGS, and add that object to the link list OBJS.
build() { $S-gcc $CF "${@:3}" -c "$1" -o "obj/$2.o"; OBJS="$OBJS obj/$2.o"; }
build ../core/rules.c rules
build ../core/gen/data.c data
build ../core/automap.c automap
build ../snes/src/ui.c ui -Dmain=ui_main     # (the SNES UI's main() becomes ui_main(), called by sh2/plat.c)
build ../snes/src/glue.c glue
build ../snes/src/campaign.c campaign
build ../snes/src/save.c save
for c in plat r32x sound32x libc; do build sh2/$c.c $c; done
# libgcc supplies the SH-2's division and shift helpers; the link puts the
# code in SDRAM (sh2/sh2.ld)
LIBGCC=$($S-gcc -m2 -mb -print-libgcc-file-name)
$S-ld -T sh2/sh2.ld --no-warn-rwx-segments -o obj/sh2.elf $OBJS $LIBGCC
$S-objcopy -O binary -R .bss obj/sh2.elf obj/sh2.bin
$S-nm obj/sh2.elf > obj/sh2.syms
$S-size obj/sh2.elf | tail -1     # (print the code and data sizes)
# the game's data (snes/build.sh's parts, the resources with the word 32X):
# snes_ui_res.py makes the UI resources (title word "32X"), snes_rom.py lays
# out the SNES cartridge image around them and the campaign block (the maps
# and their data, from the Mega Drive game's build)
python3 ../tools/snes_ui_res.py $MD obj/res_32x.bin ../snes/out/spc_streams.bin "$(cat VERSION)" "32X" ${NOJAR_RES[@]+"${NOJAR_RES[@]}"} > /dev/null
python3 ../tools/snes_rom.py ../snes/out/deimos.bin ../snes/out/tables.bin obj/res_32x.bin \
    $MD/build/gen/campaign.bin obj/deimos32x_snes.sfc > /dev/null
python3 tools/mars_image.py obj/deimos32x_snes.sfc out/deimos32x.sfc   # (without the SNES's code and tables: more room for maps)
# the cartridge: 68000 code, SH-2 code, data, and the test script if SCRIPT is set
python3 tools/make_rom.py obj/m68k.bin obj/sh2.bin obj/sh2.syms out/deimos32x.sfc $OUT ${SCRIPT:-}
