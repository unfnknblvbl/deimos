// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Entry point for the command-line-only build (no window toolkit).
// This is the whole of deimos-editor-cli (see CMakeLists.txt): it links only
// editor_core, the editor's code without SDL2 or Dear ImGui, so it builds and
// runs without a display. Every argument goes to run_cli (cli.cpp), which
// does the work and returns the exit code. The windowed editor reaches the
// same run_cli from main.cpp when its first argument starts with "--".
#include "cli.h"
int main(int argc, char **argv) { return run_cli(argc, argv); }   // the CLI's jobs: --selftest, --new-rom...
