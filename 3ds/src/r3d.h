/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the 3DS: the 3D view (r3d.c). Licence: GPL-2.0-or-later. */
#ifndef R3D_H
#define R3D_H
#include <citro3d.h>
/* Used by src/plat.c. Screen widths (sw) are 400 for the top screen and
 * 320 for the bottom; flat coordinates are pixels, y down. */
void r3d_init(void);                    /* once, after C3D_Init */
void r3d_frame_start(void);             /* each frame, before any drawing: empties the vertex buffer */
void r3d_target(void);                  /* the camera goes to the player (S) */
int r3d_moving(void);                   /* 1 while the camera glides */
void r3d_colours(unsigned short floor, unsigned short ceil);   /* BGR555 */
void r3d_draw(C3D_RenderTarget *left, C3D_RenderTarget *right, float iod);   /* right 0: mono */
/* a texture's (u0, v0)-(u1, v1) (tx 0: a plain rectangle) at (x, y, w, h), tinted rgba, maybe added */
void r3d_flat(C3D_RenderTarget *t, int sw, C3D_Tex *tx, float x, float y, float w, float h, float u0, float v0,
              float u1, float v1, const float *rgba, int additive);
void r3d_automap(C3D_RenderTarget *t, int sw, float x, float y, float w, float h);   /* the walls seen, in that box */
#endif
