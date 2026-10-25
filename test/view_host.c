/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * The SNES's 3D view (view/view.c) on the build machine: the reference
 * the SNES's pictures are compared with, and a way to look at them.
 *
 *   view_host <campaign.bin> <map> <out.ppm|out.bin> [x z facing] [couplings]
 *
 * plays nothing: a fresh game, map_change to <map> (unless 0), the player put
 * at (x, z, facing) if given, then one picture. .ppm: the picture in its
 * colours (2x, as the SNES shows it); .bin: the frame buffer as it is.
 *
 * It links view/view.c and core/rules.c with this file's platform hooks:
 * the view's data access (vdata.h: the campaign block, read into memory),
 * the rules' map hooks, and draw_walls / draw_spr written plainly in C
 * (the consoles' versions must produce the same frame buffer: fb, in
 * view.h's FB_OFF layout, a byte a pixel, 0 the backdrop, else a colour
 * slot). With VIEW_FX (view_host_fx) a free camera can be given too.
 * Licence: GPL-2.0-or-later.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/rules.h"
#include "../view/view.h"
#include "../view/vdata.h"

static unsigned char *blk;              /* the campaign block, whole */
static long blk_size;
u8 fb[FB_BYTES];                        /* the frame buffer view.c draws into */

/* big-endian reads at block offset a */
static u32 be32(u32 a) { return ((u32)blk[a] << 24) | ((u32)blk[a + 1] << 16) | ((u32)blk[a + 2] << 8) | blk[a + 3]; }
static u16 be16(u32 a) { return (u16)((blk[a] << 8) | blk[a + 1]); }


/* vdata.h's access: block offsets straight into memory */
u8 vd_u8(u32 a) { return blk[a]; }
u16 vd_u16(u32 a) { return be16(a); }
u32 vd_u32(u32 a) { return be32(a); }
void vd_words(u32 a, s16 *dst, u8 n)
{
    u8 i;
    for (i = 0; i < n; i++) dst[i] = (s16)be16(a + 2 * i);
}

/* map's pack entry e (campaign.h's P_* order): its offset, its size into
   *size when asked */
static u32 entry(u8 map, u8 e, u32 *size)
{
    u32 pack = be32(32 + 32 * map);
    if (size) *size = be32(pack + 32 + 8 * e + 4);
    return be32(pack + 32 + 8 * e);
}

/* map's pack into *p (as gbc/src/plat.c's: the entries by number) */
void vd_pack(u8 map, struct vpack *p)
{
    u32 pk = be32(32 + 32 * map);
    u8 i;
    p->n_walls = be16(pk + 8);
    p->n_tex = be16(pk + 10);
    p->n_spr = be16(pk + 12);
    p->n_scenery = be16(pk + 14);
    p->floor = be16(pk + 16);
    p->ceiling = be16(pk + 18);
    p->n_swaps = blk[pk + 20];
    for (i = 0; i < 2 * p->n_swaps && i < 10; i++) p->swaps[i] = blk[pk + 21 + i];
    p->walls = entry(map, 0, 0);
    p->tex_off = entry(map, 1, 0);
    p->tex_pal = entry(map, 2, 0);
    p->spr_cols = entry(map, 4, 0);
    p->spr_off = entry(map, 5, 0);
    p->spr_coff = entry(map, 6, 0);
    p->pvs_index = entry(map, 7, 0);
    p->pvs = entry(map, 8, 0);
    p->spr_pal = entry(map, 9, 0);
    p->scenery = entry(map, 11, 0);
    p->def_spr = entry(map, 12, 0);
    p->vis_index = entry(map, 13, 0);
    p->vis = entry(map, 14, 0);
    p->tex_bank = be32(8);
    p->spr_bank = be32(12);
}

/* ---- the rules' hooks ---- */
static u8 cur_map;
/* the level blob (entry 15) and the map's strings (entry 16) */
const u8 *map_level(u8 map)
{
    cur_map = map;
    return blk + entry(map, 15, 0);
}
const char *level_string(u8 id)
{
    u32 s = entry(cur_map, 16, 0);
    return (const char *)blk + s + be16(s + 2 * id);
}
/* the game's text id: the block's texts table (its offset at byte 24:
   a big-endian u16 offset per text, then the texts) */
const char *gtext(u8 id)
{
    u32 t = be32(24);
    return (const char *)blk + t + be16(t + 2 * id);
}
/* messages and sounds: ignored */
void say(const char *s) { (void)s; }
void say2(const char *a, const char *b) { (void)a; (void)b; }
void say3(const char *a, const char *b, const char *c) { (void)a; (void)b; (void)c; }
void say_num(const char *a, u16 n, const char *b) { (void)a; (void)n; (void)b; }
void sfx(u8 id) { (void)id; }

/* ---- the drawing (the SNES's is snes/src/draw.s) ---- */
/* one column at screen x of height h: texture rows (8.8, dv = 64 * 256 /
   h, each pixel sampled at its middle) through the remap rm; the column's
   top at VIEW_HALF_H - (28 h + 32) / 64, rows past the view's edges
   skipped. A
   sprite has data rows top .. top + len - 1 only and 255 holes */
static void column(u8 x, u16 h, u32 data, u8 top, u8 len, const u8 *rm, u8 sprite)
{
    s16 y0 = VIEW_HALF_H - (s16)((h * 28 + 32) >> 6), y;
    u16 dv = 16384 / h, v;
    s16 k0 = y0 < 0 ? -y0 : 0;
    v = (dv >> 1) + k0 * dv;
    for (y = y0 + k0; y < VIEW_H && y < y0 + (s16)h; y++, v += dv) {
        u8 r = (u8)(v >> 8);
        if (sprite) {
            u8 c;
            if (r < top || r >= top + len) continue;
            c = blk[data + r - top];
            if (c != 255) fb[FB_OFF(x, y)] = rm[c];
        } else
            fb[FB_OFF(x, y)] = rm[blk[data + r]];
    }
}

/* view.h's hooks: every wall column (the buffer cleared first), one sprite column */
void draw_walls(void)
{
    u8 x;
    memset(fb, 0, sizeof fb);
    for (x = 0; x < VIEW_W; x++)
        if (col_h[x]) column(x, col_h[x], col_tex[x], 0, 64, tex_rm[col_t[x]], 0);
}

void draw_spr(void)
{
    column(ds_x, ds_h, ds_data, ds_top, ds_len, spr_rm[ds_s], 1);
}

/* the backdrop: the level's colour, 0.3 + 0.9 t (t: 0 at the horizon, 1
   at the edge of the view), per screen line (2 VIEW_H of them: 176 at the
   default size; snes/src/video.c's shade) */
static void backdrop(u16 line, u8 *rgb)
{
    u16 c = line < VIEW_H ? view_ceiling : view_floor;
    u16 t = line < VIEW_H ?             /* (t in 1/256: the line's distance from the horizon) */ (u16)(((VIEW_H - 1 - line) * 2 + 1) * 256 / (2 * VIEW_H)) : (u16)(((line - VIEW_H) * 2 + 1) * 256 / (2 * VIEW_H));
    u16 f = 77 + ((t * 230) >> 8), k;
    for (k = 0; k < 3; k++) {
        u16 v = (u16)(((c >> (5 * k)) & 31) * f) >> 8;
        rgb[k] = (u8)((v > 31 ? 31 : v) * 255 / 31);
    }
}

/* see the usage at the top; prints the position and the map's colour count */
int main(int argc, char **argv)
{
    FILE *f;
    int map;
    if (argc < 4) {
        fprintf(stderr, "usage: view_host <campaign.bin> <map> <out> [x z facing] [couplings]\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    blk_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    blk = malloc(blk_size);
    if (fread(blk, 1, blk_size, f) != (size_t)blk_size) return 1;
    fclose(f);
    map = atoi(argv[2]);
    game_init();                        /* (the core: a fresh game, map 0) */
    if (map) map_change((u8)map);
    if (argc >= 7) {
        S.px = (u8)atoi(argv[4]);
        S.pz = (u8)atoi(argv[5]);
        S.facing = (u8)atoi(argv[6]);
    }
    if (argc >= 8) S.couplings = (u8)atoi(argv[7]);    /* (the optional couplings argument: S.couplings) */
#ifdef VIEW_FX
    if (argc >= 11) {                   /* the free camera: cx cz angle */
        view_camera((s16)atoi(argv[8]), (s16)atoi(argv[9]), (u8)atoi(argv[10]));
        view_render_cam();
    } else
#endif
    view_render();                      /* (view.c: calls draw_walls, draw_spr) */
    if (getenv("VIEW_DEBUG")) {         /* (each column's height, texture, slot) */
        int x;
        for (x = 0; x < VIEW_W; x++) printf("%d ", col_h[x]);
        printf("\n");
        for (x = 0; x < VIEW_W; x++) printf("%u ", (unsigned)col_tex[x]);
        printf("\n");
        for (x = 0; x < VIEW_W; x++) printf("%d ", col_t[x]);
        printf("\n");
#ifdef VIEW_LOG
        {
            extern u16 vlog[64][4];
            extern u8 vlog_n;
            for (x = 0; x < vlog_n; x++) printf("(%d %d %d %d) ", vlog[x][0], vlog[x][1], vlog[x][2], vlog[x][3]);
            printf("\n");
        }
#endif
    }
    f = fopen(argv[3], "wb");
    if (strstr(argv[3], ".ppm")) {
        u16 y, x;
        fprintf(f, "P6\n%d %d\n255\n", 2 * VIEW_W, 2 * VIEW_H);
        for (y = 0; y < 2 * VIEW_H; y++)
            for (x = 0; x < 2 * VIEW_W; x++) {
                u8 p = fb[FB_OFF(x / 2, y / 2)], rgb[3];    /* (each pixel twice wide, twice tall) */
                if (!p) backdrop(y, rgb);
                else {
                    u16 c = view_cgram[p - VIEW_SLOT0];
                    rgb[0] = (u8)((c & 31) * 255 / 31);
                    rgb[1] = (u8)(((c >> 5) & 31) * 255 / 31);
                    rgb[2] = (u8)(((c >> 10) & 31) * 255 / 31);
                }
                fwrite(rgb, 1, 3, f);
            }
    } else
        fwrite(fb, 1, sizeof fb, f);
    fclose(f);
    printf("map %d at (%d,%d,%d): %d colours\n", map, S.px, S.pz, S.facing, view_n_colours);
    return 0;
}
