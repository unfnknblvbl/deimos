/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// Master System: everything in the ROM that depends on the level, rebuilt
// from a project. Port of tools/live_data.py (renderer data, level header,
// border), tools/build_sprites.py (hardware sprites, both view sizes),
// tools/gen_cdata.py (level + strings) and tools/rom_dir.py (the ROM
// directory), producing byte-identical output.
// Licence: GPL-2.0-or-later.
//
// Called by engine.cpp (new ROMs) and the editor's previews (level_data,
// render, sprite_set). The
// ROM layout it writes is described in docs/rom_format.md: the level
// blob in bank 2, the level header in bank 3, strings and hardware sprite
// directories in bank 4, the level's big tables from bank 9 on, then the
// hardware sprites.
// Jargon: a "pair byte" is how the renderer stores a texel: two of the
// level's 16 colours, high nibble on even (x + y), low nibble on odd, so
// the eye sees their 50/50 mix. A "billboard" is a prop drawn by the
// renderer (into the view's tiles); a "hardware sprite" is a thing drawn
// by the video chip's sprites, prepared at 4 distances. A "view" is the
// list of wall pieces and props visible from one cell and facing.
#pragma once
#include "build.h"
#include "colour.h"
#include "geometry.h"
#include "project.h"

#include <array>
#include <map>
#include <vector>

namespace sms {

const int BANK = 0x4000;   // a 16 KB ROM bank
// bank 3 holds the level header and the small tables; the level's big
// tables start at bank 9 (banks 0..8 are the engine's)
const int LEVEL_BANK = 3, FIRST_DATA_BANK = 9;
const int VIEW_W[2] = {160, 128}, VIEW_H[2] = {96, 80};    // large, small (pixels)

// the art made a little more vivid before its colours are matched
// (tools/live_data.py brighter: integers, so both give the same)
RGB brighter(RGB c);

// the floor fades into the ceiling colour towards the horizon: the pair
// byte of floor row y (tools/live_data.py floor_row_pair)
u8 floor_row_pair(int size, int y);

// the level's 16 colours: pixels are one colour or a 50/50 pair
struct Colours {
    RGB rgb[16];          // the 16 colours (0 = ceiling, 1 = floor)
    u8 cram[16];          // the same as colour RAM bytes (--BBGGRR)
    // every colour a pixel can show: the 16 alone (A = B), then each pair
    // (A, B) whose mix is allowed; cand holds their Lab values
    std::vector<Lab> cand;
    std::vector<int> A, B;
    std::map<u32, u8> cache;   // 0xRRGGBB -> pair byte (shared by the build threads)
    // the nearest candidate to brighter(c), as a pair byte
    u8 pair(RGB c);                                   // A << 4 | B
};

// what the renderer draws from (also used by the editor's preview)
struct LevelData {
    Geometry geo;
    std::vector<Seg> walls;                           // = geo.segs, tex = image index
    Colours col;
    std::vector<std::array<u8, 64>> pool;             // texture columns (pair bytes)
    std::vector<std::array<u16, 64>> texcols;         // per texture image: column ids
    std::vector<int> bill_tiles;                      // billboard sprite slot -> tile
    // a billboard: per column its first opaque row and how many rows
    // follow (0 = an empty column), and those rows' pair bytes (key =
    // transparent)
    struct Bill {
        u8 top[64], len[64];
        std::vector<u8> data[64];
    };
    std::vector<Bill> bills;
    std::vector<Scenery> scen;                        // billboards (no decals)
    std::vector<int> scen_slot;                       // each prop's billboard slot
    u8 key = 0;                                       // transparent pair byte
    std::map<int, std::pair<std::vector<int>, std::vector<int>>> pvs;   // (cell*4 + f) -> walls, scenery
    std::vector<u8> border_tiles[2], border_map[2];   // large, small
    std::vector<u8> border_idx[2];                    // preview: 16 rows of tiles x 256 px
};

// pvs = false: skip the (slow) visibility lists
// Fills d from p: geometry, colours, texture columns, billboards, border
// and (pvs) the visibility lists; false with err when p breaks a limit.
bool level_data(const Project &p, LevelData &d, bool pvs, const BuildOptions &opt, std::string &err);

// hardware sprites for one entity type: 4 records (1..4 cells) + 16 CRAM bytes
// (records[k - 1]: the sprite as seen from k cells away; see sprite_record
// in sms_build.cpp for the layout)
struct SpriteSet {
    Bytes records[4];
    u8 cram[16];          // the 16 colours as colour RAM bytes (0 = transparent)
    RGB pal[16];
};
SpriteSet sprite_set(const Art &canvas, int size);   // size 0 = large, 1 = small
void sprite_colours(const Art &canvas, RGB pal[16]); // the 15 console colours a thing gets (+ 0)

// writes the level-dependent parts of a ROM image (the engine's): p in
// the single-level form (Project::single_level), as console opt.console
// ("sms" or "gg") has it, and texts, the texts table for bank 2 at $8000
// (gametext::sms_blob: the game's texts from the jar, for that console).
// rom is replaced only on success (then a power of two, 128 KB to 1 MB)
bool build(Bytes &rom, const Project &p, const Bytes &texts, const BuildOptions &opt, BuildStats &stats,
           std::string &err);

// the bit-exact model of rom/render.asm: H x W colour indices
// (sms_model.cpp) size 0 large, 1 small; (gx, gz, f) the cell and facing;
// line_open[slot] != 0 for an open door
std::vector<u8> render(const LevelData &d, int size, int gx, int gz, int f, const std::vector<u8> &line_open,
                       bool low_detail = false, bool solid_walls = false);   // the pause menu's DETAIL and WALLS

}  // namespace sms
