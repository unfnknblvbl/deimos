/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the 3DS: the 3D view on the top screen, drawn by the GPU
 * (citro3d) in stereo: the map's walls as textured quads (the open doors
 * left out), the things as billboards facing the camera, the decals flat
 * on their walls, the floor and the ceiling shaded into the distance by
 * their vertex colours (the GPU's fog is set up in r3d_init, but no batch
 * turns it on). The data is the campaign's block (the Mega Drive game's format, as
 * the SNES port reads it); the camera glides to where the rules put the
 * player (a step or a quarter turn in R3D_GLIDE frames).
 *
 * World: x east, z south (64 a cell), y up (the walls 0..64, the eye at
 * 36, 16 units behind the cell's centre).
 *
 * Where it fits: src/plat.c calls r3d_init once, then each frame
 * r3d_frame_start, r3d_draw (the top screen, one target per eye),
 * r3d_flat for the UI's pictures over it and on the bottom screen, and
 * r3d_automap (the bottom screen); r3d_target when the game asks for a
 * new view; r3d_colours for the floor and ceiling. The game state S
 * (core/rules.h) gives the player, the doors and the entities.
 *
 * How a frame is drawn: build() turns the map into triangles in one vertex
 * buffer (vbuf, in the GPU-visible linear memory), grouped into batches of
 * one texture each; r3d_draw then issues one draw call per batch per eye.
 * The UI's quads and the automap's lines are added to the same buffer
 * after the view's, and drawn at once. The vertex shader is
 * shaders/view.v.pica.
 * Licence: GPL-2.0-or-later.
 */
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "r3d.h"
#include "../../core/rules.h"

#define MAX_TEX 64                      /* textures per map (a power of two: used as a mask) */
#define MAX_SPR 96                      /* sprites per map */
#define MAX_VERT 40000                  /* the vertex buffer's size, for a whole frame */
#define FLOOR_R 20                      /* cells of floor around the camera */
#define EYE 36.0f                       /* the eye's height (world units) */
#define BACK 16.0f                      /* the eye's distance behind the camera point */
#define STEREO_SCREEN 32.0f             /* stereo: the distance of the screen's plane (r3d_draw) */
#define R3D_GLIDE 8                     /* frames a step or a turn takes */

extern const u8 *rom_image;            /* the cartridge image (plat.c) */
extern u8 block_bank;                   /* the campaign block's first SNES bank (campaign.c) */

static const u8 *blk;                   /* the campaign's block */
/* be32 / be16 / bes: a big-endian u32, u16 or s16 at offset a in the block */
static u32 be32(u32 a) { return ((u32)blk[a] << 24) | ((u32)blk[a + 1] << 16) | ((u32)blk[a + 2] << 8) | blk[a + 3]; }
static u16 be16(u32 a) { return (u16)((blk[a] << 8) | blk[a + 1]); }
static s16 bes(u32 a) { return (s16)be16(a); }

/* ---- the map */
#define MAX_WALLS 4096
static u8 wall_seen[MAX_WALLS];      /* the automap's */
static void look(void);
/* the loaded map: its counts and where its tables are in the block (the
   layout is the same as in 32x/sh2/r32x.c's map_load) */
static struct {
    u8 loaded, map;                     /* a map is loaded; which */
    u16 n_walls, n_tex, n_spr, n_scen;
    u32 walls, tex_off, tex_pal, spr_cols, spr_off, spr_coff, spr_pal, scenery, def_spr, tex_bank, spr_bank;
    u8 n_swaps, swaps[10];              /* texture swaps: up to 5 (from, to) pairs */
} M;
static C3D_Tex tex[MAX_TEX], spr[MAX_SPR];     /* the map's textures and sprites as GPU textures */
static u8 tex_ok[MAX_TEX], spr_ok[MAX_SPR];    /* 1 where that texture was made */

/* a Mega Drive colour level (0..7) -> 8 bits */
static const u8 LEV[8] = {0, 52, 87, 116, 144, 172, 206, 255};

/* md_rgb: a Mega Drive colour (0000 BBB0 GGG0 RRR0) -> 8-bit r, g, b */
static void md_rgb(u16 c, u8 *r, u8 *g, u8 *b)
{
    *r = LEV[(c >> 1) & 7];
    *g = LEV[(c >> 5) & 7];
    *b = LEV[(c >> 9) & 7];
}

/* the texel (x, y) of a w-wide tiled texture (y = 0 at the bottom)
 * (Note: as this file and plat.c use it, stored row y is sampled at
 * v = 1 - y / height, so row 0 appears at v = 1, the top of each quad: a
 * picture is stored top row first.)
 * The GPU's textures are stored in 8 x 8 tiles, the tiles row by row, and
 * the 64 texels in a tile in Morton (Z) order: m interleaves the bits of x
 * and y (x0 y0 x1 y1 x2 y2, from the lowest). Returns the texel's index. */
static u32 tiled(u32 x, u32 y, u32 w)
{
    u32 m = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
    return ((y >> 3) * (w >> 3) + (x >> 3)) * 64 + m;
}

/* free_textures: delete the textures of the map loaded before */
static void free_textures(void)
{
    int i;
    for (i = 0; i < MAX_TEX; i++)
        if (tex_ok[i]) {
            C3D_TexDelete(&tex[i]);
            tex_ok[i] = 0;
        }
    for (i = 0; i < MAX_SPR; i++)
        if (spr_ok[i]) {
            C3D_TexDelete(&spr[i]);
            spr_ok[i] = 0;
        }
}

/* entry: table e of the map pack at pk (its directory at pk + 32, 8 bytes an entry) */
static u32 entry(u32 pk, int e) { return be32(pk + 32 + 8 * e); }

/* map_load: load map number map: its tables (see 32x/sh2/r32x.c's map_load
 * for the layout), its textures and sprites made into GPU textures, and
 * the automap's memory cleared. Called by r3d_target when the map changes. */
static void map_load(u8 map)
{
    u32 pk;
    int i, x, y;
    blk = rom_image + (((u32)block_bank - 0xC0) << 16);
    free_textures();
    pk = be32(32 + 32 * (u32)map);
    M.n_walls = be16(pk + 8);
    M.n_tex = be16(pk + 10);
    M.n_spr = be16(pk + 12);
    M.n_scen = be16(pk + 14);
    M.n_swaps = blk[pk + 20];
    for (i = 0; i < 2 * M.n_swaps && i < 10; i++) M.swaps[i] = blk[pk + 21 + i];
    M.walls = entry(pk, 0);
    M.tex_off = entry(pk, 1);
    M.tex_pal = entry(pk, 2);
    M.spr_cols = entry(pk, 4);
    M.spr_off = entry(pk, 5);
    M.spr_coff = entry(pk, 6);
    M.spr_pal = entry(pk, 9);
    M.scenery = entry(pk, 11);
    M.def_spr = entry(pk, 12);
    M.tex_bank = be32(8);
    M.spr_bank = be32(12);
    /* the textures: 64 columns of 64 rows, 32 colours (RGB565) */
    for (i = 0; i < M.n_tex && i < MAX_TEX; i++) {
        u16 pal[32];
        u32 at = M.tex_bank + be32(M.tex_off + 4 * i);
        u16 *d;
        for (x = 0; x < 32; x++) {
            u8 r, g, b;
            md_rgb(be16(M.tex_pal + 64 * i + 2 * x), &r, &g, &b);
            pal[x] = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
        if (!C3D_TexInit(&tex[i], 64, 64, GPU_RGB565)) continue;     /* (out of memory: the walls left out) */
        tex_ok[i] = 1;
        d = (u16 *)tex[i].data;
        for (x = 0; x < 64; x++)        /* the source: column x's 64 bytes, row y */
            for (y = 0; y < 64; y++) d[tiled(x, y, 64)] = pal[blk[at + x * 64 + y] & 31];
        C3D_TexSetFilter(&tex[i], GPU_NEAREST, GPU_NEAREST);   /* sharp texels, as the original */
        C3D_TexSetWrap(&tex[i], GPU_REPEAT, GPU_REPEAT);       /* (u runs past 1 on long walls) */
        C3D_TexFlush(&tex[i]);          /* write the CPU cache out for the GPU */
    }
    /* the sprites: a 64 x 64 canvas, columns of rows top..top+len-1 (255 a hole) */
    for (i = 0; i < M.n_spr && i < MAX_SPR; i++) {
        u16 pal[16];
        u32 base = M.spr_bank + be32(M.spr_off + 4 * i);
        u16 *d;
        for (x = 0; x < 16; x++) {
            u8 r, g, b;
            md_rgb(be16(M.spr_pal + 32 * i + 2 * x), &r, &g, &b);
            pal[x] = (u16)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1);  /* RGBA5551, opaque */
        }
        if (!C3D_TexInit(&spr[i], 64, 64, GPU_RGBA5551)) continue;
        spr_ok[i] = 1;
        d = (u16 *)spr[i].data;
        memset(d, 0, 64 * 64 * 2);      /* all see-through (alpha 0) to start */
        for (x = 0; x < 64; x++) {
            u8 top = blk[M.spr_cols + i * 128 + x * 2], len = blk[M.spr_cols + i * 128 + x * 2 + 1];
            u32 data = base + be16(M.spr_coff + i * 128 + x * 2);
            for (y = 0; y < len && top + y < 64; y++) {
                u8 c = blk[data + y];
                if (c != 255) d[tiled(x, top + y, 64)] = pal[c & 15];
            }
        }
        C3D_TexSetFilter(&spr[i], GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&spr[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        C3D_TexFlush(&spr[i]);
    }
    M.loaded = 1;
    M.map = map;
    memset(wall_seen, 0, sizeof wall_seen);
}

/* ---- the camera: where it is, where it goes */
static float cam_x, cam_z, cam_a;                /* a: radians, 0 = north (-z), clockwise */
static float from_x, from_z, from_a, to_x, to_z, to_a;     /* a glide's start and end */
static int glide = 0;                   /* frames of the glide left */
static u8 have_cam;                     /* the camera has been placed once */

/* r3d_target: aim the camera at the player (S.px, S.pz, S.facing): load
 * the map if it changed, update the automap, then jump there (a new
 * camera, or more than one cell away) or glide there in R3D_GLIDE frames,
 * turning the shorter way. Called by plat.c when the game asks for a view. */
void r3d_target(void)
{
    float tx = (float)S.px * 64 + 32, tz = (float)S.pz * 64 + 32, ta = (float)(S.facing & 3) * (float)M_PI / 2;
    if (!M.loaded || M.map != S.map) map_load(S.map);
    look();                             /* what the player sees now goes on the automap */
    /* (S.map != M.map cannot be true here: map_load has just set M.map) */
    if (!have_cam || fabsf(tx - cam_x) + fabsf(tz - cam_z) > 64.5f || S.map != M.map) {
        cam_x = to_x = tx;
        cam_z = to_z = tz;
        cam_a = to_a = ta;
        glide = 0;
        have_cam = 1;
        return;
    }
    /* the shortest way round */
    while (ta - cam_a > (float)M_PI) ta -= 2 * (float)M_PI;
    while (ta - cam_a < -(float)M_PI) ta += 2 * (float)M_PI;
    from_x = cam_x;
    from_z = cam_z;
    from_a = cam_a;
    to_x = tx;
    to_z = tz;
    to_a = ta;
    glide = R3D_GLIDE;
}

/* r3d_moving: 1 while the camera is gliding (plat.c keeps redrawing) */
int r3d_moving(void) { return glide > 0; }

/* ---- the automap: what the player has had in sight. 360 rays from the
   player (to 16 cells) each mark the first wall they meet (two-sided ones,
   the bars and grates, are seen through); open doors are not walls. */
static void look(void)
{
    int r;
    u32 k, n = M.n_walls < MAX_WALLS ? M.n_walls : MAX_WALLS;
    float px = (float)S.px * 64 + 32, pz = (float)S.pz * 64 + 32;
    for (r = 0; r < 360; r++) {
        float a = (float)r * (2 * (float)M_PI / 360), dx = sinf(a), dz = -cosf(a), best = 1024;
        int hit = -1;
        for (k = 0; k < n; k++) {
            u32 w = M.walls + 16 * k;
            u8 lock = blk[w + 11];
            float ax = bes(w) - px, az = bes(w + 2) - pz, ex = bes(w + 4) - bes(w), ez = bes(w + 6) - bes(w + 2);
            float den = dx * ez - dz * ex, t, u;     /* (the ray and the wall: their cross product) */
            if (lock < 64 && (S.l_flags[lock] & L_OPEN)) continue;
            if (den > -1e-4f && den < 1e-4f) continue;  /* parallel */
            t = (ax * ez - az * ex) / den;           /* along the ray */
            u = (ax * dz - az * dx) / den;           /* along the wall */
            if (t <= 0 || t >= best || u < -0.01f || u > 1.01f) continue;   /* behind, farther, or missed */
            if (blk[w + 12] & 1) {      /* two-sided: seen, and the ray goes on */
                wall_seen[k] = 1;
                continue;
            }
            best = t;
            hit = (int)k;
        }
        if (hit >= 0) wall_seen[hit] = 1;
    }
}

/* camera_step: one frame of a glide (straight-line, no easing) */
static void camera_step(void)
{
    if (glide > 0) {
        float k;
        glide--;
        k = 1.0f - (float)glide / R3D_GLIDE;
        cam_x = from_x + (to_x - from_x) * k;
        cam_z = from_z + (to_z - from_z) * k;
        cam_a = from_a + (to_a - from_a) * k;
    }
}

/* ---- colours */
static float floor_c[3] = {0.3f, 0.3f, 0.3f}, ceil_c[3] = {0.5f, 0.5f, 0.5f};    /* r, g, b in 0..1 */

/* r3d_colours: the floor's and ceiling's colours from the game (BGR555:
   red in bits 0..4, green 5..9, blue 10..14) */
void r3d_colours(u16 floor, u16 ceil)
{
    int k;
    for (k = 0; k < 3; k++) {
        floor_c[k] = (float)((floor >> (5 * k)) & 31) / 31.0f;
        ceil_c[k] = (float)((ceil >> (5 * k)) & 31) / 31.0f;
    }
}

/* ---- drawing */
/* a vertex as the shader takes it: v0 position, v1 texture coordinate, v2 colour */
typedef struct {
    float x, y, z, u, v, r, g, b, a;
} vert;
static vert *vbuf;                      /* the frame's vertices (linear memory) */
static int vn;                          /* how many are used */
static DVLB_s *dvlb;                    /* the parsed shader binary */
static shaderProgram_s prog;
static int u_mvp;                       /* the uniform mvp's register */
static C3D_FogLut fog;                  /* (made, but no batch uses fog) */

#include "view_shbin.h"                 /* (made by build.sh: view_shbin, view_shbin_size) */

/* r3d_init: load the shader, describe the vertex layout, allocate the
 * vertex buffer and make the fog table. Called once by plat.c after
 * citro3d is up. */
void r3d_init(void)
{
    C3D_AttrInfo *ai;
    C3D_BufInfo *bi;
    dvlb = DVLB_ParseFile((u32 *)view_shbin, view_shbin_size);
    shaderProgramInit(&prog);
    shaderProgramSetVsh(&prog, &dvlb->DVLE[0]);
    C3D_BindProgram(&prog);
    u_mvp = shaderInstanceGetUniformLocation(prog.vertexShader, "mvp");
    ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);    /* v0: x, y, z */
    AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);    /* v1: u, v */
    AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 4);    /* v2: r, g, b, a */
    vbuf = (vert *)linearAlloc(sizeof(vert) * MAX_VERT);   /* (the GPU reads only linear memory) */
    bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, vbuf, sizeof(vert), 3, 0x210);   /* 3 attributes in one buffer, in the order 0, 1, 2 */
    FogLut_Exp(&fog, 0.0018f, 1.0f, 0.01f, 4096.0f);   /* (exponential fog, density 0.0018, near 0.01, far 4096) */
}

/* push: n more vertices at the end of the buffer (0 if it is full) */
static vert *push(int n)
{
    vert *v;
    if (vn + n > MAX_VERT) return 0;
    v = &vbuf[vn];
    vn += n;
    return v;
}

/* set_v: fill a vertex: position, texture coordinate (u, t), colour c (r, g, b; alpha 1) */
static void set_v(vert *v, float x, float y, float z, float u, float t, const float *c)
{
    v->x = x;
    v->y = y;
    v->z = z;
    v->u = u;
    v->v = t;
    v->r = c[0];
    v->g = c[1];
    v->b = c[2];
    v->a = 1.0f;
}

static const float WHITE[3] = {1, 1, 1};       /* the texture's own colours */

/* a quad A..B standing on the floor (h high), u from ua to ub
 * (two triangles; the texture's v is 1 at the top, 0 at the floor) */
static void quad(float ax, float az, float bx, float bz, float ua, float ub, float h, const float *c)
{
    vert *v = push(6);
    if (!v) return;
    set_v(&v[0], ax, h, az, ua, 1, c);
    set_v(&v[1], ax, 0, az, ua, 0, c);
    set_v(&v[2], bx, 0, bz, ub, 0, c);
    set_v(&v[3], ax, h, az, ua, 1, c);
    set_v(&v[4], bx, 0, bz, ub, 0, c);
    set_v(&v[5], bx, h, bz, ub, 1, c);
}

/* tex_env: the GPU's colour combiner, stage 0: the texture times the
   vertex colour (textured), or the vertex colour alone */
static void tex_env(int textured)
{
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    if (textured) {
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
    } else {
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    }
}

/* one draw call per texture: the walls of texture t from first */
struct batch {
    C3D_Tex *t;                         /* the texture (0: untextured) */
    int first, n, fog;                  /* its first vertex, how many, fog on */
};
#define MAX_BATCH 512
static struct batch batches[MAX_BATCH];
static int nb;                          /* batches this frame */

/* begin_batch: start a batch for texture t, or carry on with the last one
   if it has the same texture; an empty last batch is dropped. When all
   MAX_BATCH are used, the vertices go to the last batch whatever its
   texture. */
static void begin_batch(C3D_Tex *t, int fog)
{
    if (nb && batches[nb - 1].t == t && batches[nb - 1].fog == fog) return;
    if (nb && batches[nb - 1].n == 0) nb--;
    if (nb >= MAX_BATCH) return;
    batches[nb].t = t;
    batches[nb].first = vn;
    batches[nb].n = 0;
    batches[nb].fog = fog;
    nb++;
}

/* end_batch: the last batch's count, up to the vertices pushed so far */
static void end_batch(void)
{
    if (nb) batches[nb - 1].n = vn - batches[nb - 1].first;
}

/* tex_map_of: texture t, or what the map swaps it for once both power
   couplings are destroyed (S.couplings >= 2: the portal closes) */
static u8 tex_map_of(u8 t)
{
    int i;
    if (S.couplings >= 2)
        for (i = 0; i < M.n_swaps; i++)
            if (M.swaps[2 * i] == t) return M.swaps[2 * i + 1];
    return t;
}

/* the geometry, from the state and the camera
 * Fills vbuf and batches[]: the floor and ceiling (untextured), the walls
 * (a batch per texture), the things (a batch per sprite). Called by
 * r3d_draw each frame. */
static void build(void)
{
    u32 k;
    float fx = sinf(cam_a), fz = -cosf(cam_a), rx = cosf(cam_a), rz = sinf(cam_a);
    nb = 0;
    /* the floor and the ceiling: squares (a cell each) around the camera,
       darker with distance as the SNES port's backdrop is (0.3 + 0.9 t, t
       the line's distance from the horizon: 26 / d below, 20 / d above) */
    begin_batch(0, 0);
    {
        int cx = (int)floorf(cam_x / 64), cz = (int)floorf(cam_z / 64), gx, gz;
        float ex = cam_x - BACK * fx, ez = cam_z - BACK * fz;     /* the eye */
        for (gz = cz - FLOOR_R; gz <= cz + FLOOR_R; gz++)
            for (gx = cx - FLOOR_R; gx <= cx + FLOOR_R; gx++) {
                float x0 = gx * 64.0f, z0 = gz * 64.0f, x1 = x0 + 64, z1 = z0 + 64, cf[4][3], cc[4][3];
                float xs[4] = {x0, x0, x1, x1}, zs[4] = {z0, z1, z1, z0};
                int k, j;
                vert *v;
                if ((x0 + 32 - ex) * fx + (z0 + 32 - ez) * fz < -64) continue;   /* behind */
                for (k = 0; k < 4; k++) {
                    float d = (xs[k] - ex) * fx + (zs[k] - ez) * fz, tf, tc;  /* the corner's depth */
                    if (d < 1) d = 1;
                    tf = 0.3f + 0.9f * (26.2f / d);
                    tc = 0.3f + 0.9f * (20.4f / d);
                    for (j = 0; j < 3; j++) {
                        cf[k][j] = floor_c[j] * tf > 1 ? 1 : floor_c[j] * tf;
                        cc[k][j] = ceil_c[j] * tc > 1 ? 1 : ceil_c[j] * tc;
                    }
                }
                v = push(12);           /* the floor's two triangles at y 0, the ceiling's at 64 */
                if (!v) continue;
                set_v(&v[0], xs[0], 0, zs[0], 0, 0, cf[0]);
                set_v(&v[1], xs[1], 0, zs[1], 0, 0, cf[1]);
                set_v(&v[2], xs[2], 0, zs[2], 0, 0, cf[2]);
                set_v(&v[3], xs[0], 0, zs[0], 0, 0, cf[0]);
                set_v(&v[4], xs[2], 0, zs[2], 0, 0, cf[2]);
                set_v(&v[5], xs[3], 0, zs[3], 0, 0, cf[3]);
                set_v(&v[6], xs[0], 64, zs[0], 0, 0, cc[0]);
                set_v(&v[7], xs[2], 64, zs[2], 0, 0, cc[2]);
                set_v(&v[8], xs[1], 64, zs[1], 0, 0, cc[1]);
                set_v(&v[9], xs[0], 64, zs[0], 0, 0, cc[0]);
                set_v(&v[10], xs[3], 64, zs[3], 0, 0, cc[3]);
                set_v(&v[11], xs[2], 64, zs[2], 0, 0, cc[2]);
            }
    }
    end_batch();
    /* the walls, by texture */
    {
        int t;
        u8 used[MAX_TEX];
        memset(used, 0, sizeof used);
        for (k = 0; k < M.n_walls; k++) used[tex_map_of(blk[M.walls + 16 * k + 9]) & (MAX_TEX - 1)] = 1;  /* the textures in use */
        for (t = 0; t < MAX_TEX; t++) {
            if (!used[t] || !tex_ok[t]) continue;
            begin_batch(&tex[t], 0);
            for (k = 0; k < M.n_walls; k++) {
                u32 w = M.walls + 16 * k;
                u8 lock = blk[w + 11];
                s16 ax = bes(w), az = bes(w + 2), bx = bes(w + 4), bz = bes(w + 6);
                s16 ua = blk[w + 13], ub = bes(w + 14);
                s8 du;
                if ((tex_map_of(blk[w + 9]) & (MAX_TEX - 1)) != t) continue;
                if (lock < 64 && (S.l_flags[lock] & L_OPEN)) continue;
                /* the side it is seen from (two-sided ones from both) */
                /* (the cross product of eye-to-A with A-to-B; <= 0: from behind,
                   so a two-sided wall is turned round: A and B, and u, swapped) */
                if ((ax - cam_x + BACK * fx) * (bz - az) - (az - cam_z + BACK * fz) * (bx - ax) <= 0) {
                    s16 tmp;
                    if (!(blk[w + 12] & 1)) continue;
                    tmp = ax; ax = bx; bx = tmp;
                    tmp = az; az = bz; bz = tmp;
                    tmp = ua; ua = ub; ub = tmp;
                }
                du = (s8)(ub - ua);     /* u in texels -> 0..1 of the texture below */
                quad(ax, az, bx, bz, (float)(u8)ua / 64.0f, ((float)(u8)ua + du) / 64.0f, 64, WHITE);
            }
            end_batch();
        }
    }
    /* the things: billboards and decals */
    {
        int i;
        float px = rx, pz = rz;         /* a billboard lies along the camera's right */
        for (i = 0; i < M.n_scen + n_ents; i++) {   /* scenery first, then entity i - n_scen */
            s16 x, z;
            u16 info = 0;
            u8 sp;
            if (i < M.n_scen) {
                u32 s = M.scenery + 10 * i;
                s16 hid = bes(s + 8);
                if (hid >= 0 && (S.static_hidden[(u16)hid >> 3] & (1 << (hid & 7)))) continue;  /* removed in play */
                x = bes(s);
                z = bes(s + 2);
                sp = (u8)be16(s + 4);
                info = be16(s + 6);
            } else {
                int e = i - M.n_scen;
                u8 d = S.e_def[e];
                if (!(S.e_flags[e] & F_LINKED) || d == 0xFF) continue;    /* not on the map, or no kind */
                x = (s16)S.e_x[e] * 64 + 32;
                z = (s16)S.e_z[e] * 64 + 32;
                sp = blk[M.def_spr + d];
            }
            if (sp >= MAX_SPR || !spr_ok[sp]) continue;
            begin_batch(&spr[sp], 0);
            if (info & 0x78) {          /* a decal: bits 3..6 say which way its wall faces; $200 the other side */
                float s = (info & 0x200) ? -32 : 32, ax, az, bx, bz;
                if (info & 0x08) { ax = x - s; az = z; bx = x + s; bz = z; }
                else if (info & 0x10) { ax = x + s; az = z; bx = x - s; bz = z; }
                else if (info & 0x40) { ax = x; az = z - s; bx = x; bz = z + s; }
                else { ax = x; az = z + s; bx = x; bz = z - s; }
                quad(ax, az, bx, bz, 0, 1, 64, WHITE);
            } else {                    /* a billboard, 16 units nearer the camera as the original draws it */
                float cx = x - 16 * fx, cz = z - 16 * fz;
                quad(cx - 32 * px, cz - 32 * pz, cx + 32 * px, cz + 32 * pz, 0, 1, 64, WHITE);
            }
            end_batch();
        }
    }
}

/* r3d_draw: move the camera a frame, build the geometry and draw it into
 * left (and right, for stereo: right non-zero) with the eyes iod apart (the
 * 3D slider's value from plat.c, negated for the left eye). Called once a
 * frame by plat.c, after C3D_FrameBegin. */
void r3d_draw(C3D_RenderTarget *left, C3D_RenderTarget *right, float iod)
{
    int eye, i;
    C3D_Mtx proj, view, mvp;
    C3D_FVec eyep, at, up;
    float fx, fz;
    if (!M.loaded) return;
    camera_step();
    fx = sinf(cam_a);
    fz = -cosf(cam_a);
    build();
    eyep = FVec3_New(cam_x - BACK * fx, EYE, cam_z - BACK * fz);
    at = FVec3_New(eyep.x + fx, EYE, eyep.z + fz);
    up = FVec3_New(0, 1, 0);
    Mtx_LookAt(&view, eyep, at, up, false);    /* (false: a right-handed view) */
    C3D_BindProgram(&prog);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);   /* (citro3d's projections: nearer is greater) */
    C3D_CullFace(GPU_CULL_NONE);        /* (the walls' sides are chosen in build) */
    C3D_AlphaTest(true, GPU_GREATER, 0x80);    /* the sprites' see-through texels are dropped */
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);   /* no blending */
    for (eye = 0; eye < (right ? 2 : 1); eye++) {
        float e = eye == 0 ? -iod : iod;
        /* 69 degrees of field of view, near 2, far 4096; "tilt": the
           3DS's screens are rotated a quarter turn. The screen's plane is
           STEREO_SCREEN units away, nearer than the wall in front (48:
           the cell's half width plus BACK), so the world is seen behind
           the screen and the weapon, drawn flat on the screen's plane
           over it (plat.c), in front of it all, as it is drawn; the eyes
           e x 4 x STEREO_SCREEN / 96 apart (far things as far apart as
           with the plane at 96 and the eyes e x 4) */
        Mtx_PerspStereoTilt(&proj, C3D_AngleFromDegrees(69.0f), C3D_AspectRatioTop, 2.0f, 4096.0f,
                            e * 4.0f * STEREO_SCREEN / 96.0f, STEREO_SCREEN, false);
        Mtx_Multiply(&mvp, &proj, &view);
        C3D_FrameDrawOn(eye == 0 ? left : right);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_mvp, &mvp);  /* the shader's mvp */
        for (i = 0; i < nb; i++) {      /* a draw call a batch */
            struct batch *b = &batches[i];
            if (!b->n) continue;
            if (b->fog) {
                C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false);
                C3D_FogColor(0);
                C3D_FogLutBind(&fog);
            } else
                C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
            if (b->t) {
                C3D_TexBind(0, b->t);
                tex_env(1);
            } else
                tex_env(0);
            C3D_DrawArrays(GPU_TRIANGLES, b->first, b->n);
        }
    }
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
}

/* ---- flat pictures (the overlays): a texture's (u0, v0)-(u1, v1) at
   (x, y, w, h) on a screen sw x 240 (y down), tinted
   t the target, sw the screen's width (400 top, 320 bottom), tx the
   texture (0: a plain rectangle of the colour), rgba the tint (and its
   alpha), additive: added to what is there instead of replacing it.
   Drawn at once, with no depth test. Called by plat.c. */
void r3d_flat(C3D_RenderTarget *t, int sw, C3D_Tex *tx, float x, float y, float w, float h, float u0, float v0,
              float u1, float v1, const float *rgba, int additive)
{
    C3D_Mtx proj;
    vert *v = push(6);
    float c[3] = {rgba[0], rgba[1], rgba[2]};
    int first;
    if (!v) return;
    first = (int)(v - vbuf);
    set_v(&v[0], x, y, 0.5f, u0, v0, c);
    set_v(&v[1], x, y + h, 0.5f, u0, v1, c);
    set_v(&v[2], x + w, y + h, 0.5f, u1, v1, c);
    set_v(&v[3], x, y, 0.5f, u0, v0, c);
    set_v(&v[4], x + w, y + h, 0.5f, u1, v1, c);
    set_v(&v[5], x + w, y, 0.5f, u1, v0, c);
    for (int i = 0; i < 6; i++) v[i].a = rgba[3];
    C3D_FrameDrawOn(t);
    C3D_BindProgram(&prog);
    Mtx_OrthoTilt(&proj, 0.0f, (float)sw, 240.0f, 0.0f, 0.0f, 1.0f, true);  /* pixels, y down */
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_mvp, &proj);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    if (tx) {
        C3D_AlphaTest(true, GPU_GREATER, 0x80);
        C3D_TexBind(0, tx);
        tex_env(1);
    } else {
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        tex_env(0);
    }
    if (additive) C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
    else C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_DrawArrays(GPU_TRIANGLES, first, 6);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);    /* back to no blending */
}

/* r3d_frame_start: empty the vertex buffer for a new frame (plat.c calls
   it at the start of each frame) */
void r3d_frame_start(void) { vn = 0; }

/* the automap in (x0, y0, w, h) of a screen sw x 240: the player in the
   middle (a little low), facing up, 12 pixels a cell (K)
   line2: a line from (ax, ay) to (bx, by), wd pixels each side of it, as
   two triangles of colour c */
static void line2(float ax, float ay, float bx, float by, float wd, const float *c)
{
    float dx = bx - ax, dy = by - ay, l = sqrtf(dx * dx + dy * dy), nx, ny;
    vert *v;
    if (l < 0.01f) return;
    nx = -dy / l * wd;
    ny = dx / l * wd;
    v = push(6);
    if (!v) return;
    set_v(&v[0], ax + nx, ay + ny, 0.5f, 0, 0, c);
    set_v(&v[1], ax - nx, ay - ny, 0.5f, 0, 0, c);
    set_v(&v[2], bx - nx, by - ny, 0.5f, 0, 0, c);
    set_v(&v[3], ax + nx, ay + ny, 0.5f, 0, 0, c);
    set_v(&v[4], bx - nx, by - ny, 0.5f, 0, 0, c);
    set_v(&v[5], bx + nx, by + ny, 0.5f, 0, 0, c);
}

/* clip the segment to the box (Liang-Barsky) -> 0 if nothing is left
   (t0..t1: the part of the segment inside all four edges) */
static int clip(float *ax, float *ay, float *bx, float *by, float x0, float y0, float x1, float y1)
{
    float t0 = 0, t1 = 1, dx = *bx - *ax, dy = *by - *ay;
    float p[4] = {-dx, dx, -dy, dy}, q[4] = {*ax - x0, x1 - *ax, *ay - y0, y1 - *ay};
    int i;
    for (i = 0; i < 4; i++) {
        if (p[i] == 0) {
            if (q[i] < 0) return 0;
        } else {
            float r = q[i] / p[i];
            if (p[i] < 0) { if (r > t1) return 0; if (r > t0) t0 = r; }
            else { if (r < t0) return 0; if (r < t1) t1 = r; }
        }
    }
    *bx = *ax + t1 * dx;
    *by = *ay + t1 * dy;
    *ax = *ax + t0 * dx;
    *ay = *ay + t0 * dy;
    return 1;
}

/* r3d_automap: the walls the player has seen (wall_seen), turned so that
   the player faces up, red, the doors yellow, over a dark box, with an
   arrow for the player. Called by plat.c for the bottom screen. */
void r3d_automap(C3D_RenderTarget *t, int sw, float x0, float y0, float w, float h)
{
    static const float BG[3] = {0.063f, 0.063f, 0.094f}, WALL[3] = {0.78f, 0.22f, 0.18f},
                       DOOR[3] = {0.95f, 0.82f, 0.25f}, ME[3] = {1, 1, 1};
    const float K = 12.0f / 64;         /* pixels a world unit */
    float fx, fz, rx, rz, cx = x0 + w / 2, cy = y0 + h * 0.6f, x1 = x0 + w - 2, y1 = y0 + h - 2;
    C3D_Mtx proj;
    vert *v;
    int first = vn;
    u32 k;
    if (!M.loaded) return;
    fx = sinf(cam_a);
    fz = -cosf(cam_a);
    rx = cosf(cam_a);
    rz = sinf(cam_a);
    v = push(6);
    if (!v) return;
    set_v(&v[0], x0, y0, 0.5f, 0, 0, BG);
    set_v(&v[1], x0, y0 + h, 0.5f, 0, 0, BG);
    set_v(&v[2], x0 + w, y0 + h, 0.5f, 0, 0, BG);
    set_v(&v[3], x0, y0, 0.5f, 0, 0, BG);
    set_v(&v[4], x0 + w, y0 + h, 0.5f, 0, 0, BG);
    set_v(&v[5], x0 + w, y0, 0.5f, 0, 0, BG);
    for (k = 0; k < M.n_walls && k < MAX_WALLS; k++) {
        u32 wl = M.walls + 16 * k;
        u8 lock = blk[wl + 11];
        float ax, ay, bx, by, px, pz;
        if (!wall_seen[k]) continue;
        if (lock < 64 && (S.l_flags[lock] & L_OPEN)) continue;
        px = bes(wl) - cam_x;           /* A relative to the camera, then turned: right is +x, ahead is up */
        pz = bes(wl + 2) - cam_z;
        ax = cx + (px * rx + pz * rz) * K;
        ay = cy - (px * fx + pz * fz) * K;
        px = bes(wl + 4) - cam_x;
        pz = bes(wl + 6) - cam_z;
        bx = cx + (px * rx + pz * rz) * K;
        by = cy - (px * fx + pz * fz) * K;
        if (!clip(&ax, &ay, &bx, &by, x0 + 2, y0 + 2, x1, y1)) continue;
        line2(ax, ay, bx, by, 0.8f, lock < 64 ? DOOR : WALL);
    }
    line2(cx, cy - 6, cx - 4, cy + 4, 0.7f, ME);   /* the player: an arrow pointing up */
    line2(cx, cy - 6, cx + 4, cy + 4, 0.7f, ME);
    line2(cx - 4, cy + 4, cx, cy + 1, 0.7f, ME);
    line2(cx + 4, cy + 4, cx, cy + 1, 0.7f, ME);
    for (k = (u32)first; k < (u32)vn; k++) vbuf[k].a = 1.0f;   /* (set_v already sets alpha 1) */
    C3D_FrameDrawOn(t);
    C3D_BindProgram(&prog);
    Mtx_OrthoTilt(&proj, 0.0f, (float)sw, 240.0f, 0.0f, 0.0f, 1.0f, true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_mvp, &proj);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    tex_env(0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_DrawArrays(GPU_TRIANGLES, first, vn - first);
}
