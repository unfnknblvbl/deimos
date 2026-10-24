/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the Game Boy Color: the campaign's block (plat.c; the SNES's
 * campaign.h without its far-copy registers). Licence: GPL-2.0-or-later.
 *
 * The "campaign block" is megadrive/tools/md_campaign.py output: every map
 * of the game in one big-endian blob, offsets counted from its first byte
 * ("block offsets"). On the Game Boy it sits in ROM from bank DATA_BANK0
 * (plat.h); plat.c implements these functions with far_u8 / far_copy, which
 * map the right 16 KB ROM bank for a moment.
 *
 * Block layout (as plat.c and tools/campaign_block.py read it):
 *   0     "MDCP"
 *   4     u32, low byte: the number of maps
 *   8     u32 texture bank offset, 12 u32 sprite bank offset, 16 u32 weapons
 *   32 + 32 m   map m's record: u32 its pack's offset, u8 the original
 *               game's map id, (a byte), 26 bytes of title (NUL-padded)
 * A map's "pack": at +8 six u16 counts (walls, textures, sprites, scenery,
 * floor colour, ceiling colour), +20 u8 number of texture swaps, +21 the
 * swaps; at +32 + 8 e, for each entry e of the enum below, u32 offset and
 * u32 size of that table. */
#ifndef CAMPAIGN_H
#define CAMPAIGN_H
#include "../../core/types.h"
/* a map pack's entries, in the block's order (the index e above): the
   3D view's tables, then P_LEVEL the level blob (the core's map data) and
   P_STRINGS the map's strings (u16 offsets, then NUL-terminated text) */
enum { P_WALLS, P_TEX_OFF, P_TEX_PAL, P_TEX_HIST, P_SPR_COLS, P_SPR_OFF, P_SPR_COFF, P_PVS_INDEX, P_PVS,
       P_SPR_PAL, P_SPR_HIST, P_SCENERY, P_DEF_SPR, P_VIS_INDEX, P_VIS, P_LEVEL, P_STRINGS };
u32 blk_u32(u32 off);                   /* the big-endian u32 at block offset off */
u16 blk_u16(u32 off);                   /* the big-endian u16 at block offset off */
void blk_copy(u32 off, void *dst, u16 len);     /* len bytes from block offset off into dst */
void blk_strcpy(u32 off, char *dst, u16 max);   /* a string, at most max chars; dst[max] gets a NUL */
u8 n_maps(void);                        /* how many maps the block holds */
u32 map_pack(u8 map);                   /* the block offset of map's pack */
u32 pack_entry(u8 map, u8 e, u32 *size);    /* entry e's offset (P_*); its size into *size if not 0 */
const char *map_title(u8 map);          /* the map's title (in a static buffer: copy it before the next call) */
u8 map_orig_id(u8 map);                 /* the original game's id of the map */
#endif
