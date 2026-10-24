/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* the Mega Drive game - the campaign's data block (tools/md_campaign.py): the maps,
 * the shared texture and sprite banks, the weapons. pack_init(map) points
 * the renderer's and the rules' tables at one map.
 *
 * The "campaign block" is one big-endian blob at the end of the ROM (the
 * .pack section, symbol md_campaign); the Doom RPG editor can replace it
 * without recompiling. Every offset in it counts from its start:
 *   0   "MDCP", u16 version, u16 number of maps (byte 7: the low byte)
 *   8   u32 texture bank, 12 u32 sprite bank, 16 u32 weapons,
 *   20  u32 block size, 24 u32 editable maps, 28 u32 their size
 *   32  per map, 32 bytes: u32 its map pack, u8 the original game's map
 *       id, u8 0, the title (26 bytes, zero-padded)
 * The weapons: 4 x (u32 offset, u32 size): frames, sprites, palettes,
 * tiles. A "map pack" (one map's tables):
 *   0   "MDPK", u16 version, u16 number of entries
 *   8   u16 walls, textures, sprites, scenery, floor colour, ceiling colour
 *   20  u8 number of texture swaps (the Reactor's portal), then the swaps
 *       as (open slot, closed slot) byte pairs
 *   32  per entry (P_* below): u32 offset, u32 size
 *
 * Callers: ui.c (main at start-up; through the rules, map_change and a
 * loaded game) and rules.s. The tables set here are read by render.s,
 * area.c, weapon.c and rules.s (level_blob, through level_load).
 * Licence: GPL-2.0-or-later. */
#include "md.h"
#include "data.h"
#include "render_data.h"
#include "weapon_data.h"
#include "pack.h"

extern const u8 md_campaign[];      /* the block (build/gen/campaign.s) */

/* the renderer's view of the map in play (render_data.h describes each):
   counts, the map's floor and ceiling colours (CRAM words), and pointers
   into its map pack */
u16 r_n_walls, r_n_tex, r_n_spr, r_n_scenery, r_floor_colour, r_ceiling_colour;
const short *r_walls, *r_scenery;
const unsigned char *r_spr_cols, *r_spr_data, *r_def_spr;
const unsigned short *r_tex_pal, *r_tex_hist, *r_spr_coff, *r_pvs_index, *r_pvs, *r_spr_pal, *r_spr_hist,
    *r_vis_index, *r_vis;
const unsigned long *r_spr_off;
const unsigned char *r_tex_addr[R_MAX_TEX];       /* each texture's 4096 texels */
static unsigned long spr_addr[R_MAX_SPR];         /* each sprite's column data (absolute) */
u8 r_tex_map[R_MAX_TEX];                          /* wall texture slot -> the slot drawn */
/* the portal's texture swaps: n_swaps pairs (open slot, closed slot) in
   swaps[]; swapped = 1 while the closed textures are in */
static u8 n_swaps, swaps[10], swapped;
const unsigned char *level_blob, *strings_blob;     /* the rules' level blob; the map's strings */
const unsigned short *wp_frames, *wp_sprites, *wp_pal;  /* the weapons (weapon_data.h) */
const unsigned char *wp_tiles;
u8 n_maps;
void area_init(u8 map);                             /* (area.c) */

/* the entries of a map pack, in order (tools/md_campaign.py ENTRIES) */
enum { P_WALLS, P_TEX_OFF, P_TEX_PAL, P_TEX_HIST, P_SPR_COLS, P_SPR_OFF, P_SPR_COFF, P_PVS_INDEX, P_PVS,
       P_SPR_PAL, P_SPR_HIST, P_SCENERY, P_DEF_SPR, P_VIS_INDEX, P_VIS, P_LEVEL, P_STRINGS };

/* a big-endian u32 at any (even odd) address */
static u32 rd32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* the map pack of map `map` (its record's first long) */
static const u8 *map_pack(u8 map)
{
    return md_campaign + rd32(md_campaign + 32 + 32 * map);
}

/* entry i (P_*) of map pack pk: its address */
static const void *entry(const u8 *pk, int i)
{
    return md_campaign + rd32(pk + 32 + 8 * i);
}

/* the title of map `map` ("SECTOR 1"...), zero-ended in its record */
const char *map_title(u8 map)
{
    return (const char *)md_campaign + 32 + 32 * map + 6;
}

/* the original game's id of map `map`.
   NOTE: the id is at byte 4 of the map's record, 32 + 32 * map + 4; this
   reads 24 + 32 * map + 4, eight bytes earlier. Nothing calls it now
   (rules.s keeps map_id from the level blob instead). */
u8 map_orig_id(u8 map)
{
    return md_campaign[24 + 32 * map + 4];
}

/* chooses map `map` (0..n_maps-1; a bad number gives map 0): sets n_maps,
   the weapon pointers, every r_* table pointer and count, the texture
   addresses (r_tex_addr) and swaps (reset to open), level_blob and
   strings_blob, then area_init for the map's colours. Called from main
   at start-up and by rules.s when a map starts or a game is loaded. */
void pack_init(u8 map)
{
    const u8 *pk, *wp = md_campaign + rd32(md_campaign + 16);
    const u8 *tb = md_campaign + rd32(md_campaign + 8), *sb = md_campaign + rd32(md_campaign + 12);
    const u16 *h;
    const u32 *to, *so;
    u16 i;
    n_maps = md_campaign[7];                          /* (the count's low byte) */
    wp_frames = (const void *)(md_campaign + rd32(wp));
    wp_sprites = (const void *)(md_campaign + rd32(wp + 8));
    wp_pal = (const void *)(md_campaign + rd32(wp + 16));
    wp_tiles = md_campaign + rd32(wp + 24);
    if (map >= n_maps) map = 0;
    pk = map_pack(map);
    h = (const u16 *)(pk + 8);                        /* the map's counts and colours */
    r_n_walls = h[0];
    r_n_tex = h[1];
    r_n_spr = h[2];
    r_n_scenery = h[3];
    r_floor_colour = h[4];
    r_ceiling_colour = h[5];
    if (r_n_tex > R_MAX_TEX) r_n_tex = R_MAX_TEX;      /* (the builders never write more) */
    if (r_n_spr > R_MAX_SPR) r_n_spr = R_MAX_SPR;
    r_walls = entry(pk, P_WALLS);
    to = entry(pk, P_TEX_OFF);
    for (i = 0; i < r_n_tex; i++) r_tex_addr[i] = tb + to[i];     /* bank offset -> address */
    for (i = 0; i < R_MAX_TEX; i++) r_tex_map[i] = i;              /* no swap in force */
    /* at most 5 swaps (swaps[] has room for 5 pairs); a slot out of range
       becomes 0 */
    n_swaps = pk[20] > 5 ? 5 : pk[20];
    for (i = 0; i < 2 * n_swaps; i++) swaps[i] = pk[21 + i] < r_n_tex ? pk[21 + i] : 0;
    swapped = 0;
    r_tex_pal = entry(pk, P_TEX_PAL);
    r_tex_hist = entry(pk, P_TEX_HIST);
    r_spr_cols = entry(pk, P_SPR_COLS);
    so = entry(pk, P_SPR_OFF);
    for (i = 0; i < r_n_spr; i++) spr_addr[i] = (unsigned long)(sb + so[i]);
    r_spr_off = spr_addr;                             /* (absolute: the renderer adds r_spr_data) */
    r_spr_data = 0;
    r_spr_coff = entry(pk, P_SPR_COFF);
    r_pvs_index = entry(pk, P_PVS_INDEX);
    r_pvs = entry(pk, P_PVS);
    r_spr_pal = entry(pk, P_SPR_PAL);
    r_spr_hist = entry(pk, P_SPR_HIST);
    r_scenery = entry(pk, P_SCENERY);
    r_def_spr = entry(pk, P_DEF_SPR);
    r_vis_index = entry(pk, P_VIS_INDEX);
    r_vis = entry(pk, P_VIS);
    level_blob = entry(pk, P_LEVEL);
    strings_blob = entry(pk, P_STRINGS);
    area_init(map);                                   /* the map's area colours */
}

/* The Reactor's portal closed: its walls are drawn with their closed
 * textures (the map's texture swaps). closed: nonzero once both power
 * couplings are destroyed (ui.c passes S.couplings >= 2 before every
 * picture); 0 puts the open textures back. Changes r_tex_map only when
 * the state changes. */
void pack_portal(u8 closed)
{
    u8 i;
    closed = closed != 0;
    if (closed == swapped) return;
    swapped = closed;
    for (i = 0; i < n_swaps; i++)
        r_tex_map[swaps[2 * i]] = closed ? swaps[2 * i + 1] : swaps[2 * i];
}
