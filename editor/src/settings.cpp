// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Licence: GPL-2.0-or-later.
//
// Reading and writing the editor's settings file (declared in settings.h).
// load() parses the .ini into a "section.key" -> value map and copies the
// keys it knows into the Settings fields; save() writes them all back with
// comments for a person editing the file by hand.
#include "settings.h"
#include "projectfile.h"   // (PROJECT_EXT)

#include <SDL.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

// The settings folder: the editor program's own folder (SDL_GetBasePath),
// so each copy of the editor keeps its own settings beside it and never
// picks up another (older) version's; the current folder if SDL can't say.
std::string Settings::dir() const
{
    char *b = SDL_GetBasePath();
    std::string d = b ? b : "";
    SDL_free(b);   // (SDL allocated the string)
    if (d.empty()) d = fs::current_path().string();
    return d;
}

std::string Settings::file() const { return (fs::path(dir()) / "deimos-editor.ini").string(); }


// s without leading and trailing spaces, tabs and line ends
static std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

void Settings::load()
{
    std::ifstream f(file());
    if (!f) {
        maximized = true;   // first run: use the whole screen, whatever its size
        return;
    }
    // every "key=value" line, as kv["section.key"] = value
    std::string line, section;
    std::map<std::string, std::string> kv;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;   // blank or comment
        if (line[0] == '[') {
            section = line.substr(1, line.find(']') - 1);   // "[name]": the section from here on
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;   // not a key=value line: ignored
        kv[section + "." + trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    // helpers: copy key k into v if the file has it (string, int, float,
    // bool written as 1 / true / yes)
    auto s = [&](const char *k, std::string &v) {
        if (kv.count(k)) v = kv[k];
    };
    auto i = [&](const char *k, int &v) {
        if (kv.count(k)) v = std::atoi(kv[k].c_str());
    };
    auto fl = [&](const char *k, float &v) {
        if (kv.count(k)) v = (float)std::atof(kv[k].c_str());
    };
    auto b = [&](const char *k, bool &v) {
        if (kv.count(k)) v = kv[k] == "1" || kv[k] == "true" || kv[k] == "yes";
    };
    s("files.last_project", last_project);
    s("files.last_dir", last_dir);
    s("files.jar", jar);
    for (int n = 0; n < 8; n++) {   // recent0 .. recent7, empty ones skipped
        std::string k = "files.recent" + std::to_string(n);
        if (kv.count(k) && !kv[k].empty() && fs::path(kv[k]).extension() == PROJECT_EXT) recent.push_back(kv[k]);
    }
    s("tools.emulator", emulator);
    s("tools.emulator_md", emulator_md);
    i("window.width", window_w);
    i("window.height", window_h);
    b("window.maximized", maximized);
    fl("window.ui_scale", ui_scale);
    fl("map.zoom", map_zoom);
    fl("map.panel", map_panel);
    b("map.show_grid", show_grid);
    b("map.show_things", show_things);
    b("map.show_props", show_props);
    b("map.show_scripts", show_scripts);
    i("map.preview", preview_console);
    b("map.preview_small", preview_small);
    b("map.preview_low", preview_low);
    b("map.preview_solid", preview_solid);
    i("art.zoom", art_zoom);
    b("art.tile_grid", art_grid);
    b("art.pixel_grid", art_pixel_grid);
    b("art.fit", art_fit);
    i("build.threads", build_threads);
    i("build.play", play_console);
    // keep hand-edited values within what the editor can use
    // (preview_console 0..8: the nine entries of App::NEWROM_CONSOLES)
    ui_scale = std::max(0.5f, std::min(3.0f, ui_scale));
    map_zoom = std::max(6.0f, std::min(80.0f, map_zoom));
    map_panel = std::max(0.1f, std::min(1.0f / 3.0f, map_panel));
    art_zoom = std::max(2, std::min(48, art_zoom));
    preview_console = std::max(0, std::min(8, preview_console));
    play_console = std::max(0, std::min(8, play_console));
}

// Writes the whole file again, section by section (bools are written as 1 or 0).
void Settings::save() const
{
    std::error_code ec;
    fs::create_directories(dir(), ec);   // the folder, the first time
    std::ofstream f(file());
    if (!f) return;   // can't write: the settings are just not kept
    f << "; Deimos Editor settings. Safe to edit while the editor is closed.\n\n";
    f << "[files]\n; projects (.deimos)\nlast_project=" << last_project << "\nlast_dir=" << last_dir << "\njar=" << jar << "\n";
    for (size_t n = 0; n < recent.size() && n < 8; n++) f << "recent" << n << "=" << recent[n] << "\n";
    f << "\n[tools]\n; commands that run an emulator; %f is replaced by the ROM file\n";
    f << "emulator=" << emulator << "\n";
    f << "emulator_md=" << emulator_md << "\n";
    f << "\n[window]\nwidth=" << window_w << "\nheight=" << window_h << "\nmaximized=" << (maximized ? 1 : 0)
      << "\n; text and widget size (0.5 - 3.0)\nui_scale=" << ui_scale << "\n";
    f << "\n[map]\nzoom=" << map_zoom << "\n; right-hand panel width, share of the window (up to 0.333)\npanel=" << map_panel << "\nshow_grid=" << show_grid << "\nshow_things=" << show_things
      << "\nshow_props=" << show_props << "\nshow_scripts=" << show_scripts
      << "\n; view preview: 0 Master System, 1 Game Gear, 2 Mega Drive, 3 SNES, 4 SNES SuperFX, 5 3DS, 6 32X, 7 Game Boy Color, 8 Game Boy Advance\npreview=" << preview_console
      << "\npreview_small=" << preview_small << "\npreview_low=" << preview_low
      << "\npreview_solid=" << preview_solid << "\n";
    f << "\n[art]\nzoom=" << art_zoom << "\ntile_grid=" << art_grid << "\npixel_grid=" << art_pixel_grid << "\nfit=" << art_fit << "\n";
    f << "\n[build]\n; 0 = use every CPU core\nthreads=" << build_threads
      << "\n; F5 makes a ROM for this console and plays it (as preview= above)\nplay=" << play_console << "\n";
}

void Settings::add_recent(const std::string &path)
{
    // remove it if already listed, put it first, keep 8
    recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
    recent.insert(recent.begin(), path);
    if (recent.size() > 8) recent.resize(8);
    last_project = path;
}
