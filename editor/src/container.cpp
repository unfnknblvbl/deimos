// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Licence: GPL-2.0-or-later.
//
// Reading and writing the source container (layout in container.h).
// Called by rom.cpp (opening and saving ROMs) and the builders, which put
// the container at the end of every ROM they make.
#include "container.h"
#include "zx0.h"

#include <cstring>
#include <stdexcept>

// see container.h. Every entry is unpacked; a ZX0 entry also keeps its
// packed bytes, so writing it back unchanged doesn't recompress it.
bool read_entries(const u8 *c, size_t size, std::vector<SrcEntry> &out, std::string &err)
{
    out.clear();
    // 14 = magic (8) + total size (4) + entry count (2)
    if (size < 14 || memcmp(c, "DRPGSRC", 7) != 0) {
        err = "the ROM's copy of the level is missing or damaged";
        return false;
    }
    if (c[7] != '2') {
        err = "this ROM was made by an older version of the port; rebuild it with a current one";
        return false;
    }
    u32 total = get32(c + 8);
    unsigned n = get16(c + 12);
    if (total > size) {
        err = "the ROM's copy of the level is cut short";
        return false;
    }
    size_t pos = 14;
    try {
        for (unsigned i = 0; i < n; i++) {
            if (pos + 1 > total) throw std::runtime_error("entry table overruns the container");
            int len = c[pos++];
            // the name, then 9 bytes: raw size, stored size, method
            if (pos + len + 9 > total) throw std::runtime_error("entry table overruns the container");
            SrcEntry e;
            e.name.assign((const char *)c + pos, len);
            pos += len;
            u32 raw = get32(c + pos), packed = get32(c + pos + 4);
            int method = c[pos + 8];
            pos += 9;
            if (pos + packed > total) throw std::runtime_error("entry " + e.name + " overruns the container");
            if (method == 1) {   // ZX0
                e.data = zx0_decompress(c + pos, packed);
                e.packed.assign(c + pos, c + pos + packed);
            } else   // stored as it is
                e.data.assign(c + pos, c + pos + packed);
            if (e.data.size() != raw) throw std::runtime_error("entry " + e.name + " has the wrong size");
            pos += packed;
            out.push_back(std::move(e));
        }
    } catch (const std::exception &e) {
        err = std::string("the ROM's copy of the level: ") + e.what();
        return false;
    }
    for (auto &e : out)
        if (e.name == "locked") {   // (locked_container)
            err = "this ROM is locked: it was saved without its editable copy, to make room. Open the unlocked "
                  "copy it was made from instead.";
            return false;
        }
    return true;
}

// see container.h
Bytes locked_container() { return write_entries({{"locked", Bytes{'1'}, {}}}); }

// Builds p from the JSON entry named json_name ("project.json", or a
// campaign's "map/<name>.json") and every "shape/<key>" entry, then checks
// that each picture's drawing exists and drops the drawings this map
// doesn't use (a campaign's container holds every map's).
bool project_from_entries(const std::vector<SrcEntry> &entries, const std::string &json_name, Project &p,
                          std::string &err)
{
    const SrcEntry *pj = nullptr;
    for (auto &e : entries)
        if (e.name == json_name) pj = &e;
    if (!pj) {
        err = "the ROM's copy of the level has no " + json_name;
        return false;
    }
    if (!p.from_json(std::string(pj->data.begin(), pj->data.end()), err)) return false;
    for (auto &e : entries)
        if (e.name.rfind("shape/", 0) == 0) {   // the name starts with "shape/"
            if (e.data.size() != 4096) {
                err = "drawing " + e.name + " is not 64x64";
                return false;
            }
            // key = the name after "shape/"
            memcpy(p.shapes[e.name.substr(6)].px, e.data.data(), 4096);
        }
    // false with err when picture r (described by what) has no drawing
    auto check = [&](const ArtRef &r, const std::string &what) {
        if (!p.shapes.count(r.shape)) {
            err = what + " uses a drawing that isn't there (" + r.shape + ")";
            return false;
        }
        return true;
    };
    for (auto &kv : p.textures)
        if (!check(kv.second, "texture " + std::to_string(kv.first))) return false;
    for (auto &kv : p.sprites)
        if (!check(kv.second, "sprite " + std::to_string(kv.first))) return false;
    if (!check(p.border, "the border")) return false;
    for (auto &w : p.weapons)
        if (!check(w, "a weapon")) return false;
    p.drop_unused_shapes();
    return true;
}

// see container.h
bool read_container(const u8 *c, size_t size, Project &p, std::string &err)
{
    std::vector<SrcEntry> entries;
    return read_entries(c, size, entries, err) && project_from_entries(entries, "project.json", p, err);
}

// see container.h. Every entry is written with method 1 (ZX0); new ones
// are compressed in quick mode, as tools/source_container.py does.
Bytes write_entries(const std::vector<SrcEntry> &entries)
{
    // magic, then the total size (filled in at the end)
    Bytes out = {'D', 'R', 'P', 'G', 'S', 'R', 'C', '2', 0, 0, 0, 0};
    put16(out, (unsigned)entries.size());
    for (auto &e : entries) {
        Bytes packed = e.packed.empty() ? zx0_compress(e.data, true) : e.packed;
        out.push_back((u8)e.name.size());
        out.insert(out.end(), e.name.begin(), e.name.end());
        put32(out, (u32)e.data.size());
        put32(out, (u32)packed.size());
        out.push_back(1);   // method: ZX0
        out.insert(out.end(), packed.begin(), packed.end());
    }
    // the total size, little-endian, at offset 8
    for (int i = 0; i < 4; i++) out[8 + i] = (out.size() >> (8 * i)) & 0xFF;
    return out;
}

// see container.h
Bytes make_container(const Project &p)
{
    std::vector<SrcEntry> entries;
    std::string js = p.to_json();
    entries.push_back({"project.json", Bytes(js.begin(), js.end()), {}});
    // only the drawings in use, sorted by key (std::map order = Python sorted())
    Project q = p;
    q.drop_unused_shapes();
    for (auto &kv : q.shapes) entries.push_back({"shape/" + kv.first, Bytes(kv.second.px, kv.second.px + 4096), {}});
    return write_entries(entries);
}
