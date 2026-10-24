// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Licence: GPL-2.0-or-later.
//
// Reading and writing project files (layout in projectfile.h): the
// entries, then the maps and drawings they hold. Called by the App and the
// command-line tool to open and save projects.
#include "projectfile.h"
#include "rom.h"

#include "../third_party/json/json.hpp"
#include "../third_party/stb/stb_image.h"   // (stbi_zlib_decode_malloc; compiled in projectio.cpp)

#include <cstdlib>
#include <cstring>
#include <set>
#include <stdexcept>

// stb_image_write's deflate (compiled in projectio.cpp, not declared in its
// header): data_len bytes -> a malloc'ed zlib stream of *out_len bytes
extern "C" unsigned char *stbi_zlib_compress(unsigned char *data, int data_len, int *out_len, int quality);

// data packed with zlib's deflate at stb's best setting (8)
static Bytes zlib_pack(const Bytes &data)
{
    int n = 0;
    unsigned char *z = stbi_zlib_compress(const_cast<unsigned char *>(data.data()), (int)data.size(), &n, 8);
    if (!z) throw std::runtime_error("out of memory");
    Bytes out(z, z + n);
    free(z);
    return out;
}

// n bytes of a zlib stream unpacked (empty when they are damaged)
static Bytes zlib_unpack(const u8 *z, size_t n)
{
    int len = 0;
    char *raw = stbi_zlib_decode_malloc((const char *)z, (int)n, &len);
    if (!raw) return {};
    Bytes out(raw, raw + len);
    free(raw);
    return out;
}

// see projectfile.h. Every entry is unpacked; a packed entry also keeps its
// packed bytes, so writing it back unchanged doesn't recompress it.
bool read_entries(const u8 *c, size_t size, std::vector<SrcEntry> &out, std::string &err)
{
    out.clear();
    // 14 = magic (8) + total size (4) + entry count (2)
    if (size < 14 || memcmp(c, "DEIMPRJ", 7) != 0) {
        err = "this is not a Deimos project file";
        return false;
    }
    if (c[7] != '1') {
        err = "this project file was made by a newer version of the editor";
        return false;
    }
    u32 total = get32(c + 8);
    unsigned n = get16(c + 12);
    if (total > size) {
        err = "the project file is cut short";
        return false;
    }
    size_t pos = 14;
    try {
        for (unsigned i = 0; i < n; i++) {
            if (pos + 1 > total) throw std::runtime_error("the entry table runs past the end");
            int len = c[pos++];
            // the name, then 9 bytes: raw size, stored size, method
            if (pos + len + 9 > total) throw std::runtime_error("the entry table runs past the end");
            SrcEntry e;
            e.name.assign((const char *)c + pos, len);
            pos += len;
            u32 raw = get32(c + pos), packed = get32(c + pos + 4);
            int method = c[pos + 8];
            pos += 9;
            if (pos + packed > total) throw std::runtime_error("entry " + e.name + " runs past the end");
            if (method == 2) {   // zlib
                e.data = zlib_unpack(c + pos, packed);
                e.packed.assign(c + pos, c + pos + packed);
            } else if (method == 0)   // stored as it is
                e.data.assign(c + pos, c + pos + packed);
            else
                throw std::runtime_error("entry " + e.name + " is packed in a way the editor doesn't know");
            if (e.data.size() != raw) throw std::runtime_error("entry " + e.name + " has the wrong size");
            pos += packed;
            out.push_back(std::move(e));
        }
    } catch (const std::exception &e) {
        err = std::string("the project file is damaged: ") + e.what();
        return false;
    }
    return true;
}

// see projectfile.h. Builds p from the JSON entry named json_name and
// every "shape/<key>" entry, then checks that each picture's drawing
// exists and drops the drawings this map doesn't use.
bool project_from_entries(const std::vector<SrcEntry> &entries, const std::string &json_name, Project &p,
                          std::string &err)
{
    const SrcEntry *pj = nullptr;
    for (auto &e : entries)
        if (e.name == json_name) pj = &e;
    if (!pj) {
        err = "the project file has no " + json_name;
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
    for (auto &pc : p.per_console) {   // the consoles' own versions
        for (auto &kv : pc.second.textures)
            if (!check(kv.second, "texture " + std::to_string(kv.first) + " (" + pc.first + ")")) return false;
        for (auto &kv : pc.second.sprites)
            if (!check(kv.second, "sprite " + std::to_string(kv.first) + " (" + pc.first + ")")) return false;
    }
    p.drop_unused_shapes();
    return true;
}

// see projectfile.h. Every entry is written with method 2 (zlib).
Bytes write_entries(std::vector<SrcEntry> &entries)
{
    // magic, then the total size (filled in at the end)
    Bytes out = {'D', 'E', 'I', 'M', 'P', 'R', 'J', '1', 0, 0, 0, 0};
    put16(out, (unsigned)entries.size());
    for (auto &e : entries) {
        if (e.packed.empty()) e.packed = zlib_pack(e.data);
        out.push_back((u8)e.name.size());
        out.insert(out.end(), e.name.begin(), e.name.end());
        put32(out, (u32)e.data.size());
        put32(out, (u32)e.packed.size());
        out.push_back(2);   // method: zlib
        out.insert(out.end(), e.packed.begin(), e.packed.end());
    }
    // the total size, little-endian, at offset 8
    for (int i = 0; i < 4; i++) out[8 + i] = (out.size() >> (8 * i)) & 0xFF;
    return out;
}

// see projectfile.h. campaign.json gives the maps and their order; each
// map's level is read with the drawings it uses.
bool read_project_file(const Bytes &file, ProjectFile &pf, std::string &err)
{
    std::vector<SrcEntry> entries;
    if (!read_entries(file.data(), file.size(), entries, err)) return false;
    const SrcEntry *cj = nullptr;
    for (auto &e : entries)
        if (e.name == "campaign.json") cj = &e;
    if (!cj) {
        err = "the project file has no list of maps (campaign.json)";
        return false;
    }
    ProjectFile out;
    try {
        nlohmann::json j = nlohmann::json::parse(std::string(cj->data.begin(), cj->data.end()));
        for (auto &m : j.at("maps")) {
            ProjectMap pm;
            pm.info.name = m.at("name").get<std::string>();
            pm.info.title = m.at("title").get<std::string>();
            pm.info.id = m.at("id").get<int>();
            out.maps.push_back(std::move(pm));
        }
    } catch (const std::exception &e) {
        err = std::string("the project's list of maps: ") + e.what();
        return false;
    }
    if (out.maps.empty()) {
        err = "the project has no maps";
        return false;
    }
    for (auto &pm : out.maps)
        if (!project_from_entries(entries, "map/" + pm.info.name + ".json", pm.level, err)) {
            err = pm.info.title + ": " + err;
            return false;
        }
    for (auto &e : entries) out.stored[e.name] = std::move(e);
    pf = std::move(out);
    return true;
}

// compact JSON, as Project::to_json writes it
static std::string json_text(const nlohmann::ordered_json &j) { return j.dump(-1, ' ', true); }

// see projectfile.h
bool write_project_file(ProjectFile &pf, Bytes &file, std::string &err)
{
    if (pf.maps.empty()) {
        err = "the project has no maps";
        return false;
    }
    std::set<std::string> names;
    for (auto &pm : pf.maps)
        if (!names.insert(pm.info.name).second) {
            err = "two maps of the project are both called " + pm.info.name;
            return false;
        }
    // ---- the drawings: each key once; a clash in a later map gets a new key
    std::map<std::string, const Drawing *> all;   // key -> the drawing (in its map's level)
    for (auto &pm : pf.maps) {
        Project &q = pm.level;
        q.drop_unused_shapes();
        std::vector<std::string> keys;
        for (auto &kv : q.shapes) keys.push_back(kv.first);
        for (auto &k : keys) {
            auto it = all.find(k);
            if (it == all.end() || memcmp(it->second->px, q.shapes[k].px, 4096) == 0) continue;
            // the same key, another drawing: the first free <key>_<map>[n]
            std::string k2;
            for (int t = 1;; t++) {
                k2 = k + "_" + pm.info.name + (t > 1 ? std::to_string(t) : "");
                if (!all.count(k2) && !q.shapes.count(k2)) break;
            }
            q.rename_shape(k, k2);
        }
        for (auto &kv : q.shapes) all[kv.first] = &kv.second;
    }
    // ---- the entries, in the order the layout gives
    std::vector<SrcEntry> entries;
    // an entry with this data; the bytes packed before if it is unchanged
    auto entry = [&](const std::string &name, Bytes data) {
        SrcEntry e{name, std::move(data), {}};
        auto it = pf.stored.find(name);
        if (it != pf.stored.end() && it->second.data == e.data) e.packed = it->second.packed;
        entries.push_back(std::move(e));
    };
    nlohmann::ordered_json cj;
    cj["maps"] = nlohmann::ordered_json::array();
    for (auto &pm : pf.maps) {
        nlohmann::ordered_json o;
        o["name"] = pm.info.name;
        o["title"] = pm.info.title;
        o["id"] = pm.info.id;
        cj["maps"].push_back(o);
    }
    std::string t = json_text(cj);
    entry("campaign.json", Bytes(t.begin(), t.end()));
    for (auto &pm : pf.maps) {
        t = pm.level.to_json();
        entry("map/" + pm.info.name + ".json", Bytes(t.begin(), t.end()));
    }
    for (auto &kv : all) entry("shape/" + kv.first, Bytes(kv.second->px, kv.second->px + 4096));
    file = write_entries(entries);
    pf.stored.clear();   // (what is in the file now)
    for (auto &e : entries) pf.stored[e.name] = std::move(e);
    return true;
}

// see projectfile.h
bool load_project_file(const std::string &path, ProjectFile &pf, std::string &err)
{
    Bytes b;
    if (!read_file(path, b, err)) return false;
    if (!read_project_file(b, pf, err)) {
        err = path + ": " + err;
        return false;
    }
    return true;
}

// see projectfile.h
bool save_project_file(const std::string &path, ProjectFile &pf, std::string &err)
{
    Bytes b;
    return write_project_file(pf, b, err) && write_file(path, b, err);
}
