#!/usr/bin/env bash
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.
#
# Deimos: Doom RPG on retro consoles, and the Deimos Editor - builds from source.
#
#   ./build.sh               asks what to build (Enter: "all"; not at a
#                            terminal: "all" without asking)
#   ./build.sh <what> ...    without asking; <what> is one or more of
#                            all editor engines roms sms md snes snesfx 32x 3ds gbc gba vb
#
#   all      the editor's engines from this source, then the Deimos Editor.
#            Needs no DoomRPG.jar: the editor asks its user for theirs, and
#            makes every console's ROM from it.
#   editor   the Deimos Editor alone, from the engines in editor/engines/
#            (the consoles' game code with nothing of the original game in
#            it: the ones in the repository, or the last "engines" made).
#   engines  remakes those engines from this source (after a change to a
#            game's code). Every console is built without the jar: each part
#            that would come from it is left as zeros, as an engine has it
#            (the consoles' DEIMOS_NOJAR mode). Needs each console's compilers.
#   sms ...  a console's ROM, into roms/, made with the jar (as the editor
#            makes it). Needs DoomRPG.jar.
#   roms     every console's ROM (needs the jar).
#
# The ROMs are made from your own copy of the original game, DoomRPG.jar
# (the J2ME version): put it in import/ (or give its path when asked). None
# of the original game is in this repository.
#
# The ROMs go to roms/. The editor goes to editor/build/<linux|windows>/.
#
# Each console needs its own compilers (README.md, "Toolchains"). The script
# checks for them first, says what is missing and offers to install it
# (tools/deps.sh). Where a tool you have is not on PATH, set its variable:
#   CROSS=m68k-elf-     (Mega Drive)        M68K, SH2      (32X: compiler prefixes)
#   ARM=arm-none-eabi   (GBA)               GBDK=/opt/gbdk (Game Boy Color)
#   DKP=...             (3DS: 3ds/TOOLCHAIN.md)
#   CC65_LIB, WLA_SPC700, WLA_SUPERFX, WLALINK, ZX0   (SNES, SuperFX, zx0)
#   DEIMOS_YES=1 (install what is missing without asking), DEIMOS_TOOLS=DIR
#   (where tools built from source go; default ~/.local/share/deimos-tools)
#   EDITOR_FOR=linux|windows|both                    (the editor: without asking)
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$ROOT"
export MEGADRIVE_DIR=$ROOT/megadrive
JOBS=$(nproc 2>/dev/null || echo 4)

# Every console target, in build order (sms builds the Master System and Game Gear ROMs).
ALL_ROMS="sms md snes snesfx 32x 3ds gbc gba vb"
# name_of TARGET: prints the target's name for people (for the menu and the log).
name_of() {
    case $1 in
        sms) echo "Master System + Game Gear" ;;   md) echo "Mega Drive / Genesis" ;;
        snes) echo "SNES" ;;                        snesfx) echo "SNES with the Super FX chip" ;;
        32x) echo "32X" ;;                          3ds) echo "Nintendo 3DS" ;;
        gbc) echo "Game Boy Color" ;;               gba) echo "Game Boy Advance" ;;
        vb) echo "Virtual Boy" ;;
        editor) echo "the Deimos Editor only, from the engines in editor/engines/" ;;
        engines) echo "the editor's engines only, remade from this source" ;;
    esac
}

# ---- what to build -------------------------------------------------------
what=("$@")
if [ ${#what[@]} -eq 0 ]; then
    if [ -t 0 ]; then
        echo "Deimos (Doom RPG on retro consoles) - build which?"
        printf "  %2d) %s\n" 1 "the Deimos Editor and its engines (no DoomRPG.jar needed) [Enter]"
        i=2
        for t in editor engines; do printf "  %2d) %s\n" $i "$(name_of $t)"; i=$((i + 1)); done
        for t in $ALL_ROMS; do printf "  %2d) %s ROM (needs DoomRPG.jar)\n" $i "$(name_of $t)"; i=$((i + 1)); done
        printf "  %2d) every console's ROM (needs DoomRPG.jar)\n" $i
        read -r -p "Choose a number (or several, e.g. \"5 7\"): " answer
    fi
    for a in ${answer:-1}; do
        case $a in
            1) what+=(all) ;; 2) what+=(editor) ;; 3) what+=(engines) ;;
            4) what+=(sms) ;; 5) what+=(md) ;; 6) what+=(snes) ;; 7) what+=(snesfx) ;;
            8) what+=(32x) ;; 9) what+=(3ds) ;; 10) what+=(gbc) ;; 11) what+=(gba) ;; 12) what+=(vb) ;;
            13) what+=(roms) ;;
            *) echo "\"$a\" is not one of the choices" >&2; exit 2 ;;
        esac
    done
fi
# targets: the targets to build; keep: the consoles whose ROMs go to roms/
# (those asked for by name, or by "roms"); engines=1 to remake the editor's
# engines (which needs every console built first).
targets=() keep=() engines=0
for w in "${what[@]}"; do
    case $w in
        all) targets+=($ALL_ROMS editor); engines=1 ;;
        engines) targets+=($ALL_ROMS); engines=1 ;;
        roms) targets+=($ALL_ROMS); keep+=($ALL_ROMS) ;;
        sms | md | snes | snesfx | 32x | 3ds | gbc | gba | vb) targets+=("$w"); keep+=("$w") ;;
        editor) targets+=(editor) ;;
        *) echo "Unknown target \"$w\" (all editor engines roms sms md snes snesfx 32x 3ds gbc gba vb)" >&2; exit 2 ;;
    esac
done
# nojar=1: no console's ROM is wanted, only the engines - the consoles are
# built without DoomRPG.jar (DEIMOS_NOJAR=1: each part made from the jar is
# left as zeros, as the engines have it), so no jar is needed
nojar=0
[ ${#keep[@]} -gt 0 ] || nojar=1
[ $nojar = 0 ] || export DEIMOS_NOJAR=1
# wants TARGET: succeeds when TARGET is one of the chosen targets.
wants() { local t; for t in "${targets[@]}"; do [ "$t" = "$1" ] && return 0; done; return 1; }
# keeps TARGET: succeeds when TARGET's ROM goes to roms/.
keeps() { local t; for t in "${keep[@]}"; do [ "$t" = "$1" ] && return 0; done; return 1; }
# rom TARGET FROM TO: copies a built ROM to roms/TO, if TARGET's ROM is kept.
rom() { if keeps "$1"; then cp "$2" "roms/$3"; built+=("$1"); fi; }
# roms=1: at least one console is built (so Python is needed; the jar too unless nojar);
# ports=1: one of the consoles built from the Mega Drive and SNES builds' parts.
roms=0; for t in $ALL_ROMS; do wants $t && roms=1; done
ports=0; for t in snes snesfx 32x 3ds gbc gba vb; do wants $t && ports=1; done

# ---- tools ---------------------------------------------------------------
# Everything the chosen targets need, checked in one go by tools/deps.sh
# (the same file every build script here uses): it says what is missing and
# offers to install it, with this distribution's package manager (pacman,
# apt, dnf, zypper) or by building it from source into
# ~/.local/share/deimos-tools. Each console's own build.sh checks again, so
# they can also be run on their own.
source tools/deps.sh
items=()
if [ $roms = 1 ]; then items+=(python3 pillow numpy cc); fi
if [ $nojar = 0 ]; then items+=(unzip); fi
if wants sms; then items+=(wla-z80 zx0); fi
if wants md || [ $ports = 1 ]; then items+=(m68k-elf zx0); fi     # (the ports use the Mega Drive build's maps)
if [ $ports = 1 ]; then items+=(cc65 wla-spc700); fi              # (and the SNES build's parts)
if wants snesfx; then items+=(wla-superfx); fi
if wants 32x; then items+=(sh-elf); fi
if wants gba; then items+=(arm-none-eabi); fi
if wants vb; then items+=(v810-gcc); fi
if wants gbc; then items+=(gbdk); fi
if wants 3ds; then items+=(devkitpro-3ds); fi
if wants editor; then items+=(cmake cxx make git pkgconf sdl-deps); fi
deps_require "${items[@]}"

# ---- the original game (for the ROMs) ------------------------------------
if [ $roms = 1 ] && [ $nojar = 0 ]; then
    JAR=${JAR:-$(ls import/*.jar 2>/dev/null | head -1 || true)}
    while [ -z "$JAR" ] || [ ! -f "$JAR" ]; do
        [ -t 0 ] || { echo "Put DoomRPG.jar in import/ (or set JAR=path)." >&2; exit 1; }
        read -r -p "Where is DoomRPG.jar? (path; or put it in import/ and press Enter): " JAR
        JAR=${JAR:-$(ls import/*.jar 2>/dev/null | head -1 || true)}
        JAR=${JAR/#\~/$HOME}
    done
    X=build/jar
    if [ ! -f $X/.from ] || [ "$(cat $X/.from)" != "$(cksum < "$JAR")" ]; then
        echo "== Unpacking $JAR"
        rm -rf $X && mkdir -p $X
        unzip -q -o "$JAR" -d $X
        [ -f $X/entities.db ] && [ -f $X/intro.bsp ] || { echo "$JAR is not Doom RPG's jar (no entities.db, intro.bsp)" >&2; exit 1; }
        cksum < "$JAR" > $X/.from
    fi
    for d in megadrive mastersystem; do
        mkdir -p $d/assets
        rm -rf $d/assets/jar
        ln -s ../../build/jar $d/assets/jar
    done
fi

# step TEXT: a heading in the build log.
step() { echo; echo "######## $*"; }
mkdir -p roms
built=()
# ver DIR: the version in DIR/VERSION (it goes in the ROM file names).
ver() { cat "$1/VERSION"; }

# ---- the consoles --------------------------------------------------------
if wants sms; then
    step "$(name_of sms)"
    (cd mastersystem && TEST_STEPS=0 ./build.sh)
    rom sms mastersystem/rom/deimos.sms "deimos_sms_$(ver mastersystem).sms"
    keeps sms && cp mastersystem/rom/deimos.gg "roms/deimos_gg_$(ver mastersystem).gg"
fi
# build_md: builds the Mega Drive ROM once (the ports need its maps and tables first).
md_done=0
build_md() {
    [ $md_done = 1 ] && return
    step "$(name_of md)"
    (cd megadrive && TEST_STEPS=0 ./build.sh)
    rom md megadrive/out/deimos.md "deimos_md_$(ver megadrive).md"
    md_done=1
}
if wants md; then build_md; fi
if [ $ports = 1 ]; then
    build_md                                     # (the ports read its maps and tables)
    python3 tools/gen_core.py megadrive core/gen > /dev/null
    step "$(name_of snes)"                       # (the other ports use the SNES build's parts)
    snes/build.sh
    rom snes snes/out/deimos.sfc "deimos_snes_$(ver snes).sfc"
fi
if wants snesfx; then
    step "$(name_of snesfx)"; snes/build_fx.sh
    rom snesfx snes/out/deimos_fx.sfc "deimos_snes_superfx_$(ver snes).sfc"
fi
if wants 32x; then
    step "$(name_of 32x)"; 32x/build.sh
    rom 32x 32x/out/deimos32x.32x "deimos_32x_$(ver 32x).32x"
fi
if wants 3ds; then
    step "$(name_of 3ds)"; 3ds/build.sh
    rom 3ds 3ds/out/deimos.3dsx "deimos_3ds_$(ver 3ds).3dsx"
    keeps 3ds && mkdir -p roms/3ds_licences && cp 3ds/THIRD_PARTY.md 3ds/licences/*.txt roms/3ds_licences/
fi
if wants gbc; then
    step "$(name_of gbc)"; gbc/build.sh
    rom gbc gbc/out/deimos_gbc.gbc "deimos_gbc_$(ver gbc).gbc"
fi
if wants gba; then
    step "$(name_of gba)"; gba/build.sh
    rom gba gba/out/deimos_gba.gba "deimos_gba_$(ver gba).gba"
fi
if wants vb; then
    step "$(name_of vb)"; vb/build.sh
    rom vb vb/out/deimos_vb.vb "deimos_vb_$(ver vb).vb"
fi

# ---- the editor's engines ("engines", "all"): the ROMs just built, blanked
# (tools/snes_make_engine.py and the Master System's and Mega Drive's
# make_engine.py cut out every region made from the jar; the editor makes
# them again from its user's jar)
if [ $engines = 1 ]; then
    step "The editor's engines"
    E=$ROOT/editor/engines
    # (the ten are independent: made side by side, each one's failure reported)
    T=tools/snes_make_engine.py
    jobs_=()
    (cd mastersystem && python3 tools/make_engine.py rom/deimos.sms rom/deimos.sym build/gfx $E/sg8.eng) & jobs_+=($!)
    (cd mastersystem && python3 tools/make_engine.py rom/deimos.gg rom/deimos_gg.sym build/gfx $E/sg8hh.eng --gg) & jobs_+=($!)
    (cd megadrive && python3 tools/make_engine.py build/deimos.elf out/deimos.md build/gen $E/sg16.eng) & jobs_+=($!)
    python3 $T snes/out/deimos.sfc snes/out/res.bin "$(ver snes)" $E/ntnd16.eng 3 & jobs_+=($!)
    python3 $T snes/out/deimos_fx.sfc snes/out/res_fx.bin "$(ver snes)" $E/ntnd16fx.eng 4 & jobs_+=($!)
    python3 $T 3ds/out/deimos3ds.sfc 3ds/out/res_3ds.bin "$(ver 3ds)" $E/ntnd3Dhh.eng 5 3ds/out/deimos.3dsx & jobs_+=($!)
    python3 $T 32x/out/deimos32x.sfc 32x/obj/res_32x.bin "$(ver 32x)" $E/sg32.eng 6 32x/out/deimos32x.32x & jobs_+=($!)
    python3 $T gbc/obj/deimos_snes.sfc snes/out/res.bin "$(ver gbc)" $E/ntnd8hh.eng 7 gbc/out/deimos_gbc.gbc & jobs_+=($!)
    python3 $T gba/out/deimos_gba.sfc gba/obj/res_gba.bin "$(ver gba)" $E/ntnd32hh.eng 8 gba/out/deimos_gba.gba & jobs_+=($!)
    python3 $T vb/obj/deimos_vb.sfc vb/obj/res_vb.bin "$(ver vb)" $E/ntnd32vr.eng 9 vb/out/deimos_vb.vb & jobs_+=($!)
    failed=0
    for j in "${jobs_[@]}"; do wait "$j" || failed=1; done
    [ $failed = 0 ] || { echo "Making an engine failed (above)." >&2; exit 1; }
    built+=(engines)
fi

# ---- the editor ----------------------------------------------------------
if wants editor; then
    step "$(name_of editor)"
    for_=${EDITOR_FOR:-}                         # (linux, windows or both; unset: it asks)
    if [ -z "$for_" ] && ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
        echo "(MinGW-w64 is not installed, so only the Linux editor: README.md, \"Toolchains\")"
        for_=linux
    fi
    editor/build.sh $for_
    built+=(editor)
fi

echo
echo "Done: ${built[*]}"
[ ${#keep[@]} -gt 0 ] && echo "ROMs in roms/:" && ls -1 roms | sed 's/^/  /'
[ $engines = 1 ] && echo "The engines: editor/engines/ (copied beside the editor when it is built)"
wants editor && echo "The editor: editor/build/" || true
