/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the SNES: the campaign's block. Licence: GPL-2.0-or-later. */
/* Reads of the campaign block (the whole game's data, see campaign.h) by
 * block offset, through far.s's copy routines. Its callers are glue.c
 * (level data and strings for the rules), vdata.c (the 3D view's map
 * pack) and ui.c (titles). Also compiled for the 32X, 3DS, GBC and GBA. */
#include "campaign.h"

/* far_src = the ROM address of block offset off (bits 24-31 dropped) */
static void set_src(u32 off)
{
    u32 a = BLOCK_ADDR + off;
    far_src[0] = (u8)a;
    far_src[1] = (u8)(a >> 8);
    far_src[2] = (u8)(a >> 16);
}

/* the big-endian long at block offset off */
u32 blk_u32(u32 off)
{
    set_src(off);
    return far_u32();                   /* read 4 bytes, top byte first */
}

/* the big-endian word at block offset off: the top half of the long there
   (reads two bytes past the word, which is harmless in the ROM) */
u16 blk_u16(u32 off)
{
    return (u16)(blk_u32(off) >> 16);
}

/* copy len bytes from block offset off to dst (work RAM) */
void blk_copy(u32 off, void *dst, u16 len)
{
    set_src(off);
    far_dst = (u8 *)dst;
    far_len = len;
    far_copy();                         /* the copy itself (far.s) */
}

/* copy the NUL-terminated string at block offset off to dst, at most max
   characters; dst must hold max + 1 bytes (dst[max] is always set to 0) */
void blk_strcpy(u32 off, char *dst, u16 max)
{
    set_src(off);
    far_dst = (u8 *)dst;
    far_len = max;
    far_strcpy();                       /* copy up to and with the NUL (far.s) */
    dst[max] = 0;                       /* in case no NUL came within max */
}

/* the number of maps: the low byte of the long at 4 (u16 version, u16 count) */
u8 n_maps(void)
{
    return (u8)(blk_u32(4));
}

/* the map's pack offset: the first long of its 32-byte record at 32 */
u32 map_pack(u8 map)
{
    return blk_u32(32 + 32UL * map);
}

/* the block offset of entry e (P_*) of the map's pack; its size into *size
   when size is not 0. The entry list is at pack + 32: 8 bytes per entry,
   u32 offset (already from the block's start) then u32 size. */
u32 pack_entry(u8 map, u8 e, u32 *size)
{
    u32 pk = map_pack(map);
    if (size) *size = blk_u32(pk + 36 + 8UL * e);
    return blk_u32(pk + 32 + 8UL * e);
}

static char title[27];                  /* map_title's buffer: 26 chars and a NUL */

/* the map's title (26 bytes at record + 6), in a static buffer that the
   next call overwrites */
const char *map_title(u8 map)
{
    blk_copy(32 + 32UL * map + 6, title, 26);
    title[26] = 0;
    return title;
}

/* the map's id in the original game: the byte at record + 4 (the top byte
   of the long read there) */
u8 map_orig_id(u8 map)
{
    return (u8)(blk_u32(32 + 32UL * map + 4) >> 24);
}
