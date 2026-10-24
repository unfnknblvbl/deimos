// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Deimos Editor - entry point.
// Licence: GPL-2.0-or-later.
//
// main() does three things:
// 1. With a first argument starting "--" it runs the command-line tool
//    instead (run_cli in cli.cpp) and never opens a window.
// 2. Otherwise it sets up SDL2 (window, renderer), Dear ImGui (context, the
//    SDL2 + SDL_Renderer backends, the DejaVu Sans font sized to the display)
//    and the App (app.h), finds DoomRPG.jar and opens the ROM named on the
//    command line, or the last one used.
// 3. It runs the main loop: read SDL events (quit, dropped files, display
//    changes), call App::frame() to build the UI, render it, present.
// On exit it waits for a running build, saves the settings and shuts down.
//
// Several DRPG_* environment variables are test hooks used by the
// project's automatic tests: they take screenshots, play mouse events,
// open tabs or make ROMs without anyone at the keyboard. Normal use sets
// none of them.
#include "app.h"
#include "engine.h"
#include "cli.h"
#include "projectio.h"
#include "../third_party/fonts/dejavu_sans.h"

#include "../third_party/imgui/backends/imgui_impl_sdl2.h"
#include "../third_party/imgui/backends/imgui_impl_sdlrenderer2.h"
#include "../third_party/imgui/imgui.h"
#include <SDL.h>

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// Sizes follow the desktop's display scale: on Wayland (and macOS) the
// window is already measured in scaled units and only the font needs more
// pixels; elsewhere (X11, Windows) the display's DPI says how much to scale.
// The Settings "text size" multiplies that.
// g_ui: the global copy of App::ui, for code that has no App at hand
// (declared in filedialog.h). 1.0 = the base size; 2.0 = everything twice as big.
float g_ui = 1.0f;

// Works out the size scale and (re)builds the ImGui style and the font atlas
// at that size. Called once at start and again whenever app.fonts_dirty is
// set (the window moved to another display, or the text size setting changed).
// Writes app.dpi_scale, app.ui, g_ui; clears app.fonts_dirty.
static void setup_fonts(App &app)
{
    ImGuiIO &io = ImGui::GetIO();
    int ww, wh, pw, ph;
    SDL_GetWindowSize(app.win, &ww, &wh);          // the window in screen units
    SDL_GetRendererOutputSize(app.ren, &pw, &ph);  // the window in real pixels
    // fb: real pixels per screen unit (above 1 on Wayland / macOS high-DPI screens)
    float fb = ww > 0 ? (float)pw / ww : 1.0f;
    if (fb < 1.0f) fb = 1.0f;
    float dpi = 1.0f;
    if (fb < 1.01f) {
        // no framebuffer scaling: take the scale from the display's DPI
        // (96 DPI = 1.0), rounded to quarters and kept within 1..4
        float ddpi = 0;
        if (SDL_GetDisplayDPI(SDL_GetWindowDisplayIndex(app.win), &ddpi, nullptr, nullptr) == 0 && ddpi > 0)
            dpi = std::round(ddpi / 96.0f * 4) / 4;
        dpi = std::max(1.0f, std::min(4.0f, dpi));
    }
    if (const char *e = std::getenv("DRPG_SCALE")) dpi = (float)std::atof(e);   // test hook: force a scale
    app.dpi_scale = dpi * fb;
    app.ui = dpi * app.set.ui_scale;   // times the "text size" setting
    g_ui = app.ui;
    // the dark style at size 1.0, kept once so it can be rescaled from scratch each time
    static ImGuiStyle base;
    static bool have_base = false;
    if (!have_base) {
        ImGui::StyleColorsDark();
        base = ImGui::GetStyle();
        have_base = true;
    }
    ImGuiStyle st = base;
    st.ScaleAllSizes(app.ui);   // padding, spacing, rounding... to the new size
    ImGui::GetStyle() = st;
    // the font: DejaVu Sans (built in), 15 px at size 1.0, rasterised at the
    // real pixel size (x fb) and drawn scaled back by 1/fb, so it is sharp
    io.Fonts->Clear();
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;   // the font data is a static array: ImGui must not free it
    fc.OversampleH = 2;
    io.Fonts->AddFontFromMemoryTTF((void *)dejavu_sans_ttf, (int)dejavu_sans_size, 15.0f * app.ui * fb, &fc);
    io.FontGlobalScale = 1.0f / fb;
    // the renderer backend's copy of the font atlas, remade at the new size
    ImGui_ImplSDLRenderer2_DestroyFontsTexture();
    ImGui_ImplSDLRenderer2_CreateFontsTexture();
    app.fonts_dirty = false;
}

// argv[1]: "--something" runs the command-line tool (cli.cpp); otherwise an
// optional ROM file to open. Returns the process exit code.
int main(int argc, char **argv)
{
    if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == '-') return run_cli(argc, argv);   // no window: the CLI

    App app;
    app.set.load();   // the .ini file (window size, recent files, emulators...)
    SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");   // sharp text on scaled Windows displays
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");     // show the system's input-method window when typing text
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);   // deliver files dropped on the window
    // the window: resizable, high-DPI aware, maximised if it was last time
    Uint32 wf = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (app.set.maximized ? SDL_WINDOW_MAXIMIZED : 0);
    app.win = SDL_CreateWindow("Deimos Editor " EDITOR_VERSION, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, app.set.window_w,
                               app.set.window_h, wf);
    if (!app.win) {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        return 1;
    }
    // a hardware renderer synced to the display, else the software one
    app.ren = SDL_CreateRenderer(app.win, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
    if (!app.ren) app.ren = SDL_CreateRenderer(app.win, -1, SDL_RENDERER_SOFTWARE);
    if (!app.ren) {
        fprintf(stderr, "SDL renderer: %s\n", SDL_GetError());
        return 1;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    // ImGui's own layout file goes beside the settings (static: ImGui keeps the pointer)
    static std::string imgui_ini = (std::filesystem::path(app.set.dir()) / "imgui-layout.ini").string();
    std::error_code ec;
    std::filesystem::create_directories(app.set.dir(), ec);
    io.IniFilename = imgui_ini.c_str();
    // hook ImGui to SDL's input and to the SDL renderer
    ImGui_ImplSDL2_InitForSDLRenderer(app.win, app.ren);
    ImGui_ImplSDLRenderer2_Init(app.ren);
    setup_fonts(app);   // style and font at the display's scale

    app.init_folders();   // the import and export folders beside the program
    // look for DoomRPG.jar in the import folder (asks for it if none); the
    // test hooks skip this
    if (!std::getenv("DRPG_NEWROM") && !std::getenv("DRPG_NO_JAR")) app.find_jar_at_start();
    // the ROM named on the command line, else the last one opened
    std::string start = argc >= 2 ? argv[1] : app.set.last_rom;
    if (!start.empty() && std::filesystem::exists(start, ec)) app.open_rom(start);
    else app.status = "File > Open ROM to start.";

    // test hook: DRPG_SCREENSHOT=file.bmp [DRPG_FRAMES=n DRPG_TAB=t] saves one frame and quits
    const char *shot = std::getenv("DRPG_SCREENSHOT");
    int shot_frames = std::getenv("DRPG_FRAMES") ? std::atoi(std::getenv("DRPG_FRAMES")) : 10;   // the frame to save (default 10)
    if (std::getenv("DRPG_TAB")) app.requested_tab = std::atoi(std::getenv("DRPG_TAB"));   // tab 0 Map ... 5 Build
    // more test hooks: a campaign map, the Settings window, the preview's
    // console, the Map tool, the original game's levels window
    if (const char *m = std::getenv("DRPG_MAP")) app.switch_map(std::atoi(m));   // test hook: a campaign's map
    if (std::getenv("DRPG_ABOUT")) app.show_about = true;   // test hook
    if (const char *pv = std::getenv("DRPG_PREVIEW")) app.set.preview_console = std::atoi(pv);   // test hook
    if (std::getenv("DRPG_TOOL")) app.tool = std::atoi(std::getenv("DRPG_TOOL"));
    if (const char *jp = std::getenv("DRPG_JAR")) app.open_jar(jp);   // test hook
    // test hook: DRPG_NEWROM=jar [DRPG_NEWROM_CONSOLE=n,m...: indexes into NEWROM_CONSOLES] [DRPG_NEWROM_MAPS=1:
    // the UAC Base only] [DRPG_NEWROM_OUT=file, or a folder for several consoles]: the new-ROM window, or (with OUT)
    // its build, then quit once the (first) ROM is open
    const char *newrom_out = std::getenv("DRPG_NEWROM_OUT");
    if (const char *nj = std::getenv("DRPG_NEWROM")) {
        if (app.load_jar(nj)) {
            app.newrom_reset();   // all maps ticked
            if (const char *c = std::getenv("DRPG_NEWROM_CONSOLE")) {
                // tick only the listed consoles: a comma-separated list of numbers
                app.newrom_cons.assign(app.NEWROM_CONSOLES.size(), 0);
                for (const char *p = c; *p;) {
                    size_t k = (size_t)std::atoi(p);
                    if (k < app.newrom_cons.size()) app.newrom_cons[k] = 1;
                    while (*p && *p != ',') p++;   // skip to the next comma
                    if (*p) p++;
                }
            }
            if (std::getenv("DRPG_NEWROM_MAPS"))   // only the first map (the UAC Base)
                for (size_t i = 1; i < app.newrom_sel.size(); i++) app.newrom_sel[i] = 0;
            int n = 0;
            for (char v : app.newrom_cons) n += v;   // consoles ticked
            if (newrom_out) app.start_new_rom(newrom_out, n > 1);   // several: OUT is a folder
            else app.show_newrom = true;
        }
    }
    // test hook: DRPG_PIC=kind,id,tile picks the Art tab's picture and tile
    if (const char *pk = std::getenv("DRPG_PIC")) sscanf(pk, "%d,%d,%d", &app.art.pic.kind, &app.art.pic.id, &app.art.tile);
    // test hook: DRPG_EVENTS="frame,m|d|u|r|R,x,y;..." synthetic mouse input
    // m: move; d / u: left button down / up; r / R: right button down / up;
    // x, y in window coordinates; frame: the frame number to send it on
    struct Ev { int f; char t; int x, y; };
    std::vector<Ev> script;
    if (const char *se = std::getenv("DRPG_EVENTS")) {
        std::string s = se;
        size_t p = 0;
        // split at ';' and parse each item; malformed items are skipped
        while (p < s.size()) {
            size_t q = s.find(';', p);
            std::string item = s.substr(p, q == std::string::npos ? std::string::npos : q - p);
            Ev ev{};
            if (sscanf(item.c_str(), "%d,%c,%d,%d", &ev.f, &ev.t, &ev.x, &ev.y) == 4) script.push_back(ev);
            if (q == std::string::npos) break;
            p = q + 1;
        }
    }
    // test hook: DRPG_SAVE=out.rom saves through the window's build (on its thread) and quits
    const char *save_to = std::getenv("DRPG_SAVE");
    bool save_started = false;
    int frame_no = 0;     // frames drawn so far (the test hooks count them)
    bool quit = false;
    // ---- the main loop: one pass per frame (vsync paces it)
    while (!quit) {
        // the test hook's mouse events due this frame go into SDL's queue
        for (auto &ev : script)
            if (ev.f == frame_no) {
                SDL_Event se{};
                SDL_WarpMouseInWindow(app.win, ev.x, ev.y);   // keeps the global mouse state in step
                if (ev.t == 'm') {
                    se.type = SDL_MOUSEMOTION;
                    se.motion.x = ev.x;
                    se.motion.y = ev.y;
                    se.motion.windowID = SDL_GetWindowID(app.win);
                } else {
                    se.type = (ev.t == 'd' || ev.t == 'r') ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
                    se.button.button = (ev.t == 'r' || ev.t == 'R') ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
                    se.button.x = ev.x;
                    se.button.y = ev.y;
                    se.button.clicks = 1;
                    se.button.windowID = SDL_GetWindowID(app.win);
                }
                SDL_PushEvent(&se);   // handled below like real input
            }
        if (const char *dp = std::getenv("DRPG_DROP"))   // test hook: a file dropped on the window
            if (frame_no == 5) {
                SDL_Event de{};
                de.type = SDL_DROPFILE;
                de.drop.file = SDL_strdup(dp);   // SDL_free'd below, as SDL's own drops are
                de.drop.windowID = SDL_GetWindowID(app.win);
                SDL_PushEvent(&de);
            }
        // ---- input: every pending SDL event goes to ImGui, and a few to the App
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);   // keyboard, mouse, text for ImGui
            if (e.type == SDL_QUIT) app.want_quit = true;
            if (e.type == SDL_DROPFILE && e.drop.file) {   // a ROM (or the original game) dropped on the window
                app.dropped(e.drop.file);
                SDL_free(e.drop.file);
            }
            // moved to another display: its scale may differ, so remake the font
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED) app.fonts_dirty = true;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
                e.window.windowID == SDL_GetWindowID(app.win))
                app.want_quit = true;
        }
        // a quit request: refused during a build, asked about with unsaved changes
        bool ask_quit = false;
        if (app.want_quit) {
            app.want_quit = false;
            if (app.building) app.status = "Wait for the build to finish (or cancel it) before quitting";
            else if (app.unsaved) ask_quit = true;
            else quit = true;
        }
        if (app.fonts_dirty) setup_fonts(app);   // (must happen between frames)
        // ---- build this frame's UI
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        app.frame();   // the whole editor window
        if (ask_quit) ImGui::OpenPopup("Quit?");
        if (ImGui::BeginPopupModal("Quit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("There are unsaved changes. Quit anyway?");
            if (ImGui::Button("Quit without saving")) quit = true;
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        // ---- draw it: ImGui's draw lists through the SDL renderer
        ImGui::Render();
        SDL_RenderSetScale(app.ren, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);   // screen units to real pixels
        SDL_SetRenderDrawColor(app.ren, 30, 30, 34, 255);   // the background, dark grey
        SDL_RenderClear(app.ren);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), app.ren);
        ++frame_no;
        // test hooks: start the DRPG_SAVE build on frame 5; once a test build
        // has finished, print the log and quit
        if (save_to && frame_no == 5) {
            app.start_build(save_to, false);
            save_started = true;
        }
        if (newrom_out && !app.building && frame_no > 6 && !shot) {   // (test hook: the new ROM is made)
            std::lock_guard<std::mutex> l(app.log_mu);
            for (auto &s : app.log) printf("%s\n", s.c_str());
            printf("open: %s\n", app.have_rom ? app.rom.path.c_str() : "-");
            quit = true;
        }
        if (save_to && save_started && !app.building && frame_no > 6) {
            std::lock_guard<std::mutex> l(app.log_mu);
            for (auto &s : app.log) printf("%s\n", s.c_str());
            quit = true;
        }
        if (shot && frame_no == shot_frames) {
            // test hook: read the frame back from the renderer (before
            // presenting it) and save it as a .bmp
            int w, h;
            SDL_GetRendererOutputSize(app.ren, &w, &h);
            SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
            SDL_RenderReadPixels(app.ren, nullptr, SDL_PIXELFORMAT_ARGB8888, sf->pixels, sf->pitch);
            SDL_SaveBMP(sf, shot);
            SDL_FreeSurface(sf);
            quit = true;
        }
        SDL_RenderPresent(app.ren);   // show the frame (waits for vsync)
        // remember the window's size (only when not maximised) and state for next time
        if (!(SDL_GetWindowFlags(app.win) & SDL_WINDOW_MAXIMIZED)) SDL_GetWindowSize(app.win, &app.set.window_w, &app.set.window_h);
        app.set.maximized = (SDL_GetWindowFlags(app.win) & SDL_WINDOW_MAXIMIZED) != 0;
    }
    // ---- shut down
    if (app.worker.joinable()) app.worker.join();   // a build still running finishes first
    if (const char *ex = std::getenv("DRPG_EXPORT")) {   // test hook: the level as files, at exit
        std::string err;
        export_project(app.proj, ex, err);
    }
    app.set.save();       // window size, zoom, recent files...
    app.clear_images();   // (moves them to the graveyard; the renderer frees them below)
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(app.ren);
    SDL_DestroyWindow(app.win);
    SDL_Quit();
    return 0;
}
