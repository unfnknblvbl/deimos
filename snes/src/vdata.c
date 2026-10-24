/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the SNES: the 3D view's access to the map pack (view/vdata.h);
 * the byte reads are in vdata.s. Licence: GPL-2.0-or-later. */
/* view/view.c calls vd_pack when a map is loaded, to learn where the map's
 * tables are in the campaign block (the game's data, see campaign.h), and
 * vd_words to read runs of big-endian words. All addresses are block
 * offsets. */
#include "../../view/vdata.h"
#include "campaign.h"

/* n big-endian words from block offset a into dst */
void vd_words(u32 a, s16 *dst, u8 n)
{
    while (n--) {
        *dst++ = (s16)vd_u16(a);
        a += 2;
    }
}

/* fill p with the map's pack: the counts and colours at the pack's start
   (offset 8: u16 walls, textures, sprites, scenery, floor colour, ceiling
   colour; 20: u8 number of texture swaps, then the swap pairs, at most 5)
   and each table's offset (pack_entry); then the block's shared texture
   and sprite banks (the longs at block offsets 8 and 12) */
void vd_pack(u8 map, struct vpack *p)
{
    u32 pk = map_pack(map);             /* the pack's block offset */
    u8 i;
    p->n_walls = vd_u16(pk + 8);
    p->n_tex = vd_u16(pk + 10);
    p->n_spr = vd_u16(pk + 12);
    p->n_scenery = vd_u16(pk + 14);
    p->floor = vd_u16(pk + 16);
    p->ceiling = vd_u16(pk + 18);
    p->n_swaps = vd_u8(pk + 20);
    for (i = 0; i < 2 * p->n_swaps && i < 10; i++) p->swaps[i] = vd_u8(pk + 21 + i);
    p->walls = pack_entry(map, P_WALLS, 0);
    p->tex_off = pack_entry(map, P_TEX_OFF, 0);
    p->tex_pal = pack_entry(map, P_TEX_PAL, 0);
    p->spr_cols = pack_entry(map, P_SPR_COLS, 0);
    p->spr_off = pack_entry(map, P_SPR_OFF, 0);
    p->spr_coff = pack_entry(map, P_SPR_COFF, 0);
    p->pvs_index = pack_entry(map, P_PVS_INDEX, 0);
    p->pvs = pack_entry(map, P_PVS, 0);
    p->spr_pal = pack_entry(map, P_SPR_PAL, 0);
    p->scenery = pack_entry(map, P_SCENERY, 0);
    p->def_spr = pack_entry(map, P_DEF_SPR, 0);
    p->vis_index = pack_entry(map, P_VIS_INDEX, 0);
    p->vis = pack_entry(map, P_VIS, 0);
    p->tex_bank = vd_u32(8);
    p->spr_bank = vd_u32(12);
}
