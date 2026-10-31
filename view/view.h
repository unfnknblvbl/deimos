/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the SNES: the 3D view, a software renderer for a 128x88
 * chunky picture (shown by Mode 7 at twice the size; the SNES builds make
 * it 104x72, see VIEW_W below). The same C builds on
 * the SNES (cc65; the pixel loops are in assembler there) and on the build
 * machine (gcc; test/view_host.c), so the pictures can be compared byte
 * for byte.
 *
 * It draws the view the way the Mega Drive game does (the original's camera: 16 units
 * behind the cell centre, the eye 36 units up, walls 64 high, a 90 degree
 * field of view), from the campaign block's map pack (the Mega Drive game's
 * tools/md_campaign.py): the walls that can be seen from the cell
 * (pvs), the props and the things in the cells that can be seen (vis).
 *
 * The picture (fb) is in the order the SNES uploads it (snes/src/video.s:
 * VMAIN's 8-bit rotation turns it into Mode 7 tiles):
 *   offset(x, y) = ((x >> 3) * 3 + (y >> 5)) * 256 + (x & 7) * 32 + (y & 31)
 * so a screen column runs down 32 rows at a time. At any other size than
 * 128 x 88 (the SNES builds' 104 x 72) it is "tiled" instead (FB_TILED):
 * Mode 7 tiles of 8 x 8 pixels, each holding its 8 columns one after the
 * other, VIEW_H / 8 tiles down each 8-column strip, the strips in turn:
 *   offset(x, y) = ((x >> 3) * (VIEW_H / 8) + (y >> 3)) * 64 + (x & 7) * 8 + (y & 7)
 * which is the picture transposed, tile by tile: video.s uploads it as it
 * is (no remapping, no rows below the view: VIEW_W x VIEW_H bytes, 7488)
 * and Mode 7's matrix turns it back (x and y swapped). Either way a
 * pixel's offset is a column's part plus a row's (the compiled scalers
 * rely on that). Colour 0 is the floor
 * and ceiling (the backdrop shows through); the map's colours are 32..
 *
 * The other ports that use view.c (the Game Boy Color, the GBA) take the
 * same column and sprite outputs but draw them with their own draw_walls
 * and draw_spr into their own picture layouts; FB_OFF is the SNES's (and
 * the build machine's test) layout.
 *
 * Jargon: a "slot" is an index into the picture's palette (32..223); a
 * "remap" turns a texture's or sprite's own colour number into a slot; a
 * "block address" is an offset into the campaign block (the game's data,
 * read through view/vdata.h); "hq" is a height in pixels x 16.
 *
 * Licence: GPL-2.0-or-later.
 */
#ifndef VIEW_VIEW_H
#define VIEW_VIEW_H
#include "../core/types.h"

/* the view's size in pixels: 128 x 88 unless the build says otherwise
   (the SNES builds pass -D VIEW_W=104 -D VIEW_H=72, the same picture at
   80% of the size: snes/build.sh, build_fx.sh; every other port keeps
   these defaults). VIEW_W must be a multiple of 8 (at most 128), VIEW_H
   at 128 x 88 or else a multiple of 8 (at most 88). The picture buffer
   (fb) has FB_ROWS rows and FB_BYTES bytes: at 128 x 88, 96 rows (3
   blocks of 32; rows 88..95 not shown) and 12288 bytes; tiled (FB_TILED,
   any other size), VIEW_H rows and VIEW_W x VIEW_H bytes. FB_OFF(x, y) is
   a pixel's byte offset in it (see above); VIEW_HALF_H: the horizon row;
   VIEW_FOCAL: the focal length in pixels, half the width (a 90 degree
   field of view at any size); V_MAX_TEX / V_MAX_SPR: the most textures
   and sprites a map may have */
#ifndef VIEW_W
#define VIEW_W 128
#endif
#ifndef VIEW_H
#define VIEW_H 88
#endif
#if VIEW_W == 128 && VIEW_H == 88
#define FB_ROWS 96                      /* (rows 88..95: under the status bar) */
#define FB_BYTES (VIEW_W / 8 * FB_ROWS * 8)
#define FB_OFF(x, y) ((((u16)(x) >> 3) * 3 + ((y) >> 5)) * 256 + ((x) & 7) * 32 + ((y) & 31))
#else
#define FB_TILED 1                      /* the tiled, transposed layout */
#define FB_ROWS VIEW_H
#define FB_BYTES (VIEW_W * VIEW_H)
#define FB_OFF(x, y) ((((u16)(x) >> 3) * (VIEW_H / 8) + ((y) >> 3)) * 64 + ((x) & 7) * 8 + ((y) & 7))
#endif
#define VIEW_HALF_H (VIEW_H / 2)
#define VIEW_FOCAL (VIEW_W / 2)
#define VIEW_SLOT0 32                   /* the first colour slot of the view */
#define VIEW_SLOTS 192                  /* 32..223 (224.. are the sprites' palettes 6, 7) */
#define V_MAX_TEX 48
#define V_MAX_SPR 64

/* a new map: its colours (view_cgram, view_n_colours) and remaps
   (view_render calls it itself when S.map changes) */
void view_map_load(u8 map);
/* draws the view from the state S (the player's cell and facing, the
   doors, the things) */
void view_render(void);
/* the same in two parts (the fixed camera, not on the SNES): view_prepare
   works out the walls and the things; view_draw(x0, x1) draws columns
   x0 .. x1 - 1 of it (the platform's draw_walls draws strip_x0 ..
   strip_x1 - 1). Strips drawn in any order make the same picture. */
void view_prepare(void);
void view_draw(u8 x0, u8 x1);
extern u8 strip_x0, strip_x1;

/* the map's palette: view_n_colours slots used from 32; the floor and
   ceiling colours; per texture its 32 colours' slots, per sprite its 16 */
extern u16 view_cgram[VIEW_SLOTS];      /* SNES colours (BGR555) of slots 32.. */
extern u8 view_n_colours;
extern u16 view_floor, view_ceiling;    /* the level's colours (BGR555) */
extern u8 tex_rm[V_MAX_TEX][32];        /* texture / sprite colour -> slot */
extern u8 spr_rm[V_MAX_SPR][16];

/* per column, after the walls are worked out (the drawing reads them) */
extern u16 col_h[VIEW_W];               /* wall height in pixels (0: no wall) */
extern u32 col_tex[VIEW_W];             /* its texture column (block address) */
extern u8 col_t[VIEW_W];                /* its texture slot (remap) */

/* ---- the platform's: the map pack (view/vdata.h) and the drawing ----
   draw_walls: draw every column x with col_h[x] > 0: texture column
   col_tex[x] (64 bytes, rows 0..63) remapped by tex_rm[col_t[x]], col_h[x]
   pixels high around the horizon; the rest of the column colour 0.
   draw_spr: draw one sprite column as described by ds_* below. */
void draw_walls(void);                  /* every column, from col_*; floor and ceiling 0 */
/* one sprite column (ds_*): at screen column ds_x, canvas rows ds_top ..
   ds_top + ds_len - 1 from ds_data (block address of row ds_top; 255 is a
   hole), height ds_h, remap spr_rm[ds_s] */
extern u8 ds_x, ds_top, ds_len, ds_s;
extern u16 ds_h;
extern u32 ds_data;
void draw_spr(void);

#ifdef VIEW_FX
/* ---- the SuperFX build (see view.c) ----
   the free camera's limits: things (FX_MAX_TH), kept walls (FX_MAX_VW),
   things drawn (FX_MAX_PT), walls looked at past the cheap first test
   (FX_MAX_CAND), sprite columns (FX_MAX_SC), walls in a map
   (FX_MAX_WALLS). The GSU (snes/gsu/view.s) has the same limits. */
#define FX_MAX_TH 255
#define FX_MAX_VW 254
#define FX_MAX_PT 255
#define FX_MAX_CAND 320                 /* walls past the first pass */
#define FX_MAX_SC 640                   /* sprite columns */
#define FX_MAX_WALLS 768
/* a block of 4 x 4 cells' walls (fx_blk[64], 8 x 8 blocks): fx_worder
   lists the walls block by block */
struct fx_blk {
    u16 start, count;                   /* in fx_worder */
    s16 x0, z0, x1, z1;                 /* its walls' bounding box */
};
extern u16 fx_worder[FX_MAX_WALLS], fx_nwalls;
extern struct fx_blk fx_blk[64];
/* a thing to draw: its world position, sprite slot, and the scenery's
   flags (info & 0x78: a decal, on a wall) */
struct fx_th {
    s16 x, z;                   /* world */
    u8 spr, pad;
    u16 info;                   /* scenery's: decals 0x78 */
};
/* the camera (view_camera: x, z, forward x, z, right x, z; 8.8 for the
   vectors), the doors' flags (S.l_flags), the things list */
extern s16 fx_cam[6];
extern u8 *fx_open;
extern struct fx_th fx_th[FX_MAX_TH];
extern u8 fx_nth;
/* the camera at world (cx, cz) (a cell's centre, or between), angle a
   (256 a turn; facing f is 64 f); view_render_cam draws from it */
void view_camera(s16 cx, s16 cz, u8 a);
void view_render_cam(void);
void fx_draw(void);                     /* the platform's (the GSU) on the SNES */
struct vpack;
/* the SNES's (snes/src/fx.c): give a new map to the GSU */
void fx_map_load(const struct vpack *p);
#endif
#endif
