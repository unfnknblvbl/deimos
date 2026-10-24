/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * The Mega Drive game - the shaded floor and ceiling behind the view (backdrop.c).
 *
 * ui.c calls backdrop_init when the game screen is set up and whenever the
 * floor or ceiling colour changes, and uses backdrop_entry to fill plane B
 * (and to restore it when a dialog box closes): the backdrop tiles inside
 * the view, blank everywhere else. The H interrupt (crt0.s)
 * reads bd_rows / bd_rows_red through vdp.c's bd_rows_p.
 *
 * Licence: GPL-2.0-or-later.
 */
#ifndef BACKDROP_H
#define BACKDROP_H
#include "md.h"

#define TILE_BACKDROP 1504          /* VIEW_TH (16) tiles after the font, $BC00 */
#define BD_ROW_SLOT 13              /* palette 3, slots 13 and 14, per tile row */

/* per 16-line band of the view (VIEW_TH / 2 = 8 bands): the CRAM colours for slots 13
   (high word) and 14 (low word) of palette 3; _red: the same pushed to red
   for the damage flash */
extern u32 bd_rows[VIEW_TH / 2], bd_rows_red[VIEW_TH / 2];
/* make the shaded tiles and band colours for these CRAM colours, and upload
   the tiles */
void backdrop_init(u16 floor_colour, u16 ceiling_colour);
/* plane B's name table entry for screen tile column x (0..31), row y (0..27) */
u16 backdrop_entry(u8 x, u8 y);

#endif
