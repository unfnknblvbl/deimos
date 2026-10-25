// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Licence: GPL-2.0-or-later.
//
// Reading the engine files (format in engine.h, in the engines/ folder
// beside the program) and making new ROMs from them, the user's jar and
// the maps (the jar's own or a project's). Called by the App's "Make ROMs"
// windows and the command-line tool's --new-rom and --make-rom.
#include "engine.h"
#include "assets.h"
#include "gametext.h"
#include "levelblob.h"
#include "md_build.h"
#include "sms_build.h"
#include "zx0.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace engine {

// see engine.h: each console's engine file name, in Console's order
const char *file_name(Console c)
{
    static const char *const N[] = {"sg8.eng",     "sg8hh.eng",    "sg16.eng",     "ntnd16.eng", "ntnd16fx.eng",
                                    "ntnd3Dhh.eng", "sg32.eng",     "ntnd8hh.eng",  "ntnd32hh.eng", "ntnd32vr.eng"};
    return N[(int)c];
}

// the folder the running program is in ("" when it can't be told)
static fs::path program_dir()
{
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);   // the .exe's full path
    if (n == 0 || n >= MAX_PATH) return {};
    return fs::path(std::wstring(buf, n)).parent_path();
#else
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);  // (Linux: the running program)
    return ec ? fs::path() : exe.parent_path();
#endif
}

// see engine.h: $DEIMOS_ENGINES if set; else engines/ beside the program
// (the editor as it is passed on); else ../../engines from it (the editor
// run from its build folder, cmake-build/<platform>/, in the source)
std::string folder()
{
    if (const char *env = std::getenv("DEIMOS_ENGINES"); env && *env) return env;
    fs::path dir = program_dir();
    std::error_code ec;
    if (fs::is_directory(dir / "engines", ec)) return (dir / "engines").string();
    if (fs::is_directory(dir / ".." / ".." / "engines", ec)) return fs::weakly_canonical(dir / ".." / ".." / "engines", ec).string();
    return (dir / "engines").string();
}

// see engine.h
bool available(Console c)
{
    std::error_code ec;
    return fs::is_regular_file(fs::path(folder()) / file_name(c), ec);
}

// the whole of a file into out; false when it can't be read
static bool read_file(const fs::path &path, Bytes &out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// a little-endian u32
static u32 le32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

// see engine.h
bool load(Console c, Engine &e, std::string &err)
{
    // the console's engine file, read whole
    fs::path path = fs::path(folder()) / file_name(c);
    Bytes file;
    if (!read_file(path, file)) {
        err = std::string("this console's engine, ") + file_name(c) + ", is not in " + folder() +
              " (put the engines/ folder that came with the editor beside it)";
        return false;
    }
    const u8 *d = file.data();
    size_t n = file.size();
    auto bad = [&]() {
        err = std::string("the engine file ") + path.string() + " is damaged";
        return false;
    };
    // 20 = magic, console and padding (12), template size, region count
    if (n < 20 || memcmp(d, "DEIMENG1", 8) != 0) return bad();
    e = Engine();
    e.console = c;
    u32 size = le32(d + 12), nreg = le32(d + 16);
    size_t p = 20;
    for (u32 i = 0; i < nreg; i++) {
        if (p >= n) return bad();
        int l = d[p++];
        if (p + l + 8 > n) return bad();
        Region r;
        r.name.assign((const char *)d + p, l);
        p += l;
        r.offset = le32(d + p);
        r.size = le32(d + p + 4);
        p += 8;
        e.regions.push_back(r);
    }
    if (p >= n) return bad();
    int vl = d[p++];
    if (p + vl + 12 > n) return bad();
    e.version.assign((const char *)d + p, vl);
    p += vl;
    e.maps_addr = le32(d + p);
    e.maps_limit = le32(d + p + 4);
    u32 packed = le32(d + p + 8);
    p += 12;
    if (p + packed > n) return bad();
    e.rom = zx0_decompress(d + p, packed);   // the template
    if (e.rom.size() != size) return bad();
    p += packed;
    if (c == Console::N3DS || c == Console::MARS || c == Console::GBC || c == Console::GBA || c == Console::VB) {   // (then the .3dsx's code, icon and romfs
                                                        // headers, or the 32X's, Game Boy Color's or Game Boy Advance's code: u32 size, u32 packed, ZX0)
        if (p + 8 > n) return bad();
        u32 wsize = le32(d + p), wpacked = le32(d + p + 4);
        p += 8;
        if (p + wpacked > n) return bad();
        e.wrap = zx0_decompress(d + p, wpacked);
        if (e.wrap.size() != wsize) return bad();
    }
    return true;
}

// see engine.h (unpacks the whole engine to read it, once: kept per console
// with the file's time, so a replaced file is read again)
std::string version(Console c)
{
    static std::map<int, std::pair<fs::file_time_type, std::string>> seen;   // console -> (file time, version)
    std::error_code ec;
    auto t = fs::last_write_time(fs::path(folder()) / file_name(c), ec);
    if (ec) return "?";
    auto it = seen.find((int)c);
    if (it != seen.end() && it->second.first == t) return it->second.second;
    Engine e;
    std::string err;
    std::string v = load(c, e, err) ? e.version : "?";
    seen[(int)c] = {t, v};
    return v;
}

// see engine.h: the maps below that the jar has
std::vector<std::string> campaign_maps(const Jar &jar)
{
    // the game's maps in the order they are played (tools/build_campaign.py MAPS)
    std::vector<std::string> out;
    for (const char *m : {"intro", "junction", "level01", "level02", "level03", "level04", "level05", "level06",
                          "level07", "junction_destroyed", "reactor"})
        if (jar.get(std::string(m) + ".bsp")) out.push_back(m);
    return out;
}

// The console's engine with every blanked region filled from the jar (the
// status bar, fonts, title screen, music and sounds): the ROM to build the
// map(s) into, in rom. False with err when the jar's pictures don't match.
static bool filled_engine(Console c, const Jar &jar, Engine &e, Bytes &rom, const BuildOptions &opt, std::string &err)
{
    if (!load(c, e, err)) return false;
    // ---- the graphics and sounds from the jar
    opt.say("making the status bar, fonts, title screen" + std::string(md_family(c) ? ", music and sounds" : "") +
            " from the game...");
    // the console's assets; for the SNES-image consoles the argument is the
    // word on the title screen's logo
    assets::Blocks b;
    bool ok = sms_family(c) ? assets::sms_ui(jar, e.version, c == Console::GG, b, err)
              : c == Console::SNES ? assets::snes_res(jar, e.version, b, err)
              : c == Console::SNESFX ? assets::snes_res(jar, e.version, b, err, "SUPER FX")
              : c == Console::N3DS ? assets::snes_res(jar, e.version, b, err, "3D")
              : c == Console::MARS ? assets::snes_res(jar, e.version, b, err, "32X")
              : c == Console::GBA ? assets::snes_res(jar, e.version, b, err, "ADVANCE")
              : c == Console::VB ? assets::snes_res(jar, e.version, b, err, "VIRTUAL BOY")
              : c == Console::GBC ? assets::snes_res(jar, e.version, b, err)        // (the Game Boy takes the logo only)
                                   : assets::md_ui(jar, e.version, b, err) && assets::md_music(jar, b, err) &&
                                         assets::md_sfx(jar, b, err);
    if (!ok) return false;
    // fill every blanked region; each block must be exactly the region's size
    rom = e.rom;
    for (auto &r : e.regions) {
        auto it = b.find(r.name);
        if (it == b.end() || it->second.size() != r.size || (size_t)r.offset + r.size > rom.size()) {
            err = "this DoomRPG.jar's " + r.name + " is not the one the editor knows (a different version of the game?)";
            return false;
        }
        memcpy(&rom[r.offset], it->second.data(), r.size);
    }
    return true;
}

// The ROM for console c from an engine filled from the jar and the given
// maps' projects (infos: their names, titles and ids; the Master System
// and Game Gear take the first only, which must be a single level). Each
// project is built as console c has it (Project::for_console).
static bool rom_from_projects(Console c, const Jar &jar, const std::vector<MapInfo> &infos,
                              const std::vector<Project> &projects, Rom &out, const BuildOptions &opt_in,
                              BuildStats &stats, std::string &err)
{
    if (projects.empty()) {
        err = "no map chosen";
        return false;
    }
    BuildOptions opt = opt_in;
    opt.console = CONSOLE_KEYS[(int)c];
    Engine e;
    Bytes rom;
    if (!filled_engine(c, jar, e, rom, opt, err)) return false;
    if (sms_family(c)) {   // one map, in the Master System's form
        if (!sms::build(rom, projects[0], gametext::sms_blob(jar, c == Console::GG), opt, stats, err)) return false;
    } else {   // every map, in the campaign's form, as one block
        // (snes: every SNES-image console but the SuperFX; snes_fx: the SuperFX)
        if (!md::new_campaign(rom, e.maps_addr, e.maps_limit, infos, projects, gametext::md_blob(jar), opt, stats, err,
                              c == Console::SNES || c == Console::N3DS || c == Console::MARS || c == Console::GBC ||
                                  c == Console::GBA || c == Console::VB,
                              c == Console::SNESFX))
            return false;
    }
    out = Rom();
    out.console = c;
    out.data = std::move(rom);
    out.wrap = e.wrap;
    return true;
}

// see engine.h. The maps are read from the jar (the Master System's one as
// a single level), then built as above.
bool new_rom(Console c, const Jar &jar, const std::vector<std::string> &maps, Rom &out, const BuildOptions &opt,
             BuildStats &stats, std::string &err, std::vector<std::string> *notes)
{
    if (maps.empty()) {
        err = "no map chosen";
        return false;
    }
    std::vector<MapInfo> infos;
    std::vector<Project> projects;
    for (auto &m : maps) {
        Project p;
        opt.say("reading " + jar.short_title(m) + " from the game...");
        if (!import_map(jar, m, p, err, notes, !sms_family(c))) return false;
        infos.push_back({m, jar.short_title(m), p.map_id});
        projects.push_back(std::move(p));
        if (sms_family(c)) break;   // (one map)
    }
    return rom_from_projects(c, jar, infos, projects, out, opt, stats, err);
}

// see engine.h
bool new_project(const Jar &jar, const std::vector<std::string> &maps, ProjectFile &out, const BuildOptions &opt,
                 std::string &err, std::vector<std::string> *notes)
{
    if (maps.empty()) {
        err = "no map chosen";
        return false;
    }
    ProjectFile pf;
    for (auto &m : maps) {
        ProjectMap pm;
        opt.say("reading " + jar.short_title(m) + " from the game...");
        if (!import_map(jar, m, pm.level, err, notes, true)) return false;   // (the whole game's form)
        pm.info = {m, jar.short_title(m), pm.level.map_id};
        pf.maps.push_back(std::move(pm));
    }
    out = std::move(pf);
    return true;
}

// see engine.h
std::string why_not(Console c, const ProjectFile &pf, int map)
{
    if (pf.maps.empty()) return "the project has no maps";
    if (!sms_family(c)) return "";
    if (map < 0 || map >= (int)pf.maps.size()) return "no such map";
    std::string lim = check_limits(pf.maps[map].level.single_level());   // the Master System's limits
    return lim.empty() ? "" : pf.maps[map].info.title + " doesn't fit: " + lim;
}

// see engine.h
bool project_rom(Console c, const Jar &jar, const ProjectFile &pf, int map, Rom &out, const BuildOptions &opt,
                 BuildStats &stats, std::string &err)
{
    if (pf.maps.empty()) {
        err = "the project has no maps";
        return false;
    }
    std::vector<MapInfo> infos;
    std::vector<Project> projects;
    if (sms_family(c)) {   // the one map, as the Master System has it
        if (map < 0 || map >= (int)pf.maps.size()) {
            err = "the project has no map " + std::to_string(map);
            return false;
        }
        infos.push_back(pf.maps[map].info);
        projects.push_back(pf.maps[map].level.single_level());
    } else
        for (auto &pm : pf.maps) {
            infos.push_back(pm.info);
            projects.push_back(pm.level);
        }
    return rom_from_projects(c, jar, infos, projects, out, opt, stats, err);
}

}  // namespace engine
