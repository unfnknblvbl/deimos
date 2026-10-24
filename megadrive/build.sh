#!/usr/bin/env bash
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

# The Mega Drive game - full build: game data from the original game's files, then
# the ROM. Run from anywhere; needs python3 (Pillow, numpy) and the 68000
# cross compiler m68k-elf-gcc on PATH (see README.md).
#
#   ./build.sh               build out/deimos.md
#   TEST_STEPS=300 ./build.sh   also run the lockstep test (needs the emulator core)
#
# The steps: (1) read the original game's maps into analysis/ and turn each
# into a level blob (the rules' data) under build/gen/maps; (2) generate
# the renderer's compiled scalers, the UI graphics, sound effects, music
# and weapon sprites, then the campaign block (every map's data at the end
# of the ROM) and the areas table (the view's palettes per enclosed area,
# by the host program tools/md_areas.c); (3) compile and assemble src/ and
# build/gen/, link with src/md.ld, make the flat binary and fix its header.
# Environment: CROSS (the cross compiler's prefix, default m68k-elf-),
# HOSTCC (the host C compiler for md_areas, default cc), TEST_STEPS.
set -euo pipefail                   # stop at the first failing command or unset variable
cd "$(dirname "${BASH_SOURCE[0]}")"   # work from the repository's top

# the tools this build needs: ../tools/deps.sh finds them, and offers to install
# what is missing (with your package manager, or built from source)
source ../tools/deps.sh
deps_require python3 pillow numpy cc m68k-elf zx0

VERSION=$(cat VERSION)              # e.g. 0.1.0 (shown on the title screen)
ASSETS=assets/jar # the original game's files, from DoomRPG.jar
GEN=build/gen                       # generated sources and data
CROSS=${CROSS:-m68k-elf-}
mkdir -p analysis "$GEN" build/obj out

echo "== 1. The campaign's maps =="
# the maps (BSP, entities, events) as JSON in analysis/, then each map's
# level blob and strings, and the C headers for the game state (state.h)
python3 tools/build_campaign.py "$ASSETS" analysis | tail -1
python3 tools/gen_cdata.py analysis/campaign.json "$GEN" "$GEN/maps"

echo "== 2. Renderer data, graphics, sound =="
# the compiled scalers: remade only when the generator is newer than its output
[ "$GEN/scalers.s" -nt tools/gen_scalers.py ] || python3 tools/gen_scalers.py "$GEN/scalers.s"
python3 tools/build_md_ui.py "$ASSETS" "$GEN" "$VERSION"   # font, faces, icons, title
python3 tools/build_sfx.py "$ASSETS" "$GEN"                 # sound effects
python3 tools/build_music.py "$ASSETS" "$GEN"               # the tunes
python3 tools/build_weapons.py "$ASSETS" "$GEN"             # the weapon sprites
# (tools/md_pack.py's single-map pack, left by an older build, would be
# linked too: remove it)
rm -f "$GEN/pack.s" "$GEN/pack.bin"
# the campaign block (campaign.bin) and campaign.s, which puts it and
# areas.bin in the ROM's .pack section
python3 tools/md_campaign.py analysis "$ASSETS" "$GEN"
# the areas table: a host program, built and run here
${HOSTCC:-cc} -O2 -std=c99 -DMD_AREAS_MAIN tools/md_areas.c -o build/md_areas
build/md_areas "$GEN/campaign.bin" "$GEN/areas.bin"

echo "== 3. Compile =="
# plain 68000 code, no C library (-ffreestanding: libc.s has the few
# routines needed), headers from src/ and build/gen/
CFLAGS="-m68000 -O2 -fomit-frame-pointer -ffreestanding -fno-builtin -Wall -Wextra -Wno-unused-parameter -std=gnu11 -Isrc -I$GEN"
objs=()                             # every object file, for the link
for f in src/*.c $GEN/*.c; do
    o=build/obj/$(basename "${f%.c}").o
    ${CROSS}gcc $CFLAGS -c "$f" -o "$o"
    objs+=("$o")
done
# assembler files go through the C preprocessor; registers may be written
# without the % prefix (d0, a1, sp)
for f in src/*.s $GEN/*.s; do
    o=build/obj/$(basename "${f%.s}")_s.o
    ${CROSS}gcc -m68000 -c -x assembler-with-cpp -Wa,--register-prefix-optional "$f" -o "$o"
    objs+=("$o")
done
# link (libgcc for the 32-bit multiply and divide the 68000 lacks); the map
# file lets the tests find variables such as S in RAM
${CROSS}gcc -m68000 -nostdlib -T src/md.ld -o build/deimos.elf "${objs[@]}" -lgcc -Wl,-Map=build/deimos.map
${CROSS}objcopy -O binary build/deimos.elf build/deimos.bin
python3 tools/fix_header.py build/deimos.bin out/deimos.md   # header checksum

if [ "${TEST_STEPS:-0}" != "0" ]; then
    echo "== 4. Lockstep test: ROM vs the Python rules model =="
    python3 tools/md_lockstep.py out/deimos.md "$TEST_STEPS" 1 2>/dev/null | tail -3
fi
echo "Built out/deimos.md (version $VERSION)"
