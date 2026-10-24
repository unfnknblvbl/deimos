/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the Virtual Boy: view/view.c's drawing (the platform's
 * part: draw_walls, draw_spr) into fb, 128 x 88, column by column (gfx.c
 * makes the two eyes' pictures from it) - the build machine's way
 * (test/view_host.c), the GBA's view_draw.c with the picture turned on
 * its side: pixel (x, y) is fb[x * VIEW_H + y], so a column is 88 bytes
 * in a row.
 *
 * view.c works out, for each of the 128 columns, the nearest wall's
 * height in pixels (col_h), its texture column (col_tex, a block offset of
 * 64 bytes) and texture (col_t), then calls draw_walls; it then calls
 * draw_spr once per sprite column (ds_ globals), far to near. Colours go
 * through the per-texture and per-sprite remaps (tex_rm, spr_rm) to
 * palette slots 32.. . Pixels left 0 show the backdrop: gfx.c's floor and
 * ceiling shade for the row.
 *
 * For the stereo picture, draw_spr also notes in spr_h each column's
 * tallest thing (the nearest: a thing's height in pixels is 4096 over
 * its depth), so the column takes its depth (gfx.c's col_depth).
 * Licence: GPL-2.0-or-later.
 */
#include "plat.h"
#include "gfx.h"
#include "libc.h"

u8 fb[VIEW_W * VIEW_H] __attribute__((aligned(4)));     /* the view: x * VIEW_H + y */
u16 spr_h[VIEW_W];                      /* per column: the tallest thing drawn there (0 none) */

/* column: a column of a wall (sprite 0) or a sprite: texture rows from
   data (in the cartridge), height h (pixels), rows top..top+len-1 (a
   sprite's: 255 a hole); returns 1 if it drew a pixel */
static u8 column(u32 x, u32 h, const u8 *data, u32 top, u32 len, const u8 *rm, u32 sprite)
{
    /* the top: 28 of the 64 units above the eye (rounded) */
    s32 y0 = VIEW_HALF_H - (s32)((h * 28 + 32) >> 6), y, y1;
    /* dv: 64 texture rows over h pixels, in 1/256ths; k0: rows cut off above */
    u32 dv = 16384 / h, v, k0 = y0 < 0 ? (u32)-y0 : 0;
    u8 *d = fb + x * VIEW_H, drew = 0;
    v = (dv >> 1) + k0 * dv;            /* (sampled at the middle of each pixel) */
    y1 = y0 + (s32)h;
    if (y1 > VIEW_H) y1 = VIEW_H;
    y = y0 + (s32)k0;
    if (!sprite) {                      /* a wall: every row opaque */
        for (; y < y1; y++, v += dv) d[y] = rm[data[v >> 8]];
        return 1;
    }
    for (; y < y1; y++, v += dv) {
        u32 r = (v >> 8) - top;         /* the row in the column's data */
        u8 c;
        if (r >= len) continue;         /* (above top it wraps to a large value: skipped too) */
        c = data[r];
        if (c != 255) {
            d[y] = rm[c];
            drew = 1;
        }
    }
    return drew;
}

/* draw_walls: clear fb and the things' heights, then each column's wall
   (view.c's col_ arrays) */
void draw_walls(void)
{
    u32 x;
    memset(fb, 0, VIEW_W * VIEW_H);
    memset(spr_h, 0, sizeof spr_h);
    for (x = 0; x < VIEW_W; x++)
        if (col_h[x]) column(x, col_h[x], blk + col_tex[x], 0, 64, tex_rm[col_t[x]], 0);
}

/* draw_spr: one sprite column, as view.c's ds_ globals describe it; its
   height noted in spr_h if it showed and is the tallest there */
void draw_spr(void)
{
    if (column(ds_x, ds_h, blk + ds_data, ds_top, ds_len, spr_rm[ds_s], 1) && ds_h > spr_h[ds_x]) spr_h[ds_x] = ds_h;
}
