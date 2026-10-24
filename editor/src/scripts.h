/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// The script (event) editor panel. Licence: GPL-2.0-or-later.
//
// A "script" (event) is a list of commands attached to one map cell; the
// game runs it when the player walks onto, off, turns on, or uses the
// cell (see Event and Cmd in project.h). The Map tab's inspector and the
// Text & scripts tab call these.
#pragma once
#include <string>

class App;
struct Cmd;
struct Project;

// draws the editor for app.proj.events[event_index] with Dear ImGui (call
// once per frame inside a window); edits go straight into the project,
// with app.changed() (and app.push_undo() for most edits)
void draw_script_editor(App &app, int event_index);
// one-line plain-English summary of a command
std::string describe_cmd(const Project &p, const Cmd &c);
