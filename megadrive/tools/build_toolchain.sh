#!/usr/bin/env bash
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

# Builds the 68000 cross compiler the Mega Drive game needs (GNU binutils + gcc, C
# only, target m68k-elf), with tools/deps.sh: it first installs what the
# build needs with your distribution's package manager (GMP, MPFR, MPC, a
# host compiler), asking first, then downloads and builds binutils 2.43.1
# and gcc 13.3.0 into ~/.local/share/deimos-tools/m68k-elf (10-30 minutes).
#
# You don't need to run this yourself: ./build.sh does the same when the
# compiler is missing. Builds find the compiler there by themselves (deps.sh
# puts it on PATH); to use it in your own shell, add to ~/.bashrc:
#   export PATH="$HOME/.local/share/deimos-tools/m68k-elf/bin:$PATH"
#
# Environment: DEIMOS_TOOLS (where to install), DEIMOS_YES=1 (don't ask).
set -euo pipefail                       # stop at the first failing command
cd "$(dirname "${BASH_SOURCE[0]}")"     # (the repository's tools/deps.sh is two levels up)
source ../../tools/deps.sh
deps_require m68k-elf
