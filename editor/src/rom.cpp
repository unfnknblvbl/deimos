// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Licence: GPL-2.0-or-later.
//
// Saving a ROM of any console (see rom.h), and reading and writing whole
// files. The wrapped consoles' formats are in n3ds.cpp, mars.cpp,
// gbc.cpp and gba.cpp.
#include "rom.h"
#include "n3ds.h"
#include "mars.h"
#include "vb.h"
#include "gba.h"
#include "gbc.h"

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

// see rom.h
bool Rom::file_bytes(Bytes &out, std::string &err) const
{
    switch (console) {
    case Console::N3DS: return n3ds::wrap(wrap, data, out, err);
    case Console::MARS: return mars::wrap(wrap, data, out, err);
    case Console::GBA: return gba::wrap(wrap, data, out, err);
    case Console::GBC: return gbc::wrap(wrap, data, out, err);
    case Console::VB: return vb::wrap(wrap, data, out, err);
    default: out = data; return true;   // (the cartridge as it is)
    }
}

// see rom.h
bool Rom::save(const std::string &file, std::string &err) const
{
    Bytes f;
    return file_bytes(f, err) && write_file(file, f, err);
}
