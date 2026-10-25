/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * The Virtual Boy's view test, the build machine's side: the build
 * machine's renderer (test/view_host.c, with the free camera: VIEW_FX,
 * included here as it is) drawing the views of a list, with what the
 * Virtual Boy's stereo picture needs of each besides the picture: the
 * colours, each column's wall height and its tallest thing.
 *
 *   vb_view_ref <campaign.bin> spawns              each map's start: "map x z facing" lines
 *   vb_view_ref <campaign.bin> <views.bin> <out.bin>
 *
 * views.bin: tools/vb_view_test.py's list (src/viewtest.c's format: "VIEW",
 * u16 n, 10 bytes a view). out.bin, per view: fb (FB_BYTES, view.h's
 * FB_OFF layout), view_cgram (192 u16), col_h, the things' heights (128
 * u16 each), S.floor_rgb, S.ceil_rgb (3 bytes each); little-endian.
 *
 * A thing's height counts in its column when it drew a pixel there: the
 * same test as src/view_draw.c's, worked out here from the sprite
 * column's description (view_host.c's own drawing does the drawing).
 * Build: cc -O2 -DVIEW_FX -o vb_view_ref vb_view_ref.c ../../view/view.c
 * ../../core/rules.c ../../core/gen/data.c (tools/vb_view_test.py does).
 * Licence: GPL-2.0-or-later.
 */
#define main view_host_main
#define draw_spr host_draw_spr
#include "../../test/view_host.c"
#undef main
#undef draw_spr

static u16 spr_tall[VIEW_W];            /* each column's tallest thing that drew a pixel */

/* drew: 1 if the sprite column view.c describes (ds_ globals) has a pixel
   to draw: a row of the texture inside its span that is not a hole (255),
   on a screen row inside the view (view_host.c's column(), not drawing) */
static int drew(void)
{
    s16 y0 = VIEW_HALF_H - (s16)((ds_h * 28 + 32) >> 6), y;
    u16 dv = 16384 / ds_h, v;
    s16 k0 = y0 < 0 ? -y0 : 0;
    v = (dv >> 1) + k0 * dv;
    for (y = y0 + k0; y < VIEW_H && y < y0 + (s16)ds_h; y++, v += dv) {
        u8 r = (u8)(v >> 8);
        if (r < ds_top || r >= ds_top + ds_len) continue;
        if (blk[ds_data + r - ds_top] != 255) return 1;
    }
    return 0;
}

/* draw_spr: view.c's sprite column: its height noted, then view_host.c's drawing */
void draw_spr(void)
{
    if (drew() && ds_h > spr_tall[ds_x]) spr_tall[ds_x] = ds_h;
    host_draw_spr();
}

/* put16: n little-endian u16 to f */
static void put16(FILE *f, const u16 *p, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        fputc(p[i] & 255, f);
        fputc(p[i] >> 8, f);
    }
}

/* see the top of the file */
int main(int argc, char **argv)
{
    FILE *f;
    unsigned char *list;
    long n;
    int i, nv;
    if (argc < 3) {
        fprintf(stderr, "usage: vb_view_ref <campaign.bin> spawns | <views.bin> <out.bin>\n");
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
    if (!strcmp(argv[2], "spawns")) {   /* every map's start */
        int m, nm = blk[7];             /* (the number of maps: the block's u32 at 4, campaign.c's n_maps) */
        for (m = 0; m < nm; m++) {
            game_init();
            if (m) map_change((u8)m);
            printf("%d %d %d %d\n", m, S.px, S.pz, S.facing);
        }
        return 0;
    }
    f = fopen(argv[2], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    list = malloc(n);
    if (fread(list, 1, n, f) != (size_t)n) return 1;
    fclose(f);
    nv = (list[4] << 8) | list[5];
    f = fopen(argv[3], "wb");
    for (i = 0; i < nv; i++) {
        const unsigned char *v = list + 6 + 10 * i;
        game_init();                    /* (as src/viewtest.c: a fresh game, the map, the place) */
        if (v[0]) map_change(v[0]);
        S.px = v[1];
        S.pz = v[2];
        S.facing = v[3];
        memset(spr_tall, 0, sizeof spr_tall);
        if (v[4]) {
            view_camera((s16)((v[6] << 8) | v[7]), (s16)((v[8] << 8) | v[9]), v[5]);
            view_render_cam();
        } else
            view_render();
        fwrite(fb, 1, sizeof fb, f);
        put16(f, view_cgram, VIEW_SLOTS);
        put16(f, col_h, VIEW_W);
        put16(f, spr_tall, VIEW_W);
        fwrite(S.floor_rgb, 1, 3, f);
        fwrite(S.ceil_rgb, 1, 3, f);
    }
    fclose(f);
    return 0;
}
