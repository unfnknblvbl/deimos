/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * The Mega Drive game - the shaded floor and ceiling.
 *
 * The 3D view leaves its floor and ceiling pixels at colour 0, which is
 * transparent, so plane B behind it shows through. Plane B holds one tile
 * per tile row of the view (all the view's columns use the same tile; the
 * border round the view is blank, so it shows black): the
 * level's floor and ceiling colours, darker towards the horizon, with a
 * 4x4 ordered dither between neighbouring shades, so the fade is smooth
 * even with the Mega Drive's 8 levels per channel. It costs nothing per
 * picture: the tiles are made once when the game screen is set up.
 *
 * Colours: palette 3 (shared with the text and the weapon) slots 13 and
 * 14: each 16-line band of the view dithers between two shades, which the
 * H interrupt puts in those slots just before the band is drawn (crt0.s
 * hblank_handler; bd_rows, and bd_rows_red for the damage flash).
 * The bands follow the view: band b is view rows 16b..16b+15, screen
 * lines VIEW_LINE0 + 16b.. (crt0.s works out the band from the line).
 * Brightness, t = 0 at the horizon .. 1 at the top / bottom of the view:
 * 0.3 + 0.9 t times the level's colour (so the nearest rows are a little
 * brighter than the colour the level gives, the farthest nearly black).
 *
 * Callers: ui.c (backdrop_init through update_backdrop: the game screen,
 * a new map, a floor or ceiling colour change, the Berserker's red;
 * backdrop_entry when it fills plane B or closes a dialog box).
 * Tiles: VIEW_TH (16) tiles from TILE_BACKDROP; tile r holds pixel rows
 * 8r..8r+7 of the view. Bands 0..3 (rows 0..63, above the horizon
 * HALF_H = 64) are the ceiling, bands 4..7 (rows 64..127) the floor.
 *
 * Licence: GPL-2.0-or-later.
 */
#include "md.h"
#include "backdrop.h"

/* the brightness (0..255) of each of the 8 levels of a CRAM colour channel
   (the console's measured output levels: not evenly spaced) */
static const u8 LEV[8] = {0, 52, 87, 116, 144, 172, 206, 255};
/* the 4x4 ordered (Bayer) dither thresholds, 0..15 */
static const u8 BAYER4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

u32 bd_rows[VIEW_TH / 2], bd_rows_red[VIEW_TH / 2];   /* per 16-line band: slot 13's colour << 16 | slot 14's */

/* the level (0..7) whose brightness is nearest v (0..255; the lower on a
   tie) */
static u8 nearest_level(u16 v)
{
    u8 i, best = 0;
    u16 bd = 0xFFFF;
    for (i = 0; i < 8; i++) {
        u16 d = v > LEV[i] ? v - LEV[i] : LEV[i] - v;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

/* the colour c (CRAM) scaled by f/256, as levels r, g, b and a brightness:
   returns the nearest CRAM colour; *bright = the sum of the scaled channel
   brightnesses (0..765, before rounding to levels) */
static u16 scaled(u16 c, u16 f, u16 *bright)
{
    u16 r = ((u16)LEV[(c >> 1) & 7] * f) >> 8, g = ((u16)LEV[(c >> 5) & 7] * f) >> 8,
        b = ((u16)LEV[(c >> 9) & 7] * f) >> 8;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    *bright = r + g + b;
    return (nearest_level(b) << 9) | (nearest_level(g) << 5) | (nearest_level(r) << 1);
}

/* the brightness of CRAM colour c: its three channels' LEV summed (0..765) */
static u16 cram_bright(u16 c)
{
    return LEV[(c >> 1) & 7] + LEV[(c >> 5) & 7] + LEV[(c >> 9) & 7];
}

/* f(t) in 1/256: 0.3 + 0.9 t, t in 1/256 (77 = 0.3 x 256, 230 = 0.9 x
   256): 77 at the horizon .. 307 at the edge */
static u16 fade(u16 t)
{
    return 77 + ((t * 230) >> 8);
}

/* the shades of one colour: distinct quantised colours along the fade,
   at most max of them (evenly picked), darkest first; -> how many */
static u8 ladder(u16 c, u16 *out, u8 max)
{
    u16 all[65], bright;
    u8 n = 0, i;
    /* 65 samples of the fade, t = 0, 4, .. 256; brightness only rises with
       t, so equal colours come together and one compare drops repeats */
    for (i = 0; i <= 64; i++) {
        u16 q = scaled(c, fade(i * 4), &bright);
        if (!n || all[n - 1] != q) all[n++] = q;
    }
    if (n <= max) {
        for (i = 0; i < n; i++) out[i] = all[i];
        return n;
    }
    /* too many: max of them, evenly spaced, the first and the last kept */
    for (i = 0; i < max; i++) out[i] = all[(u16)i * (n - 1) / (max - 1)];
    return max;
}

static u32 tiles[VIEW_TH][8];       /* one 8x8 tile per tile row of the view (a u32 = 8 pixels, 4 bits each) */

/* t of pixel row y: 0 at the horizon (between rows 63 and 64) .. 256 at the edge
   (measured at the row's middle: rows 63 and 64 give 2, rows 0 and 127
   give 254) */
static u16 row_t(u8 y)
{
    return y < VIEW_H / 2 ? (u16)(((VIEW_H / 2 - 1 - y) * 2 + 1) * 256 / VIEW_H)
                          : (u16)(((y - VIEW_H / 2) * 2 + 1) * 256 / VIEW_H);
}

/* band b (pixel rows 16b .. 16b + 15, two tile rows): its two shades
   (those around the brightness of its middle) and each pixel row's
   dither between them */
static void shade_band(u8 b, u16 c, const u16 *sh, u8 n)
{
    u16 target, lo_b, hi_b, k, lo = 0, hi = 0;
    u8 y, x;
    /* the brightness wanted at the band's middle row; the pair of shades
       (sh is darkest first) around it, or the end shade if beyond them */
    scaled(c, fade(row_t(b * 16 + 8)), &target);
    if (target <= cram_bright(sh[0]))
        lo = hi = 0;
    else if (target >= cram_bright(sh[n - 1]))
        lo = hi = n - 1;
    else
        for (k = 0; k + 1 < n; k++)
            if (target >= cram_bright(sh[k]) && target <= cram_bright(sh[k + 1])) {
                lo = k;
                hi = k + 1;
                break;
            }
    /* a wider pair when the band's ends go past it */
    {
        u16 t0, t1;
        scaled(c, fade(row_t(b * 16)), &t0);
        scaled(c, fade(row_t(b * 16 + 15)), &t1);
        if (t1 < t0) {
            u16 tt = t0;
            t0 = t1;
            t1 = tt;
        }
        while (lo > 0 && t0 < cram_bright(sh[lo])) lo--;
        while (hi + 1 < n && t1 > cram_bright(sh[hi])) hi++;
    }
    lo_b = cram_bright(sh[lo]);
    hi_b = cram_bright(sh[hi]);
    /* the band's colours: slot 13 the darker, slot 14 the brighter; for the
       damage flash the red channel set to full ($00E) */
    bd_rows[b] = ((u32)sh[lo] << 16) | sh[hi];
    bd_rows_red[b] = ((u32)(sh[lo] | 0x00E) << 16) | (sh[hi] | 0x00E);
    /* each pixel row: frac (0..16) = how far its brightness is from the dark
       shade to the bright one; a pixel takes the bright shade (slot 14)
       where frac is above its dither threshold, else the dark (slot 13).
       The 8 pixels go into one u32, the leftmost in the top nibble. */
    for (y = b * 16; y < b * 16 + 16; y++) {
        u16 frac = 0;
        u32 row = 0;
        scaled(c, fade(row_t(y)), &target);
        if (hi_b > lo_b) frac = target <= lo_b ? 0 : target >= hi_b ? 16 : (u16)((target - lo_b) * 16 / (hi_b - lo_b));
        for (x = 0; x < 8; x++) row = (row << 4) | (frac > BAYER4[y & 3][x & 3] ? BD_ROW_SLOT + 1 : BD_ROW_SLOT);
        tiles[y >> 3][y & 7] = row;
    }
}

/* make the VIEW_TH (16) tiles and the VIEW_TH / 2 (8) bands' colours for
   these floor and ceiling colours (CRAM words), and upload the tiles to
   VRAM (waits for the DMA). Up to 16 shades of each colour. */
void backdrop_init(u16 floor_colour, u16 ceiling_colour)
{
    u16 fsh[16], csh[16];
    u8 nf = ladder(floor_colour, fsh, 16), nc = ladder(ceiling_colour, csh, 16), r;
    for (r = 0; r < VIEW_TH / 2; r++)       /* bands 0..3 the ceiling, 4..7 the floor */
        if (r < VIEW_TH / 4) shade_band(r, ceiling_colour, csh, nc);
        else shade_band(r, floor_colour, fsh, nf);
    vdp_dma_vram(tiles, TILE_BACKDROP * 32, VIEW_TH * 16);     /* 16 tiles = 256 words */
}

/* plane B's name table entry for screen column x, row y outside dialog
   boxes: inside the view its row's backdrop tile in palette 3, elsewhere
   (the message line, the border round the view, under the status bar)
   the blank (transparent) tile */
u16 backdrop_entry(u8 x, u8 y)
{
    if (y >= VIEW_ROW0 && y < VIEW_ROW0 + VIEW_TH && x >= VIEW_COL0 && x < VIEW_COL0 + VIEW_TW)
        return NT_PAL(3) | (TILE_BACKDROP + y - VIEW_ROW0);
    return TILE_BLANK;
}
