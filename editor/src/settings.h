/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// Editor settings, kept between sessions in deimos-editor.ini
// in the editor program's own folder (each copy of the editor its own).
// Licence: GPL-2.0-or-later.
//
// One Settings object lives in App::set. main.cpp loads it at start and
// saves it at exit; the editor also saves it when it opens or saves a
// project or loads a jar. The file is a small .ini: [section] headers, key=value
// lines, ';' or '#' comments (see Settings::save for the sections).
#pragma once
#include <string>
#include <vector>

struct Settings {
    std::string last_project;                 // the project opened at start (the most recent one)
    std::vector<std::string> recent;          // projects, most recent first, max 8
    std::string last_dir;                     // file dialog start folder
    std::string jar;                          // the user's copy of DoomRPG.jar
    // each console's emulator command, %f = the ROM file, in the Console
    // order (rom.h: CONSOLE_KEYS sms gg md snes snesfx 3ds 32x gbc gba vb);
    // "" = none set
    static const int N_EMULATORS = 10;
    std::string emulators[N_EMULATORS];
    int window_w = 1400, window_h = 900;      // the window's size when not maximised (screen units)
    bool maximized = false;                   // the window was maximised (true the first time)
    float ui_scale = 1.0f;                    // the "text size" setting, 0.5..3.0 (times the display's scale)
    float map_zoom = 24.0f;                   // pixels per cell
                                              // (6..80, before the ui scale)
    float map_panel = 0.28f;                  // width of the map's right-hand panel (share of the window)
                                              // (0.1..1/3)
    // the Map tab's "show" boxes: cell grid, things, props, script markers
    bool show_grid = true, show_things = true, show_props = true, show_scripts = true;
    int preview_console = 0;                  // the view preview's console: App::PREVIEW_CONSOLES
                                              // (an index into App::NEWROM_CONSOLES, 0..8; the
                                              // name PREVIEW_CONSOLES above is out of date)
    bool preview_small = false;               // SMS small view
    bool preview_low = false;                 // SMS low detail (pause menu DETAIL)
    bool preview_solid = false;               // SMS solid-colour walls (pause menu WALLS)
    int build_threads = 0;                    // 0 = all cores
    int play_console = 2;                     // F5's console: an index into App::NEWROM_CONSOLES (2: the Mega Drive)
    int art_zoom = 8;                         // the Art tab's zoom, screen pixels per picture pixel (2..48)
    bool art_grid = true, art_pixel_grid = true;   // the Art tab's 8x8 tile grid and pixel grid
    bool art_fit = true;                      // zoom the picture to fit its panel

    std::string dir() const;                  // the folder: the program's own
    std::string file() const;                 // full path of the .ini
    // Reads the file (or the old deimos-editor one); missing keys keep their
    // defaults, numbers are clamped to their ranges. No file: maximized = true.
    void load();
    // Writes every setting (failures are silently ignored).
    void save() const;
    // Puts a project first in the recent list (removing it from further
    // down), keeps 8 at most, and makes it last_project. Does not save.
    void add_recent(const std::string &path);
};
