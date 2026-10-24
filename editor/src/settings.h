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
// saves it at exit; the editor also saves it when it opens or saves a ROM
// or loads a jar. The file is a small .ini: [section] headers, key=value
// lines, ';' or '#' comments (see Settings::save for the sections).
#pragma once
#include <string>
#include <vector>

struct Settings {
    std::string last_rom;                     // the ROM opened at start (the most recent one)
    std::vector<std::string> recent;          // most recent first, max 8
    std::string last_dir;                     // file dialog start folder
    std::string jar;                          // the user's copy of DoomRPG.jar
    std::string emulator = "";                // Master System emulator command, %f = ROM file
                                              // (also used for the Game Gear)
    std::string emulator_md = "";             // Mega Drive emulator command
                                              // (used for every console but the Master System and Game Gear)
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
    // Puts rom first in the recent list (removing it from further down),
    // keeps 8 at most, and makes it last_rom. Does not save.
    void add_recent(const std::string &rom);
};
