/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the SNES with the SuperFX: the main CPU's side of the view
 * (view/view.c's VIEW_FX: fx_map_load, fx_draw): what the GSU's program
 * (snes/gsu/view.s) needs, put in its RAM, then the GSU run.
 * Licence: GPL-2.0-or-later. */
/* Built only for the SuperFX build (build_fx.sh, -D VIEW_FX). The view
 * (view/view.c) works out the camera, the open doors, the texture swaps and
 * the things to draw in C, then calls fx_draw; on a new map it calls
 * fx_map_load. Both copy what the GSU program needs into the GSU's RAM
 * (bank $70) with gsu_cpu.s's copy routines and start the program at one
 * of its two entry points (0: draw the picture; 4: sort a new map's walls
 * into blocks). The GSU draws the picture into its RAM at $0200; video.s's
 * view_upload (gsu_cpu.s's fx_out) then sends it to VRAM.
 *
 * Addresses into the ROM are given to the GSU as two words: the low 16
 * bits and the bank (the GSU's ROM banks are 64 KB, numbered from 0). */
#include "../../view/view.h"
#include "../../view/vdata.h"

/* gsu_cpu.s: the copy parameters and routines (the GSU must be stopped) */
extern u16 fxp_dst, fxp_n;              /* destination in the GSU's RAM; bytes */
extern const void *fxp_src;             /* source: work RAM (fx_copy) or ROM address (fx_romcopy) */
void fx_copy(void);                     /* work RAM -> GSU RAM */
void fx_romcopy(void);                  /* ROM (fxp_bank:fxp_src) -> GSU RAM, a word at a time */
extern u16 fxp_bank;                    /* fx_romcopy's source bank */
void gsu_run(u16 entry);                /* run the GSU from entry until its STOP */
extern u8 block_bank;                   /* the campaign block's first ROM bank (vdata.s) */
extern u32 tex_addr[V_MAX_TEX], spr_addr[V_MAX_SPR];   /* view.c: block offsets of the map's textures, sprites */
extern u8 tex_map[V_MAX_TEX];           /* view.c: the texture swaps in force */

/* snes/gsu/view.s's RAM (offsets in bank $70; the GSU program's names
   without the G_). $00-$0B is the camera (VX, VZ, FWX, FWZ, RTX, RTZ),
   $18 STOPAT. */
#define G_NWALLS 0x0C                   /* u16 the map's walls */
#define G_NTH 0x0E                      /* u16 things in G_TH */
#define G_SCOLS 0x10                    /* sprite column data, then (+4) column offsets: lo, bank each */
#define G_OPEN 0x3200                   /* [64] the doors' flags (bit 0: open) */
#define G_TEXMAP 0x3240                 /* [48] the texture swaps */
#define G_TEXADDR 0x3280                /* [48] textures: lo, bank (words) */
#define G_SPRADDR 0x3340                /* [64] sprites: lo, bank */
#define G_TEXRM 0x3600                  /* [48][32] texture colour -> slot */
#define G_SPRRM 0x3C00                  /* [64][16] sprite colour -> slot */
#define G_TH 0x4200                     /* the things, 8 bytes each (struct fx_th) */
#define G_WALLS 0x8300                  /* the sorted walls (written by the GSU; not used here) */

/* copy n bytes from work RAM at src to the GSU's RAM at dst */
static void put(u16 dst, const void *src, u16 n)
{
    fxp_dst = dst;
    fxp_src = src;
    fxp_n = n;
    fx_copy();                          /* MVN into bank $70 (gsu_cpu.s) */
}

/* a block offset -> lo, bank (the GSU's ROM address) */
static u16 ga[2];                       /* rom_at's result: low 16 bits, bank */
static void rom_at(u32 a)
{
    ga[0] = (u16)a;
    ga[1] = (u16)(a >> 16) + block_bank;
}

/* a new map (called by view/view.c after its own map load): give the GSU
   the map's walls, sorted into blocks by the GSU itself, and the
   addresses and colour remaps of its textures and sprites */
void fx_map_load(const struct vpack *p)
{
    u16 k, n = p->n_walls < FX_MAX_WALLS ? p->n_walls : FX_MAX_WALLS;
    /* the walls as they are (the GSU sorts them into blocks: entry 4) */
    fxp_src = (const void *)(u16)p->walls;
    fxp_bank = block_bank + (u16)(p->walls >> 16);
    fxp_dst = 0x0200;                   /* (RAW: the picture's area, free until the next draw) */
    fxp_n = 16 * n;                     /* 16 bytes a wall */
    fx_romcopy();
    put(G_NWALLS, &n, 2);
    gsu_run(4);                         /* MAPLOAD: sort them into WALLS and BLK */
    rom_at(p->spr_cols);
    put(G_SCOLS, ga, 4);
    rom_at(p->spr_coff);
    put(G_SCOLS + 4, ga, 4);
    for (k = 0; k < V_MAX_TEX; k++) {   /* every texture slot's ROM address */
        rom_at(tex_addr[k]);
        put(G_TEXADDR + 4 * k, ga, 4);
    }
    for (k = 0; k < V_MAX_SPR; k++) {   /* every sprite slot's */
        rom_at(spr_addr[k]);
        put(G_SPRADDR + 4 * k, ga, 4);
    }
    put(G_TEXRM, tex_rm, sizeof tex_rm);
    put(G_SPRRM, spr_rm, sizeof spr_rm);
}

u16 ticks(void);                        /* hw.s: scanlines since start */
u16 fx_prof[4];                         /* profiling: ticks at the start, before the run, after it */
u16 fx_stopat = 255;                    /* STOPAT: the GSU stops after that pass (testing); 255 none */

/* draw a picture (called by view/view.c's view_render_cam with VIEW_FX): the
   frame's camera, things, doors and swaps into the GSU's RAM, then the
   GSU's whole pipeline (entry 0). The CPU waits in work RAM meanwhile. */
void fx_draw(void)
{
    u16 n = fx_nth;
    fx_prof[0] = ticks();
    put(0, fx_cam, 12);                 /* VX VZ FWX FWZ RTX RTZ: 6 words */
    put(G_NTH, &n, 2);
    put(0x18, &fx_stopat, 2);           /* STOPAT */
    put(G_OPEN, fx_open, 64);
    put(G_TEXMAP, tex_map, V_MAX_TEX);
    put(G_TH, fx_th, 8 * n);
    fx_prof[1] = ticks();
    gsu_run(0);                         /* the picture, into the GSU's RAM at $0200 */
    fx_prof[2] = ticks();
}
