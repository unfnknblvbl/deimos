/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// The graphics and sounds outside the levels, made from the user's
// DoomRPG.jar when the editor makes a new ROM: byte for byte what the
// ports' own build tools make (tools/portable_img.py, doom_font.py,
// build_ui_gfx.py of the Master System port; build_md_ui.py,
// build_music.py and build_sfx.py of the Mega Drive game).
// Licence: GPL-2.0-or-later.
//
// Only engine.cpp's new_rom calls the *_ui / md_music / md_sfx / snes_res
// makers: each fills named blocks that must match, name for name and
// byte for byte in size, the regions make_engine.py blanked in the engine.
// The jar's pictures used: a.png (the font), doomrpg.png (the Master
// System's face), l.png (faces), m.png (icons), c.png (stars), j.png (the
// logo); its MIDI files give the music and some effects.
#pragma once
#include "common.h"
#include "jar.h"

#include <map>
#include <string>
#include <vector>

namespace assets {

// a paletted PNG: colour numbers, its palette and transparency
struct Indexed {
    int w = 0, h = 0;
    std::vector<u8> idx;      // w * h colour numbers, row by row
    u8 pal[256][3] = {};      // PLTE (RGB)
    u8 alpha[256];            // tRNS (255 where it gives none)
    Indexed() { std::fill(alpha, alpha + 256, 255); }
};
// decodes a non-interlaced paletted PNG (1, 2, 4 or 8 bits); false with
// err for anything else
bool load_png(const Bytes &file, Indexed &out, std::string &err);

// an image of 3 (RGB) or 4 (RGBA) bytes a pixel
struct Img {
    int w = 0, h = 0, c = 3;   // size, channels
    std::vector<u8> px;        // row by row, c bytes a pixel
    Img() = default;
    Img(int w_, int h_, int c_) : w(w_), h(h_), c(c_), px((size_t)w_ * h_ * c_, 0) {}
    // pixel (x, y)'s first channel (no bounds check)
    u8 *at(int x, int y) { return &px[((size_t)y * w + x) * c]; }
    const u8 *at(int x, int y) const { return &px[((size_t)y * w + x) * c]; }
};
// the colours of a paletted picture (with_alpha: RGBA, tRNS as alpha)
Img to_img(const Indexed &im, bool with_alpha);
// one of Deimos's own title pictures (art/<name>.png, embedded: "title_bg",
// "title_logo", "title_icon"), with `channels` channels (3 RGB, 4 RGBA)
Img art_img(const char *name, int channels);
// resize to w x h, bilinear in 8-bit fixed point (as portable_img.py)
Img resize_bilinear(const Img &a, int w, int h);
// draws RGBA src over RGB(A) dst with its top-left at (x0, y0), alpha blended
void blend(Img &dst, const Img &src, int x0, int y0);
// greedy_palette: colours ascending, counts per colour
// Returns up to n colours: `fixed` first, then each time the colour with
// the largest count x distance (dist) to those chosen.
std::vector<int> greedy_palette(const std::vector<int> &colours, const std::vector<int> &counts, int n,
                                const std::vector<int> &fixed, int (*dist)(int, int));

// the game's font (a.png): 8x8 tiles of 0/1
struct Font {
    std::map<int, std::vector<u8>> glyph;   // char -> 12 x 9 white core
    bool ok = false;
};
// a.png: a 16-wide grid of 9 x 12 cells, '!' (33) to '~' (126); a glyph's
// pixels are those of colour 1
Font load_font(const Indexed &a_png);
std::vector<u8> glyph_tile(const std::vector<u8> &g);   // 12x9 -> 8x8

// what a console's engine needs from the jar: named byte blocks
typedef std::map<std::string, Bytes> Blocks;
// gg: the Game Gear's status bar and title (tools/build_ui_gfx.py's *_gg.bin)
bool sms_ui(const Jar &jar, const std::string &version, bool gg, Blocks &out, std::string &err);
// word: the title's word over the logo (MEGA; the SNES's SUPER)
// (the Mega Drive's font, faces, icons, palettes and title)
bool md_ui(const Jar &jar, const std::string &version, Blocks &out, std::string &err, const std::string &word = "MEGA");
// the three songs (t.mid, i.mid, e.mid) as the sound driver's streams
bool md_music(const Jar &jar, Blocks &out, std::string &err);
// the 37 sound effects: some from the jar's MIDI files, the rest made here
bool md_sfx(const Jar &jar, Blocks &out, std::string &err);
// the SNES's resources (the Deimos repository's tools/snes_ui_res.py
// and snes_sound.py streams): the Mega Drive's graphics with the title
// word SUPER, and the music and effects streams laid out for the SPC
bool snes_res(const Jar &jar, const std::string &version, Blocks &out, std::string &err,
              const char *word = "SUPER");

}  // namespace assets
