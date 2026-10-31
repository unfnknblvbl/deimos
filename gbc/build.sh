#!/usr/bin/env bash
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

# Doom RPG for the Game Boy Color: out/deimos_gbc.gbc (MBC5, 4 MB, 128 KB
# battery RAM). The game: the portable core (split in three banks by
# tools/split_core.py), the SNES port's menus and boxes (src/ui.c), its 3D
# view (view/view.c), drawn by src/gfx.c; the data: the campaign's block
# and the Game Boy's resources (tools/gb_res.py) from ROM bank 64.
#   ./build.sh          the game
#   ./build.sh test     the script mode (SCRIPT= a tools/make_script.py script)
# The compiler: GBDK-2020 over SDCC 4.5 (TOOLCHAIN.md).
#
# The steps: the other ports' C files are preprocessed (sdcc -E) and
# rewritten by the tools for the Game Boy's banks (split_core.py,
# bankify.py, view_ram.py: every function __banked, the view's variables
# moved to cartridge RAM); everything is compiled by lcc into obj/*.o;
# check_asm.py and check_calls.py look for known SDCC miscompilations and
# unbanked cross-bank calls in the generated assembler; the link makes
# obj/code.gbc (banks 0..63) and gb_pack.py appends the SNES ROM's data and
# the Game Boy's resources. Nothing here changes the sources.
set -euo pipefail
cd "$(dirname "$0")"

# the tools this build needs: ../tools/deps.sh finds them, and offers to install
# what is missing (with your package manager, or built from source)
source ../tools/deps.sh
deps_require python3 pillow numpy cc gbdk
# the tools (overridable from the environment)
GBDK=${GBDK:-/opt/gbdk}
SDCC=${SDCC:-$GBDK/bin}
MD=${MEGADRIVE_DIR:-$(cd ../megadrive 2>/dev/null && pwd)}      # (megadrive/: its build makes the maps' block)
VERSION=$(cat VERSION)
SNES=${SNES_ROM:-obj/deimos_snes.sfc}       # (the SNES layout of the data: made below, unless given)
LCC="$GBDK/bin/lcc"
EXTRA=""
OUT=out/deimos_gbc.gbc
# the test build: PLAT_SCRIPT (src/pad.c plays the script packed at $3E0000)
if [ "${1:-}" = test ]; then EXTRA="-DPLAT_SCRIPT"; OUT=out/deimos_gbc_test.gbc; fi
mkdir -p obj out
# PLAT_LEVEL_FAR: the core reads three level tables through plat.c (they
# live in cartridge RAM bank 2); PLAT_CELL_HEAD: the core's cell_head
# array at $A000 (cartridge RAM bank 1, src/plat.c). XDEFS: extra defines
# (for example -DVIEW_PROF)
DEFS="${XDEFS:-} -DPLAT_LEVEL_FAR -DPLAT_CELL_HEAD=0xA000 $EXTRA"
# the preprocessor alone (SDCC for the sm83, the Game Boy's CPU)
PP="$SDCC/sdcc -msm83 -E $DEFS -I$GBDK/include"
# the core in two banks, its API
$PP ../core/rules.c > obj/rules_pp.c
[ -n "${SKIP_SPLIT:-}" ] || python3 tools/split_core.py obj/rules_pp.c ../core/rules.h obj > /dev/null
# save.c and view.c of the SNES port, banked
sed 's|#include "../../core/rules.h"|#include "core_api.h"|' ../snes/src/save.c > obj/save_src.c
$PP -Iobj -I../snes/src obj/save_src.c > obj/save_pp.c
python3 tools/bankify.py obj/save_pp.c obj/save_gb.c ../snes/src/save.h obj/save_api.h > /dev/null
# the 3D view (view/view.c): banked, its variables in the cartridge RAM's bank 3
$PP -DVIEW_PLAT -Isrc ../view/view.c > obj/view_pp.c
python3 tools/view_ram.py obj/view_pp.c ../view/view.c obj/view_gb.c obj/view_ram.h > /dev/null
echo "#define GB_VERSION \"$VERSION\"" > obj/version.h
# the sound's tables (from the Mega Drive game's instruments)
python3 tools/gb_sound.py $MD/build/gen obj/snd_tab.c
# compile: cc SOURCE NAME [flags]: obj/NAME.o, added to OBJS (the link's
# list). -Wf-bo255 / #pragma bank 255: the linker (-autobank) picks the bank
CF="-Wf--opt-code-size $DEFS -Iobj -Isrc"
OBJS=""
cc() { $LCC $CF "${@:3}" -c -o "obj/$2.o" "$1"; OBJS="$OBJS obj/$2.o"; }
for k in 0 1 2; do cc obj/core_p$k.c core_p$k -Wf-bo255; done
cc obj/core_const.c core_const
cc ../core/gen/data.c data
cc obj/save_gb.c save
cc obj/view_gb.c view
cc ../core/automap.c automap -Wf-bo255 -DAM_GB    # (the automap: banked, with the core API and campaign of plat.h)
# bank 0 (no #pragma bank): the code that switches ROM banks itself
for f in plat main vdraw snd; do cc src/$f.c $f; done
cc obj/snd_tab.c snd_tab
for f in ui gfx pad sound vdraw_map; do cc src/$f.c $f; done
python3 tools/check_asm.py obj/*.asm      # (SDCC's shift bug: TOOLCHAIN.md)
# the link: -yt0x1B MBC5 + RAM + battery, -ya16 16 RAM banks (128 KB),
# -yo256 256 ROM banks (4 MB), -yC Game Boy Color only, -yn the title;
# -m / -j the map and the .noi symbol files the checks read
$LCC -autobank -Wl-m -Wl-j -Wl-yt0x1B -Wl-ya16 -Wl-yo256 -Wm-yC -Wm-yn"DEIMOS" -o obj/code.gbc $OBJS
python3 - <<'PY'                          # (bank 0 must not run over into bank 1)
import re
m = open("obj/code.map").read()
ends = [int(a, 16) + int(n, 16) for a, n in re.findall(r"^_(?:CODE|HOME|INITIALIZER|GSINIT|GSFINAL|BASE) +([0-9A-F]{8}) +([0-9A-F]{8})", m, re.M)]
if max(ends) > 0x4000:
    raise SystemExit(f"bank 0 is full: up to ${max(ends):04X}")
PY
python3 tools/check_calls.py obj/code.noi obj/*.asm   # (a plain call into another bank)
# the cartridge: the code, the SNES data from bank 64, the resources, the script
# the data in the SNES game's layout (snes/build.sh's parts and the
# campaign block: the same bytes as the SNES game's cartridge)
if [ -z "${SNES_ROM:-}" ]; then
    python3 ../tools/snes_rom.py ../snes/out/deimos.bin ../snes/out/tables.bin ../snes/out/res.bin \
        $MD/build/gen/campaign.bin $SNES > /dev/null
fi
python3 tools/gb_pack.py obj/code.gbc $SNES "${SCRIPT:-}" $OUT
