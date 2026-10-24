/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// The editor window: state shared by all the panels.
// Licence: GPL-2.0-or-later.
//
// Overview for a newcomer:
// - There is one App object (made in main.cpp). main.cpp owns the SDL window
//   and the Dear ImGui context, and calls App::frame() once per screen
//   refresh. frame() draws everything: the menu bar, the six tabs, the status
//   line and any modal windows.
// - The member functions are spread over several files:
//     app.cpp      undo, image cache, opening/saving projects, making ROMs,
//                  menus, the "new project / make ROMs" and "original
//                  game's levels" windows, and the text helpers
//                  hint()/para();
//     ui_map.cpp   the Map tab (tools, canvas, inspector, view preview);
//     ui_art.cpp   the Art tab (the paint program);
//     ui_data.cpp  the Thing types, Text & scripts, Limits, Build tabs and the
//                  Help / Settings windows;
//     scripts.cpp  the script editor (another file, not documented here).
// - The editor edits a project (App::doc, a ProjectFile, projectfile.h; a
//   .deimos file): the maps of the whole game, in play order, or a single
//   level. The map being edited (doc.maps[cur_map]) is App::proj, a Project
//   (project.h): "the level", i.e. one map with its walls, things, scripts,
//   texts and art; store_map() puts it back into doc. Saving writes the
//   project file; ROMs are made from the project on a worker thread
//   (start_project_roms / poll_build) into the export folder.
// - Every console but the Master System and Game Gear holds all of a
//   project's maps in one ROM; the Master System and Game Gear hold one
//   map, the one being edited.
// - "Picture": a 64x64 drawing (palette indices) plus its own 16 colours
//   (ArtRef in project.h). Several pictures can share one drawing ("shape").
#pragma once
#include <functional>
#include "build.h"
#include "filedialog.h"
#include "geometry.h"
#include "jar.h"
#include "project.h"
#include "projectfile.h"
#include "rom.h"
#include "settings.h"
#include "sms_build.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

// SDL types, declared here so this header does not need SDL.h
struct SDL_Renderer;
struct SDL_Window;
struct SDL_Texture;

// The Map tab's tools, in the order of the tool panel; keys 1-9 pick them
// (ui_map.cpp). T_COUNT is the number of tools.
//   T_SELECT pick a cell; T_WALL make a cell solid; T_FLOOR open a cell
//   (walls are put around it); T_PAINT change wall textures; T_DOOR place a
//   door or sliding wall; T_THING place a thing; T_PROP place a prop or wall
//   decal; T_START move the player's start; T_SCRIPT attach a script to a cell.
enum Tool { T_SELECT, T_WALL, T_FLOOR, T_PAINT, T_DOOR, T_THING, T_PROP, T_START, T_SCRIPT, T_COUNT };
// The Art tab's tools: pencil, line, box, filled box, flood fill, pick colour.
enum ArtTool { A_PENCIL, A_LINE, A_RECT, A_FILLRECT, A_FILL, A_PICK, A_COUNT };

// which picture the art editor shows
// P_TEXTURE: a wall texture (Project::textures); P_SPRITE: a thing or prop
// sprite (Project::sprites); P_BORDER: the Master System's border around the
// view (Project::border); P_WEAPON: one frame of the player's weapon
// (Project::weapons, N_WEAPON_FRAMES of them).
enum PicKind { P_TEXTURE, P_SPRITE, P_BORDER, P_WEAPON };
// Names one picture: its kind and its number. Ordered so it can key a std::map.
struct PicId {
    int kind = P_TEXTURE;
    int id = 0;   // texture id, sprite tile, weapon frame
    bool operator==(const PicId &o) const { return kind == o.kind && id == o.id; }
    bool operator<(const PicId &o) const { return kind != o.kind ? kind < o.kind : id < o.id; }
};

// The Art tab's state, kept between frames (App::art).
struct ArtEditState {
    PicId pic;                   // the picture being edited
    int fg = 1, bg = 0;          // palette indices (CLEAR = transparent)
                                 // fg is drawn with the left button, bg with the right
    int tool = A_PENCIL;         // an ArtTool
    bool drawing = false;        // a mouse button is held down on the picture
    int sx = 0, sy = 0;          // line/rect start (pixels)
    int tile = -1;               // selected 8x8 tile (index = ty * 8 + tx), -1 = none
    bool tile_clip_full = false; // the tile clipboard holds a copied tile
    u8 tile_clip[64] = {};       // the tile clipboard: 8x8 palette indices, row by row
    std::string view_pal;        // "" = its own colours, else the picture whose colours to try
    int look = 0;                // 0 own colours, 1 Master System, 2 Mega Drive
    int edit_colour = -1;        // palette entry being edited (popup)
    // "Edit for": -1 every console (the shared pictures), else a console
    // (an index into CONSOLE_KEYS, the Console order): its own versions
    int console = -1;
};

// The whole editor: the open project and map, the UI state of every tab,
// the image cache and the background build. One instance, made by main.cpp.
class App {
public:
    SDL_Window *win = nullptr;   // the main window (main.cpp makes it)
    SDL_Renderer *ren = nullptr; // its SDL renderer; textures are made with it
    Settings set;                // the .ini file's settings (settings.h)
    float ui = 1.0f;             // size of everything: the display's scale x the "text size" setting
                                 // (main.cpp keeps the global g_ui equal to it)
    float dpi_scale = 1.0f;      // what the display asks for (shown in Settings)
    bool fonts_dirty = false;    // rebuild the font and sizes before the next frame
    // something to do once the user says the unsaved changes may go (frame()'s
    // "Discard changes?"), and what it is, for the question
    std::function<void()> pending_discard;
    std::string pending_what;
    // Runs `then` now, or (with unsaved changes) once the user agrees to lose them.
    void ask_discard(const std::string &what, std::function<void()> then);
    // Edits map m of the project: the current one is kept in doc first.
    void switch_map(int m);
    // After a new project or map is loaded into proj: clears undo and the
    // image cache, selects the start cell, sets the window title and the
    // status line to `how`.
    void reset_view(const std::string &how);
    // "Title  (name)" of map m of the project, "" if out of range.
    std::string map_label(int m) const;
    // Opens a project, or asks first if there are unsaved changes.
    void request_open(const std::string &path);
    // A file dropped on the window (main.cpp): a project, a jar or a PNG.
    void dropped(const std::string &path);
    ProjectFile doc;             // the open project (the map being edited is in proj)
    int cur_map = 0;             // the map being edited: an index into doc.maps
    std::string doc_path;        // the project's file ("" until it is saved)
    bool have_project = false;   // false until a project is open: frame() shows the start page
    Project proj;                // the level being edited
    bool unsaved = false;        // the project changed since it was opened or saved
    // Puts proj back into doc (and its weapon into every map: they share it).
    void store_map();
    bool want_quit = false;      // File > Quit: main.cpp's loop ends
    std::string status;          // the status line at the bottom of the window

    // undo
    // Whole copies of proj, newest at the back. The undo stack keeps at most 80.
    std::vector<Project> undo_stack, redo_stack;
    void push_undo();            // call before an edit: saves proj, clears redo
    void undo();
    void redo();
    void changed(bool art = false);   // call after every edit
                                      // art = true also drops the cached images

    // map view
    int tool = T_SELECT;         // the Map tab's tool (a Tool)
    int sel_x = -1, sel_z = -1;  // the selected cell, -1 = none
    int preview_facing = 0;      // the preview's direction: 0 north, 1 east, 2 south, 3 west
    float pan_x = 20, pan_y = 20;   // map canvas scroll: screen pixels from the canvas's corner to cell 0,0
    // the tool panel's choices: wall texture id, thing type (index into
    // proj.defs), prop sprite id, door kind (an edit::DoorKind)
    int cur_tex = 0, cur_def = 0, cur_prop = 0, door_kind = 0;
    bool prop_blocks = true;     // new props block movement
    bool dragging = false;       // the left button went down on the map and is still held
    int last_paint_cell = -1;    // the cell a drag last changed (so each cell is done once)
    std::vector<u8> preview_open;   // doors drawn open in the preview
                                    // one entry per proj.lines slot, nonzero = open

    ArtEditState art;            // the Art tab's state

    // level data for the previews (recomputed after changes)
    std::unique_ptr<sms::LevelData> sms_data;   // the Master System builder's level data (its colours, walls, props)
    std::unique_ptr<Geometry> geo;              // wall pieces and decals for the other consoles' previews
    std::string preview_err;     // why the level data could not be made ("" = fine)
    bool level_dirty = true;     // proj changed: remake sms_data and geo before the next draw
    void refresh_level_data();

    // graphics cache
    // SDL textures by a key naming what they show (picture, colours, look);
    // cleared when the art changes
    std::map<std::string, SDL_Texture *> img_cache;
    // A 64x64 texture of a picture: own colours (look 0), the Master System's
    // (1) or the Mega Drive's (2). Sprites get transparent pixels.
    SDL_Texture *picture_img(const ArtRef &r, bool sprite, int look = 0);
    SDL_Texture *art_img(bool sprite, int id);   // thumbnails (own colours)
    // Makes (or finds in the cache) a texture under `key` from w x h pixels;
    // alpha is optional (nullptr = opaque).
    SDL_Texture *upload(const std::string &key, const std::vector<RGB> &px, const std::vector<u8> *alpha, int w, int h);
    void clear_images();         // empties the cache (textures go to the graveyard)
    void forget_shape_images(const std::string &shape);   // drops the cached images of one drawing
    std::vector<SDL_Texture *> graveyard;   // destroyed next frame (this frame's drawing may still use them)
    SDL_Texture *preview_tex = nullptr;     // the Map tab's view preview
    int preview_w = 0, preview_h = 0, preview_shown = -1;   // its size, and which console it shows (NEWROM_CONSOLES index)
    std::string preview_note;    // the console view's note shown under it
    bool preview_stale = true;   // redraw the preview before showing it

    // build (runs on a thread)
    std::thread worker;          // the build thread (start_project_roms, start_new_rom)
    // building: a build is running; cancel: asks it to stop; build_finished:
    // the thread has finished and poll_build may collect the result
    std::atomic<bool> building{false}, cancel{false}, build_finished{false};
    std::atomic<float> progress{0};   // 0..1, for the progress bars
    std::mutex log_mu;           // guards log (the thread adds to it)
    std::vector<std::string> log;     // the Build tab's log lines
    bool build_ok = false;       // the result, set by the thread before build_finished
    std::string build_err, build_target;   // the error; the (first) ROM file written
    BuildStats build_stats;      // sizes and notes from the builder (the last ROM's)
    // after the build: start the emulator for this console on build_target (F5)
    bool build_then_run = false;
    Console run_console = Console::MD;
    // Makes a ROM of the project (as it is now, proj included) for each
    // console in cons on the worker thread, each saved under its usual name
    // in folder out (or, with one console, as the file out when folder is
    // false); run_after: then start the emulator on it (F5).
    void start_project_roms(const std::vector<Console> &cons, const std::string &out, bool folder, bool run_after);
    // F5: a ROM of the project for the "play on" console (Settings::play_console),
    // export/<its usual name>, then the emulator
    void make_and_play();
    // Saves the project to path (store_map first); records it in the recent
    // files; false and status when it can't.
    bool save_project(const std::string &path);
    // Save (Ctrl+S): to its file, or asks for one when it has none.
    void save_or_ask();
    void save_as();              // asks for the file, then save_project
    // Called every frame: once the thread has finished, joins it and takes the result.
    void poll_build();
    void add_log(const std::string &s);   // adds a Build tab log line (any thread)

    FileDialog fd;               // the one file dialog (filedialog.h)

    // the original game (DoomRPG.jar): its maps
    // One map of the jar, as checked when the jar was loaded.
    struct GameMap {
        std::string name, title, verdict;           // file name (e.g. "level01"), its title, "fits: ..." or why not
        std::vector<std::string> notes;             // the converter's notes (e.g. skipped commands)
        bool fits = false;                          // within a whole-game map's limits
        bool fits_sms = false;                      // within the Master System's / Game Gear's limits
        std::string sms_verdict;                    // why not ("" if it fits)
    };
    std::unique_ptr<Jar> jar;    // the loaded DoomRPG.jar (jar.h), or none
    std::vector<GameMap> jar_maps;   // its maps
    bool show_jar = false;       // open the "original game's levels" window next frame
    // Loads a jar and shows its maps.
    void open_jar(const std::string &path);
    // Loads a jar, converts each map to check it against the limits, and
    // remembers the jar's path in the settings. false (and status) if it fails.
    bool load_jar(const std::string &path);
    // the folders beside the program: import (the user's DoomRPG.jar), export (the ROMs it makes)
    std::string base_dir, import_dir, export_dir;
    void init_folders();
    std::string export_start() const;                 // the folder save dialogs start in
    std::string unused_name(const std::string &dir, const std::string &file) const;   // file, or "name (2).ext"...
    bool jar_ok() const { return jar && !jar_maps.empty(); }   // a jar with maps is loaded
    bool show_find_jar = false;  // open the "Your copy of Doom RPG" window next frame
    void find_jar_at_start();                          // the import folder's jar, else ask for one
    void draw_find_jar_window();
    void with_jar(std::function<void()> then);         // then, once a jar is loaded (asking for it first)
    void draw_jar_window();
    // the "maps and consoles" window (src/engine.*), in one of three modes:
    // NR_JAR_ROMS new ROMs straight from the jar (maps and consoles),
    // NR_NEW_PROJECT a new project from the jar (maps only), NR_PROJECT_ROMS
    // ROMs of the open project (consoles only). show_newrom: open it next frame.
    enum NewRomMode { NR_JAR_ROMS, NR_NEW_PROJECT, NR_PROJECT_ROMS };
    NewRomMode newrom_mode = NR_JAR_ROMS;
    bool show_newrom = false;
    // A console the editor can make ROMs for. This list is also the order of
    // the Map tab's preview console menu (Settings::preview_console indexes it).
    struct NewRomConsole {
        Console c;
        const char *label, *file;                     // its name in the UI; its usual ROM file name
        bool campaign;                                // the whole game (else one map)
    };
    const std::vector<NewRomConsole> NEWROM_CONSOLES = {
        {Console::SMS, "Master System", "deimos.sms", false},
        {Console::GG, "Game Gear", "deimos.gg", false},
        {Console::MD, "Mega Drive / Genesis", "deimos.md", true},
        {Console::SNES, "SNES / Super Famicom", "deimos.sfc", true},
        {Console::SNESFX, "SNES with the SuperFX", "deimos_fx.sfc", true},
        {Console::N3DS, "3DS (in 3D)", "deimos.3dsx", true},
        {Console::MARS, "32X", "deimos32x.32x", true},
        {Console::GBC, "Game Boy Color", "deimos.gbc", true},
        {Console::GBA, "Game Boy Advance", "deimos.gba", true},
        {Console::VB, "Virtual Boy (in 3D)", "deimos.vb", true}};
    std::vector<char> newrom_cons;                    // the consoles ticked (NEWROM_CONSOLES' order)
    std::vector<std::string> newrom_maps;             // the game's maps in the order they are played
    std::vector<char> newrom_sel;                     // the maps ticked
    // why console k can't take the maps (the ticked ones, or for
    // NR_PROJECT_ROMS the project's) ("" if it can)
    std::string newrom_why_not(size_t k) const;
    void newrom_reset();         // the jar's maps, all ticked; the Mega Drive ticked the first time
    // opens the window in mode m (asking for the jar first if needed)
    void ask_new_rom(NewRomMode m = NR_JAR_ROMS);
    void draw_newrom_window();
    // the ROMs from the jar: out is the file (one console) or the folder
    // (several: each console's default name in it)
    void start_new_rom(const std::string &out, bool folder);
    // a new project (unsaved, no file yet) of the maps ticked
    void new_project();

    // actions
    // Loads a project and edits its first map; false (and status says why) if it fails.
    bool open_project(const std::string &path);
    // Empties the map (keeping art, thing types and texts) to a 4x4 room.
    void new_map();
    // Starts the emulator command from the settings (console c's) on a ROM file.
    void run_emulator(const std::string &rom_path, Console c);
    // the level, as one of the game's maps, in place of the map being edited
    // (it keeps the map's place: its name and id); undoable
    void replace_level(Project p);

    // drawing (each called from frame(), once per frame, inside ImGui)
    void frame();                // the whole window (app.cpp)
    void draw_menu();            // the menu bar (app.cpp)
    void draw_map_tab();         // ui_map.cpp: the Map tab and its parts
    void draw_map_canvas();
    void draw_tools_panel();
    void draw_inspector();
    void draw_preview();
    void draw_art_tab();         // ui_art.cpp: the "Edit for" bar, then the tab (draw_art_tab_body)
    void draw_art_tab_body();    // ui_art.cpp: the picture list, paint area and side panel
    void draw_types_tab();       // ui_data.cpp: the other tabs and windows
    void draw_text_tab();
    void draw_build_tab();
    void draw_limits();
    void draw_help();
    bool show_help = false, show_about = false;   // the How to use / Settings windows are open
    int requested_tab = -1;      // a tab to switch to next frame (0 Map ... 5 Build), -1 none

    // art helpers (ui_art.cpp)
    ArtRef *pic_ref(const PicId &id);           // the picture, or nullptr if it doesn't exist
                                                // (with "Edit for" a console: its own version, if it has one)
    ArtRef *own_ref(const std::string &console, const PicId &id);   // a console's own version, or nullptr
    const ArtRef &shown_ref(const PicId &id, const ArtRef &shared);   // what the list shows for a picture
    std::string pic_name(const PicId &id) const;   // its name in the picture list
    // sprites and weapon frames have transparent pixels; textures and the border don't
    bool pic_sprite(const PicId &id) const { return id.kind == P_SPRITE || id.kind == P_WEAPON; }
};

// text helpers (app.cpp): a greyed note that wraps at the panel's edge, and
// paragraphs kept to a comfortable reading width on big screens
void hint(const char *fmt, ...);   // printf-style, greyed, wrapped
void para(const char *fmt, ...);   // printf-style, wrapped at reading_width()
float reading_width();             // about 65 letters, in pixels at the current size

// what a colour looks like on each console (ui_art.cpp)
// (they are defined in app.cpp: the nearest Master System colour; the colour
// the Mega Drive shows for it)
RGB look_sms(RGB c);
RGB look_md(RGB c);
