/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the SNES: the 3D view's access to the map pack (the
 * campaign block, big-endian), provided by the platform: the SNES reads
 * the ROM (snes/src/vdata.c), the build machine a file (test/view_host.c).
 * Addresses are block offsets.
 *
 * The campaign block is megadrive/tools/md_campaign.py format: a header,
 * a shared texture bank (64 x 64 textures, 4096 bytes each, column-major:
 * byte u * 64 + v) and sprite bank, and one "map pack" per map: counts,
 * then its tables (filled into struct vpack by vd_pack):
 *   walls      16 bytes per wall: ax, az, bx, bz, texture, line (0xFF a
 *              plain wall), ua | 0x100 if two-sided, ub (big-endian s16)
 *   tex_off / spr_off   u32 per texture / sprite: offset in its bank
 *   tex_pal / spr_pal   32 / 16 Mega Drive colours per texture / sprite
 *   spr_cols   per sprite 64 x (top row, length) of the opaque span
 *   spr_coff   per sprite 64 x u16: each column's data offset
 *   pvs_index, pvs   per view (cell * 4 + facing): the walls it can see
 *   vis_index, vis   per view: the cells whose things can show
 *   scenery    10 bytes per prop or decal: x, z, sprite, info, hiding cell
 *   def_spr    per entity def: its sprite slot
 * floor / ceiling: Mega Drive colours; swaps: up to 5 (from, to) texture
 * pairs for the Reactor's portal; tex_bank / spr_bank: the banks'
 * block offsets.
 * Licence: GPL-2.0-or-later. */
#ifndef VDATA_H
#define VDATA_H
#include "../core/types.h"

/* the map's pack: its counts and where its tables are */
struct vpack {
    u16 n_walls, n_tex, n_spr, n_scenery, floor, ceiling;
    u8 n_swaps, swaps[10];
    u32 walls, tex_off, tex_pal, spr_cols, spr_off, spr_coff, pvs_index, pvs, spr_pal, scenery, def_spr,
        vis_index, vis;
    u32 tex_bank, spr_bank;
};
/* vd_pack: fills p for map `map`; vd_u8 / vd_u16 / vd_u32: read the
   block at offset a */
void vd_pack(u8 map, struct vpack *p);
u8 vd_u8(u32 a);
u16 vd_u16(u32 a);                      /* big-endian */
u32 vd_u32(u32 a);
void vd_words(u32 a, s16 *dst, u8 n);   /* n big-endian words */
#endif
