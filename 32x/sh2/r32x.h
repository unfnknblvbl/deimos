/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the 32X: the 3D view (r32x.c). Licence: GPL-2.0-or-later. */
/* Used by sh2/plat.c (the master's frame loop) and sh2/sound32x.c (the
 * slave's loop, for r32x_slave_poll). "S" is the game state of core/rules.h. */
#ifndef R32X_H
#define R32X_H
#include "mars.h"
void r32x_init(void);                   /* once, before the first picture */
void r32x_target(void);                 /* the camera goes to the player (S) */
int r32x_step(int frame);               /* the camera at this frame -> 1 if it moved */
int r32x_changed(int buf);              /* the view differs from what buffer buf has */
void r32x_draw(int buf);                /* into the frame buffer's rows 8..183 */
void r32x_colours(u16 floor, u16 ceil); /* BGR555 */
const u16 *r32x_palette(void);          /* the view's colours (slots 32..223) */
void r32x_slave_poll(void);            /* (the slave) the right half of a picture, when asked */
#endif
