// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Licence: GPL-2.0-or-later.
//
// Project folders and PNG pictures (see projectio.h). This file also
// holds the one copy of stb_image's and stb_image_write's code (the
// *_IMPLEMENTATION defines below); jar.cpp uses stb_image's zlib decoder
// from here.
#include "projectio.h"

#include "../third_party/json/json.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

// compile stb_image here, PNG only
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb/stb_image_write.h"

namespace fs = std::filesystem;

// see projectio.h: a 64x64 RGBA PNG; a sprite's CLEAR pixels are fully
// transparent black, everything else opaque
bool art_to_png(const Art &a, bool sprite, const std::string &file, std::string &err)
{
    std::vector<u8> rgba(64 * 64 * 4);
    for (int i = 0; i < 4096; i++) {
        bool clear = sprite && a.clear(i);
        RGB c = a.at(i);
        rgba[i * 4] = clear ? 0 : c.r;
        rgba[i * 4 + 1] = clear ? 0 : c.g;
        rgba[i * 4 + 2] = clear ? 0 : c.b;
        rgba[i * 4 + 3] = clear ? 0 : 255;
    }
    if (!stbi_write_png(file.c_str(), 64, 64, 4, rgba.data(), 64 * 4)) {
        err = "can't write " + file;
        return false;
    }
    return true;
}

// see projectio.h. rgba is w x h pixels, 4 bytes each. The picture is
// sampled down (or up) to 64x64 by nearest pixel; for a sprite, alpha
// below 128 is transparent. The palette is the 16 most used colours (ties:
// the lower RGB value first), unused entries black; every pixel takes the
// nearest palette colour (squared RGB distance).
void art_from_rgba(const u8 *rgba, int w, int h, bool sprite, Art &a)
{
    // sample to 64x64 (nearest), collect colours
    std::vector<RGB> px(4096);
    std::vector<bool> clear(4096);
    std::map<u32, int> count;   // 0xRRGGBB -> how many opaque pixels have it
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const u8 *s = rgba + ((size_t)(y * h / 64) * w + (x * w / 64)) * 4;
            int i = y * 64 + x;
            clear[i] = sprite && s[3] < 128;
            px[i] = {s[0], s[1], s[2]};
            if (!clear[i]) count[((u32)s[0] << 16) | (s[1] << 8) | s[2]]++;
        }
    // palette: up to 16 most used colours, then merge the rest to the nearest
    // sort by -count, so the most used come first
    std::vector<std::pair<int, u32>> byuse;
    for (auto &kv : count) byuse.push_back({-kv.second, kv.first});
    std::sort(byuse.begin(), byuse.end());
    int n = std::min<int>(16, (int)byuse.size());
    for (int i = 0; i < 16; i++) a.pal[i] = {0, 0, 0};
    for (int i = 0; i < n; i++) a.pal[i] = {(u8)(byuse[i].second >> 16), (u8)(byuse[i].second >> 8), (u8)byuse[i].second};
    for (int i = 0; i < 4096; i++) {
        if (clear[i]) {
            a.px[i] = CLEAR;
            continue;
        }
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < std::max(n, 1); k++) {   // (no colours at all: index 0)
            int dr = px[i].r - a.pal[k].r, dg = px[i].g - a.pal[k].g, db = px[i].b - a.pal[k].b;
            int d = dr * dr + dg * dg + db * db;
            if (d < bd) {
                bd = d;
                best = k;
            }
        }
        a.px[i] = best;
    }
}

// see projectio.h: loads any PNG as RGBA, then art_from_rgba
bool art_from_png(const std::string &file, bool sprite, Art &a, std::string &err)
{
    int w, h, n;
    u8 *d = stbi_load(file.c_str(), &w, &h, &n, 4);   // 4 = convert to RGBA
    if (!d) {
        err = "can't read " + file + " (PNG only)";
        return false;
    }
    art_from_rgba(d, w, h, sprite, a);
    stbi_image_free(d);
    return true;
}

namespace {
// one picture of the project and its PNG's path inside the folder
struct Pic {
    std::string file;      // relative path, e.g. "textures/3.png"
    ArtRef *ref;           // the picture (to change on import)
    const ArtRef *cref;    // the same, read-only
    bool sprite;           // may be transparent (sprites and weapons)
};

// every picture of p: textures, sprites, the border, the 18 weapon frames
std::vector<Pic> pictures(Project &p)
{
    std::vector<Pic> out;
    for (auto &kv : p.textures) out.push_back({"textures/" + std::to_string(kv.first) + ".png", &kv.second, &kv.second, false});
    for (auto &kv : p.sprites) out.push_back({"sprites/" + std::to_string(kv.first) + ".png", &kv.second, &kv.second, true});
    out.push_back({"border.png", &p.border, &p.border, false});
    for (size_t i = 0; i < p.weapons.size(); i++)
        out.push_back({"weapons/" + std::to_string(i / 2) + (i & 1 ? "_firing" : "") + ".png", &p.weapons[i],
                       &p.weapons[i], true});
    return out;
}
}  // namespace

// see projectio.h
bool export_project(const Project &p0, const MapInfo &info, const std::string &dir, std::string &err)
{
    Project p = p0;   // a copy: pictures() wants a non-const project
    try {
        for (const char *sub : {"textures", "sprites", "weapons", "drawings"}) fs::create_directories(fs::path(dir) / sub);
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }
    std::ofstream(fs::path(dir) / "project.json") << p.to_json();   // (a write error isn't detected)
    nlohmann::ordered_json mj;   // the map's place in the project
    mj["name"] = info.name;
    mj["title"] = info.title;
    mj["id"] = info.id;
    std::ofstream(fs::path(dir) / "map.json") << mj.dump(1, ' ', true) << "\n";
    // the drawings themselves: grey levels = palette index (255 = clear)
    for (auto &kv : p.shapes)
        if (!stbi_write_png((fs::path(dir) / "drawings" / (kv.first + ".png")).string().c_str(), 64, 64, 1,
                            kv.second.px, 64)) {
            err = "can't write drawing " + kv.first;
            return false;
        }
    // and every picture in its colours, to edit with any paint program
    for (auto &pic : pictures(p))
        if (!art_to_png(p.flat(*pic.cref), pic.sprite, (fs::path(dir) / pic.file).string(), err)) return false;
    return true;
}

// see projectio.h. Steps: map.json, project.json, then the drawings
// (64x64 grey PNGs; others are skipped), then each picture PNG compared
// with its drawing in its colours, then a check that every picture has a
// drawing.
bool import_project(const std::string &dir, Project &p, MapInfo &info, std::string &err)
{
    if (std::ifstream mf{fs::path(dir) / "map.json"}) {
        std::stringstream ms;
        ms << mf.rdbuf();
        try {
            nlohmann::json mj = nlohmann::json::parse(ms.str());
            info.name = mj.at("name").get<std::string>();
            info.title = mj.at("title").get<std::string>();
            info.id = mj.at("id").get<int>();
        } catch (const std::exception &e) {
            err = std::string("map.json: ") + e.what();
            return false;
        }
    }
    std::ifstream f(fs::path(dir) / "project.json");
    if (!f) {
        err = "no project.json in " + dir;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    if (!p.from_json(ss.str(), err)) return false;
    fs::path dd = fs::path(dir) / "drawings";
    if (fs::exists(dd))
        for (auto &e : fs::directory_iterator(dd)) {
            if (e.path().extension() != ".png") continue;
            int w, h, n;
            u8 *d = stbi_load(e.path().string().c_str(), &w, &h, &n, 1);   // 1 = as grey
            if (!d) continue;
            if (w == 64 && h == 64) memcpy(p.shapes[e.path().stem().string()].px, d, 4096);
            stbi_image_free(d);
        }
    // a picture changed in another program gets a drawing of its own
    for (auto &pic : pictures(p)) {
        fs::path file = fs::path(dir) / pic.file;
        if (!fs::exists(file)) continue;
        Art now;
        if (!art_from_png(file.string(), pic.sprite, now, err)) return false;
        // same = every pixel shows the same colour (or both transparent)
        Art was = p.flat(*pic.ref);
        bool same = p.drawing(*pic.ref) != nullptr;
        for (int i = 0; i < 4096 && same; i++) {
            bool ca = pic.sprite && was.clear(i), cb = pic.sprite && now.clear(i);
            if (ca != cb || (!ca && was.at(i) != now.at(i))) same = false;
        }
        if (same) continue;
        // changed: a new drawing ("snewN" / "tnewN") with the PNG's palette
        std::string key = p.new_shape_key(pic.sprite ? "s" : "t");
        memcpy(p.shapes[key].px, now.px, 4096);
        pic.ref->shape = key;
        for (int i = 0; i < 16; i++) pic.ref->pal[i] = now.pal[i];
    }
    for (auto &pic : pictures(p))
        if (!p.drawing(*pic.ref)) {
            err = pic.file + ": its drawing (" + pic.ref->shape + ") is missing";
            return false;
        }
    p.drop_unused_shapes();
    return true;
}
