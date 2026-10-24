/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the Game Boy Color: the 3D view's colours for a new map
 * (vdraw.c's: banked, once a map). Licence: GPL-2.0-or-later.
 *
 * view.c's map load fills the "remaps" (VR.tex_rm, VR.spr_rm: per texture
 * or sprite, its colour index -> a colour slot 32.. whose SNES colour is in
 * VR.view_cgram). The Game Boy cannot show those colours directly: this
 * rewrites every remap entry, in place, into the pixel value vdraw.c
 * stores, family * 4 + shade (shade 1..3, family 0..2), looked up in the
 * map's table from tools/gb_res.py (resource RES_MAPS: 530 bytes per map,
 * a 512-byte table indexed by a 9-bit colour, then the families' colours).
 * Runs with the view's RAM (cartridge RAM bank 3) in place: vdraw.c's
 * draw_walls calls it when the map changes. */
#pragma bank 255
#include <gb/gb.h>
#include "plat.h"
#include "../../view/view.h"
#include "../../view/vdata.h"
#include "view_ram.h"
#include "vdraw.h"

/* ---- the map's colours: the remaps' slots become pixel values */
/* a 5-bit SNES colour level -> the 3-bit Mega Drive level it was made from
   (view.c turns the Mega Drive's 8 levels into these 8 values); 0 when it
   is not one of them */
static u8 lev_of(u8 c5)
{
    static const u8 L5[8] = {0, 6, 10, 14, 18, 21, 25, 31};    /* view.c's LEV >> 3 */
    u8 i;
    for (i = 0; i < 8; i++)
        if (L5[i] == c5) return i;
    return 0;
}

/* a colour slot -> its pixel value: the slot's SNES colour (BGR555) back
   to a 9-bit Mega Drive colour (b2..0 g2..0 r2..0), looked up in the map's
   table at block offset table. Slots under VIEW_SLOT0 give 0 */
static u8 slot_value(u32 table, u8 slot)
{
    u16 c, i9;
    if (slot < VIEW_SLOT0) return 0;
    c = VR.view_cgram[slot - VIEW_SLOT0];
    i9 = lev_of(c & 31) | ((u16)lev_of((c >> 5) & 31) << 3) | ((u16)lev_of((c >> 10) & 31) << 6);
    return far_u8(table + i9);          /* (family * 4 + shade) */
}

/* every remap of the map just loaded (VR.cur_map_loaded) turned into pixel
   values; vd_conv_map records which map they are for. Called by vdraw.c's
   draw_walls, with cartridge RAM bank 3 mapped */
void vd_conv_remaps(void) BANKED
{
    u8 m = (u8)VR.cur_map_loaded, t, k, val[VIEW_SLOTS], n = VR.view_n_colours;
    u32 table = res_at(RES_MAPS) + 530UL * m;   /* this map's 512-byte table */
    for (k = 0; k < n; k++) val[k] = slot_value(table, VIEW_SLOT0 + k);     /* each slot's pixel value, once */
    /* the textures' remaps (32 colours each), then the sprites' (16): a slot
       outside the map's colours becomes 0, the backdrop */
    for (t = 0; t < V_MAX_TEX; t++)
        for (k = 0; k < 32; k++) {
            u8 s = VR.tex_rm[t][k];
            VR.tex_rm[t][k] = s >= VIEW_SLOT0 && s < VIEW_SLOT0 + n ? val[s - VIEW_SLOT0] : 0;
        }
    for (t = 0; t < V_MAX_SPR; t++)
        for (k = 0; k < 16; k++) {
            u8 s = VR.spr_rm[t][k];
            VR.spr_rm[t][k] = s >= VIEW_SLOT0 && s < VIEW_SLOT0 + n ? val[s - VIEW_SLOT0] : 0;
        }
    vd_conv_map = m;
}
