/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// Command-line mode of the editor (see cli.cpp).
// Called by main() in main.cpp (when argv[1] starts with "--") and in
// main_cli_only.cpp (always).
#pragma once
// Runs one command-line job. argc/argv: main()'s, so argv[1] is the job
// ("--new-project", "--make-rom"...) and the rest its arguments. Prints its
// results and errors and returns the process exit code (0 = success).
int run_cli(int argc, char **argv);
