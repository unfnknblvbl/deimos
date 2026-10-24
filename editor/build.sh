#!/usr/bin/env bash
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

# Builds the Deimos Editor (its version: VERSION) for Linux and/or Windows.
#   ./build.sh            asks which (just Enter: both)
#   ./build.sh linux      Linux only
#   ./build.sh windows    Windows only (cross-compiled with MinGW-w64)
#   ./build.sh both       both, without asking
#
# Each platform's editor goes to an empty build/<platform>/ (linux, windows),
# ready to zip up and pass on, with only what it needs:
#   Deimos Editor(.exe)   the editor (nothing else to install)
#   engines/               the consoles' games without the original's data
#                          (sg8.eng ... ntnd3Dhh.eng: engine.h), which the
#                          editor reads; a console whose file is missing is
#                          not offered
#   README.md              how to use it (dist/README.md)
#   docs/                  the licences
#   import/                where the user puts DoomRPG.jar (the editor looks there)
#   export/                where the editor puts the ROMs it makes
# CMake's own files stay in cmake-build/<platform>/.
#
# Linux needs: a C++17 compiler, CMake 3.16+ and git (SDL2 is downloaded and
# built into the program, so the editor runs on Linux machines without it).
# Windows needs the MinGW-w64 cross compiler (x86_64-w64-mingw32-g++):
#   Arch / CachyOS:   sudo pacman -S mingw-w64-gcc
#   Debian / Ubuntu:  sudo apt install g++-mingw-w64-x86-64-posix
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"   # work in the editor folder
JOBS=$(nproc 2>/dev/null || echo 4)   # parallel compile jobs: one per core
VERSION=$(cat VERSION)

rm -rf build/cmake                      # (where CMake's files went before 0.9.5)
what=${1:-}
# no argument: ask when run from a terminal, else build both
if [ -z "$what" ]; then
    if [ -t 0 ]; then
        echo "Build the Deimos Editor $VERSION for:"
        echo "  1) Linux"
        echo "  2) Windows"
        echo "  3) both"
        read -r -p "Choose 1, 2 or 3 [Enter: both]: " answer
        case "${answer:-3}" in
            1 | l | linux | Linux) what=linux ;;
            2 | w | windows | Windows) what=windows ;;
            3 | b | both | all | "") what=both ;;
            *) echo "\"$answer\" is not one of the choices" >&2; exit 2 ;;
        esac
    else
        what=both
    fi
fi

# the tools this build needs: ../tools/deps.sh finds them, and offers to install what is
# missing (with your package manager): CMake, a C++ compiler, git (SDL2 is
# downloaded), and for Linux the X11/Wayland/audio headers SDL2 is built
# against; for Windows, MinGW-w64
source ../tools/deps.sh
deps_require cmake cxx make git
case "$what" in linux | both | all) deps_require pkgconf sdl-deps ;; esac
case "$what" in windows | both | all) deps_require mingw || true ;; esac   # (missing: said below)

# true when the MinGW-w64 C++ cross compiler is on the PATH
have_mingw() {
    command -v x86_64-w64-mingw32-g++ >/dev/null
}
# tells how to install MinGW-w64
no_mingw() {
    echo "MinGW-w64 (x86_64-w64-mingw32-g++) is not installed, so there is no Windows build." >&2
    echo "  Arch / CachyOS:  sudo pacman -S mingw-w64-gcc" >&2
    echo "  Debian / Ubuntu: sudo apt install g++-mingw-w64-x86-64-posix" >&2
}

# build/<platform>/, empty first: the editor ($2), its engines, the README,
# the licences, the import and export folders
# Arguments: $1 platform (linux, windows), $2 the program's file name,
# $3 the CMake build folder it is in.
package() {
    local platform=$1 work=$3 out=build/$1
    rm -rf "$out"
    mkdir -p "$out/docs" "$out/import" "$out/export" "$out/engines"
    cp "$work/$2" "$out/"
    cp engines/*.eng "$out/engines/"
    sed "s/@VERSION@/$VERSION/g" dist/README.md > "$out/README.md"
    cp dist/import/* "$out/import/"
    cp third_party/licences/GPL-2.0.txt "$out/docs/GPL-2.0.txt"
    cp third_party/imgui/LICENSE.txt "$out/docs/Dear-ImGui licence.txt"
    cp third_party/json/LICENSE.MIT "$out/docs/nlohmann-json licence.txt"
    cp third_party/stb/LICENSE "$out/docs/stb licence.txt"
    cp third_party/zx0/LICENSE "$out/docs/ZX0 licence.txt"
    cp third_party/fonts/LICENSE "$out/docs/DejaVu fonts licence.txt"
    # the libraries inside the consoles' game code the editor carries (its
    # engines): the 3DS game's libctru, citro3d and newlib, the Game Boy
    # Color game's GBDK-2020 library
    cp third_party/licences/libctru.txt "$out/docs/3DS libctru licence.txt"
    cp third_party/licences/citro3d.txt "$out/docs/3DS citro3d licence.txt"
    cp third_party/licences/newlib.txt "$out/docs/3DS newlib licences.txt"
    cp third_party/licences/gbdk-2020.txt "$out/docs/Game Boy Color GBDK-2020 licence.txt"
    # SDL2's licence, when SDL2 was downloaded into this build
    local sdl
    sdl=$(find "$work/_deps" -maxdepth 2 -name LICENSE.txt -path "*sdl2-src*" 2>/dev/null | head -1)
    if [ -n "$sdl" ]; then cp "$sdl" "$out/docs/SDL2 licence.txt"; fi
    echo "Deimos Editor $VERSION for $platform: $(pwd)/$out"
    (cd "$out" && find . -mindepth 1 | sort | sed 's/^\.\//    /')   # list what is in it
}

# cmake's build of one platform; when it fails, the compiler's first errors again
# at the end (they are easily lost among the other jobs' output)
# $1: the build folder's name under cmake-build/ (its log: cmake-build/$1.log)
compile() {
    local log=cmake-build/$1.log
    if ! cmake --build "cmake-build/$1" -j "$JOBS" 2>&1 | tee "$log"; then
        echo >&2
        echo "The $1 build failed. Its first errors:" >&2
        grep -E "error:|Error:|fatal" "$log" | head -15 >&2
        echo "(all of it: $(pwd)/$log)" >&2
        return 1
    fi
}

# configure, compile and package the Linux editor (SDL2 downloaded)
build_linux() {
    cmake -S . -B cmake-build/linux -DCMAKE_BUILD_TYPE=Release -DEDITOR_FETCH_SDL=ON
    if compile linux; then
        package linux "Deimos Editor" cmake-build/linux
        return
    fi
    # SDL2 built here failed (a compiler newer than it knows, say): the
    # system's SDL2 instead, if it has one - the editor then needs SDL2
    # installed where it runs (most Linux desktops have it)
    if grep -q "sdl2-build" cmake-build/linux.log && pkg-config --exists sdl2 2>/dev/null; then
        echo >&2
        echo "Building SDL2 failed; using this system's SDL2 instead." >&2
        cmake -S . -B cmake-build/linux-system -DCMAKE_BUILD_TYPE=Release -DEDITOR_FETCH_SDL=OFF
        if compile linux-system; then
            package linux "Deimos Editor" cmake-build/linux-system
            echo "Note: this Linux editor uses the system's SDL2: the computers it is passed on to need SDL2 (sdl2 or sdl2-compat) installed." >&2
            return
        fi
    fi
    exit 1
}
# configure, compile and package the Windows editor with the MinGW toolchain file
build_windows() {
    cmake -S . -B cmake-build/windows -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release
    compile windows || exit 1
    package windows "Deimos Editor.exe" cmake-build/windows
}

# (with "both" and no MinGW, the Linux editor is still made, then the
# script ends with an error)
case "$what" in
    linux) build_linux ;;
    windows)
        if have_mingw; then build_windows; else no_mingw; exit 1; fi ;;
    both | all)
        build_linux
        if have_mingw; then build_windows; else no_mingw; exit 1; fi ;;
    *) echo "usage: $0 [linux|windows|both]" >&2; exit 2 ;;
esac
