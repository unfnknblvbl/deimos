// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Command-line functions of the editor (no window needed):
//   --selftest ROM                 unpack the level, rebuild the ROM from it, and
//                                  check the result is byte-for-byte the same
//   --rebuild ROM OUT              rebuild a ROM from its own copy of the level
//   --transfer FROM_ROM TO_ROM OUT put the level of one ROM into another
//                                  (e.g. a Master System level into the Mega Drive game)
//   --render ROM X Z FACING OUT.ppm [small] [low] [solid]   the Master System view from a cell
//   --export ROM DIR / --import DIR ROM OUT   the level as plain files
// and also (see run_cli and its usage text at the bottom):
//   --version, --rom-maps ROM, --new-rom JAR CONSOLE OUT [maps...],
//   --maps JAR, --from-game JAR MAP ROM OUT, and two for the tests,
//   --demo-map ROM OUT [JSON], --dump-assets JAR CONSOLE VERSION DIR and
//   --dump-strings JAR CONSOLE DIR.
//   --map N anywhere picks the map of a campaign ROM to work on.
// Licence: GPL-2.0-or-later.
//
// run_cli() is called by main() in main.cpp and main_cli_only.cpp. Each job
// is a static function here that returns the process exit code (0 success,
// 1 failure; 2 from run_cli for a usage error). They use the editor's core:
// Rom (rom.h) to load, rebuild and save ROMs, Project (project.h) for the
// level, the Master System builder and renderer (sms_build.h), the jar
// reader and converter (jar.h), engine::new_rom (engine.h) and
// export_project / import_project (projectio.h).
//
// A "campaign" ROM holds the whole game (the Mega Drive format, also used by
// the SNES, 3DS, 32X, GBC and GBA); Rom::maps lists its maps and Rom::map
// says which one read_project / build work on.
#include "cli.h"
#include "colour.h"
#include "container.h"
#include "edit.h"
#include "jar.h"
#include "assets.h"
#include "engine.h"
#include "levelblob.h"
#include "projectio.h"
#include "rom.h"
#include "sms_build.h"

#include <algorithm>
#include <chrono>
#include <vector>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>

// Prints "error: e" to stderr and returns 1 (the failure exit code), so a
// job can end with "return fail(err);".
static int fail(const std::string &e)
{
    std::cerr << "error: " << e << "\n";
    return 1;
}

static int g_map = -1;   // --map N: which map of a Mega Drive campaign ROM
                         // -1 = not given (map 0 is used; --selftest then checks every map)

// Loads a ROM file, selecting map g_map (or 0) if it is a campaign ROM.
// Output: rom; false and err if the file can't be read or --map is out of range.
static bool load_rom(const char *path, Rom &rom, std::string &err)
{
    rom.map = std::max(0, g_map);
    if (!rom.load(path, err)) return false;
    if (g_map >= (int)rom.maps.size() && rom.is_campaign()) {
        err = "this ROM has " + std::to_string(rom.maps.size()) + " maps (0.." + std::to_string(rom.maps.size() - 1) + ")";
        return false;
    }
    return true;
}

// Loads a ROM and reads its level (the selected map's) into p. On failure
// prints the error and returns false.
static bool load(const char *path, Rom &rom, Project &p)
{
    std::string err;
    if (!load_rom(path, rom, err)) {
        fail(err);
        return false;
    }
    if (!editable(rom.console)) {   // (rom.h: these consoles' ROMs are made locked, not edited)
        fail(std::string("the ") + rom.console_name() + " version isn't edited (its ROMs leave out the editable "
             "level); use the Mega Drive, 3DS, Game Boy Color or Game Boy Advance ROM, and --make-rom");
        return false;
    }
    if (!rom.read_project(p, err)) {
        fail(err);
        return false;
    }
    return true;
}

// Build options whose log goes to stdout, indented (one thread per core,
// no progress callback, no cancel).
static BuildOptions verbose()
{
    BuildOptions o;
    o.log = [](const std::string &s) { std::cout << "  " << s << std::endl; };
    return o;
}

static int selftest_one(const char *path);

// --selftest ROM: proves that unpacking and repacking changes nothing. A
// campaign ROM without --map is tested map by map (selftest_one with g_map
// set to each); otherwise the one level. Returns 1 if any test failed.
static int selftest(const char *path)
{
    Rom rom;
    std::string err;
    if (!load_rom(path, rom, err)) return fail(err);
    if (!rom.is_campaign() || g_map >= 0) return selftest_one(path);   // one level, or the map asked for
    int bad = 0;
    for (size_t m = 0; m < rom.maps.size(); m++) {
        std::cout << "== map " << m << ": " << rom.maps[m].name << " (" << rom.maps[m].title << ")\n";
        g_map = (int)m;
        bad += selftest_one(path);   // reloads the ROM with map m selected
    }
    g_map = -1;
    std::cout << (bad ? "CAMPAIGN SELFTEST FAILED\n" : "campaign selftest ok\n");
    return bad ? 1 : 0;
}

// The selftest of one level (map g_map of a campaign ROM). Three checks:
// 1. the level survives project.json: to_json, from_json, to_json again
//    gives the same text;
// 2. rebuilding the ROM from the level gives the same bytes as the file
//    (if not, the rebuilt ROM is written to ROM.rebuilt for comparison);
// 3. the level read back from the rebuilt ROM is the same.
// Returns 0 if all pass, else 1.
static int selftest_one(const char *path)
{
    Rom rom;
    Project p;
    if (!load(path, rom, p)) return 1;
    std::cout << rom.console_name() << " ROM, " << rom.data.size() / 1024 << " KB\n";
    std::cout << "level: " << p.walls.size() << " walls, " << p.entities.size() << " things, " << p.events.size()
              << " scripts, " << p.strings.size() << " texts, " << p.textures.size() << " textures, "
              << p.sprites.size() << " sprites, " << p.shapes.size() << " drawings\n";
    int bad = 0;   // checks failed
    std::string err;
    // 1. through JSON and back
    Project p2;
    bool jok = p2.from_json(p.to_json(), err) && p2.to_json() == p.to_json();
    std::cout << "project survives save/load: " << (jok ? "yes" : "NO " + err) << "\n";
    bad += !jok;
    // 2. rebuild a copy of the ROM from the level, timed
    auto t0 = std::chrono::steady_clock::now();
    Rom r2 = rom;
    BuildStats st;
    BuildOptions o = verbose();
    if (!r2.build(p, o, st, err)) return fail(err);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    // compare byte by byte; past the end of the shorter one counts as $EE
    // (so a size difference shows up as differing bytes)
    size_t n = std::max(rom.data.size(), r2.data.size()), diff = 0, first = 0;
    for (size_t i = 0; i < n; i++) {
        u8 a = i < rom.data.size() ? rom.data[i] : 0xEE, b = i < r2.data.size() ? r2.data[i] : 0xEE;
        if (a != b) {
            if (!diff) first = i;   // the first difference's offset
            diff++;
        }
    }
    std::cout << "rebuilt in " << secs << " s: " << r2.data.size() / 1024 << " KB, ";
    if (diff) {
        char buf[64];
        snprintf(buf, sizeof buf, "%zu bytes differ (first at $%06zX)", diff, first);
        std::cout << buf << "\n";
        bad++;
        std::string out = std::string(path) + ".rebuilt";
        r2.save(out, err);   // kept for a binary diff
        std::cout << "(written to " << out << ")\n";
    } else
        std::cout << "byte-for-byte identical to the original\n";
    // 3. the rebuilt ROM's level, compared as JSON
    Project p3;
    bool rok = r2.read_project(p3, err) && p3.to_json() == p.to_json();
    std::cout << "rebuilt ROM reads back: " << (rok ? "yes" : "NO") << "\n";
    bad += !rok;
    std::cout << (bad ? "SELFTEST FAILED\n" : "selftest ok\n");
    return bad ? 1 : 0;
}

// --rebuild ROM OUT: rebuilds the ROM from its own level and writes OUT.
static int rebuild(const char *in, const char *outp)
{
    Rom rom;
    Project p;
    if (!load(in, rom, p)) return 1;
    BuildStats st;
    std::string err;
    if (!rom.build(p, verbose(), st, err) || !rom.save(outp, err)) return fail(err);
    std::cout << "wrote " << outp << " (" << st.rom_kb << " KB)\n";
    return 0;
}

// --lock ROM OUT: ROM rebuilt without its editable copy (BuildOptions::
// locked): smaller, and the editor can't open it afterwards
static int lock(const char *in, const char *outp)
{
    Rom rom;
    Project p;
    if (!load(in, rom, p)) return 1;
    BuildStats st;
    std::string err;
    BuildOptions o = verbose();
    o.locked = true;
    size_t before = rom.data.size();
    if (!rom.build(p, o, st, err) || !rom.save(outp, err)) return fail(err);
    std::cout << "wrote " << outp << " (locked, " << st.rom_kb << " KB; " << (long)(before - rom.data.size()) / 1024
              << " KB smaller, " << st.free_kb << " KB free for maps)\n";
    return 0;
}

// --transfer FROM TO OUT: the level of ROM `from` built into a copy of ROM
// `to` (which may be another console's), written to OUT. With --map N, N
// picks the map in both ROMs (FROM's level, and the map of TO it replaces).
// (`unused` is never used.)
static int transfer(const char *from, const char *to, const char *outp)
{
    Rom a, b;
    Project p, unused;
    if (!load(from, a, p)) return 1;
    std::string err;
    if (!load_rom(to, b, err)) return fail(err);
    BuildStats st;
    if (!b.build(p, verbose(), st, err) || !b.save(outp, err)) return fail(err);
    std::cout << "the " << a.console_name() << " level is now in " << outp << " (" << b.console_name() << ", "
              << st.rom_kb << " KB)\n";
    return 0;
}

// --render ROM X Z FACING OUT.ppm [small] [low] [solid]: draws the view
// from cell (x, z) facing f (0 north, 1 east, 2 south, 3 west) with the
// Master System's renderer (sms_model.cpp), doors closed, and writes it as
// a binary PPM (P6). small / low / solid: the game's smaller view, low
// detail and solid walls settings. x, z and f are not range-checked.
static int render(const char *in, int x, int z, int f, const char *outp, bool small, bool low, bool solid)
{
    Rom rom;
    Project p;
    if (!load(in, rom, p)) return 1;
    sms::LevelData d;
    std::string err;
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
// changed art and text: proves that "edit -> build -> play" works end to
// end on either console (used by the tests).
// --demo-map ROM OUT [JSON]: keeps the ROM's art, thing types and texts,
// replaces the map, builds and writes OUT, and (optionally) the level's
// project.json to JSON.
static int demo_map(const char *in, const char *outp, const char *json_out)
{
    Rom rom;
    Project p;
    if (!load(in, rom, p)) return 1;
    // an empty map: every cell solid, nothing in it
    std::fill(p.wall.begin(), p.wall.end(), 1);
    std::fill(p.static_block.begin(), p.static_block.end(), 0);
    p.walls.clear();
    p.scenery.clear();
    p.entities.clear();
    p.lines.clear();
    p.events.clear();
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
    BuildStats st;
    std::string err;
    if (!rom.build(p, verbose(), st, err) || !rom.save(outp, err)) return fail(err);
    if (json_out) std::ofstream(json_out) << p.to_json();
    std::cout << "wrote " << outp << "\n";
    return 0;
}

// the maps in the original game, and whether each fits the consoles
// --maps JAR: converts each map in the jar and prints its counts and whether
// it fits the Master System's limits (check_limits), plus the converter's notes.
static int list_maps(const char *jar_path)
{
    Jar jar;
    std::string err;
    if (!jar.open(jar_path, err)) return fail(err);
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
                  << (lim.empty() ? "fits" : lim) << "\n";
        for (auto &n : notes) std::cout << "    " << n << "\n";
    }
    return 0;
}

// --from-game JAR MAP ROM OUT: converts one map of the original game
// (e.g. "reactor") and builds it into a copy of ROM (in place of its level,
// or of map --map N of a campaign ROM), written to OUT.
static int from_game(const char *jar_path, const char *map, const char *in, const char *outp)
{
    Jar jar;
    Project p;
    std::vector<std::string> notes;
    std::string err;
    if (!jar.open(jar_path, err) || !import_map(jar, map, p, err, &notes)) return fail(err);
    for (auto &n : notes) std::cout << "note: " << n << "\n";
    Rom rom;
    if (!load_rom(in, rom, err)) return fail(err);
    BuildStats st;
    if (!rom.build(p, verbose(), st, err) || !rom.save(outp, err)) return fail(err);
    std::cout << "wrote " << outp << " (" << rom.console_name() << ", " << st.rom_kb << " KB)\n";
    return 0;
}

// --rom-maps ROM: a campaign ROM's maps, numbered as --map takes them.
static int list_rom_maps(const char *path)
{
    Rom rom;
    std::string err;
    if (!load_rom(path, rom, err)) return fail(err);
    if (!rom.is_campaign()) {
        std::cout << "one level (no campaign)\n";
        return 0;
    }
    for (size_t m = 0; m < rom.maps.size(); m++)
        std::cout << m << ": " << rom.maps[m].name << " (" << rom.maps[m].title << "), map id " << rom.maps[m].id << "\n";
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
    if (!jar.open(jar_path, err)) return fail(err);
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
    if (!jar.open(jar_path, err)) return fail(err);
    bool sms = !strcmp(console, "sms"), gg = !strcmp(console, "gg");
    int bad = 0;   // maps skipped
    for (auto &m : engine::campaign_maps(jar)) {
        Project p;
        if (!import_map(jar, m, p, err, nullptr, !(sms || gg))) return fail(err);   // (as engine::new_rom reads it)
        Project q = p.for_console(console);
        Bytes b;
        try {   // (the builders' own calls: sms_build.cpp's, md_build.cpp's make_cmap)
            b = sms || gg ? make_strings_blob(q, false, gg) : make_strings_blob_md(q);
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

// --make-rom JAR ROM CONSOLE OUT: a ROM for CONSOLE made from ROM's maps
// (engine::make_rom_for; the graphics and sounds from the jar); locked for
// the consoles the editor doesn't edit
static int make_rom(const char *jar_path, const char *in, const char *console, const char *outp)
{
    Jar jar;
    std::string err;
    if (!jar.open(jar_path, err)) return fail(err);
    Rom src;
    Project p;
    if (!load(in, src, p)) return 1;
    int k = -1;   // the console named, by its CONSOLE_KEYS name
    for (int i = 0; i < 9; i++)
        if (!strcmp(console, CONSOLE_KEYS[i])) k = i;
    if (k < 0) return fail(std::string("unknown console ") + console + " (sms gg md snes snesfx 3ds 32x gbc gba)");
    Rom out;
    BuildStats st;
    if (!engine::make_rom_for((Console)k, jar, src, p, out, verbose(), st, err) || !out.save(outp, err)) return fail(err);
    std::cout << "wrote " << outp << " (" << console_name((Console)k) << (editable((Console)k) ? "" : ", locked") << ")\n";
    return 0;
}

// a new ROM from the original game: the Master System with one map (UAC Base
// by default), the Mega Drive with the given maps in order (all by default)
// --new-rom JAR CONSOLE OUT [maps...]: makes a ROM from the jar with the
// console's engine file (engines/) (engine::new_rom) and writes it to OUT.
static int new_rom(const char *jar_path, const char *console, const char *outp, std::vector<std::string> maps)
{
    Jar jar;
    std::string err;
    if (!jar.open(jar_path, err)) return fail(err);
    // the console name; any name not listed (including "md") means the Mega Drive
    Console c = !strcmp(console, "sms") ? Console::SMS : !strcmp(console, "gg") ? Console::GG
                : !strcmp(console, "snes") ? Console::SNES : !strcmp(console, "snesfx") ? Console::SNESFX
                : !strcmp(console, "3ds") ? Console::N3DS : !strcmp(console, "32x") ? Console::MARS
                : !strcmp(console, "gbc") ? Console::GBC : !strcmp(console, "gba") ? Console::GBA : Console::MD;
    // no maps named: the UAC Base ("intro") for the one-map consoles, else
    // the whole game in playing order
    if (maps.empty()) maps = sms_family(c) ? std::vector<std::string>{"intro"} : engine::campaign_maps(jar);
    Rom rom;
    BuildStats st;
    std::vector<std::string> notes;
    // build the ROM in memory, then save it
    if (!engine::new_rom(c, jar, maps, rom, verbose(), st, err, &notes) || !rom.save(outp, err)) return fail(err);
    for (auto &n : notes) std::cout << "note: " << n << "\n";
    std::cout << "wrote " << outp << " (" << rom.console_name() << ", " << rom.data.size() / 1024 << " KB, "
              << maps.size() << (maps.size() == 1 ? " map" : " maps") << ")\n";
    return 0;
}

// Picks the job from argv[1] (once --map N is taken out) and checks it has
// enough arguments; anything else prints the usage and returns 2.
int run_cli(int argc, char **argv)
{
    // --map N anywhere: the map of a campaign ROM to work on
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
    if (argc >= 3 && !strcmp(argv[1], "--rom-maps")) return list_rom_maps(argv[2]);
    if (argc >= 6 && !strcmp(argv[1], "--make-rom")) return make_rom(argv[2], argv[3], argv[4], argv[5]);
    if (argc >= 5 && !strcmp(argv[1], "--new-rom"))
        return new_rom(argv[2], argv[3], argv[4], std::vector<std::string>(argv + 5, argv + argc));
    if (argc >= 6 && !strcmp(argv[1], "--dump-assets")) return dump_assets(argv[2], argv[3], argv[4], argv[5]);
    if (argc >= 5 && !strcmp(argv[1], "--dump-strings")) return dump_strings(argv[2], argv[3], argv[4]);
    if (argc >= 3 && !strcmp(argv[1], "--maps")) return list_maps(argv[2]);
    if (argc >= 6 && !strcmp(argv[1], "--from-game")) return from_game(argv[2], argv[3], argv[4], argv[5]);
    if (argc >= 4 && !strcmp(argv[1], "--demo-map")) return demo_map(argv[2], argv[3], argc >= 5 ? argv[4] : nullptr);
    if (argc >= 3 && !strcmp(argv[1], "--selftest")) return selftest(argv[2]);
    if (argc >= 4 && !strcmp(argv[1], "--rebuild")) return rebuild(argv[2], argv[3]);
    if (argc >= 4 && !strcmp(argv[1], "--lock")) return lock(argv[2], argv[3]);
    if (argc >= 5 && !strcmp(argv[1], "--transfer")) return transfer(argv[2], argv[3], argv[4]);
    if (argc >= 7 && !strcmp(argv[1], "--render")) {
        bool small = false, low = false, solid = false;     // optional: small, low, solid
        for (int i = 7; i < argc; i++) {
            small |= !strcmp(argv[i], "small");
            low |= !strcmp(argv[i], "low");
            solid |= !strcmp(argv[i], "solid");
        }
        return render(argv[2], atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), argv[6], small, low, solid);
    }
    if (argc >= 4 && !strcmp(argv[1], "--export")) {
        // --export ROM DIR: the ROM's level as project.json + drawings + PNGs
        Rom rom;
        Project p;
        if (!load(argv[2], rom, p)) return 1;
        std::string err;
        if (!export_project(p, argv[3], err)) return fail(err);
        std::cout << "exported to " << argv[3] << "\n";
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "--import")) {
        // --import DIR ROM OUT: a level folder (as --export writes) built into a copy of ROM
        Rom rom;
        Project p;
        std::string err;
        if (!load_rom(argv[3], rom, err)) return fail(err);
        if (!import_project(argv[2], p, err)) return fail(err);
        BuildStats st;
        if (!rom.build(p, verbose(), st, err) || !rom.save(argv[4], err)) return fail(err);
        std::cout << "wrote " << argv[4] << "\n";
        return 0;
    }
    // no job matched: the usage, exit code 2
    std::cerr << "Deimos Editor " EDITOR_VERSION "\nusage:\n"
                 "  \"Deimos Editor\" [ROM]                      open the editor\n"
                 "  deimos-editor-cli --version                     the version\n"
                 "  deimos-editor-cli --selftest ROM                check unpack/repack\n"
                 "  deimos-editor-cli --new-rom DoomRPG.jar sms|gg|md|snes|snesfx|3ds|32x|gbc|gba OUT [maps...]   a new ROM from the original game\n"
                 "                 (Master System, Game Gear: one map, intro = the UAC Base by default;\n"
                 "                  Mega Drive: the maps in order, the whole game by default)\n"
                 "  deimos-editor-cli --rom-maps ROM                a Mega Drive campaign ROM's maps\n"
                 "  (add --map N to work on map N of a campaign ROM; the default is 0)\n"
                 "  deimos-editor-cli --rebuild ROM OUT\n"
                 "  deimos-editor-cli --make-rom DoomRPG.jar ROM sms|gg|md|snes|snesfx|3ds|32x|gbc|gba OUT   another console's ROM of ROM's maps\n"
                 "  deimos-editor-cli --lock ROM OUT                 a locked copy: no editable level (smaller; can't be opened)\n"
                 "  deimos-editor-cli --transfer FROM_ROM TO_ROM OUT   copy a level to another ROM\n"
                 "  deimos-editor-cli --render ROM X Z FACING OUT.ppm [small] [low] [solid]  Master System view\n"
                 "  deimos-editor-cli --maps DoomRPG.jar            the original game's maps, and which fit\n"
                 "  deimos-editor-cli --from-game DoomRPG.jar MAP ROM OUT   build a ROM with one of them\n"
                 "  deimos-editor-cli --export ROM DIR              level as JSON + PNG files\n"
                 "  deimos-editor-cli --import DIR ROM OUT          build a ROM from such files\n";
    return 2;
}
