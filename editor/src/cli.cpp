// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Command-line functions of the editor (no window needed):
//   --new-rom JAR CONSOLE OUT [maps...]    a ROM straight from the original game
//   --new-project JAR [maps...] OUT        a new project (.deimos) of the game's maps
//   --make-rom JAR PROJECT CONSOLE OUT     a ROM of a project's maps
//   --project-maps PROJECT                 a project's maps, numbered
//   --selftest PROJECT                     save / reopen check of a project
//   --export PROJECT DIR / --import DIR OUT [PROJECT]   a map as plain files
//   --render PROJECT X Z FACING OUT.ppm [small] [low] [solid]   the Master System view from a cell
//   --from-game JAR MAP PROJECT OUT        one of the game's maps in place of a project's
//   --maps JAR                             the original game's maps, and which fit
// and for the tests --demo-map PROJECT OUT [JSON], --check-forms JAR,
// --dump-assets JAR CONSOLE VERSION DIR and --dump-strings JAR CONSOLE DIR
// (see run_cli and its usage text at the bottom). --map N anywhere picks
// the map of a project to work on (default 0).
// Licence: GPL-2.0-or-later.
//
// run_cli() is called by main() in main.cpp and main_cli_only.cpp. Each job
// is a static function here that returns the process exit code (0 success,
// 1 failure; 2 from run_cli for a usage error). They use the editor's core:
// project files (projectfile.h) for the maps, the Master System builder
// and renderer (sms_build.h), the jar reader and converter (jar.h), the
// ROM makers (engine.h) and export_project / import_project (projectio.h).
#include "cli.h"
#include "assets.h"
#include "edit.h"
#include "engine.h"
#include "jar.h"
#include "levelblob.h"
#include "projectfile.h"
#include "projectio.h"
#include "rom.h"
#include "sms_build.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

// Prints "error: e" to stderr and returns 1 (the failure exit code), so a
// job can end with "return fail(err);".
static int fail(const std::string &e)
{
    std::cerr << "error: " << e << "\n";
    return 1;
}

static int g_map = 0;   // --map N: which map of a project (default the first)

// Reads a project file and checks that it has map g_map. False with err.
static bool load(const char *path, ProjectFile &pf, std::string &err)
{
    if (!load_project_file(path, pf, err)) return false;
    if (g_map < 0 || g_map >= (int)pf.maps.size()) {
        err = std::string(path) + " has " + std::to_string(pf.maps.size()) + " maps (--map 0.." +
              std::to_string(pf.maps.size() - 1) + ")";
        return false;
    }
    return true;
}

// Opens the jar at path into jar; false with err when it isn't Doom RPG's.
static bool open_jar(const char *path, Jar &jar, std::string &err) { return jar.open(path, err); }

// Build options whose log goes to stdout, indented (one thread per core,
// no progress callback, no cancel).
static BuildOptions verbose()
{
    BuildOptions o;
    o.log = [](const std::string &s) { std::cout << "  " << s << std::endl; };
    return o;
}

// The console named by its CONSOLE_KEYS name (sms, gg, md, snes, snesfx,
// 3ds, 32x, gbc, gba); false when the name is none of them.
static bool console_of(const char *name, Console &c)
{
    for (int i = 0; i < 9; i++)
        if (!strcmp(name, CONSOLE_KEYS[i])) {
            c = (Console)i;
            return true;
        }
    return false;
}

// --new-rom JAR CONSOLE OUT [maps...]: a ROM from the original game with
// the console's engine file (engine::new_rom): the Master System and Game
// Gear with one map (the UAC Base by default), the others with the given
// maps in order (the whole game by default). Writes it to OUT.
static int new_rom(const char *jar_path, const char *console, const char *outp, std::vector<std::string> maps)
{
    Jar jar;
    std::string err;
    Console c;
    if (!console_of(console, c)) return fail(std::string("unknown console ") + console + " (sms gg md snes snesfx 3ds 32x gbc gba)");
    if (!open_jar(jar_path, jar, err)) return fail(err);
    // no maps named: the UAC Base ("intro") for the one-map consoles, else
    // the whole game in playing order
    if (maps.empty()) maps = sms_family(c) ? std::vector<std::string>{"intro"} : engine::campaign_maps(jar);
    Rom rom;
    BuildStats st;
    std::vector<std::string> notes;
    // build the ROM in memory, then save it
    if (!engine::new_rom(c, jar, maps, rom, verbose(), st, err, &notes) || !rom.save(outp, err)) return fail(err);
    for (auto &n : notes) std::cout << "note: " << n << "\n";
    size_t n = sms_family(c) ? 1 : maps.size();
    std::cout << "wrote " << outp << " (" << rom.console_name() << ", " << st.rom_kb << " KB, " << n
              << (n == 1 ? " map" : " maps") << ")\n";
    return 0;
}

// --new-project JAR [maps...] OUT: a project of the game's maps, in the
// order given (the whole game by default), written to OUT.
static int new_project(const char *jar_path, std::vector<std::string> maps, const char *outp)
{
    Jar jar;
    std::string err;
    if (!open_jar(jar_path, jar, err)) return fail(err);
    if (maps.empty()) maps = engine::campaign_maps(jar);
    ProjectFile pf;
    std::vector<std::string> notes;
    if (!engine::new_project(jar, maps, pf, verbose(), err, &notes) || !save_project_file(outp, pf, err))
        return fail(err);
    for (auto &n : notes) std::cout << "note: " << n << "\n";
    std::cout << "wrote " << outp << " (" << pf.maps.size() << (pf.maps.size() == 1 ? " map)\n" : " maps)\n");
    return 0;
}

// --make-rom JAR PROJECT CONSOLE OUT: a ROM for CONSOLE of the project's
// maps (engine::project_rom; the Master System and Game Gear take map
// --map N), the graphics and sounds from the jar.
static int make_rom(const char *jar_path, const char *in, const char *console, const char *outp)
{
    Jar jar;
    std::string err;
    Console c;
    if (!console_of(console, c)) return fail(std::string("unknown console ") + console + " (sms gg md snes snesfx 3ds 32x gbc gba)");
    ProjectFile pf;
    if (!load(in, pf, err) || !open_jar(jar_path, jar, err)) return fail(err);
    std::string why = engine::why_not(c, pf, g_map);
    if (!why.empty()) return fail(why);
    Rom out;
    BuildStats st;
    if (!engine::project_rom(c, jar, pf, g_map, out, verbose(), st, err) || !out.save(outp, err)) return fail(err);
    std::cout << "wrote " << outp << " (" << console_name(c) << ", " << st.rom_kb << " KB; the maps use "
              << st.used_kb << " KB, " << st.free_kb << " KB free)\n";
    return 0;
}

// --project-maps PROJECT: the project's maps, numbered as --map takes them.
static int project_maps(const char *path)
{
    ProjectFile pf;
    std::string err;
    if (!load_project_file(path, pf, err)) return fail(err);
    for (size_t m = 0; m < pf.maps.size(); m++) {
        auto &pm = pf.maps[m];
        std::cout << m << ": " << pm.info.name << " (" << pm.info.title << "), map id " << pm.info.id << ", "
                  << pm.level.walls.size() << " walls, " << pm.level.entities.size() << " things, "
                  << pm.level.events.size() << " scripts\n";
    }
    return 0;
}

// --selftest PROJECT: proves that saving and opening a project changes
// nothing. Three checks: every map survives its JSON (to_json, from_json,
// to_json again gives the same text); the project written again is the
// same bytes as the file; and read back, every map has the same JSON and
// drawings. Returns 1 if any failed.
static int selftest(const char *path)
{
    ProjectFile pf;
    std::string err;
    Bytes file;
    if (!read_file(path, file, err) || !read_project_file(file, pf, err)) return fail(err);
    int bad = 0;   // checks failed
    for (auto &pm : pf.maps) {
        Project p2;
        bool ok = p2.from_json(pm.level.to_json(), err) && p2.to_json() == pm.level.to_json();
        if (!ok) {
            std::cout << pm.info.name << ": the level does not survive its JSON " << err << "\n";
            bad++;
        }
    }
    std::cout << pf.maps.size() << " maps survive their JSON: " << (bad ? "NO" : "yes") << "\n";
    ProjectFile pf2 = pf;
    Bytes again;
    if (!write_project_file(pf2, again, err)) return fail(err);
    bool same = again == file;
    std::cout << "written again: " << again.size() / 1024 << " KB, "
              << (same ? "byte-for-byte the same as the file" : "DIFFERENT from the file") << "\n";
    bad += !same;
    ProjectFile pf3;
    bool rok = read_project_file(again, pf3, err) && pf3.maps.size() == pf.maps.size();
    for (size_t m = 0; rok && m < pf.maps.size(); m++)
        rok = pf3.maps[m].level.to_json() == pf.maps[m].level.to_json() &&
              pf3.maps[m].info.name == pf.maps[m].info.name && pf3.maps[m].level.shapes.size() == pf.maps[m].level.shapes.size();
    std::cout << "reads back: " << (rok ? "yes" : "NO " + err) << "\n";
    bad += !rok;
    std::cout << (bad ? "SELFTEST FAILED\n" : "selftest ok\n");
    return bad ? 1 : 0;
}

// --check-forms JAR (tests): every map of the game read as a single level
// (as the Master System has it) must equal the same map read in the whole
// game's form and turned into a single level (Project::single_level, which
// the Master System and Game Gear ROMs of a project use). Prints each
// map's result; 1 if any differs.
static int check_forms(const char *jar_path)
{
    Jar jar;
    std::string err;
    if (!open_jar(jar_path, jar, err)) return fail(err);
    int bad = 0;
    for (auto &m : jar.maps()) {
        Project single, camp;
        if (!import_map(jar, m, single, err) || !import_map(jar, m, camp, err, nullptr, true)) return fail(err);
        bool same = camp.single_level().to_json() == single.to_json();
        std::cout << m << ": " << (same ? "the same" : "DIFFERENT") << "\n";
        bad += !same;
    }
    return bad ? 1 : 0;
}

// --render PROJECT X Z FACING OUT.ppm [small] [low] [solid]: draws the view
// of map --map N from cell (x, z) facing f (0 north, 1 east, 2 south, 3
// west) with the Master System's renderer (sms_model.cpp), doors closed,
// and writes it as a binary PPM (P6). small / low / solid: the game's
// smaller view, low detail and solid walls settings. x, z and f are not
// range-checked.
static int render(const char *in, int x, int z, int f, const char *outp, bool small, bool low, bool solid)
{
    ProjectFile pf;
    std::string err;
    if (!load(in, pf, err)) return fail(err);
    Project p = pf.maps[g_map].level.single_level().for_console("sms");
    sms::LevelData d;
    BuildOptions o;
    // the Master System builder's level data (true: with the visibility
    // lists the renderer needs; the editor's preview passes false)
    if (!sms::level_data(p, d, true, o, err)) return fail(err);
    std::ofstream out(outp, std::ios::binary);
    int W = sms::VIEW_W[small], H = sms::VIEW_H[small];   // the view's size in pixels
    // the picture as colour numbers 0..15 into the level's 16 colours
    auto img = sms::render(d, small, x, z, f, std::vector<u8>(p.lines.size(), 0), low, solid);
    out << "P6\n" << W << " " << H << "\n255\n";   // PPM header: width, height, max value
    for (u8 c : img) out.write((const char *)&d.col.rgb[c], 3);   // each pixel as R, G, B bytes
    return 0;
}

// A small custom map made only with the editor's own operations, with
// changed art and text: proves that "edit -> make a ROM -> play" works end
// to end on any console (used by the tests).
// --demo-map PROJECT OUT [JSON]: map --map N keeps its art, thing types
// and texts, gets a new map and a new text, and the project is written to
// OUT (and, optionally, that map's JSON to JSON).
static int demo_map(const char *in, const char *outp, const char *json_out)
{
    ProjectFile pf;
    std::string err;
    if (!load(in, pf, err)) return fail(err);
    Project &p = pf.maps[g_map].level;
    // an empty map: every cell solid, nothing in it
    std::fill(p.wall.begin(), p.wall.end(), 1);
    std::fill(p.static_block.begin(), p.static_block.end(), 0);
    p.walls.clear();
    p.scenery.clear();
    p.entities.clear();
    p.lines.clear();
    p.events.clear();
    p.thin.clear();
    int tex = p.textures.count(48) ? 48 : p.textures.begin()->first;   // texture 48 if there is one, else the first
    // a 3x3 start room (z 20..22), a corridor north (x 15, z 14..19) and a
    // 7x5 room at the end (z 9..13)
    for (int z = 20; z <= 22; z++)
        for (int x = 14; x <= 16; x++) edit::make_floor(p, x, z, tex);
    for (int z = 14; z <= 19; z++) edit::make_floor(p, 15, z, tex);
    for (int z = 9; z <= 13; z++)
        for (int x = 12; x <= 18; x++) edit::make_floor(p, x, z, tex);
    // a door in the corridor (texture 0 if there is one)
    std::string e = edit::add_door(p, 15, 16, edit::DOOR_NORMAL, p.textures.count(0) ? 0 : tex);
    if (!e.empty()) return fail(e);
    p.spawn_x = 15;   // start in the first room, facing north
    p.spawn_z = 21;
    p.spawn_f = 0;
    // the index of the thing type with this name (0 if there is none)
    auto def_of = [&](const char *name) {
        for (size_t i = 0; i < p.defs.size(); i++)
            if (p.defs[i].name == name) return (int)i;
        return 0;
    };
    edit::add_entity(p, def_of("Health Vial"), 15, 19);   // in the corridor
    edit::add_entity(p, def_of("Zombie Pvt"), 15, 11);    // in the far room
    edit::add_entity(p, def_of("Barrel"), 13, 10);
    edit::add_scenery(p, 140, 12, 9, true);   // a lamp
    // a script in the start room shows a new text (8 = DIALOG, a1 = the
    // string's number; '|' in a text is a line break); a2 holds the
    // script command's flag bits, as the original game's scripts use them
    int ev = edit::ensure_event(p, 15, 20);
    p.strings.push_back("Made with the|editor!");
    p.events[ev].cmds.push_back({8, (u32)(p.strings.size() - 1), 0x10000000 | 0x400 | 0x200});
    // a script on cell 15,8 (solid, just north of the far room): 2 =
    // CHANGEMAP, with a1 $80000000 and a2 $100 (flag bits as above)
    int ex = edit::ensure_event(p, 15, 8);
    p.events[ex].cmds.push_back({2, 0x80000000u, 0x100});
    // the art: a new drawing for the corridor texture (a big X), and new colours
    ArtRef &t = p.textures[tex];
    std::string key = p.new_shape_key("t");   // an unused drawing name starting "t"
    p.shapes[key] = *p.drawing(t);            // a copy of the old drawing, used by this texture only
    t.shape = key;
    Drawing &d = p.shapes[key];
    // the two diagonals in colour 1 (the first one two pixels thick)
    for (int i = 0; i < 64; i++) {
        d.px[i * 64 + i] = 1;
        d.px[i * 64 + 63 - i] = 1;
        if (i < 63) d.px[i * 64 + i + 1] = 1;
    }
    t.pal[1] = {255, 40, 40};   // colour 1: red
    // a monster in other colours
    // (red and blue swapped in each of its 16 colours)
    for (auto &dd : p.defs)
        if (dd.name == "Zombie Pvt") {
            ArtRef &sp = p.sprites[dd.tile];
            for (int i = 0; i < 16; i++) std::swap(sp.pal[i].r, sp.pal[i].b);
        }
    p.drop_unused_shapes();   // the old drawing, if nothing else uses it
    if (!save_project_file(outp, pf, err)) return fail(err);
    if (json_out) std::ofstream(json_out) << p.to_json();
    std::cout << "wrote " << outp << "\n";
    return 0;
}

// --maps JAR: converts each map in the jar and prints its counts and whether
// it fits the Master System's limits (check_limits), plus the converter's notes.
static int list_maps(const char *jar_path)
{
    Jar jar;
    std::string err;
    if (!open_jar(jar_path, jar, err)) return fail(err);
    for (auto &m : jar.maps()) {
        Project p;
        std::vector<std::string> notes;
        if (!import_map(jar, m, p, err, &notes)) {   // the map as a level
            std::cout << m << ": " << err << "\n";
            continue;
        }
        std::string lim = check_limits(p);   // "" if it fits the Master System
        std::cout << m << " (" << Jar::map_title(m) << "): " << p.walls.size() << " walls, " << p.entities.size()
                  << " things, " << p.events.size() << " scripts, " << p.strings.size() << " texts -> "
                  << (lim.empty() ? "fits the Master System" : lim) << "\n";
        for (auto &n : notes) std::cout << "    " << n << "\n";
    }
    return 0;
}

// Puts level p in place of map m of pf, as the App's "Import a level"
// does: the map keeps its place (its name, so the exits to it still lead
// there, and its id), takes the level's title, and the level becomes a
// map of the whole game in that place.
static void replace_map(ProjectFile &pf, int m, Project p)
{
    ProjectMap &pm = pf.maps[m];
    if (p.campaign()) p.map_id = pm.info.id;
    pm.info.title = p.name;
    pm.level = std::move(p);
}

// --from-game JAR MAP PROJECT OUT: one map of the original game (e.g.
// "reactor") in place of map --map N of the project, written to OUT.
static int from_game(const char *jar_path, const char *map, const char *in, const char *outp)
{
    Jar jar;
    Project p;
    std::vector<std::string> notes;
    std::string err;
    ProjectFile pf;
    if (!load(in, pf, err) || !open_jar(jar_path, jar, err) || !import_map(jar, map, p, err, &notes, true))
        return fail(err);
    for (auto &n : notes) std::cout << "note: " << n << "\n";
    replace_map(pf, g_map, std::move(p));
    if (!save_project_file(outp, pf, err)) return fail(err);
    std::cout << "wrote " << outp << "\n";
    return 0;
}

// --export PROJECT DIR: map --map N of the project as a level folder
// (project.json, map.json, the drawings and every picture as a PNG).
static int export_dir(const char *in, const char *dir)
{
    ProjectFile pf;
    std::string err;
    if (!load(in, pf, err)) return fail(err);
    if (!export_project(pf.maps[g_map].level, pf.maps[g_map].info, dir, err)) return fail(err);
    std::cout << "exported " << pf.maps[g_map].info.title << " to " << dir << "\n";
    return 0;
}

// --import DIR OUT [PROJECT]: a level folder (as --export writes it) as a
// new one-map project in OUT; with PROJECT, in place of its map --map N
// (the rest of PROJECT kept), written to OUT.
static int import_dir(const char *dir, const char *outp, const char *into)
{
    ProjectFile pf;
    Project p;
    std::string err;
    MapInfo info{"level", "", 1};   // (a folder without map.json: one level of its own)
    if (into && !load(into, pf, err)) return fail(err);
    if (!import_project(dir, p, info, err)) return fail(err);
    if (info.title.empty()) info.title = p.name;
    if (into) replace_map(pf, g_map, std::move(p));
    else pf.maps.push_back({info, std::move(p)});
    if (!save_project_file(outp, pf, err)) return fail(err);
    std::cout << "wrote " << outp << "\n";
    return 0;
}

// (tests) the graphics and sounds a new ROM gets from the jar, one file each
// --dump-assets JAR CONSOLE VERSION DIR: CONSOLE is sms, gg, md, snes,
// snesfx, 3ds, 32x, gbc or gba; VERSION the version text the assets show
// (e.g. on the title screen). Writes each block as DIR/<name>.bin.
static int dump_assets(const char *jar_path, const char *console, const char *version, const char *dir)
{
    Jar jar;
    std::string err;
    if (!open_jar(jar_path, jar, err)) return fail(err);
    assets::Blocks b;   // block name -> bytes
    bool md = !strcmp(console, "md");
    // the SNES family's resources (the last argument: the edition's label,
    // none for the plain SNES and the GBC); the Mega Drive's status bar,
    // fonts and title plus its music and sound effects; else the Master
    // System's or (gg) the Game Gear's
    bool ok = !strcmp(console, "snes") ? assets::snes_res(jar, version, b, err)
              : !strcmp(console, "snesfx") ? assets::snes_res(jar, version, b, err, "SUPER FX")
              : !strcmp(console, "3ds") ? assets::snes_res(jar, version, b, err, "3D")
              : !strcmp(console, "32x") ? assets::snes_res(jar, version, b, err, "32X")
              : !strcmp(console, "gba") ? assets::snes_res(jar, version, b, err, "ADVANCE")
              : !strcmp(console, "gbc") ? assets::snes_res(jar, version, b, err)
              : md ? assets::md_ui(jar, version, b, err) && assets::md_music(jar, b, err) && assets::md_sfx(jar, b, err)
                   : assets::sms_ui(jar, version, !strcmp(console, "gg"), b, err);
    if (!ok) return fail(err);
    for (auto &kv : b) {
        std::string e;
        if (!write_file(std::string(dir) + "/" + kv.first + ".bin", kv.second, e)) return fail(e);
        std::cout << kv.first << ": " << kv.second.size() << " bytes\n";
    }
    return 0;
}

// (tests) the strings blob of every map of the jar as console CONSOLE's
// builder makes it, to compare with the Python builds' (the Mega Drive game's
// build/gen/maps/<map>/strings.bin for the Mega Drive and the ports that
// read its block; this repository's build/gfx/strings.bin and
// strings_gg.bin for the Master System and Game Gear, LEVEL=<map>)
// --dump-strings JAR CONSOLE DIR: writes DIR/<map>/strings.bin (DIR/<map>
// must exist) for each of the game's maps the jar has; a map whose texts
// don't fit the console (most, on the Master System) is reported and
// skipped, and the exit code is then 1.
static int dump_strings(const char *jar_path, const char *console, const char *dir)
{
    Jar jar;
    std::string err;
    if (!open_jar(jar_path, jar, err)) return fail(err);
    bool sms = !strcmp(console, "sms"), gg = !strcmp(console, "gg");
    int bad = 0;   // maps skipped
    for (auto &m : engine::campaign_maps(jar)) {
        Project p;
        if (!import_map(jar, m, p, err, nullptr, !(sms || gg))) return fail(err);   // (as engine::new_rom reads it)
        Project q = p.for_console(console);
        Bytes b;
        try {   // (the builders' own calls: sms_build.cpp's, md_build.cpp's make_cmap)
            b = sms || gg ? make_strings_blob(q, gg) : make_strings_blob_md(q);
        } catch (const std::exception &e) {
            fail(m + ": " + e.what());
            bad++;
            continue;
        }
        if (!write_file(std::string(dir) + "/" + m + "/strings.bin", b, err)) return fail(err);
        std::cout << m << ": " << b.size() << " bytes\n";
    }
    return bad ? 1 : 0;
}

// Picks the job from argv[1] (once --map N is taken out) and checks it has
// enough arguments; anything else prints the usage and returns 2.
int run_cli(int argc, char **argv)
{
    // --map N anywhere: the map of a project to work on
    // (taken out of the argument list, so the jobs see only their own arguments)
    std::vector<char *> args;
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "--map") && i + 1 < argc) g_map = atoi(argv[++i]);
        else args.push_back(argv[i]);
    argc = (int)args.size();
    argv = args.data();
    // the jobs; each line checks the job's name and its count of arguments
    if (argc >= 2 && !strcmp(argv[1], "--version")) {
        std::cout << "Deimos Editor " EDITOR_VERSION "\n";
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "--new-rom"))
        return new_rom(argv[2], argv[3], argv[4], std::vector<std::string>(argv + 5, argv + argc));
    if (argc >= 4 && !strcmp(argv[1], "--new-project"))   // (the maps between the jar and OUT)
        return new_project(argv[2], std::vector<std::string>(argv + 3, argv + argc - 1), argv[argc - 1]);
    if (argc >= 6 && !strcmp(argv[1], "--make-rom")) return make_rom(argv[2], argv[3], argv[4], argv[5]);
    if (argc >= 3 && !strcmp(argv[1], "--project-maps")) return project_maps(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "--selftest")) return selftest(argv[2]);
    if (argc >= 4 && !strcmp(argv[1], "--export")) return export_dir(argv[2], argv[3]);
    if (argc >= 4 && !strcmp(argv[1], "--import")) return import_dir(argv[2], argv[3], argc >= 5 ? argv[4] : nullptr);
    if (argc >= 7 && !strcmp(argv[1], "--render")) {
        bool small = false, low = false, solid = false;     // optional: small, low, solid
        for (int i = 7; i < argc; i++) {
            small |= !strcmp(argv[i], "small");
            low |= !strcmp(argv[i], "low");
            solid |= !strcmp(argv[i], "solid");
        }
        return render(argv[2], atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), argv[6], small, low, solid);
    }
    if (argc >= 6 && !strcmp(argv[1], "--from-game")) return from_game(argv[2], argv[3], argv[4], argv[5]);
    if (argc >= 3 && !strcmp(argv[1], "--maps")) return list_maps(argv[2]);
    if (argc >= 4 && !strcmp(argv[1], "--demo-map")) return demo_map(argv[2], argv[3], argc >= 5 ? argv[4] : nullptr);
    if (argc >= 3 && !strcmp(argv[1], "--check-forms")) return check_forms(argv[2]);
    if (argc >= 6 && !strcmp(argv[1], "--dump-assets")) return dump_assets(argv[2], argv[3], argv[4], argv[5]);
    if (argc >= 5 && !strcmp(argv[1], "--dump-strings")) return dump_strings(argv[2], argv[3], argv[4]);
    // no job matched: the usage, exit code 2
    std::cerr << "Deimos Editor " EDITOR_VERSION "\nusage:\n"
                 "  \"Deimos Editor\" [PROJECT.deimos]               open the editor (with a project)\n"
                 "  deimos-editor-cli --version                     the version\n"
                 "  deimos-editor-cli --new-rom DoomRPG.jar CONSOLE OUT [maps...]   a ROM straight from the original game\n"
                 "                 (CONSOLE: sms gg md snes snesfx 3ds 32x gbc gba; sms, gg: one map, intro =\n"
                 "                  the UAC Base by default; the others: the maps in order, the whole game by default)\n"
                 "  deimos-editor-cli --new-project DoomRPG.jar [maps...] OUT.deimos   a new project of the game's maps\n"
                 "                 (the maps in order, the whole game by default)\n"
                 "  deimos-editor-cli --make-rom DoomRPG.jar PROJECT CONSOLE OUT   a ROM of the project's maps\n"
                 "                 (sms, gg: map --map N alone)\n"
                 "  deimos-editor-cli --project-maps PROJECT       the project's maps, numbered\n"
                 "  deimos-editor-cli --selftest PROJECT            check that saving and opening it changes nothing\n"
                 "  deimos-editor-cli --export PROJECT DIR          a map as JSON + PNG files (a level folder)\n"
                 "  deimos-editor-cli --import DIR OUT.deimos [PROJECT]   such a folder as a new project\n"
                 "                 (or with PROJECT: in place of its map --map N)\n"
                 "  deimos-editor-cli --render PROJECT X Z FACING OUT.ppm [small] [low] [solid]   Master System view\n"
                 "  deimos-editor-cli --from-game DoomRPG.jar MAP PROJECT OUT.deimos   one of the game's maps in place\n"
                 "                 of map --map N\n"
                 "  deimos-editor-cli --maps DoomRPG.jar            the original game's maps, and which fit\n"
                 "  (--map N picks a project's map, 0 = the first; tests: --demo-map PROJECT OUT [JSON],\n"
                 "   --check-forms JAR, --dump-assets JAR CONSOLE VERSION DIR, --dump-strings JAR CONSOLE DIR)\n";
    return 2;
}
