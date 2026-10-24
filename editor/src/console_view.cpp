// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Licence: GPL-2.0-or-later.
//
// The Map tab's preview for the consoles other than the Master System and
// the Game Gear (declared in console_view.h). Each console's function:
// 1. renders the view at the console's own resolution and focal length with
//    render_view (render.h), which also says which pixels are wall or sprite
//    ("covered") and which are background;
// 2. maps the covered pixels to the colours the console would use;
// 3. paints the background (floor and ceiling) the way the console does;
// 4. scales the picture to the size the console shows it, if it differs.
// The comments name the console ports' source files these imitate.
#include "console_view.h"
#include "colour.h"
#include "gbc.h"
#include "render.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace {
// the Mega Drive's 8 brightness levels (the same as colour.cpp's MD_LEVELS)
const int LEV[8] = {0, 52, 87, 116, 144, 172, 206, 255};
// a 4x4 ordered-dither (Bayer) matrix: thresholds 0..15
const int BAYER4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

// a 15-bit colour (the SNES's, the GBA's...) as the screen shows it
// (each 5-bit channel to 8 bits by repeating its top bits: 31 -> 255)
RGB c15(int r5, int g5, int b5) { return {(u8)(r5 << 3 | r5 >> 2), (u8)(g5 << 3 | g5 >> 2), (u8)(b5 << 3 | b5 >> 2)}; }

// a Mega Drive colour (what the jar's colours are kept as) in 15 bits: the
// SNES family's conversion (view.c's snes_colour)
// (the colour snapped to the Mega Drive's 3-bit levels, each level's
// brightness then cut to 5 bits)
RGB snes(RGB c)
{
    u16 m = md_colour(c);
    return c15(LEV[(m >> 1) & 7] >> 3, LEV[(m >> 5) & 7] >> 3, LEV[(m >> 9) & 7] >> 3);
}

// rgb888 -> 15 bits (the backdrop's: ui.c rgb15)
// (outputs the three 5-bit channels, 0..31)
void rgb15(RGB c, int &r, int &g, int &b)
{
    r = c.r >> 3;
    g = c.g >> 3;
    b = c.b >> 3;
}

// the SNES's shade (snes/src/video.c): 0.3 + 0.9 t over a view of n lines
// c: the floor or ceiling colour; line 0..n-1 of the view (the top half is
// ceiling). t (0..256, in 1/256ths) grows from the horizon to the top and
// bottom edges, so the backdrop is darkest at the horizon.
RGB shade(RGB c, int line, int n)
{
    int half = n / 2;
    // the distance from the horizon, measured at the line's middle, as 0..256
    int t = line < half ? ((half - 1 - line) * 2 + 1) * 256 / n : ((line - half) * 2 + 1) * 256 / n;
    // the brightness factor in 8.8 fixed point: 77/256 = 0.3, 230/256 = 0.9
    int f = 77 + ((t * 230) >> 8), r, g, b;
    rgb15(c, r, g, b);
    auto sc = [&](int v) { return std::min(31, (v * f) >> 8); };   // one channel times f, at most 31
    return c15(sc(r), sc(g), sc(b));
}

// render_view at size w x h with the given focal length (pixels, 0 = w / 2)
// and doors `open`; px gets the picture, cov 1 for wall / sprite pixels.
void render(const Project &p, const Geometry &geo, int gx, int gz, int f, const std::vector<u8> &open, int w, int h,
            double focal, std::vector<RGB> &px, std::vector<u8> &cov)
{
    ViewOptions o;
    o.w = w;
    o.h = h;
    o.focal = focal;
    o.line_open = open;
    o.covered = &cov;
    render_view(p, geo, gx, gz, f, o, px);   // the true-colour view
}

// the picture scaled to w2 x h2 (nearest: the consoles' own scaling)
// (the coverage map is scaled with it)
void scale(std::vector<RGB> &px, std::vector<u8> &cov, int w, int h, int w2, int h2)
{
    std::vector<RGB> o((size_t)w2 * h2);
    std::vector<u8> oc((size_t)w2 * h2);
    for (int y = 0; y < h2; y++)
        for (int x = 0; x < w2; x++) {
            int sx = x * w / w2, sy = y * h / h2;   // the source pixel (rounded down)
            o[y * w2 + x] = px[sy * w + sx];
            oc[y * w2 + x] = cov[sy * w + sx];
        }
    px.swap(o);
    cov.swap(oc);
}

// ---- the Mega Drive: 9-bit colours, at most 15 of them a view (the most
// used kept, the rest their nearest), the backdrop shaded per line with a
// 4x4 ordered dither between the 8 levels (megadrive/tools/backdrop.c)
void megadrive(const Project &p, const Geometry &geo, int gx, int gz, int f, const std::vector<u8> &open, ConsoleView &out)
{
    const int W = 208, H = 128;   // the Mega Drive's view size (80% of the screen's 256 x 160)
    std::vector<RGB> px;
    std::vector<u8> cov;
    render(p, geo, gx, gz, f, open, W, H, 0, px, cov);   // 90 degrees (focal W / 2)
    // how often each Mega Drive colour (CRAM word) is used by walls and sprites
    std::map<u16, int> count;
    for (int i = 0; i < W * H; i++)
        if (cov[i]) count[md_colour(px[i])]++;
    // sort by use, most first (negated count); equal counts: lower CRAM word first
    std::vector<std::pair<int, u16>> byuse;
    for (auto &kv : count) byuse.push_back({-kv.second, kv.first});
    std::sort(byuse.begin(), byuse.end());
    // the 15 kept colours, as the console shows them
    std::vector<RGB> keep;
    for (size_t i = 0; i < byuse.size() && i < 15; i++) keep.push_back(md_rgb(byuse[i].second));
    // squared RGB distance, to map the other colours to the nearest kept one
    auto d2 = [](RGB a, RGB b) { return (a.r - b.r) * (a.r - b.r) + (a.g - b.g) * (a.g - b.g) + (a.b - b.b) * (a.b - b.b); };
    // the backdrop: megadrive/tools/backdrop.c - each colour's ladder of shades
    // (6 for the floor, 5 for the ceiling), each line the two shades around
    // its brightness, mixed by a 4x4 ordered dither
    // the level (0..7) nearest a brightness; ties go to the lower
    auto nearest_level = [](int v) {
        int best = 0, bd = 1 << 30;
        for (int i = 0; i < 8; i++)
            if (std::abs(v - LEV[i]) < bd) bd = std::abs(v - LEV[i]), best = i;
        return best;
    };
    // CRAM colour c with each channel's brightness times f/256 (at most
    // 255), snapped back to a CRAM word; bright gets the sum of the three
    // scaled (unsnapped) channels
    auto scaled = [&](u16 c, int f, int &bright) {
        int r = std::min(255, LEV[(c >> 1) & 7] * f >> 8), g = std::min(255, LEV[(c >> 5) & 7] * f >> 8),
            b = std::min(255, LEV[(c >> 9) & 7] * f >> 8);
        bright = r + g + b;
        return (u16)((nearest_level(b) << 9) | (nearest_level(g) << 5) | (nearest_level(r) << 1));
    };
    // a CRAM word's brightness: the sum of its channels' levels (0..765)
    auto cram_bright = [](u16 c) { return LEV[(c >> 1) & 7] + LEV[(c >> 5) & 7] + LEV[(c >> 9) & 7]; };
    // the shading factor for t = 0..256, in 1/256ths: 0.3 + 0.9 t (as shade() above)
    auto fade = [](int t) { return 77 + ((t * 230) >> 8); };
    // the distinct shades colour c takes over t = 0, 4, ... 256, darkest
    // first; if there are more than max, max of them evenly spaced (the
    // first and the last always kept)
    auto ladder = [&](u16 c, int max) {
        std::vector<u16> all, out;
        int bright;
        for (int i = 0; i <= 64; i++) {
            u16 q = scaled(c, fade(i * 4), bright);
            if (all.empty() || all.back() != q) all.push_back(q);   // a new shade
        }
        if ((int)all.size() <= max) return all;
        for (int i = 0; i < max; i++) out.push_back(all[(size_t)i * (all.size() - 1) / (max - 1)]);
        return out;
    };
    u16 fc = md_colour(p.floor), cc = md_colour(p.ceiling);
    std::vector<u16> fsh = ladder(fc, 6), csh = ladder(cc, 5);   // floor: 6 shades, ceiling: 5
    for (int y = 0; y < H; y++) {
        bool up = y < H / 2;   // the ceiling half
        const std::vector<u16> &sh = up ? csh : fsh;
        // t: this line's distance from the horizon, 0..256 (as in shade());
        // target: the brightness the exact shade would have
        int n = (int)sh.size(), t = up ? ((H / 2 - 1 - y) * 2 + 1) * 256 / H : ((y - H / 2) * 2 + 1) * 256 / H, target,
            lo = 0, hi = 0, frac = 0;
        scaled(up ? cc : fc, fade(t), target);
        // the two ladder shades around the target (lo, hi) and how far it is
        // from lo towards hi, in 16ths (frac); beyond either end: that end
        if (target <= cram_bright(sh[0]))
            lo = hi = 0;
        else if (target >= cram_bright(sh[n - 1]))
            lo = hi = n - 1;
        else
            for (int k = 0; k + 1 < n; k++) {
                int a = cram_bright(sh[k]), b = cram_bright(sh[k + 1]);
                if (target >= a && target <= b) {
                    lo = k;
                    hi = k + 1;
                    frac = b > a ? (target - a) * 16 / (b - a) : 0;
                    break;
                }
            }
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            if (cov[i]) {
                // a wall or sprite pixel: its Mega Drive colour, or the
                // nearest of the 15 kept if it isn't one of them
                RGB v = md_rgb(md_colour(px[i]));
                RGB best = v;
                int bd = -1;
                for (auto &k : keep) {
                    int d = d2(v, k);
                    if (bd < 0 || d < bd) bd = d, best = k;
                }
                px[i] = best;
            } else
                // the backdrop: hi where frac beats the dither threshold, else lo
                px[i] = md_rgb(sh[frac > BAYER4[y & 3][x & 3] ? hi : lo]);
        }
    }
    out.w = W;
    out.h = H;
    out.px.swap(px);
    out.note = "208 x 128, the Mega Drive's colours: 15 of them for each view, the floor and ceiling dithered.";
}

// ---- the SNES family: view.c's 15-bit colours, the backdrop's colour per line
// px is w x h (already scaled to its screen size); n is the line count of
// the view the shading is worked out for (picture line y is mapped onto it).
// Wall and sprite pixels become snes() colours, the rest shade() of the
// floor or ceiling colour. Used for the SNES, SuperFX, 32X and GBA.
void shaded(const Project &p, std::vector<RGB> &px, const std::vector<u8> &cov, int w, int h, int n)
{
    for (int y = 0; y < h; y++) {
        int line = (2 * y + 1) * n / (2 * h);     // (the shade's line, of n, at this line's middle)
        RGB c = shade(line < n / 2 ? p.ceiling : p.floor, line, n);
        for (int x = 0; x < w; x++) {
            int i = y * w + x;
            px[i] = cov[i] ? snes(px[i]) : c;
        }
    }
}

// ---- the Game Boy Color: each 8 x 8 tile in one of three families of
// three shades (the map's, gbc::view_colours), the backdrop flat (three
// quarters of the level's colour)
// px is w x h (w a multiple of 8: the inner loops do not check x < w).
void gameboy_color(const Project &p, const Geometry &geo, std::vector<RGB> &px, const std::vector<u8> &cov, int w, int h)
{
    // how often each Mega Drive colour is used by the map's wall textures
    // and all its sprites (the input gbc::view_colours groups into families)
    std::map<unsigned, long> wmap;
    for (auto &t : geo.tex_imgs)
        for (auto &c : t) wmap[md_colour(c)]++;
    for (auto &kv : p.sprites) {
        Art a = p.flat(kv.second);
        for (int i = 0; i < 4096; i++)
            if (!a.clear(i)) wmap[md_colour(a.at(i))]++;
    }
    // place[9-bit colour]: family in bits 2 and up, shade 1..3 in bits 1-0;
    // fam[family][shade - 1]: the colour shown
    u8 place[512];
    RGB fam[3][3];
    gbc::view_colours(wmap, place, fam);
    // a colour's 9-bit index: red level | green << 3 | blue << 6
    auto idx9 = [](RGB c) {
        u16 m = md_colour(c);
        return ((m >> 1) & 7) | (((m >> 5) & 7) << 3) | (((m >> 9) & 7) << 6);
    };
    // the flat backdrop: three quarters of each 5-bit channel
    auto back = [&](RGB c) {
        int r, g, b;
        rgb15(c, r, g, b);
        return c15(r * 3 / 4, g * 3 / 4, b * 3 / 4);
    };
    RGB ceil = back(p.ceiling), flo = back(p.floor);
    // each 8x8 tile: the family most of its wall / sprite pixels belong to
    // (ties: the lower family) colours all of them
    for (int ty = 0; ty < h; ty += 8)
        for (int tx = 0; tx < w; tx += 8) {
            int n[3] = {0, 0, 0};   // pixels per family
            for (int y = ty; y < ty + 8 && y < h; y++)
                for (int x = tx; x < tx + 8; x++)
                    if (cov[y * w + x]) n[place[idx9(px[y * w + x])] >> 2]++;
            int f = n[1] > n[0] ? 1 : 0;
            if (n[2] > n[f]) f = 2;
            // each pixel: its shade in that family, or the backdrop (the
            // ceiling for tiles in the top half)
            for (int y = ty; y < ty + 8 && y < h; y++)
                for (int x = tx; x < tx + 8; x++) {
                    int i = y * w + x;
                    px[i] = cov[i] ? fam[f][(place[idx9(px[i])] & 3) - 1] : (ty < h / 2 ? ceil : flo);
                }
        }
}

// ---- the 3DS: the GPU's picture (400 x 240, a 69 degree view up and
// down), the floor and ceiling darker by distance (r3d.c)
// F: the focal length in pixels. Walls and sprites get snes() colours.
void three_ds(const Project &p, std::vector<RGB> &px, const std::vector<u8> &cov, int w, int h, double F)
{
    int fr, fg, fb, cr, cg, cb;   // the floor and ceiling colours in 5 bits a channel
    rgb15(p.floor, fr, fg, fb);
    rgb15(p.ceiling, cr, cg, cb);
    for (int y = 0; y < h; y++) {
        double dy = (y + 0.5) - h / 2.0, t;   // pixels below (+) or above (-) the horizon
        int r, g, b;
        // t: the brightness, 0.3 + 0.9 x (a constant / distance); the
        // constants 26.2 and 20.4 are r3d.c's; t may exceed 1 close up
        // (the result is capped at 255)
        if (dy > 0) {
            double d = 36.0 * F / dy;           // the floor's distance at this line
            t = 0.3 + 0.9 * (26.2 / std::max(1.0, d));
            r = fr, g = fg, b = fb;
        } else {
            double d = 28.0 * F / -dy;          // the ceiling's (64 - 36 = 28 units above the eye)
            t = 0.3 + 0.9 * (20.4 / std::max(1.0, d));
            r = cr, g = cg, b = cb;
        }
        // 5 bits to 0..255 (integer), times t
        RGB c = {(u8)std::min(255.0, r * 255 / 31 * t), (u8)std::min(255.0, g * 255 / 31 * t), (u8)std::min(255.0, b * 255 / 31 * t)};
        for (int x = 0; x < w; x++) {
            int i = y * w + x;
            px[i] = cov[i] ? snes(px[i]) : c;
        }
    }
}
}  // namespace

// ---- the Master System and Game Gear: 160 x 96, every wall and sprite
// pixel the console colour nearest it (the 64 of r, g, b in 0/85/170/255),
// the ceiling a flat colour and the floor fading into it towards the
// horizon by a 2 x 2 ordered dither, in the level's own SMS colours
// (Project::sms_ceil / sms_flo: by default a black ceiling and dark red
// floor), as sms_build's level data lays them out
void master_system(const Project &p, std::vector<RGB> &px, const std::vector<u8> &cov, int w, int h)
{
    // quarters of each 2 x 2 block that show the floor, per 8-line tile row
    // below the horizon (sms_build.cpp's FLOOR_LEVELS for the large view),
    // and the 2 x 2 order the quarters are filled in
    static const int LEVELS[6] = {0, 1, 2, 3, 4, 4};
    static const int BAYER2[2][2] = {{0, 2}, {3, 1}};
    // the console colour nearest c, channel by channel
    auto snap = [](RGB c) { return sms_rgb((int)py_round(c.r / 85.0) * 16 + (int)py_round(c.g / 85.0) * 4 + (int)py_round(c.b / 85.0)); };
    RGB ceil = snap(p.sms_ceil()), flo = snap(p.sms_flo());
    for (int y = 0; y < h; y++) {
        int row = std::min(std::max(y / 8 - h / 16, 0), 5);   // the tile row below the horizon (the last repeated)
        for (int x = 0; x < w; x++) {
            RGB &c = px[(size_t)y * w + x];
            if (cov[(size_t)y * w + x]) c = snap(c);                  // a wall or sprite pixel
            else if (y < h / 2) c = ceil;                             // the ceiling
            else c = BAYER2[y & 1][x & 1] < LEVELS[row] ? flo : ceil; // the floor, dithered
        }
    }
}

void console_view(Console c, const Project &p, const Geometry &geo, int gx, int gz, int f,
                  const std::vector<u8> &open, ConsoleView &out)
{
    std::vector<RGB> px;    // the picture
    std::vector<u8> cov;    // 1 where a wall or sprite was drawn
    // each case: the console's view size and focal length (64 for a 128-wide
    // view, 160 for 320: both 90 degrees), then its colours and scaling
    switch (c) {
    case Console::SMS:
    case Console::GG:
        render(p, geo, gx, gz, f, open, 160, 96, 80, px, cov);
        master_system(p, px, cov, 160, 96);
        out.w = 160;
        out.h = 96;
        out.note = c == Console::SMS ? "160 x 96, the Master System's 64 colours; a black ceiling and a red floor unless the level sets its own."
                                     : "160 x 96, the Master System's colours as the Game Gear shows them; a black ceiling and a red floor unless the level sets its own.";
        break;
    case Console::SNES:
    case Console::SNESFX:
        render(p, geo, gx, gz, f, open, 104, 72, 52, px, cov);
        scale(px, cov, 104, 72, 208, 144);   // shown doubled
        shaded(p, px, cov, 208, 144, 144);
        out.w = 208;
        out.h = 144;
        out.note = c == Console::SNES ? "104 x 72 at twice the size, 15-bit colours, the floor and ceiling shaded line by line."
                                      : "104 x 72 at twice the size (drawn by the SuperFX: steps and turns are smooth), 15-bit colours.";
        break;
    case Console::MARS:
        render(p, geo, gx, gz, f, open, 320, 176, 160, px, cov);
        shaded(p, px, cov, 320, 176, 176);
        out.w = 320;
        out.h = 176;
        out.note = "320 x 176 (the whole width), 15-bit colours, the floor and ceiling shaded.";
        break;
    case Console::GBA:
        render(p, geo, gx, gz, f, open, 128, 88, 64, px, cov);
        scale(px, cov, 128, 88, 160, 110);   // shown at 1.25 times
        shaded(p, px, cov, 160, 110, 176);   // shading worked out over 176 lines, as on the SNES
        out.w = 160;
        out.h = 110;
        out.note = "128 x 88 at 1.25 times (160 x 110), 15-bit colours, the floor and ceiling shaded line by line.";
        break;
    case Console::GBC:
        render(p, geo, gx, gz, f, open, 128, 88, 64, px, cov);
        gameboy_color(p, geo, px, cov, 128, 88);
        out.w = 128;
        out.h = 88;
        out.note = "128 x 88; each 8 x 8 tile has one of the map's three families of three colours.";
        break;
    case Console::N3DS: {
        // half the height (120 px) over the tangent of half the 69 degree vertical view
        double F = 120.0 / std::tan(34.5 * 3.14159265358979 / 180);
        render(p, geo, gx, gz, f, open, 400, 240, F, px, cov);
        three_ds(p, px, cov, 400, 240, F);
        out.w = 400;
        out.h = 240;
        out.note = "The top screen, 400 x 240, in 3D with the slider; the floor and ceiling darker with distance.";
        break;
    }
    default:
        megadrive(p, geo, gx, gz, f, open, out);   // fills out itself
        return;
    }
    out.px.swap(px);   // hand the picture over without copying
}
