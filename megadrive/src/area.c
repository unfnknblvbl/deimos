/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * The Mega Drive game - the view's colours: three palettes per enclosed area.
 *
 * The map is cut into areas by its doors and other lines; each area has
 * three palettes of 15 colours, and every texture and sprite one of the
 * three (chosen offline: tools/md_areas.c, the table after the campaign's
 * block, its address at $1D8 in the cartridge header). The renderer gives
 * each 8x8 tile of the view the palette of what is at its middle and draws
 * every pixel of the tile in that palette, through the remap tables made
 * here: for every texture and sprite, its colours' nearest in each of the
 * three palettes (render.s: tex_hi3 ...).
 *
 * The area is the camera cell's; in a line's cell (a doorway) it is the
 * one ahead, so stepping through a door brings the next area's colours.
 * Making the tables takes a few frames, once per area entered.
 *
 * Callers: pack.c (area_init, when a map is chosen); render.s
 * (area_select, before every picture). render.s reads area_pal,
 * area_tex_p / area_spr_p and the remap tables made here.
 *
 * The areas table (see tools/md_areas.c for the full format):
 *   "MDAR", u16 version, u16 maps, then a u32 offset per map; a map's
 *   part: u8 areas, textures, sprites, colours (n); n CRAM words (the
 *   colours the map's textures and sprites use); 1024 bytes, each cell's
 *   area ($FE a line's cell, $FF a wall); then one record per area.
 *   An area's record (rec_size bytes, even): 48 CRAM words (3 palettes of
 *   16; colour 0 unused), a palette number 0..2 per texture, then per
 *   sprite, then per palette each of the n colours' nearest slot (1..15).
 *
 * CRAM colours are 0000BBB0GGG0RRR0; cidx packs one into a 9-bit index
 * r | g << 3 | b << 6 (0..511).
 *
 * The remap tables (in render.s's RAM): for each palette p (0..2) a
 * 256-byte block per group. Textures: groups of 8 textures (768 bytes a
 * group: 3 x 256), 32 colours each, so texture t, colour k, palette p is
 * at (t >> 3) * 768 + p * 256 + (t & 7) * 32 + k. Sprites: groups of 16
 * sprites, 16 colours each, (t >> 4) * 768 + p * 256 + (t & 15) * 16 + k.
 * The _lo3 tables hold the slot (1..15) for the low nibble (the right
 * pixel of a byte), the _hi3 tables the same shifted left 4 (the left
 * pixel), so the renderer can OR two pixels into a byte.
 *
 * Licence: GPL-2.0-or-later.
 */
#include "md.h"
#include "render_data.h"

/* render.s: the remap tables (layout above), and cp, a scratch buffer free
   between pictures (used here for three 512-byte colour maps) */
extern u8 tex_hi3[], tex_lo3[], spr_hi3[], spr_lo3[], cp[];

u16 area_pal[48];                       /* the area's palettes (render.s uploads them with the picture) */
const u8 *area_tex_p, *area_spr_p;      /* each texture's / sprite's palette, 0..2 (in the table) */

static const u8 *amap, *cells;          /* this map's part of the table, its cells' areas */
static const u16 *acols;                /* the map's colours (the areas' nearest slots are for these) */
/* the map's counts: areas, textures, sprites, colours; the area in use
   (0xFF: none yet, the tables not made) */
static u8 n_areas, a_tex, a_spr, n_cols, cur_area = 0xFF;
static u16 rec_size;                    /* bytes per area record */
static const u8 zeros[R_MAX_SPR];       /* "palette 0 for everything" (no table) */
static const u8 *near_of;               /* the area's: per palette, each colour's slot */

/* a big-endian u32 at any address */
static u32 rd32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* map `map` was chosen (pack_init): find its part of the areas table and
   read its counts. If the table is missing or bad, amap stays 0 and the
   view gets a plain grey palette (use_area). The next area_select makes
   the tables (cur_area = 0xFF forces it). */
void area_init(u8 map)
{
    const u8 *t;
    /* the table's address: the long at $1D8 in the cartridge header
       (read from ROM each time, because the editor may move the table) */
    __asm__ volatile("move.l 0x1D8, %0" : "=a"(t));   /* the header's (the editor moves the table) */
    amap = 0;
    cur_area = 0xFF;
    /* sanity: inside the ROM, past the header, tagged "MDAR", and the map
       number below its map count (bytes 6-7) */
    if ((u32)t < 0x200 || (u32)t >= 0x400000 || t[0] != 'M' || t[1] != 'D' || t[2] != 'A' || t[3] != 'R') return;
    if (map >= ((t[6] << 8) | t[7])) return;
    amap = t + rd32(t + 8 + 4 * map);
    n_areas = amap[0];
    a_tex = amap[1];
    a_spr = amap[2];
    n_cols = amap[3];
    acols = (const u16 *)(amap + 4);
    cells = amap + 4 + 2 * n_cols;
    rec_size = 96 + a_tex + a_spr + 3 * n_cols;      /* 48 colours, labels, nearest slots */
    rec_size += rec_size & 1;                         /* padded to even */
}

/* CRAM colour 0000BBB0GGG0RRR0 -> 9-bit index r | g << 3 | b << 6 */
static u16 cidx(u16 c)
{
    return ((c >> 1) & 7) | ((c >> 2) & 0x38) | ((c >> 3) & 0x1C0);
}

/* the remap tables of every texture and sprite colour in use, for the
   three palettes, from the table's nearest slot of each of the map's
   colours (through a 512-entry map per palette: cp, free between pictures) */
static void make_remaps(void)
{
    u8 *memo = cp;
    u16 t, k;
    u8 p;
    /* memo[p * 512 + colour index] = that colour's slot in palette p (only
       the map's colours are filled in: they are the only ones looked up) */
    for (p = 0; p < 3; p++)
        for (k = 0; k < n_cols; k++) memo[p * 512 + cidx(acols[k])] = near_of[p * n_cols + k];
    /* every texture: its 32 colours (r_tex_pal), those it uses (r_tex_hist
       nonzero) remapped for all three palettes */
    for (t = 0; t < r_n_tex; t++) {
        const u16 *pal = r_tex_pal + t * 32, *hist = r_tex_hist + t * 32;
        u8 *lo = tex_lo3 + (t >> 3) * 768 + (t & 7) * 32, *hi = tex_hi3 + (lo - tex_lo3);
        for (k = 0; k < 32; k++)
            if (hist[k]) {
                const u8 *m = memo + cidx(pal[k]);
                u8 s0 = m[0], s1 = m[512], s2 = m[1024];    /* its slot in palettes 0, 1, 2 */
                lo[k] = s0;
                lo[k + 256] = s1;
                lo[k + 512] = s2;
                hi[k] = s0 << 4;
                hi[k + 256] = s1 << 4;
                hi[k + 512] = s2 << 4;
            }
    }
    /* every sprite the same way: 16 colours each */
    for (t = 0; t < r_n_spr; t++) {
        const u16 *pal = r_spr_pal + t * 16, *hist = r_spr_hist + t * 16;
        u8 *lo = spr_lo3 + (t >> 4) * 768 + (t & 15) * 16, *hi = spr_hi3 + (lo - spr_lo3);
        for (k = 0; k < 16; k++)
            if (hist[k]) {
                const u8 *m = memo + cidx(pal[k]);
                u8 s0 = m[0], s1 = m[512], s2 = m[1024];
                lo[k] = s0;
                lo[k + 256] = s1;
                lo[k + 512] = s2;
                hi[k] = s0 << 4;
                hi[k + 256] = s1 << 4;
                hi[k + 512] = s2 << 4;
            }
    }
}

/* switch to area a: copy its palettes into area_pal, point area_tex_p /
   area_spr_p at its labels and remake the remap tables (a few frames) */
static void use_area(u8 a)
{
    u8 i;
    cur_area = a;
    if (!amap) {                        /* (no table: one grey ramp; the remaps as they are) */
        /* slot s of each palette: grey level s / 2 (0..7) in r, g and b */
        for (i = 0; i < 48; i++) area_pal[i] = (u16)((i & 15) >> 1) * 0x222;
        area_tex_p = area_spr_p = zeros;
    } else {
        const u16 *rec = (const u16 *)(cells + 1024 + (u32)a * rec_size);   /* area a's record */
        for (i = 0; i < 48; i++) area_pal[i] = rec[i];
        area_tex_p = (const u8 *)rec + 96;
        area_spr_p = (const u8 *)rec + 96 + a_tex;
        near_of = (const u8 *)rec + 96 + a_tex + a_spr;
        make_remaps();                  /* the remap tables for these palettes */
    }
}

/* the area of cell (x, z) seen facing f; 0xFF: none (keep the last) */
static u8 area_of(u8 x, u8 z, u8 f)
{
    /* one cell ahead for facings N, E, S, W (north is -z) */
    static const s8 DX[4] = {0, 1, 0, -1}, DZ[4] = {-1, 0, 1, 0};
    u8 a;
    if (!amap || x > 31 || z > 31) return 0xFF;
    a = cells[z * 32 + x];
    if (a == 0xFE) {                    /* a line's cell: the area ahead */
        s8 nx = (s8)x + DX[f & 3], nz = (s8)z + DZ[f & 3];
        a = nx >= 0 && nx < 32 && nz >= 0 && nz < 32 ? cells[nz * 32 + nx] : 0xFF;
        /* the cell ahead is no area (a wall) and no area is in use yet
           (the map's first picture): take the first neighbour that is */
        if (a >= n_areas && cur_area == 0xFF) {     /* nothing yet: any beside it */
            u8 k;
            for (k = 0; k < 4 && a >= n_areas; k++) {
                nx = (s8)x + DX[k];
                nz = (s8)z + DZ[k];
                if (nx >= 0 && nx < 32 && nz >= 0 && nz < 32) a = cells[nz * 32 + nx];
            }
        }
    }
    return a < n_areas ? a : 0xFF;
}

/* render.s, before each picture: the camera is in cell (x, z) facing f
   (0..3). Picks the area (or keeps the last; 0 if there was none) and
   switches to it if it changed. */
void area_select(u8 x, u8 z, u8 f)
{
    u8 a = area_of(x, z, f);
    if (a == 0xFF) a = cur_area != 0xFF ? cur_area : 0;
    if (a != cur_area) use_area(a);
}
