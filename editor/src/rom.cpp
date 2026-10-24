// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Licence: GPL-2.0-or-later.
//
// Opening, saving and rebuilding a ROM of any console (see rom.h). The
// console-specific work is in sms_build.cpp, md_build.cpp and the
// wrappers n3ds.cpp, mars.cpp, gbc.cpp, gba.cpp.
#include "rom.h"
#include "md_build.h"
#include "n3ds.h"
#include "mars.h"
#include "gba.h"
#include "gbc.h"
#include "sms_build.h"

#include <cstdio>
#include <fstream>

// see rom.h
bool read_file(const std::string &file, Bytes &out, std::string &err)
{
    std::ifstream f(file, std::ios::binary);
    if (!f) {
        err = "can't open " + file;
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

// see rom.h. The old file is removed before the rename (rename doesn't
// replace an existing file on Windows), so a failed rename leaves only the
// .tmp file.
bool write_file(const std::string &file, const Bytes &data, std::string &err)
{
    std::string tmp = file + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) {
            err = "can't write " + tmp;
            return false;
        }
        f.write((const char *)data.data(), data.size());
        if (!f) {
            err = "write failed: " + tmp;
            return false;
        }
    }
    std::remove(file.c_str());
    if (std::rename(tmp.c_str(), file.c_str()) != 0) {
        err = "can't rename " + tmp + " to " + file;
        return false;
    }
    return true;
}

// see rom.h. The wrapped formats are tried first (each must contain a
// SNES-layout image), then the plain cartridges by their headers.
bool Rom::load(const std::string &file, std::string &err)
{
    Bytes d;
    if (!read_file(file, d, err)) return false;
    Bytes w;   // the wrap, for the wrapped formats
    if (n3ds::is_3dsx(d)) {
        Bytes img;
        if (!n3ds::unwrap(d, w, img, err)) return false;
        if (!md::is_snes(img)) {
            err = "this .3dsx is not Deimos (Doom RPG) for the 3DS (0.1.0 or later)";
            return false;
        }
        d = std::move(img);
        console = Console::N3DS;
    } else if (mars::is_32x(d)) {
        Bytes img;
        if (!mars::unwrap(d, w, img, err)) return false;
        if (!md::is_snes(img)) {
            err = "this 32X cartridge is not Deimos (Doom RPG) for the 32X (0.1.0 or later)";
            return false;
        }
        d = std::move(img);
        console = Console::MARS;
    } else if (gba::is_gba(d)) {
        Bytes img;
        if (!gba::unwrap(d, w, img, err)) return false;
        if (!md::is_snes(img)) {
            err = "this cartridge is not Deimos (Doom RPG) for the Game Boy Advance (0.1.0 or later)";
            return false;
        }
        d = std::move(img);
        console = Console::GBA;
    } else if (gbc::is_gbc(d)) {
        Bytes img;
        if (!gbc::unwrap(d, w, img, err)) return false;
        if (!md::is_snes(img)) {
            err = "this cartridge is not Deimos (Doom RPG) for the Game Boy Color (0.1.0 or later)";
            return false;
        }
        d = std::move(img);
        console = Console::GBC;
    } else if (sms::is_rom(d)) console = sms::is_gg(d) ? Console::GG : Console::SMS;
    else if (md::is_snes(d)) console = Console::SNES;
    else if (md::is_snes_fx(d)) console = Console::SNESFX;
    else if (md::is_rom(d)) console = Console::MD;
    else {
        err = "this isn't a Deimos (Doom RPG) ROM the editor knows: it needs the Master System version 0.2.8 or later, "
              "the Mega Drive game (Mega Drive) 0.0.4 or later, the SNES version (with or without the SuperFX), the 3DS, the 32X, the Game Boy Color or the Game Boy Advance version 0.1.0 or later";
        return false;
    }
    // the campaign's map list (empty for a single-level Mega Drive ROM)
    std::vector<md::MapInfo> m;
    if (md_family(console) && !md::campaign_maps(d, m, err)) return false;
    data = std::move(d);
    wrap = std::move(w);
    path = file;
    maps = std::move(m);
    if (map >= (int)maps.size()) map = 0;   // keep the chosen map if the new ROM has it
    return true;
}

// see rom.h
bool Rom::save(const std::string &file, std::string &err) const
{
    if (console == Console::N3DS) {
        Bytes f;
        if (!n3ds::wrap(wrap, data, f, err)) return false;
        return write_file(file, f, err);
    }
    if (console == Console::MARS) {
        Bytes f;
        if (!mars::wrap(wrap, data, f, err)) return false;
        return write_file(file, f, err);
    }
    if (console == Console::GBA) {
        Bytes f;
        if (!gba::wrap(wrap, data, f, err)) return false;
        return write_file(file, f, err);
    }
    if (console == Console::GBC) {
        Bytes f;
        if (!gbc::wrap(wrap, data, f, err)) return false;
        return write_file(file, f, err);
    }
    return write_file(file, data, err);
}

// see rom.h
bool Rom::read_project(Project &p, std::string &err) const
{
    return sms_family(console) ? sms::read_project(data, p, err) : md::read_project(data, map, p, err);
}

// see rom.h. Builds into a copy, so a failed build leaves data as it was.
// (For a campaign ROM, p replaces the map `map`.)
bool Rom::build(const Project &p, const BuildOptions &opt, BuildStats &stats, std::string &err)
{
    Bytes d = data;
    BuildOptions o = opt;
    o.console = CONSOLE_KEYS[(int)console];   // its own pictures and texts (Project::for_console)
    bool ok = sms_family(console) ? sms::build(d, p, o, stats, err) : md::build(d, map, p, o, stats, err);
    if (ok) data = std::move(d);
    return ok;
}
