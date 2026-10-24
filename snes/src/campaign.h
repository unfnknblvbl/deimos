/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the SNES: the campaign's block (the Mega Drive game's
 * tools/md_campaign.py format, big-endian, offsets from its start), in the
 * ROM from $C20000. Licence: GPL-2.0-or-later. */
/* The "campaign block" is the whole game's data in one blob made by
 * The Mega Drive game's build: every map's walls, textures, sprites, level data and
 * strings. It starts at the beginning of a ROM bank (block_bank, read from
 * the cartridge header by campaign_init), so a block offset becomes a long
 * ROM address as BLOCK_ADDR + offset. The layout used here:
 *   0   "MDCP", u16 version, u16 number of maps
 *   8   u32 texture bank offset, u32 sprite bank offset, ...
 *   32  per map, 32 bytes: u32 its "map pack" offset, u8 original map id,
 *       u8 0, title (26 bytes, zero-padded)
 * A map pack: "MDPK", version, entry count; at 8 its counts and colours;
 * at 32, per entry (the P_ enum below) a u32 offset and a u32 size.
 * Used by glue.c (levels, strings), vdata.c (the 3D view's pack), ui.c.
 * This file is also compiled for the 32X, 3DS, GBC and GBA ports, whose
 * machine layers provide far.s's functions. */
#ifndef CAMPAIGN_H
#define CAMPAIGN_H
#include "../../core/types.h"

extern u8 block_bank;                 /* the block's first bank (vdata.s) */
void campaign_init(void);             /* sets block_bank from the header (vdata.s) */
/* the block's long ROM address (bank:0000) */
#define BLOCK_ADDR ((u32)block_bank << 16)
/* map pack entries (md_campaign.py ENTRIES): the index of each table in a
   pack's entry list. P_LEVEL is the rules' level data, P_STRINGS the map's
   strings; the rest feed the 3D view (walls, textures, sprites, their
   palettes and histograms, the visibility lists). */
enum { P_WALLS, P_TEX_OFF, P_TEX_PAL, P_TEX_HIST, P_SPR_COLS, P_SPR_OFF, P_SPR_COFF, P_PVS_INDEX, P_PVS,
       P_SPR_PAL, P_SPR_HIST, P_SCENERY, P_DEF_SPR, P_VIS_INDEX, P_VIS, P_LEVEL, P_STRINGS };

u32 blk_u32(u32 off);                 /* the big-endian long at block + off */
u16 blk_u16(u32 off);                 /* the big-endian word at block + off */
void blk_copy(u32 off, void *dst, u16 len);     /* len bytes from block + off to dst */
void blk_strcpy(u32 off, char *dst, u16 max);   /* up to its NUL (dst gets one), at most max chars */
u8 n_maps(void);                      /* how many maps the block holds */
u32 map_pack(u8 map);                 /* its pack's offset */
u32 pack_entry(u8 map, u8 e, u32 *size);        /* entry e's offset; its size in *size (if not 0) */
const char *map_title(u8 map);        /* (in a static buffer) */
u8 map_orig_id(u8 map);               /* the map's number in the original game */

/* far.s: copies from a long address (anywhere in the ROM) into work RAM.
   C sets the parameters, then calls with no arguments. */
extern u8 far_src[4];                 /* the source: low, high, bank bytes (and a spare) */
extern u8 *far_dst;                   /* the destination (work RAM) */
extern u16 far_len;                   /* bytes to copy (counted down) */
void far_copy(void);
void far_strcpy(void);                /* far_len = the most it copies; a NUL ends it (and is copied) */
u32 far_u32(void);                    /* the big-endian long at far_src */
#endif
