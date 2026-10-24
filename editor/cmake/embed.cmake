# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

# Embeds the engines (engines/*.eng) into the editor as C++ arrays:
#   cmake -DIN=engines/megadrive.eng -DOUT=.../engine_megadrive.cpp -DNAME=engine_megadrive -P embed.cmake
# Run by CMakeLists.txt's add_custom_command for every engine.
# the file as a string of hex digits, two per byte
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" n)
math(EXPR size "${n} / 2")
# every byte as "0xNN,", then a line break after each 16
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," body "${hex}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" body "${body}")
# the .cpp: the array NAME and its size NAME_size (declared extern first,
# so the const definitions have external linkage)
file(WRITE "${OUT}" "// generated from ${IN} - do not edit\n#include <cstddef>\nextern const unsigned char ${NAME}[];\nextern const size_t ${NAME}_size;\nconst unsigned char ${NAME}[] = {\n${body}};\nconst size_t ${NAME}_size = ${size};\n")
