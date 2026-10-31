/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* The automap, as text: the cells of the 32 x 32 map the player has seen,
 * drawn one character a cell into a box (the pause menu's MAP), north up:
 *   #  a wall next to a seen cell      +  a closed door
 *   .  a seen floor cell               ^ > v <  the player, facing
 * A cell is seen once a view from where the player stood showed it (the
 * map's per-view lists of the cells whose things can show: the campaign
 * block's P_VIS_INDEX / P_VIS); once a script gives the map (GIVEMAP,
 * S.map_given) every cell counts as seen. The seen cells are kept for the
 * map being played (am_clear on a new map), not in the saves.
 *
 * Used by the SNES family's and the Game Boy family's ui.c (with the C
 * rules' wall_at / line_at and the campaign block) and by the Mega Drive's
 * ui.c (which defines AM_OWN_CELLS and gives am_wall / am_door itself).
 * Licence: GPL-2.0-or-later. */
#ifndef AUTOMAP_H
#define AUTOMAP_H
#include "types.h"

/* the Game Boy Color: the automap is in a ROM bank the linker picks
   (gbc/build.sh), so its functions are called through the bank switcher */
#if defined(__SDCC_sm83) || defined(__SDCC_gbz80)
#define AM_FN __banked
#else
#define AM_FN
#endif

void am_clear(void) AM_FN;                    /* a new map: nothing seen */
void am_mark(u16 cell) AM_FN;                 /* cell (z * 32 + x) seen */
/* the view (x, z, facing)'s cells seen: from the campaign block's lists of
   map index map (the SNES and Game Boy families; the Mega Drive's ui.c
   walks its own lists and calls am_mark) */
void am_look(u8 map, u8 x, u8 z, u8 facing) AM_FN;
/* row r (0 at the top) of a w x h window of the map centred on the player
   at (px, pz) facing f, into out (w characters and a NUL) */
void am_row(char *out, u8 r, u8 w, u8 h, u8 px, u8 pz, u8 f) AM_FN;

/* the cells' kinds (core/automap.c gives them from the C rules unless
   AM_OWN_CELLS: then the platform does) */
u8 am_wall(u8 x, u8 z) AM_FN;                 /* 1: a solid wall (outside the map is) */
u8 am_door(u8 x, u8 z) AM_FN;                 /* 1: a closed door */
#endif
