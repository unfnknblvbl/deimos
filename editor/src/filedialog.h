/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// A small file chooser drawn with Dear ImGui (no system dialogs needed).
// Licence: GPL-2.0-or-later.
//
// The editor has one FileDialog (App::fd). Code that needs a file calls
// open() with a callback; App::frame() calls draw() every frame, which shows
// the dialog as an ImGui modal popup until the user picks a file (the
// callback then runs with its full path) or cancels (the callback never runs).
// Only one request can be open at a time: a second open() replaces the first.
#pragma once
#include <functional>
#include <string>

extern float g_ui;   // the editor's size scale (main.cpp)

class FileDialog {
public:
    // OPEN: pick an existing file; SAVE: pick or type a name (the first
    // extension is added if missing); FOLDER: pick the folder being shown
    enum Mode { OPEN, SAVE, FOLDER };
    // ext: ".sms" / ".png" / "" (any); several separated by spaces (".sms .md")
    // title: the window title (also the popup's ImGui id); start_dir: where
    // to start (the home folder if empty or missing); on_ok: called with the
    // chosen path; default_name: the name box's starting text.
    void open(const std::string &title, Mode mode, const std::string &ext, const std::string &start_dir,
              std::function<void(const std::string &)> on_ok, const std::string &default_name = "");
    void draw();          // call every frame
    bool active() const { return visible_; }   // the dialog is showing (the editor's shortcuts are off)
    std::string dir() const { return dir_; }   // the folder shown

private:
    bool visible_ = false, just_opened_ = false;   // showing; open the popup on the next draw()
    std::string title_, ext_, dir_;                // as passed to open(); the folder being listed
    Mode mode_ = OPEN;
    char name_[512] = {};                          // the file name box (ImGui edits it in place)
    std::function<void(const std::string &)> on_ok_;
    std::string error_;                            // shown in red under the buttons ("" none)
};
