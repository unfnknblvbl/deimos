// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Editor: actions, undo, builds, menus. Licence: GPL-2.0-or-later.
//
// The App member functions that are not one tab's drawing (see app.h for
// the overview of the class):
// - undo / redo: whole copies of the Project;
// - the image cache: SDL textures of pictures, made on demand;
// - opening a ROM, switching between a campaign ROM's maps, a new empty map,
//   starting an emulator;
// - building: start_build copies the ROM and level and rebuilds on a worker
//   thread (Rom::build, the console builders); poll_build collects the
//   result on the UI thread;
// - frame(): the whole window each frame (menu bar, tabs, status line,
//   modal windows), and draw_menu();
// - DoomRPG.jar: finding and loading it, checking each of its maps, the
//   "new ROMs" window (engine::new_rom on the worker thread) and the
//   "original game's levels" window (import_map);
// - the text helpers hint(), para() and reading_width().
// Threads: only start_build / start_new_rom's worker runs off the UI thread.
// It works on its own copies and talks back through the atomics, the log
// (under log_mu) and the build_* fields, read once build_finished is set.
#include "app.h"
#include "colour.h"
#include "edit.h"
#include "levelblob.h"
#include "engine.h"
#include "projectio.h"

#include "../third_party/imgui/imgui.h"
#include <SDL.h>

#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

static const char *TITLE = "Deimos Editor " EDITOR_VERSION;   // the window title's start

// ---------------------------------------------------------------- undo
// Saves a copy of the level before an edit. The oldest copy goes past 80;
// a new edit makes the redo stack meaningless, so it is emptied.
void App::push_undo()
{
    undo_stack.push_back(proj);
    if (undo_stack.size() > 80) undo_stack.erase(undo_stack.begin());
    redo_stack.clear();
}

// Goes back one step: the current level moves to the redo stack.
void App::undo()
{
    if (undo_stack.empty()) return;
    redo_stack.push_back(proj);
    proj = undo_stack.back();
    undo_stack.pop_back();
    changed(true);   // the art may differ too: drop the cached images
    status = "undone";
}

// Goes forward one step again (the mirror of undo).
void App::redo()
{
    if (redo_stack.empty()) return;
    undo_stack.push_back(proj);
    proj = redo_stack.back();
    redo_stack.pop_back();
    changed(true);
    status = "redone";
}

// Marks the level as changed: unsaved, the preview data to be remade
// (refresh_level_data, next frame), the preview to be redrawn, and with
// art_changed the cached picture textures dropped.
void App::changed(bool art_changed)
{
    unsaved = true;
    level_dirty = true;
    preview_stale = true;
    if (art_changed) clear_images();
}

// Remakes the data the previews need after the level changed (called by
// frame() before the tabs are drawn): geo, the wall pieces for the
// true-colour views (build_geometry, geometry.h), and sms_data, the Master
// System builder's level data (its 16 colours, walls, props) without the
// slow visibility lists. Either can fail on a broken level; preview_err
// then says why (geometry's error first).
void App::refresh_level_data()
{
    if (!level_dirty) return;
    level_dirty = false;
    preview_err.clear();
    try {
        geo = std::make_unique<Geometry>(build_geometry(proj));
    } catch (const std::exception &e) {
        geo.reset();
        preview_err = e.what();
    }
    auto d = std::make_unique<sms::LevelData>();
    BuildOptions o;
    std::string err;
    // the Master System's or the Game Gear's own pictures and palette (the
    // preview's console when it is one of them)
    int pc = std::max(0, std::min((int)NEWROM_CONSOLES.size() - 1, set.preview_console));
    Project pv = proj.for_console(NEWROM_CONSOLES[pc].c == Console::GG ? "gg" : "sms");
    // false: no visibility lists (the preview works out the one view it shows)
    if (sms::level_data(pv, *d, false, o, err)) sms_data = std::move(d);
    else {
        sms_data.reset();
        if (preview_err.empty()) preview_err = err;
    }
    // one "open" flag per door; reset when the number of doors changed
    if (preview_open.size() != proj.lines.size()) preview_open.assign(proj.lines.size(), 0);
}

// ---------------------------------------------------------------- images
SDL_Texture *App::upload(const std::string &key, const std::vector<RGB> &px, const std::vector<u8> *alpha, int w, int h)
{
    auto it = img_cache.find(key);
    if (it != img_cache.end()) return it->second;   // already made
    // a static RGBA texture (ABGR8888 is R, G, B, A in byte order on
    // little-endian machines), alpha-blended, scaled without smoothing
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return nullptr;
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest);
    // the pixels as R, G, B, A bytes
    std::vector<u8> buf((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) {
        buf[i * 4] = px[i].r;
        buf[i * 4 + 1] = px[i].g;
        buf[i * 4 + 2] = px[i].b;
        buf[i * 4 + 3] = alpha ? (*alpha)[i] : 255;
    }
    SDL_UpdateTexture(t, nullptr, buf.data(), w * 4);   // pitch: 4 bytes a pixel
    img_cache[key] = t;
    return t;
}

// the nearest Master System colour; the colour the Mega Drive shows (colour.h)
RGB look_sms(RGB c) { return sms_snap(c); }
RGB look_md(RGB c) { return md_rgb(md_colour(c)); }

SDL_Texture *App::picture_img(const ArtRef &r, bool sprite, int look)
{
    // the cache key: the drawing's name, sprite or not, the look, and the
    // 16 colours as raw bytes (so other colours make another texture)
    std::string key = "pic:" + r.shape + ":" + std::to_string(sprite) + ":" + std::to_string(look);
    for (auto &c : r.pal) key += (char)c.r, key += (char)c.g, key += (char)c.b;
    auto it = img_cache.find(key);
    if (it != img_cache.end()) return it->second;
    Art a = proj.flat(r);   // the drawing with its colours
    std::vector<RGB> px(4096);
    std::vector<u8> al(4096, 255);
    for (int i = 0; i < 4096; i++) {
        if (sprite && a.clear(i)) {   // a sprite's transparent pixel
            al[i] = 0;
            continue;
        }
        RGB c = a.at(i);
        if (look == 1) {
            // Master System: the level's colours, as the console's dithered pairs
            // (the pair byte holds two colour numbers, A in the high nibble
            // and B in the low; they alternate in a checkerboard)
            if (sms_data) {
                u8 pb = sms_data->col.pair(c);
                int x = i % 64, y = i / 64;
                c = sms_data->col.rgb[((x + y) & 1) == 0 ? pb >> 4 : pb & 15];
            } else
                c = look_sms(c);   // no level data (a broken level): just the nearest colour
        } else if (look == 2)
            c = look_md(c);
        px[i] = c;
    }
    return upload(key, px, &al, 64, 64);
}

// Thumbnail of wall texture `id` (sprite = false) or sprite `id`, in its own
// colours; nullptr if there is no such picture.
SDL_Texture *App::art_img(bool sprite, int id)
{
    const auto &m = sprite ? proj.sprites : proj.textures;
    auto a = m.find(id);
    if (a == m.end()) return nullptr;
    return picture_img(a->second, sprite, 0);
}

// The textures are not destroyed here: this frame's ImGui draw lists may
// still use them. frame() destroys the graveyard at the start of the next frame.
void App::clear_images()
{
    for (auto &kv : img_cache)
        if (kv.second) graveyard.push_back(kv.second);
    img_cache.clear();
    preview_stale = true;
}

// Drops the cached images of one drawing (keys contain ":shape:"), after
// it was painted on; other pictures keep theirs.
void App::forget_shape_images(const std::string &shape)
{
    for (auto it = img_cache.begin(); it != img_cache.end();)
        if (it->first.find(":" + shape + ":") != std::string::npos) {
            graveyard.push_back(it->second);
            it = img_cache.erase(it);
        } else
            ++it;
}

// ---------------------------------------------------------------- files
bool App::open_rom(const std::string &path)
{
    Rom r;
    Project p;
    std::string err;
    // the file, then the level kept inside it (map 0 of a campaign ROM)
    if (!r.load(path, err)) {
        status = "Can't open " + path + ": " + err;
        return false;
    }
    if (!editable(r.console)) {   // (rom.h: these consoles' ROMs are made locked)
        status = std::string("The ") + r.console_name() + " version isn't edited in the editor (its ROMs leave out the "
                 "editable level, to save room). Open the Mega Drive, 3DS, Game Boy Color or Game Boy Advance ROM, "
                 "and make the " + r.console_name() + " ROM from it with File > Make a ROM for another console.";
        return false;
    }
    if (!r.read_project(p, err)) {
        status = "Can't open " + path + ": " + err;
        return false;
    }
    rom = std::move(r);
    proj = std::move(p);
    set.add_recent(path);   // first in File > Open recent, and opened at the next start
    set.last_dir = fs::path(path).parent_path().string();
    set.save();
    reset_view(std::string("Opened ") + rom.console_name() + " ROM " + path);
    return true;
}

std::string App::map_label(int m) const
{
    if (m < 0 || m >= (int)rom.maps.size()) return "";
    return rom.maps[m].title + "  (" + rom.maps[m].name + ")";
}

// a campaign ROM: edit another of its maps
// (the caller has already asked about unsaved changes; on failure the
// current map stays)
void App::switch_map(int m)
{
    Project p;
    std::string err;
    int old = rom.map;
    rom.map = m;   // read_project reads the selected map
    if (!rom.read_project(p, err)) {
        rom.map = old;
        status = "Can't open that map: " + err;
        return;
    }
    proj = std::move(p);
    reset_view("Editing " + map_label(m));
}

void App::reset_view(const std::string &how)
{
    have_rom = true;
    unsaved = false;
    undo_stack.clear();   // undo does not cross into another level
    redo_stack.clear();
    clear_images();
    level_dirty = true;
    preview_open.assign(proj.lines.size(), 0);   // all doors closed
    // select the player's start cell and direction, and the first texture,
    // thing type and prop
    sel_x = proj.spawn_x;
    sel_z = proj.spawn_z;
    preview_facing = proj.spawn_f;
    cur_tex = proj.textures.empty() ? 0 : proj.textures.begin()->first;
    cur_def = 0;
    cur_prop = proj.sprites.empty() ? 0 : proj.sprites.begin()->first;
    art.pic = {P_TEXTURE, cur_tex};
    art.tile = -1;
    for (size_t k = 0; k < NEWROM_CONSOLES.size(); k++)       // (the preview: the ROM's console)
        if (NEWROM_CONSOLES[k].c == rom.console) set.preview_console = (int)k;
    preview_stale = true;
    status = how + "  (" + std::to_string(proj.walls.size()) + " walls, " + std::to_string(proj.entities.size()) +
             " things, " + std::to_string(proj.events.size()) + " scripts)";
    // window title: "Deimos Editor 0.9.9 - console - file [- map]"
    std::string t = std::string(TITLE) + " - " + rom.console_name() + " - " + fs::path(rom.path).filename().string();
    if (rom.is_campaign()) t += " - " + map_label(rom.map);
    SDL_SetWindowTitle(win, t.c_str());
}

// File > New map: everything on the map goes, every cell solid, then a 4x4
// room (cells 14..17) dug out with the current texture, the start in it at
// 15,15 facing north. Undoable.
void App::new_map()
{
    push_undo();
    Project p = proj;   // keep the art, entity types and strings
    p.walls.clear();
    p.scenery.clear();
    p.entities.clear();
    p.lines.clear();
    p.events.clear();
    std::fill(p.wall.begin(), p.wall.end(), 1);
    std::fill(p.static_block.begin(), p.static_block.end(), 0);
    int t = cur_tex;
    for (int z = 14; z <= 17; z++)
        for (int x = 14; x <= 17; x++) edit::make_floor(p, x, z, t);   // open the cell, walls round it
    p.spawn_x = 15;
    p.spawn_z = 15;
    p.spawn_f = 0;
    p.name = "New map";
    proj = std::move(p);
    sel_x = 15;
    sel_z = 15;
    changed();
    status = "New map: a 4x4 room. Draw with the Floor tool, add things, then File > Save ROM As.";
}

// Runs the emulator command from the settings (the Master System one for
// the Master System and Game Gear, the "Mega Drive" one for all the others).
// "%f" in the command is replaced by the quoted ROM path; without it the path
// is added at the end. The command runs in the background through the shell.
void App::run_emulator(const std::string &rom_path)
{
    std::string &emu = sms_family(rom.console) ? set.emulator : set.emulator_md;
    if (emu.empty()) {
        status = std::string("Set the ") + rom.console_name() + " emulator command in Help > Settings first";
        return;
    }
    std::string cmd = emu;
    size_t p = cmd.find("%f");
    std::string q = "\"" + rom_path + "\"";
    if (p == std::string::npos) cmd += " " + q;
    else cmd.replace(p, 2, q);
    // don't wait for the emulator: "start" on Windows, "&" elsewhere
#ifdef _WIN32
    cmd = "start \"\" " + cmd;
#else
    cmd += " &";
#endif
    int rc = std::system(cmd.c_str());   // (only says whether the shell could start it)
    status = rc == 0 ? "Started: " + cmd : "Emulator command failed: " + cmd;
}

// ---------------------------------------------------------------- building
void App::add_log(const std::string &s)
{
    std::lock_guard<std::mutex> l(log_mu);   // the worker thread adds lines too
    log.push_back(s);
}

// Save (Ctrl+S), Save as, Save and play (F5) and "put this level into the
// other console's ROM" all come here. target: another ROM to build the
// level into (nullptr: the open ROM).
void App::start_build(const std::string &save_to, bool run_after, const Rom *target, bool locked)
{
    if (building) return;   // one build at a time
    const Rom &dest = target ? *target : rom;
    // the level must fit the destination: the Master System's limits, the
    // campaign's (per map) or the Mega Drive game's single-level ROMs' (levelblob.h)
    std::string err = sms_family(dest.console) ? check_limits(proj)
                      : dest.is_campaign()          ? check_limits_md(proj)
                                                    : check_limits_mdv1(proj);
    if (!err.empty()) {
        status = "Can't build: " + err;
        return;
    }
    if (worker.joinable()) worker.join();   // the last build's thread (already finished)
    building = true;
    build_finished = false;
    cancel = false;
    progress = 0;
    build_target = save_to;
    build_then_run = run_after;
    build_locked = locked;
    {
        std::lock_guard<std::mutex> l(log_mu);
        log.clear();
    }
    requested_tab = 5;   // show the Build tab and its log
    // the thread works on copies, so the user can go on editing
    auto romcopy = std::make_shared<Rom>(target ? *target : rom);
    auto projcopy = std::make_shared<Project>(proj);
    add_log("Building the game ROM image...");
    worker = std::thread([this, romcopy, projcopy, locked]() {
        BuildOptions o;
        o.locked = locked;
        o.threads = set.build_threads;
        o.cancel = &cancel;
        o.log = [this](const std::string &s) { add_log(s); };
        o.progress = [this](float f) { progress = f; };
        BuildStats st;
        std::string e;
        bool ok = romcopy->build(*projcopy, o, st, e);   // rewrite the level-dependent parts
        if (ok && !build_target.empty()) ok = romcopy->save(build_target, e);   // write the file
        // the results, for poll_build; build_finished last, so they are complete when it is seen
        build_ok = ok;
        build_err = e;
        build_stats = st;
        if (ok) build_out = std::make_unique<Rom>(*romcopy);
        build_finished = true;
    });
}

// Once per frame (from frame()): when the worker has finished, takes its
// result on the UI thread. New ROMs: opens the first one. A rebuild of the
// open ROM's console: the rebuilt ROM replaces the open one (a build into
// another console's ROM leaves it alone). Then records the save and, for
// F5, starts the emulator.
void App::poll_build()
{
    if (!building || !build_finished) return;
    worker.join();
    building = false;
    if (!build_ok) {
        status = "Build failed: " + build_err;
        add_log("FAILED: " + build_err);
        build_is_new = false;
        return;
    }
    if (build_is_new) {                 // new ROMs: the first opens (if it is an editable console's)
        build_is_new = false;
        Rom probe;
        std::string e;
        if (probe.load(build_target, e) && editable(probe.console)) open_rom(build_target);
        else status = "Saved " + build_target + " (a locked ROM: play it; it isn't opened here)";
        return;
    }
    // (a locked copy never replaces the open ROM: the editor couldn't build from it again)
    bool same_console = build_out->console == rom.console && !build_locked;
    for (auto &n : build_stats.notes) add_log(n);   // the builder's remarks
    add_log("ROM: " + std::to_string(build_stats.rom_kb) + " KB; " +
            (build_out->is_campaign() ? "the game's maps use " : "the level uses ") + std::to_string(build_stats.used_kb) +
            " KB, " + std::to_string(build_stats.free_kb) + " KB still free");
    if (same_console) {
        rom = std::move(*build_out);
        if (!build_target.empty()) {
            unsaved = false;
            rom.path = build_target;   // Save as: from now on the new file is the open ROM
        }
    }
    build_out.reset();
    if (!build_target.empty()) {
        set.add_recent(build_target);
        set.save();
        status = "Saved " + build_target;
        add_log("Saved " + build_target);
        if (same_console)
            SDL_SetWindowTitle(win, (std::string(TITLE) + " - " + rom.console_name() + " - " +
                                     fs::path(build_target).filename().string()).c_str());
        if (build_then_run) run_emulator(build_target);
    } else
        status = "Build finished";
}

// ---------------------------------------------------------------- main frame
// The whole UI for one frame, called by main.cpp between ImGui::NewFrame and
// ImGui::Render. One full-window ImGui window holds the menu bar, then
// either the start page (no ROM open) or the six tabs, then the status line;
// the modal windows (file dialog, jar windows, confirmations, help) are
// drawn after.
void App::frame()
{
    // textures dropped last frame are no longer in any draw list: free them
    for (SDL_Texture *t : graveyard)
        if (t) SDL_DestroyTexture(t);
    graveyard.clear();
    poll_build();   // a finished build's result
    ImGuiIO &io = ImGui::GetIO();
    // keyboard shortcuts, unless a text box or the file dialog has the keys:
    // Ctrl+Z undo, Ctrl+Y or Ctrl+Shift+Z redo, Ctrl+S save over the open
    // ROM, F5 save and run the emulator
    if (!io.WantTextInput && !fd.active()) {
        bool ctrl = io.KeyCtrl;
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) undo();
        if (ctrl && (ImGui::IsKeyPressed(ImGuiKey_Y) || (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)))) redo();
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S) && have_rom && !building) start_build(rom.path, false);
        if (ImGui::IsKeyPressed(ImGuiKey_F5) && have_rom && !building) start_build(rom.path, true);
    }
    // (note: Ctrl+Shift+Z also matches the Ctrl+Z test above, so it undoes
    // and then redoes in the same frame)
    // one window covering the whole SDL window, no title bar, with a menu bar
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoCollapse);
    draw_menu();
    if (!have_rom) {
        // ---- the start page: what the editor does, Make ROMs / Open, recent ROMs
        ImGui::Spacing();
        para("This editor makes and changes Doom RPG ROMs for the Master System, Game Gear, Mega Drive, SNES (with or "
             "without the SuperFX), 3DS, 32X, Game Boy Color and Game Boy Advance. It contains nothing of the original "
             "game: give it your own copy of the phone game, DoomRPG.jar, and it makes the ROMs from it - the maps, "
             "the art, the texts, the music.");
        ImGui::Spacing();
        if (building) {
            // the first ROMs being made: progress and the last 12 log lines
            ImGui::ProgressBar(progress, ImVec2(reading_width(), 0));
            std::lock_guard<std::mutex> l(log_mu);
            for (size_t i = log.size() > 12 ? log.size() - 12 : 0; i < log.size(); i++) para("%s", log[i].c_str());
            ImGui::Spacing();
        }
        ImGui::BeginDisabled(building);
        if (ImGui::Button("Make ROMs from DoomRPG.jar...")) ask_new_rom();   // the new-ROMs window
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Open a ROM...")) fd.open("Open ROM", FileDialog::OPEN, ROM_EXTS, set.last_dir.empty() ? export_start() : set.last_dir,
                                                    [this](const std::string &f) { request_open(f); });
        ImGui::Spacing();
        para("An editable ROM made here (Mega Drive, 3DS, Game Boy Color or Game Boy Advance; also the Mega Drive game from 0.0.4 on) "
             "keeps an editable copy of its levels inside: open it to change the map, the things in it, the "
             "scripts and texts, the wall textures and the sprites, and save a new ROM. The game code is never "
             "changed.");
        if (!set.recent.empty()) {
            ImGui::Spacing();
            ImGui::Text("Recent:");
            for (auto &r : set.recent)
                if (ImGui::Button(r.c_str())) {
                    request_open(r);
                    break;
                }
        }
    } else {
        refresh_level_data();   // the previews' data, if the level changed
        // ---- the six tabs (requested_tab, if set, selects one this frame)
        if (ImGui::BeginTabBar("tabs")) {
            const char *names[] = {"Map", "Art", "Thing types", "Text & scripts", "Limits", "Build"};
            for (int t = 0; t < 6; t++) {
                ImGuiTabItemFlags fl = requested_tab == t ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(names[t], nullptr, fl)) {
                    switch (t) {
                    case 0: draw_map_tab(); break;
                    case 1: draw_art_tab(); break;
                    case 2: draw_types_tab(); break;
                    case 3: draw_text_tab(); break;
                    case 4: draw_limits(); break;
                    case 5: draw_build_tab(); break;
                    }
                    ImGui::EndTabItem();
                }
            }
            requested_tab = -1;
            ImGui::EndTabBar();
        }
    }
    // status line
    // (at the bottom of the window: a progress bar during builds, the status
    // text, and "unsaved changes" on the right)
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeightWithSpacing());
    ImGui::Separator();
    if (building) {
        ImGui::ProgressBar(progress, ImVec2(200 * g_ui, 0));
        ImGui::SameLine();
    }
    ImGui::TextUnformatted(status.c_str());
    if (unsaved) {
        ImGui::SameLine(ImGui::GetWindowWidth() - 160 * g_ui);
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "unsaved changes");   // orange
    }
    // ---- the windows that float over everything (each draws nothing unless open)
    fd.draw();
    draw_jar_window();
    draw_newrom_window();
    draw_find_jar_window();
    // "Discard changes?": request_open set pending_open because the level is unsaved
    if (!pending_open.empty() && !ImGui::IsPopupOpen("Discard changes?")) ImGui::OpenPopup("Discard changes?");
    if (ImGui::BeginPopupModal("Discard changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("The level has unsaved changes. Open %s anyway?", fs::path(pending_open).filename().string().c_str());
        if (ImGui::Button("Open it, lose the changes")) {
            std::string p = pending_open;
            pending_open.clear();
            ImGui::CloseCurrentPopup();
            open_rom(p);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            pending_open.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    // "Switch map?": the Map menu picked another map while this one is unsaved
    if (pending_map >= 0 && !ImGui::IsPopupOpen("Switch map?")) ImGui::OpenPopup("Switch map?");
    if (ImGui::BeginPopupModal("Switch map?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("This map has unsaved changes. Edit %s anyway?", map_label(pending_map).c_str());
        if (ImGui::Button("Switch, lose the changes")) {
            int m = pending_map;
            pending_map = -1;
            ImGui::CloseCurrentPopup();
            switch_map(m);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            pending_map = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    // "Which map should it replace?": File > Put this level into the other
    // console's ROM picked a campaign ROM; choose its map, then where to save
    if (pending_other && !ImGui::IsPopupOpen("Which map should it replace?"))
        ImGui::OpenPopup("Which map should it replace?");
    if (ImGui::BeginPopupModal("Which map should it replace?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("That ROM holds the whole game. Pick the map this level takes the place of:");
        for (size_t m = 0; m < pending_other->maps.size(); m++) {
            auto &mi = pending_other->maps[m];
            if (ImGui::Selectable((mi.title + "  (" + mi.name + ")").c_str(), (int)m == pending_other->map))
                pending_other->map = (int)m;   // the map the build will replace
        }
        if (ImGui::Button("Replace it")) {
            auto other = pending_other;   // kept alive by the callback's copy of the pointer
            pending_other.reset();
            ImGui::CloseCurrentPopup();
            // (the save dialog always offers ".md", whatever console the
            // campaign ROM is for)
            fd.open("Save the new ROM as", FileDialog::SAVE, ".md", export_start(),
                    [this, other](const std::string &out) { start_build(out, false, other.get()); }, "my_level.md");
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            pending_other.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    draw_help();   // the How to use and Settings windows (ui_data.cpp)
    ImGui::End();
}

// The menu bar: File, Map (campaign ROMs only), Edit, Help, and on the right
// the open ROM's console and size. Menu items that need a ROM, or no build
// running, are greyed out otherwise. Most file actions go through the file
// dialog, whose callback does the work later.
void App::draw_menu()
{
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New ROMs from the original game (DoomRPG.jar)...", "", false, !building)) ask_new_rom();
        if (ImGui::MenuItem("Open ROM...", "")) {
            // start in the last folder used, else the export folder
            fd.open("Open ROM", FileDialog::OPEN, ROM_EXTS, set.last_dir.empty() ? export_start() : set.last_dir,
                    [this](const std::string &f) { request_open(f); });
        }
        if (ImGui::BeginMenu("Open recent", !set.recent.empty())) {
            for (auto &r : set.recent)
                if (ImGui::MenuItem(r.c_str())) {
                    request_open(r);
                    break;
                }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        // Save: rebuild over the open ROM's own file
        if (ImGui::MenuItem("Save ROM", "Ctrl+S", false, have_rom && !building)) start_build(rom.path, false);
        if (ImGui::MenuItem("Save ROM as...", "", false, have_rom && !building))
            fd.open("Save ROM as", FileDialog::SAVE, rom_ext(), export_start(),
                    [this](const std::string &f) {
                        set.last_dir = fs::path(f).parent_path().string();
                        start_build(f, false);
                    },
                    "my_level" + rom_ext());
        if (ImGui::MenuItem("Save and play", "F5", false, have_rom && !building)) start_build(rom.path, true);
        // a copy without the editable level: smaller, more room for maps, but
        // the editor can't open it (the open ROM stays as it is)
        if (ImGui::MenuItem("Save a locked copy...", "", false, have_rom && !building))
            fd.open("Save a locked copy (it can't be opened in the editor)", FileDialog::SAVE, rom_ext(), export_start(),
                    [this](const std::string &f) {
                        set.last_dir = fs::path(f).parent_path().string();
                        start_build(f, false, nullptr, true);
                    },
                    "my_level_locked" + rom_ext());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Leaves out the ROM's editable copy of the level (about 250 KB on a whole-game ROM): more room "
                              "for maps. Keep the unlocked ROM: the locked one can't be opened here.");
        // a new ROM for another console, of the maps open here (every map of a
        // campaign ROM; the level being edited for the Master System and Game
        // Gear); the graphics and sounds from the jar; locked for the consoles
        // the editor doesn't edit
        if (ImGui::BeginMenu("Make a ROM for another console", have_rom && !building)) {
            for (auto &nc : NEWROM_CONSOLES) {
                bool have = engine::available(nc.c);   // (its engine file is beside the editor)
                std::string label = std::string(nc.label) + (!have ? " (no engine)" : editable(nc.c) ? "..." : " (locked)...");
                if (ImGui::MenuItem(label.c_str(), "", false, (bool)jar && have)) {
                    Console c = nc.c;
                    std::string base = fs::path(nc.file).stem().string(), ext = fs::path(nc.file).extension().string();
                    fd.open(std::string("Save the ") + console_name(c) + " ROM", FileDialog::SAVE, ext, export_start(),
                            [this, c](const std::string &f) {
                                set.last_dir = fs::path(f).parent_path().string();
                                start_make_rom(c, f);
                            },
                            "deimos_" + base + ext);
                }
            }
            if (!jar) ImGui::TextDisabled("(needs your DoomRPG.jar: File > Your copy of Doom RPG)");
            ImGui::EndMenu();
        }
        // another ROM (any console) gets this level: pick it, then (for a
        // campaign ROM) the map to replace, then where to save the result
        if (ImGui::MenuItem("Put this level into the other console's ROM...", "", false, have_rom && !building))
            fd.open("Pick the other console's ROM (it is not changed)", FileDialog::OPEN, ROM_EXTS,
                    set.last_dir, [this](const std::string &f) {
                        auto other = std::make_shared<Rom>();
                        std::string err;
                        if (!other->load(f, err)) {
                            status = err;
                            return;
                        }
                        if (!editable(other->console)) {   // (made new, locked: File > Make a ROM for another console)
                            status = std::string("The ") + other->console_name() + " version is made new, not edited: "
                                     "use File > Make a ROM for another console";
                            return;
                        }
                        if (other->is_campaign()) {
                            pending_other = other;   // frame() asks which map next
                            return;
                        }
                        std::string ext = console_ext(other->console);
                        fd.open("Save the new ROM as", FileDialog::SAVE, ext, export_start(),
                                [this, other](const std::string &out) { start_build(out, false, other.get()); },
                                "my_level" + ext);
                    });
        ImGui::Separator();
        if (ImGui::MenuItem("New map (keeps art and thing types)", "", false, have_rom)) new_map();
        if (ImGui::MenuItem("Import a level from the original game (DoomRPG.jar)...", "", false, have_rom))
            with_jar([this]() { show_jar = true; });
        if (ImGui::MenuItem("Export level to folder...", "", false, have_rom))
            fd.open("Export to folder", FileDialog::FOLDER, "", set.last_dir, [this](const std::string &d) {
                std::string err;
                status = export_project(proj, d, err) ? "Exported to " + d + " (project.json + PNGs)" : "Export failed: " + err;
            });
        // a folder written by Export (project.json + PNGs) replaces the level; undoable
        if (ImGui::MenuItem("Import level from folder...", "", false, have_rom))
            fd.open("Import from folder", FileDialog::FOLDER, "", set.last_dir, [this](const std::string &d) {
                Project p;
                std::string err;
                if (!import_project(d, p, err)) {
                    status = "Import failed: " + err;
                    return;
                }
                push_undo();
                proj = std::move(p);
                changed(true);
                status = "Imported " + d;
            });
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) want_quit = true;   // main.cpp asks first if unsaved
        ImGui::EndMenu();
    }
    // the Map menu: a campaign ROM's maps, the one being edited ticked
    if (have_rom && rom.is_campaign() && ImGui::BeginMenu("Map")) {
        ImGui::TextDisabled("The game's maps, in order:");
        for (size_t m = 0; m < rom.maps.size(); m++)
            if (ImGui::MenuItem(map_label((int)m).c_str(), "", (int)m == rom.map, !building) && (int)m != rom.map) {
                if (unsaved) pending_map = (int)m;   // ask first (frame()'s "Switch map?")
                else switch_map((int)m);
            }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undo_stack.empty())) undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !redo_stack.empty())) redo();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("How to use")) show_help = true;
        if (ImGui::MenuItem("Settings / about")) show_about = true;
        ImGui::EndMenu();
    }
    // right-aligned (at a fixed distance from the right edge): which ROM is open
    if (have_rom) {
        ImGui::SameLine(ImGui::GetWindowWidth() - (rom.is_campaign() ? 470 : 260) * g_ui);
        if (rom.is_campaign()) {
            ImGui::TextDisabled("%s - %s ROM, %d KB", rom.maps[rom.map].title.c_str(), rom.console_name(),
                                (int)(rom.data.size() / 1024));
        } else
            ImGui::TextDisabled("%s ROM, %d KB", rom.console_name(), (int)(rom.data.size() / 1024));
    }
    ImGui::EndMenuBar();
}

void App::request_open(const std::string &path)
{
    if (unsaved && have_rom) pending_open = path;   // frame() asks "Discard changes?"
    else open_rom(path);
}

// By extension (any case): .jar / .zip is the original game (its levels
// window opens, once a ROM is open); .png is refused with a pointer to the
// Art tab; anything else is tried as a ROM.
void App::dropped(const std::string &path)
{
    std::string ext = fs::path(path).extension().string();
    for (auto &c : ext) c = (char)tolower((unsigned char)c);
    if (ext == ".jar" || ext == ".zip") {
        if (!have_rom) status = "Open a ROM first, then drop the original game on the window to bring in its levels";
        else open_jar(path);
    } else if (ext == ".png" && have_rom) {
        status = "To bring in a picture, use Import PNG in the Art tab";
    } else
        request_open(path);
}

// ---------------------------------------------------------------- the original game's maps
void App::open_jar(const std::string &path)
{
    if (load_jar(path)) show_jar = true;
}

// Each map is converted in full (import_map, jar.h) just to count it and
// check it against the limits, so loading a jar takes a moment.
bool App::load_jar(const std::string &path)
{
    auto j = std::make_unique<Jar>();
    std::string err;
    if (!j->open(path, err)) {
        status = err;
        return false;
    }
    jar_maps.clear();
    for (auto &m : j->maps()) {
        GameMap g;
        g.name = m;
        g.title = Jar::map_title(m);   // e.g. "UAC Base"
        Project p;
        if (!import_map(*j, m, p, err, &g.notes)) {   // convert the map to a level
            g.verdict = err;
        } else {
            std::string sms = check_limits(p);   // the Master System's limits
            g.fits_sms = sms.empty();
            g.sms_verdict = sms;
            // "fits" is judged against the open ROM: a campaign ROM's limits,
            // else the Master System's (also used for a single-level Mega
            // Drive ROM and when no ROM is open)
            std::string lim = rom.is_campaign() ? check_limits_md(p) : sms;
            g.fits = lim.empty();
            // open cells: 1024 cells minus the solid ones
            g.verdict = g.fits ? "fits: " + std::to_string(p.entities.size()) + " things, " +
                                     std::to_string(p.events.size()) + " scripts, " + std::to_string(1024 - (int)std::count(p.wall.begin(), p.wall.end(), 1)) + " open cells"
                               : "doesn't fit: " + lim;
        }
        jar_maps.push_back(g);
    }
    jar = std::move(j);
    set.jar = path;   // remembered (shown in the new-ROMs log)
    set.save();
    return true;
}

// ---------------------------------------------------------------- new ROMs from DoomRPG.jar
void App::newrom_reset()
{
    newrom_maps = engine::campaign_maps(*jar);   // the maps in playing order
    newrom_sel.assign(newrom_maps.size(), 1);
    // the console ticks survive between openings; the first time only the Mega Drive
    if (newrom_cons.size() != NEWROM_CONSOLES.size()) {
        newrom_cons.assign(NEWROM_CONSOLES.size(), 0);
        for (size_t k = 0; k < NEWROM_CONSOLES.size(); k++)
            if (NEWROM_CONSOLES[k].c == Console::MD) newrom_cons[k] = 1;
    }
}

void App::ask_new_rom()
{
    with_jar([this]() {
        newrom_reset();
        show_newrom = true;
    });
}

// ---------------------------------------------------------------- the folders beside the program
// base_dir: the program's own folder (SDL_GetBasePath), else the current
// folder; import/ and export/ in it are created if missing (errors ignored).
void App::init_folders()
{
    char *b = SDL_GetBasePath();
    base_dir = b ? b : "";
    SDL_free(b);   // SDL allocated the string
    if (base_dir.empty()) base_dir = fs::current_path().string();
    import_dir = (fs::path(base_dir) / "import").string();
    export_dir = (fs::path(base_dir) / "export").string();
    std::error_code ec;
    fs::create_directories(import_dir, ec);
    fs::create_directories(export_dir, ec);
}

// The export folder if it exists, else the last folder used.
std::string App::export_start() const
{
    std::error_code ec;
    return fs::is_directory(export_dir, ec) ? export_dir : set.last_dir;
}

// dir/file, or "dir/stem (2).ext", "(3)"... the first that doesn't exist yet.
std::string App::unused_name(const std::string &dir, const std::string &file) const
{
    std::error_code ec;
    fs::path p = fs::path(dir) / file;
    std::string stem = p.stem().string(), ext = p.extension().string();
    for (int k = 2; fs::exists(p, ec); k++) p = fs::path(dir) / (stem + " (" + std::to_string(k) + ")" + ext);
    return p.string();
}

// At start (main.cpp): the first .jar in the import folder that is Doom RPG
// is loaded; with none, the "Your copy of Doom RPG" window opens.
void App::find_jar_at_start()
{
    std::error_code ec;
    if (fs::is_directory(import_dir, ec))
        for (auto &e : fs::directory_iterator(import_dir, ec)) {
            std::string ext = e.path().extension().string();
            for (auto &c : ext) c = (char)tolower((unsigned char)c);
            if (ext != ".jar") continue;
            if (load_jar(e.path().string()) && jar_ok()) {
                status = "Using " + e.path().filename().string() + " from the import folder.";
                return;
            }
            jar.reset();   // not Doom RPG (or no maps): try the next
        }
    show_find_jar = true;
}

// Runs `then` now if a jar is loaded; otherwise asks for the jar with the
// file dialog, loads it, copies it into the import folder (so it is found at
// the next start) and then runs `then`. A file that isn't Doom RPG reopens
// the "Your copy of Doom RPG" window instead.
void App::with_jar(std::function<void()> then)
{
    if (jar_ok()) {
        then();
        return;
    }
    fd.open("Where is your DoomRPG.jar?", FileDialog::OPEN, ".jar .zip", set.last_dir, [this, then](const std::string &f) {
        if (!load_jar(f) || !jar_ok()) {
            // (status then holds load_jar's error, if it set one)
            jar.reset();
            status = f + " is not Doom RPG (DoomRPG.jar, the phone game) - " + status;
            show_find_jar = true;
            return;
        }
        // a copy in the import folder: found there next time
        std::error_code ec;
        fs::path dst = fs::path(import_dir) / fs::path(f).filename();
        // (not when the file already is the one in the import folder)
        if (fs::is_directory(import_dir, ec) && !fs::equivalent(fs::path(f), dst, ec)) {
            fs::copy_file(f, dst, fs::copy_options::overwrite_existing, ec);
            status = ec ? "Using " + f + " (it couldn't be copied into the import folder: " + ec.message() + ")"
                        : "Using " + f + "; a copy is in the import folder now, so it is found next time.";
        }
        then();
    });
}

// The "Your copy of Doom RPG" window: explains why the jar is needed, where
// the import folder is, and offers to find it (with_jar) or not now.
void App::draw_find_jar_window()
{
    if (show_find_jar) {
        ImGui::OpenPopup("Your copy of Doom RPG");   // show_* flags open a popup once
        show_find_jar = false;
    }
    ImGui::SetNextWindowSize(ImVec2(640 * ui, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Your copy of Doom RPG", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    para("The editor makes everything from your own copy of the phone game, DoomRPG.jar: the maps, the art, the "
         "texts and the sounds. There isn't one in the import folder:");
    ImGui::TextUnformatted(import_dir.c_str());
    para("Show the editor where it is, and it puts a copy in the import folder, so it finds it next time. (Or "
         "put it there yourself and start the editor again.)");
    ImGui::Spacing();
    if (ImGui::Button("Find DoomRPG.jar...")) {
        ImGui::CloseCurrentPopup();
        with_jar([]() {});   // just load it; nothing to do after
    }
    ImGui::SameLine();
    if (ImGui::Button("Not now")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// A whole-game console takes any maps. The Master System and Game Gear take
// exactly one, and only one that fits their limits (checked by load_jar).
std::string App::newrom_why_not(size_t k) const
{
    if (!engine::available(NEWROM_CONSOLES[k].c))   // (its engine file isn't beside the editor)
        return std::string("no engine: ") + engine::file_name(NEWROM_CONSOLES[k].c) + " is not in " + engine::folder();
    if (NEWROM_CONSOLES[k].campaign) return "";
    std::vector<size_t> ticked;
    for (size_t i = 0; i < newrom_sel.size(); i++)
        if (newrom_sel[i]) ticked.push_back(i);
    if (ticked.size() != 1) return "holds one map: tick just one";
    for (auto &g : jar_maps)
        if (g.name == newrom_maps[ticked[0]])
            return g.fits_sms ? "" : g.title + " doesn't fit: " + g.sms_verdict;
    return "that map isn't in the jar";
}

// The "New ROMs from the original game" window: maps on the left (ticked
// in newrom_sel), consoles on the right (newrom_cons; greyed out with the
// reason when newrom_why_not says so), and "Make the ROMs".
void App::draw_newrom_window()
{
    if (show_newrom) {
        ImGui::OpenPopup("New ROMs from the original game");
        show_newrom = false;
    }
    ImGui::SetNextWindowSize(ImVec2(980 * ui, 600 * ui), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("New ROMs from the original game", nullptr)) return;
    para("The editor makes each ROM from your DoomRPG.jar: the maps, the status bar, the fonts, the title screen, "
         "and on the consoles that have them the music and the sounds. Tick the maps to put in, and every console "
         "to make a ROM for: each gets its own, in the export folder. Then the first one opens, ready to change.");
    ImGui::Separator();
    // the two columns fill the window above the bottom row of buttons
    ImGui::BeginChild("which", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.6f), false);
    ImGui::BeginTable("cols", 2, ImGuiTableFlags_BordersInnerV);
    ImGui::TableNextColumn();
    // ---- the maps
    ImGui::TextUnformatted("Maps, in the order they are played:");
    if (ImGui::Button("The whole game")) newrom_sel.assign(newrom_maps.size(), 1);
    ImGui::SameLine();
    if (ImGui::Button("UAC Base only")) {
        newrom_sel.assign(newrom_maps.size(), 0);
        if (!newrom_sel.empty()) newrom_sel[0] = 1;
    }
    int n_maps = 0;   // maps ticked
    for (size_t i = 0; i < newrom_maps.size(); i++) {
        bool v = newrom_sel[i];
        // the label shows the title; "##name" keeps each checkbox's ImGui id unique
        if (ImGui::Checkbox((Jar::map_title(newrom_maps[i]) + "##" + newrom_maps[i]).c_str(), &v)) newrom_sel[i] = v;
        n_maps += v;
        // beside it, in green or red, whether it fits the Master System
        for (auto &g : jar_maps)
            if (g.name == newrom_maps[i]) {
                ImGui::SameLine(250 * ui);
                ImGui::TextColored(g.fits_sms ? ImVec4(0.6f, 1, 0.6f, 1) : ImVec4(1, 0.45f, 0.4f, 1),
                                   "%s", g.fits_sms ? "fits the Master System" : "too big for the Master System");
            }
    }
    hint("A map's exit to one that isn't in the ROM ends the game. The Master System and the Game Gear hold one map, "
         "within their limits.");
    ImGui::TableNextColumn();
    // ---- the consoles
    ImGui::TextUnformatted("Consoles:");
    int n_cons = 0;   // consoles ticked (and able to take the maps)
    for (size_t k = 0; k < NEWROM_CONSOLES.size(); k++) {
        std::string why = newrom_why_not(k);
        bool v = newrom_cons[k] && why.empty();   // shown unticked while it can't, but the tick is kept
        ImGui::BeginDisabled(!why.empty());
        if (ImGui::Checkbox(NEWROM_CONSOLES[k].label, &v)) newrom_cons[k] = v;
        ImGui::EndDisabled();
        if (why.empty())   // the engine's version and the file name the ROM gets
            hint("version %s: %s", engine::version(NEWROM_CONSOLES[k].c).c_str(), NEWROM_CONSOLES[k].file);
        else
            hint("%s", why.c_str());
        n_cons += v;
    }
    ImGui::EndTable();
    ImGui::EndChild();
    ImGui::BeginDisabled(n_maps == 0 || n_cons == 0 || building);
    if (ImGui::Button(n_cons > 1 ? "Make the ROMs" : "Make the ROM")) {
        ImGui::CloseCurrentPopup();
        start_new_rom(export_start(), true);   // into the export folder, each under its usual name
    }
    ImGui::SameLine();
    ImGui::TextDisabled("into %s", export_start().c_str());
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// Makes a ROM for console c of the maps open here (engine::make_rom_for) on
// the worker thread and saves it as out; poll_build then reports it (it is
// opened only if it is an editable console's).
void App::start_make_rom(Console c, const std::string &out)
{
    if (building || !jar) return;
    if (worker.joinable()) worker.join();
    building = true;
    build_finished = false;
    build_is_new = true;
    cancel = false;
    progress = 0;
    build_target = out;
    build_then_run = false;
    {
        std::lock_guard<std::mutex> l(log_mu);
        log.clear();
    }
    requested_tab = 5;
    add_log(std::string("Making the ") + console_name(c) + " ROM " + out + "...");
    status = "Making the ROM... (see the Build tab)";
    auto src = std::make_shared<Rom>(rom);
    auto cur = std::make_shared<Project>(proj);
    worker = std::thread([this, c, out, src, cur]() {
        BuildOptions o;
        o.threads = set.build_threads;
        o.cancel = &cancel;
        o.log = [this](const std::string &s) { add_log(s); };
        o.progress = [this](float f) { progress = f; };
        Rom r;
        BuildStats st;
        std::string e;
        bool ok = engine::make_rom_for(c, *jar, *src, *cur, r, o, st, e);
        if (ok) ok = r.save(out, e);
        if (ok) add_log("Saved " + out + (editable(c) ? "" : " (locked)"));
        build_ok = ok;
        build_err = e;
        build_stats = st;
        build_finished = true;
    });
}

// Makes a ROM for each ticked console that can take the ticked maps, one
// after another on the worker thread (engine::new_rom builds the ROM from
// the jar with the console's engine file). The first failure stops the
// rest. poll_build then opens the first ROM. build_stats ends up holding the
// last console's figures.
void App::start_new_rom(const std::string &out, bool folder)
{
    if (building || !jar) return;
    std::vector<std::string> maps;   // the ticked maps, in playing order
    for (size_t i = 0; i < newrom_maps.size(); i++)
        if (newrom_sel[i]) maps.push_back(newrom_maps[i]);
    std::vector<std::pair<Console, std::string>> jobs;     // each console, its file
    for (size_t k = 0; k < NEWROM_CONSOLES.size(); k++)
        if (newrom_cons[k] && newrom_why_not(k).empty())
            jobs.push_back({NEWROM_CONSOLES[k].c, folder ? unused_name(out, NEWROM_CONSOLES[k].file) : out});
    if (jobs.empty() || maps.empty()) return;
    if (worker.joinable()) worker.join();
    building = true;
    build_finished = false;
    build_is_new = true;
    cancel = false;
    progress = 0;
    build_target = jobs[0].second;   // the ROM poll_build opens
    build_then_run = false;
    {
        std::lock_guard<std::mutex> l(log_mu);
        log.clear();
    }
    requested_tab = 5;
    add_log("Making " + std::to_string(jobs.size()) + (jobs.size() > 1 ? " new ROMs" : " new ROM") + " from " +
            set.jar + "...");
    status = jobs.size() > 1 ? "Making the ROMs... (see the Build tab)" : "Making the ROM... (see the Build tab)";
    worker = std::thread([this, jobs, maps]() {
        BuildOptions o;
        o.threads = set.build_threads;
        o.cancel = &cancel;
        o.log = [this](const std::string &s) { add_log(s); };
        bool ok = true;
        std::string e;
        BuildStats st;
        for (size_t j = 0; j < jobs.size() && ok; j++) {
            Console c = jobs[j].first;
            size_t n = jobs.size();
            // the overall bar: job j's progress f within its 1/n share
            o.progress = [this, j, n](float f) { progress = ((float)j + f) / (float)n; };
            add_log(std::string("---- ") + console_name(c) + ": " + jobs[j].second);
            Rom r;
            std::vector<std::string> notes;
            // (reads the shared jar from this thread; the UI does not change
            // it while building is set)
            ok = engine::new_rom(c, *jar, maps, r, o, st, e, &notes);
            for (auto &note : notes) add_log("note: " + note);
            if (ok) ok = r.save(jobs[j].second, e);
            if (ok) add_log("Saved " + jobs[j].second);
            else e = std::string(console_name(c)) + ": " + e;
        }
        build_ok = ok;
        build_err = e;
        build_stats = st;
        build_finished = true;
    });
}

// The "original game's levels" window (File > Import a level from the
// original game, or a jar dropped on the window): each map of the jar with
// its verdict and notes, and an Open button that converts it into the level
// being edited (undoable).
void App::draw_jar_window()
{
    if (show_jar) {
        ImGui::OpenPopup("The original game's levels");
        show_jar = false;
    }
    ImGui::SetNextWindowSize(ImVec2(760 * ui, 520 * ui), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("The original game's levels", nullptr)) return;
    para("Pick a level to bring in. It replaces the level you are editing (Undo brings it back), "
                       "with the game's own art, thing types and texts. Levels marked in red have more than the "
                       "consoles can hold; you can still open them and cut them down to size. Saving checks the "
                       "rest (very big open areas can show too much at once for the Master System).");
    ImGui::Separator();
    ImGui::BeginChild("maps", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.5f), false);
    for (auto &g : jar_maps) {
        ImGui::PushID(g.name.c_str());
        if (ImGui::Button("Open")) {
            Project p;
            std::string err;
            std::vector<std::string> notes;
            // the last argument: convert it as a campaign map (for a
            // whole-game ROM), else as a single level
            if (import_map(*jar, g.name, p, err, &notes, md_family(rom.console) && rom.is_campaign())) {
                push_undo();
                proj = std::move(p);
                changed(true);
                // as reset_view does, but keeping undo: start cell, doors closed, first texture
                sel_x = proj.spawn_x;
                sel_z = proj.spawn_z;
                preview_facing = proj.spawn_f;
                preview_open.assign(proj.lines.size(), 0);
                cur_tex = proj.textures.empty() ? 0 : proj.textures.begin()->first;
                cur_def = 0;
                art.pic = {P_TEXTURE, cur_tex};
                status = "Opened " + g.title + " from the original game" +
                         (g.fits ? "" : " - it has more than the console allows (see the Limits tab)");
                requested_tab = g.fits ? 0 : 4;   // the Map tab, or Limits if too big
                ImGui::CloseCurrentPopup();
            } else
                status = err;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(g.title.c_str());
        ImGui::SameLine(260 * ui);   // the verdict and notes in a column 260 (scaled) pixels in
        ImGui::TextColored(g.fits ? ImVec4(0.6f, 1, 0.6f, 1) : ImVec4(1, 0.45f, 0.4f, 1), "%s", g.verdict.c_str());
        for (auto &n : g.notes) {
            ImGui::SetCursorPosX(260 * ui);
            ImGui::TextDisabled("%s", n.c_str());
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ---------------------------------------------------------------- text helpers
float reading_width() { return 500 * g_ui; }   // about 65 letters a line

// A greyed note (the "disabled text" colour), formatted like printf (longer
// than 2047 bytes is cut), wrapped at the panel's edge or reading_width(),
// whichever comes first.
void hint(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(ImGui::GetContentRegionAvail().x, reading_width()));
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// A paragraph of normal text, printf-style (up to 4095 bytes), wrapped the same way.
void para(const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(ImGui::GetContentRegionAvail().x, reading_width()));
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
}
