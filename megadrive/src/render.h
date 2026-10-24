/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * The Mega Drive game - the live first-person renderer.
 *
 * The C interface to src/render.s (68000 assembler), which draws the 3D
 * view into the VRAM tile buffer not on screen and queues the flip
 * (md.h: the VRAM layout). ui.c calls it after every action. Positions
 * are in world units: 64 units per map cell, so cell (gx, gz) has its
 * centre at (gx * 64 + 32, gz * 64 + 32). Facings: 0 north (-z), 1 east
 * (+x), 2 south (+z), 3 west (-x).
 *
 * Licence: GPL-2.0-or-later.
 */
#ifndef RENDER_H
#define RENDER_H
#include "md.h"

/* a thing to draw besides the walls: world position (64 units per cell)
   and sprite slot (render_data.h) */
struct robj {
    s16 x, z;
    u8 spr;
};

#define MAX_ROBJ 48         /* the most things one picture draws */

/* Draws the view from cell (gx, gz) looking along facing (0 N, 1 E, 2 S,
   3 W) into the back buffer, uploads it and flips at the next frame.
   line_open(k) tells whether door/line k is open (its wall not drawn). */
#define R_LOW 1            /* low detail: every second column, two pixels wide */
#define R_KEEP 2           /* keep the current palette */
#define R_PALETTE_ONLY 4   /* just choose the palette for this view (no picture) */
void render_view(u8 gx, u8 gz, u8 facing, const u8 *line_flags, const struct robj *objs, u8 n_objs, u8 mode);

/* an in-between picture of a step or turn (t = 0..256): half the
   columns and the last palette, so it is quick. The camera goes from cell
   (ax, az) facing af (t = 0) to (bx, bz) facing bf (t = 256). Used by
   smooth movement (ui.c animate_move). */
void render_between(u8 ax, u8 az, u8 af, u8 bx, u8 bz, u8 bf, u16 t, const u8 *line_flags,
                    const struct robj *objs, u8 n_objs);

/* after something else used the view's VRAM (the title): start again */
void render_reset(void);

/* 1 while a finished picture waits for its flip */
u8 render_busy(void);

extern u16 render_frames;   /* frames the last render took (for tuning) */

#endif
