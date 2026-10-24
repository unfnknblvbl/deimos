/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the SNES: the screen (video.s, video.c). Licence: GPL-2.0-or-later. */
/* The screen layout and VRAM / CGRAM map are described at the top of
 * video.s. Used by ui.c, fx.c and the tests; the 32X, 3DS, GBC and GBA
 * ports provide the same functions in their machine layers. */
#ifndef VIDEO_H
#define VIDEO_H
#include "../../core/types.h"
void video_init(void);                 /* forced blank: VRAM cleared, Mode 7's map, registers */
void video_game(void);                 /* (forced blank) the game screen's modes */
void video_full(u8 bg1_tiles);         /* (forced blank) Mode 1 everywhere */
void screen_on(void);                  /* display on; the NMI and the pad read on */
void screen_off(void);                 /* forced blank */
void view_upload(void);                /* send the view's picture to VRAM over the next vblanks */
u8 view_busy(void);                    /* 1 while it (or any queued copy) is still going */
void cgram_load(void);                 /* send view_cgram to colours 32.. */
void flash(u8 frames);                 /* the damage flash: red added for that many frames */
void view_gradient(u16 floor, u16 ceiling);   /* the floor and ceiling (BGR555) */
extern u8 grad_hdma[];                 /* view_gradient's HDMA table (video.s) */
#endif
