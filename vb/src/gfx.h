/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the Virtual Boy: the screen, the pad, the frames (gfx.c),
 * as the Game Boy Color's ui.c draws on them (the GBA's gfx.h, the same
 * calls). Licence: GPL-2.0-or-later. */
#ifndef VB_GFX_H
#define VB_GFX_H
#include "../../core/types.h"
#include "../../view/view.h"
#define BANKED                          /* (the Game Boy Color's bank keyword: nothing here) */

extern u16 pad;                         /* the SNES pad's bits (ui.c) */
extern volatile u16 frame_count;        /* display frames since the start (50.27 a second) */
void wait_vblank(void);                 /* show any change, wait a frame, read the pad, sound */
u16 ticks(void);                        /* frame_count */

void gfx_init(void);                    /* once: the display, the font, the frame interrupt */
void gfx_title(void);                   /* the title screen */
void gfx_game(void);                    /* the game screen, empty */
void gfx_message(const char *s);        /* the message line (up to 40 characters) */
void gfx_hud(char lines[5][21]);        /* the status bar's 5 lines of 20 */
void gfx_face(u8 f);                    /* the face picture f (0..8) */
void gfx_box(char box[16][24], u8 y, u8 h);    /* a box of h lines over the view (y unused) */
void gfx_box_off(void);
void gfx_weapon(u8 w, u8 firing);       /* weapon w's frame (firing or not) over the view */
void gfx_weapon_off(void);
void gfx_flash(u8 frames);              /* the damage flash (brighter for a few frames) */
void gfx_backdrop(const u8 *floor_rgb, const u8 *ceil_rgb, u8 berserk);   /* the floor and ceiling (8-bit r, g, b) */
void gfx_view(void);                    /* draw and show the 3D view where the player now is */

/* the view's picture and its depth (view_draw.c, gfx.c, viewtest.c) */
extern u8 fb[VIEW_W * VIEW_H];          /* the view: column by column (x * VIEW_H + y), colour slots, 0 the backdrop */
extern u16 spr_h[VIEW_W];               /* per column: the tallest (nearest) thing drawn in it (0: none) */
void gfx_picture(void);                 /* the stereo picture of fb onto the screen (a view just drawn) */
extern volatile u32 prof_last[4];       /* the last step's measurements (100 us units: gfx.c) */
#endif
