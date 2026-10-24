#!/usr/bin/env bash
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

# Full reproducible build: Doom RPG -> Sega Master System and Game Gear
# ROMs, views drawn live. Run from the project root.
#
# Needs: python3 with Pillow + numpy, WLA-DX (wla-z80, wlalink), the zx0
# compressor; for the tests the Gearsystem
# libretro core built from tools/gearsystem-debug.patch (see README).
#
#   TEST_STEPS=0 ./build.sh     skip the lockstep test
#   TEST_STEPS=N ./build.sh     run the lockstep test for N actions (default 300)
#   LEVEL=level01 ./build.sh    build another map from the jar (default: intro)
#
# Flow: the Python tools turn the original game's files (extracted from
# DoomRPG.jar into assets/) into generated data: game/gen/*.inc for the
# game logic and build/gfx/** for the engine and renderer. Then WLA-DX
# assembles rom/main.asm (which includes the engine, the renderer, the
# game logic and all the generated data) twice: once for the Master
# System and once with -DGAMEGEAR for the Game Gear. tools/rom_size.py
# trims each ROM and fills in its header. Last, the lockstep test plays
# each ROM against the Python rules model (README "The rules").
#
# Outputs: rom/deimos.sms, rom/deimos.gg (and the .o files and the
# linker's symbol files next to them); analysis/ and build/ hold the
# intermediate files.
set -euo pipefail               # stop at the first failing command or unset variable
cd "$(dirname "${BASH_SOURCE[0]}")"   # run from the project root, wherever we are called from

# the tools this build needs: ../tools/deps.sh finds them, and offers to install
# what is missing (with your package manager, or built from source)
source ../tools/deps.sh
deps_require python3 pillow numpy wla-z80 zx0

ASSETS=assets/jar   # the jar's files (level .bsp, entities, texts, images)
LEVEL=${LEVEL:-intro}           # the map to build: intro (the UAC Base), LEVEL=level01 for Sector 1
ANALYSIS=analysis               # parsed level and compiled game data (JSON)
GFX=build/gfx                   # generated binaries and .inc files the assembler includes
mkdir -p "$ANALYSIS" "$GFX/live" build/game
# drop bank files left by an earlier build, so a smaller level leaves none behind
rm -f "$GFX"/view_bank_*.bin "$GFX"/sprite_bank_[0-9]*.bin

echo "== 1. Parse $LEVEL.bsp and compile the level's game data =="
# the map file -> JSON (walls, cells, things, scripts)
python3 tools/bsp_parser.py "$ASSETS/$LEVEL.bsp" "$ANALYSIS/$LEVEL.json"
# + the entity definitions and texts -> the game's view of the level
python3 tools/gamedata.py "$ANALYSIS/$LEVEL.json" "$ASSETS/entities.db" "$ASSETS/entities.str" \
    "$ASSETS/$LEVEL.str" "$GFX" "$ANALYSIS/gamedata.json" --sms
# -> the level blob (bank 2), the strings (bank 4) and game/gen/*.inc
python3 tools/gen_cdata.py "$ANALYSIS/gamedata.json" game/gen "$GFX"

echo "== 2. Renderer data: walls, visibility, textures, billboards, tables, border =="
# writes build/gfx/live/ (the level header, data banks from 9 up, height
# and chunky-to-planar tables, the unrolled fill code) and next_bank.txt,
# the first free bank after them
python3 tools/live_data.py "$ANALYSIS/$LEVEL.json" "$ANALYSIS/gamedata.json" "$ANALYSIS/${LEVEL}_sms.json" \
    "$ASSETS" "$GFX/live"

echo "== 3. Status bar, fonts, title, sprites, sound effects =="
python3 tools/build_ui_gfx.py "$ASSETS" "$GFX"          # status bar, fonts, title screen
python3 tools/build_sfx.py "$GFX" game/gen              # sound effects and their ids
# the hardware sprites go in the banks after the renderer's data; each
# run prints the next free bank
NEXT=$(cat "$GFX/live/next_bank.txt")
NEXT=$(python3 tools/build_sprites.py "$ANALYSIS/gamedata.json" "$GFX" "$NEXT" large)
NEXT=$(python3 tools/build_sprites.py "$ANALYSIS/gamedata.json" "$GFX" "$NEXT" small)
# the ROM directory (where the level's banks are)
python3 tools/rom_dir.py "$GFX"

echo "== 4. Assemble + link the ROM (engine, renderer, game logic) =="
wla-z80 -o rom/main.o rom/main.asm
(cd rom && wlalink -S linkfile deimos.sms)         # -S: also write a symbol file
python3 tools/rom_size.py rom/deimos.sms           # trim, checksum, size code
ls -la rom/deimos.sms

echo "== 4b. The Game Gear ROM: the same game, laid out for its screen =="
wla-z80 -DGAMEGEAR -o rom/main_gg.o rom/main.asm
(cd rom && wlalink -S linkfile_gg deimos_gg.gg && mv deimos_gg.gg deimos.gg)
python3 tools/rom_size.py rom/deimos.gg --gg

# the tests need the patched Gearsystem core (README "Building")
if [ "${TEST_STEPS:-300}" != "0" ]; then
    echo "== 5. Lockstep test: ROM vs the Python rules model on the Gearsystem core =="
    # TEST_STEPS actions with random seed 1; only the summary lines are shown
    python3 tools/test_lockstep.py rom/deimos.sms "${TEST_STEPS:-300}" 1 2>/dev/null | tail -3
fi
if [ "${TEST_STEPS:-300}" != "0" ]; then
    python3 tools/test_lockstep.py rom/deimos.gg "${TEST_STEPS:-300}" 1 2>/dev/null | tail -1
fi
echo "Build complete: rom/deimos.sms, rom/deimos.gg"
