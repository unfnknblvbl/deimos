/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the Game Boy Color: the 3D view's drawing (vdraw.c, bank 0). Licence: GPL-2.0-or-later.
 * gfx.c calls vd_init once and vd_tiles after each picture; view.c calls
 * vdraw.c's draw_walls and draw_spr (view/view.h). */
#ifndef GBC_VDRAW_H
#define GBC_VDRAW_H
#include "../../core/types.h"
#define VD_ATTR 3072                    /* the tiles' palettes, after the tiles (bank 6): offset from $A000 (192 tiles x 16 bytes) */
extern u8 vd_conv_map;                  /* the map whose colours the remaps hold (255: none yet) */
void vd_init(void);                     /* the picture's rows outside the view cleared, once */
void vd_tiles(void);                    /* the picture -> 192 tiles and their palettes, in bank 6 */
#endif
