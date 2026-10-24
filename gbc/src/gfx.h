/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the Game Boy Color: the screen, the pad, the frames (gfx.c). Licence: GPL-2.0-or-later.
 *
 * What ui.c calls to draw: the title, the game screen (3D view, message
 * line, face, status bar), the boxes (dialogs, menus) on the window layer
 * and the weapon as sprites. Every gfx_* function only prepares bytes and
 * queues "jobs"; the video RAM is written by wait_vblank in the vertical
 * blank. BANKED: the functions live in a switchable ROM bank and are
 * called through GBDK's banked-call trampoline. */
#ifndef GBC_GFX_H
#define GBC_GFX_H
#include "../../core/types.h"
#include <gb/gb.h>

extern u16 pad;                         /* the SNES pad's bits (ui.c's PAD_*), read once a frame by wait_vblank */
extern volatile u16 frame_count;        /* frames since power-on (wait_vblank counts them) */
void wait_vblank(void) BANKED;          /* wait for the next frame, run the queued VRAM jobs, read the pad */
u16 ticks(void);                        /* GBDK's frame counter (sys_time) */


void gfx_title(void) BANKED;            /* the title screen: logo, "COLOR", version */
void gfx_game(void) BANKED;             /* the game screen's maps, palettes and scroll */
void gfx_message(const char *s) BANKED; /* the message line (row 0): up to 26 characters, centred */
void gfx_hud(char lines[5][21]) BANKED; /* the status bar: 5 lines of 20 characters (changed lines only) */
void gfx_face(u8 f) BANKED;             /* the face picture f (0..8) */
void gfx_box(char box[16][24], u8 y, u8 h) BANKED;  /* a text box of h lines on the window (y unused) */
void gfx_box_off(void) BANKED;          /* the window off, the status bar back */
void gfx_weapon(u8 w, u8 firing) BANKED;    /* weapon w (0..8), its firing frame if firing, as sprites */
void gfx_weapon_off(void) BANKED;       /* every sprite hidden */
void gfx_flash(u8 frames) BANKED;       /* the view tinted red for that many frames (hurt) */
void gfx_backdrop(const u8 *floor_rgb, const u8 *ceil_rgb, u8 berserk) BANKED;  /* floor / ceiling colours (8-bit RGB) */
void gfx_view(void) BANKED;             /* draw the 3D view (view.c + vdraw.c) and show it */
#endif
