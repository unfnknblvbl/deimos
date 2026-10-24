#!/usr/bin/env bash
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

# Doom RPG for the 3DS: out/deimos.3dsx (the game data, the SNES build's
# cartridge image, in its romfs). The toolchain: clang for the ARM11, LLD,
# newlib, libctru and citro3d built from source (3ds/TOOLCHAIN.md),
# under $DKP (devkitPro's, or as TOOLCHAIN.md builds them).
#
# The steps: assemble the vertex shader (picasso) and turn its binary into
# an object (bin2s), compile the portable rules (core/), the SNES port's UI
# and game glue (snes/src/) and the 3DS layer (src/), link a 3DSX-style ELF,
# make the game data (unless GAME_DATA names a ready one) and put it in the
# romfs, then make the icon/title block (smdh) and the .3dsx.
# Two toolchain layouts work: devkitPro's (devkitARM's gcc, libctru and
# citro3d in $DKP/libctru, the tools in $DKP/tools/bin: what tools/deps.sh
# installs) and the clang one in 3ds/TOOLCHAIN.md (wrappers in $DKP/bin).
# Environment: DKP (the toolchain root), EXTRA (more compiler flags, e.g.
# -DPLAT_SCRIPT for the test build), MEGADRIVE_DIR, GAME_DATA, ICON.
set -euo pipefail
cd "$(dirname "$0")"

# the tools this build needs: ../tools/deps.sh finds them, and offers to
# install what is missing (devkitPro's 3DS packages, Python's modules)
source ../tools/deps.sh
deps_require python3 pillow numpy devkitpro-3ds

DKP=${DKP:-${DEVKITPRO:-/opt/devkitpro}}
mkdir -p obj out romfs
if [ ! -x "$DKP/bin/arm-none-eabi-gcc" ] && [ -x "$DKP/devkitARM/bin/arm-none-eabi-gcc" ]; then   # (no clang wrappers in $DKP/bin)
    # devkitPro: devkitARM's gcc for the ARM11 (ARMv6K, hard float), citro3d
    # installed beside libctru; gcc gets warnings, not errors, for the few
    # things clang lets through
    DEVKITPRO_LAYOUT=1
    CC="$DKP/devkitARM/bin/arm-none-eabi-gcc -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft"
    INC="-I$DKP/libctru/include"
    WARN="-Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=implicit-function-declaration"
else
    # the clang toolchain of 3ds/TOOLCHAIN.md (its wrappers carry the CPU flags)
    DEVKITPRO_LAYOUT=0
    T=$DKP/bin                    # (the compiler wrappers)
    CC=$T/arm-none-eabi-gcc
    INC="-I$DKP/libctru/include -I$DKP/citro3d/include"
    WARN=""
fi
CFLAGS="${EXTRA:-} -O2 -g -Wall -Wno-unused-function -Wno-pointer-sign $WARN -D__3DS__ -ffunction-sections -fdata-sections \
    $INC -Iobj"
# the shader (obj/view_shbin.h declares view_shbin and view_shbin_size for r3d.c)
$DKP/tools/bin/picasso -o obj/view.shbin shaders/view.v.pica
(cd obj && $DKP/tools/bin/bin2s -H view_shbin.h view.shbin > view_shbin.s)
$CC -c obj/view_shbin.s -o obj/view_shbin.o
OBJS="obj/view_shbin.o"
# build SOURCE NAME [FLAGS...]: compile SOURCE to obj/NAME.o with any
# extra FLAGS, and add that object to the link list OBJS.
build() { $CC $CFLAGS "${@:3}" -c "$1" -o "obj/$2.o"; OBJS="$OBJS obj/$2.o"; }
build ../core/rules.c rules
build ../core/gen/data.c data
build ../snes/src/ui.c ui -Dmain=ui_main     # (the SNES UI's main() becomes ui_main(), called by src/plat.c)
build ../snes/src/glue.c glue
build ../snes/src/campaign.c campaign
build ../snes/src/save.c save
for c in plat r3d sound3ds; do build src/$c.c $c; done
# the link: libctru's start-up, the game, citro3d, libctru, newlib and the
# compiler's helpers; lld's alignment warnings about 3dsx.ld are filtered out
if [ $DEVKITPRO_LAYOUT = 1 ]; then
    # devkitARM's 3dsx.specs brings the start-up code and the linker script
    $CC -specs=3dsx.specs -g -Wl,--gc-sections -o obj/deimos.elf $OBJS -L$DKP/libctru/lib -lcitro3d -lctru -lm
else
$T/arm-none-eabi-ld -T $DKP/crt/3dsx.ld -d --emit-relocs --gc-sections -o obj/deimos.elf \
    $DKP/crt/3dsx_crt0.o $DKP/crt/crti.o $OBJS -L$DKP/citro3d/lib -L$DKP/libctru/lib \
    -L$DKP/sysroot/arm-none-eabi/lib -L$DKP/sysroot --start-group -lcitro3d -lctru -lc -lm -lsysbase \
    -lclang_rt.builtins-arm --end-group $DKP/crt/crtn.o 2>&1 | grep -v "is not a multiple of alignment" || true
fi
# the game data: the SNES build's cartridge layout (snes/build.sh's parts)
# with the resources made for the 3DS (the title's word: 3D)
MD=${MEGADRIVE_DIR:-$(cd ../megadrive 2>/dev/null && pwd)}      # (megadrive/: its build makes the maps' block)
if [ -z "${GAME_DATA:-}" ]; then
    python3 ../tools/snes_ui_res.py $MD out/res_3ds.bin ../snes/out/spc_streams.bin "$(cat VERSION)" "3D" > /dev/null
    python3 ../tools/snes_rom.py ../snes/out/deimos.bin ../snes/out/tables.bin out/res_3ds.bin \
        $MD/build/gen/campaign.bin out/deimos3ds.sfc > /dev/null
    GAME_DATA=out/deimos3ds.sfc
fi
cp $GAME_DATA romfs/deimos.sfc   # (src/plat.c reads romfs:/deimos.sfc)
# the title, description, author and icon shown by the Homebrew Launcher
$DKP/tools/bin/smdhtool --create "Deimos" "Deimos: A Doom RPG Engine ($(cat VERSION))" "unfnknblvbl" ${ICON:-icon.png} obj/deimos.smdh
$DKP/tools/bin/3dsxtool obj/deimos.elf out/deimos.3dsx --smdh=obj/deimos.smdh --romfs=romfs
ls -la out/deimos.3dsx
