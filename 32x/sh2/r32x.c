/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the 32X: the 3D view, drawn by the SH-2 into the frame
 * buffer: 320 x 176 under the message line, from the campaign block's map
 * pack (megadrive/tools/md_campaign.py, as the 3DS's r3d.c reads it): the
 * walls (64 high, textured 64 x 64), the things (64 x 64 billboards, the
 * decals flat on their walls), the floor and the ceiling in bands, darker
 * with distance. The camera is the original's (16 units behind the cell's
 * centre, the eye 36 up, a 90 degree field of view) and glides from cell
 * to cell and turns smoothly.
 *
 * Each column keeps its nearest wall (1/depth, u/depth: a "1/z buffer"),
 * then each column is drawn once; the things are drawn far to near over
 * it where they are nearer than the column's wall.
 *
 * The colours: a map's textures and sprites use at most 176 different
 * ones (Mega Drive colours, 9 bits); they get the palette's slots 32..207,
 * the floor's and ceiling's 8 shades each 208..223 (the UI keeps the rest).
 *
 * Where it fits: sh2/plat.c calls r32x_target when the game asks for a new
 * view, r32x_step each frame to move the camera, r32x_changed and
 * r32x_draw to redraw, r32x_colours for the floor and ceiling colours, and
 * reads r32x_palette when it loads the 32X's palette. The slave SH-2
 * (sh2/sound32x.c) calls r32x_slave_poll between its sound blocks to draw
 * the right half of each picture. The game state S (core/rules.h) gives
 * the player's cell and facing, the doors, and the entities.
 *
 * Units: a map cell is 64 world units; the camera and the eye are kept in
 * world units x 16 (4 fraction bits); "depth" (d) and "lateral" (l) are
 * view-space distances ahead and to the right, also in units x 16. Angles
 * run 0..1023 for a full turn, 0 facing north (-z), clockwise. Screen
 * columns 0..319, rows 0..175 of the view (frame buffer rows 8..183).
 *
 * The "campaign block" is the cartridge data made by the Mega Drive game's tools that
 * holds every map ("map pack": walls, textures, sprites, scenery); its
 * first bank is block_bank (campaign.h).
 *
 * Licence: GPL-2.0-or-later.
 */
#include "r32x.h"
#include "libc.h"
#include "../../core/rules.h"
#include "../../snes/src/campaign.h"
#include "sintab.h"

extern const u8 *rom_image;             /* the SNES image in the cartridge (plat.c) */

#define VW 320                          /* the view's width (pixels) */
#define VH 176                          /* its height */
#define HORIZON 88                      /* the eye's row (the middle) */
#define FOCAL 160                       /* pixels per unit of lateral/depth: VW / 2 gives 90 degrees */
#define EYE 36                          /* the eye's height (units; walls are 64 high) */
#define BACK 16                         /* the eye's distance behind the cell's centre (units) */
#define NEAR 4                          /* (world units) */
#define GLIDE 10                        /* frames a step or a turn takes */
#define MAX_TEX 64                      /* textures per map (a power of two: used as a mask) */
#define MAX_SPR 96                      /* sprites per map */
#define SLOT0 32                        /* the first palette slot for the map's colours */
#define SLOTS 176                       /* how many (32..207) */
#define SHADE0 208                      /* the ceiling's 8 shades, then the floor's 8 (208..223) */
#define MAX_THINGS 160                  /* things (scenery and entities) drawn per picture */

/* ---- the map */
static const u8 *blk;                   /* the campaign block's start in the cartridge */
/* be32 / be16 / bes: a big-endian u32, u16 or s16 at offset a in the block */
static u32 be32(u32 a) { return ((u32)blk[a] << 24) | ((u32)blk[a + 1] << 16) | ((u32)blk[a + 2] << 8) | blk[a + 3]; }
static u16 be16(u32 a) { return (u16)((blk[a] << 8) | blk[a + 1]); }
static s16 bes(u32 a) { return (s16)be16(a); }

/* the loaded map: its counts and where its tables are (offsets in the
   block; see map_load for each table's layout) */
static struct {
    u8 loaded, map;                     /* a map is loaded; which */
    u16 n_walls, n_tex, n_spr, n_scen;
    u32 walls, tex_off, tex_pal, spr_cols, spr_off, spr_coff, spr_pal, scenery, def_spr, tex_bank, spr_bank;
    u8 n_swaps, swaps[10];              /* texture swaps: up to 5 pairs (from, to) */
} M;
/* each texture's 32 colours and each sprite's 16 colours -> palette slots */
static u8 tex_rm[MAX_TEX][32], spr_rm[MAX_SPR][16];

/* the SH-2's divider (39 cycles; no interrupts here to share it with)
 * hdiv: a / b, signed 32 / 32. Writing DVSR ($FFFFFF00, the divisor) then
 * DVDNT ($FFFFFF04, the dividend) starts the division; reading DVDNT
 * waits for and returns the quotient. (Each SH-2 has its own divider, so
 * the master and the slave can both use it.) */
static inline s32 hdiv(s32 a, s32 b)
{
    *(volatile s32 *)0xFFFFFF00 = b;
    *(volatile s32 *)0xFFFFFF04 = a;
    return *(volatile s32 *)0xFFFFFF04;
}
/* (hi:lo) / b, signed 64 / 32
 * (DVDNTH $FFFFFF10 the high half, DVDNTL $FFFFFF14 the low half, whose
 * write starts it; the quotient comes back in DVDNTL) */
static inline s32 hdiv64(s32 hi, u32 lo, s32 b)
{
    *(volatile s32 *)0xFFFFFF00 = b;
    *(volatile s32 *)0xFFFFFF10 = hi;
    *(volatile u32 *)0xFFFFFF14 = lo;
    return *(volatile s32 *)0xFFFFFF14;
}
/* a x b / c with a 64-bit product */
static inline s32 muldiv(s32 a, s32 b, s32 c)
{
    long long p = (long long)a * b;
    return hdiv64((s32)(p >> 32), (u32)p, c);
}

/* the map's walls and scenery in SDRAM (the ROM's are bytes to put together) */
#define MAX_WALLS 1024
#define MAX_SCEN 192
/* a wall: a vertical quad from A to B, 64 units high */
struct wall {
    s16 ax, az, bx, bz;                 /* its ends (world units) */
    u8 tex, lock, two, ua;              /* texture; the door line it belongs to (< 64; else none);
                                           two-sided; u (texel column) at A */
    s8 du, pad[3];                      /* u at B minus u at A */
};
static struct wall W[MAX_WALLS];
static int n_w;
/* a piece of scenery (a static sprite) */
struct scen {
    s16 x, z, hid;                      /* position (world units); its bit in S.static_hidden (-1 none) */
    u16 info;                           /* flags: $78 a decal and which way it faces, $200 the other side */
    u8 spr, pad;                        /* the map's sprite number */
};
static struct scen SC[MAX_SCEN];
static int n_sc;
/* the sprites' parts: columns (top, len), column offsets, data */
static const u8 *spr_cols_p[MAX_SPR], *spr_coff_p[MAX_SPR], *spr_data_p[MAX_SPR];
static const u8 *tex_data[MAX_TEX];     /* each texture's 64 x 64 bytes, column by column */
static u16 pal[256];                    /* the view's colours (BGR555); slots 32..223 used */
static u16 floor_c = 0x2108, ceil_c = 0x4210;  /* the floor's and ceiling's base colours (BGR555) */

/* a Mega Drive colour level (0..7) -> 8 bits: the Mega Drive's DAC levels */
static const u8 LEV[8] = {0, 52, 87, 116, 144, 172, 206, 255};
/* md_bgr: a Mega Drive colour (0000 BBB0 GGG0 RRR0) -> BGR555 */
static u16 md_bgr(u16 c)
{
    return (u16)((LEV[(c >> 1) & 7] >> 3) | ((LEV[(c >> 5) & 7] >> 3) << 5) | ((LEV[(c >> 9) & 7] >> 3) << 10));
}

static int n_cols;                      /* palette slots handed out for this map */
static u16 col_md[SLOTS];               /* the Mega Drive colour in each slot */
/* slot_of: the palette slot for Mega Drive colour md: the slot that has it,
 * else a new slot, else (all 176 taken) the slot with the nearest colour
 * (squared level differences weighted 3 red, 4 green, 2 blue). Called by
 * map_load. */
static u8 slot_of(u16 md)
{
    int i, best = 0, bd = 1 << 30;
    md &= 0xEEE;                        /* only the colour bits */
    for (i = 0; i < n_cols; i++)
        if (col_md[i] == md) return (u8)(SLOT0 + i);
    if (n_cols < SLOTS) {
        col_md[n_cols] = md;
        pal[SLOT0 + n_cols] = md_bgr(md);
        return (u8)(SLOT0 + n_cols++);
    }
    for (i = 0; i < n_cols; i++) {         /* (full: the nearest) */
        int dr = ((md >> 1) & 7) - ((col_md[i] >> 1) & 7), dg = ((md >> 5) & 7) - ((col_md[i] >> 5) & 7),
            db = ((md >> 9) & 7) - ((col_md[i] >> 9) & 7), d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return (u8)(SLOT0 + best);
}

/* entry: table e of the map pack at pk (the pack's directory: 8 bytes an
   entry from pk + 32, the first 4 the table's offset in the block) */
static u32 entry(u32 pk, int e) { return be32(pk + 32 + 8 * e); }

/* shades: the floor's and ceiling's 8 shades into pal[208..223]. Each band
 * is 11 rows (8 bands from the horizon to the edge); the brightness t (in
 * thousandths) grows with the band's distance from the horizon, i.e. is
 * darker far away. Called by map_load and r32x_colours. */
static void shades(void)
{
    int i, k;
    for (i = 0; i < 8; i++)
        for (k = 0; k < 2; k++) {       /* k 0 the ceiling, 1 the floor */
            /* the 3DS's (and SNES's) 0.3 + 0.9 x 26 / d below, 20 / d above, at the band's middle row */
            int r = i * 11 + 6, t = k ? 300 + (900 * 262 * r) / (36 * FOCAL * 10) : 300 + (900 * 204 * r) / (28 * FOCAL * 10);
            u16 c = k ? floor_c : ceil_c;
            int rr = ((c & 31) * t) / 1000, gg = (((c >> 5) & 31) * t) / 1000, bb = (((c >> 10) & 31) * t) / 1000;
            if (rr > 31) rr = 31;
            if (gg > 31) gg = 31;
            if (bb > 31) bb = 31;
            pal[SHADE0 + k * 8 + i] = (u16)(rr | (gg << 5) | (bb << 10));
        }
}

/*
 * map_load: load map number map from the campaign block: find its pack,
 * note its tables, give its textures' and sprites' colours palette slots,
 * copy its walls and scenery into SDRAM (W, SC) and set the shades.
 * Layout (offsets in bytes, big-endian):
 *   block +8 / +12   the texture bank's and the sprite bank's offsets
 *   block +32 + 32 m map m's pack offset (pk)
 *   pk +8..+14       counts: walls, textures, sprites, scenery
 *   pk +20, +21..    the number of texture swaps, then the (from, to) pairs
 *   pk +32 + 8 e     the directory (entry): 0 walls, 1 texture offsets,
 *                    2 texture palettes, 4 sprite columns, 5 sprite data
 *                    offsets, 6 sprite column offsets, 9 sprite palettes,
 *                    11 scenery, 12 entity kind -> sprite
 *   a wall (16 bytes): ax, az, bx, bz; +9 texture; +11 door line; +12 bit 0
 *                    two-sided; +13 u at A; +14 (s16) u at B
 *   scenery (10 bytes): x, z, sprite, info, hidden bit
 *   a texture palette: 32 Mega Drive colours; a sprite's: 16
 * Called by r32x_target when the map changes.
 */
static void map_load(u8 map)
{
    u32 pk;
    int i, x;
    blk = rom_image + (((u32)block_bank - 0xC0) << 16);    /* (SNES bank $C0 is the image's start) */
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
    n_cols = 0;                         /* the palette slots: all free again */
    for (i = 0; i < M.n_tex && i < MAX_TEX; i++) {
        tex_data[i] = blk + M.tex_bank + be32(M.tex_off + 4 * i);
        for (x = 0; x < 32; x++) tex_rm[i][x] = slot_of(be16(M.tex_pal + 64 * i + 2 * x));
    }
    for (i = 0; i < M.n_spr && i < MAX_SPR; i++) {
        for (x = 0; x < 16; x++) spr_rm[i][x] = slot_of(be16(M.spr_pal + 32 * i + 2 * x));
        spr_cols_p[i] = blk + M.spr_cols + (u32)i * 128;   /* 64 columns x 2 bytes */
        spr_coff_p[i] = blk + M.spr_coff + (u32)i * 128;
        spr_data_p[i] = blk + M.spr_bank + be32(M.spr_off + 4 * i);
    }
    n_w = M.n_walls < MAX_WALLS ? M.n_walls : MAX_WALLS;
    for (i = 0; i < n_w; i++) {
        u32 w = M.walls + 16 * i;
        W[i].ax = bes(w);
        W[i].az = bes(w + 2);
        W[i].bx = bes(w + 4);
        W[i].bz = bes(w + 6);
        W[i].tex = blk[w + 9];
        W[i].lock = blk[w + 11];
        W[i].two = blk[w + 12] & 1;
        W[i].ua = blk[w + 13];
        W[i].du = (s8)(bes(w + 14) - blk[w + 13]);
    }
    n_sc = M.n_scen < MAX_SCEN ? M.n_scen : MAX_SCEN;
    for (i = 0; i < n_sc; i++) {
        u32 a = M.scenery + 10 * i;
        SC[i].x = bes(a);
        SC[i].z = bes(a + 2);
        SC[i].spr = (u8)be16(a + 4);
        SC[i].info = be16(a + 6);
        SC[i].hid = bes(a + 8);
    }
    shades();
    M.loaded = 1;
    M.map = map;
}

/* r32x_colours: the floor's and ceiling's colours (BGR555) for this map,
   from the game (plat.c's view_gradient); remakes the shades */
void r32x_colours(u16 floor, u16 ceil)
{
    floor_c = floor;
    ceil_c = ceil;
    shades();
}

/* r32x_palette: the view's 256 colours (only 32..223 are meaningful) */
const u16 *r32x_palette(void) { return pal; }

/* ---- the camera: world units x 16; the angle 1024 a turn, 0 north (-z), clockwise */
static s32 cam_x, cam_z, cam_a;         /* where the camera is now */
static s32 from_x, from_z, from_a, to_x, to_z, to_a;   /* a glide's start and end */
static int glide_start = -1000, have_cam;
/* glide_start: the frame a glide began; -1 starts at the next r32x_step,
   -1000 none. have_cam: the camera has been placed once */
static u16 view_gen = 1, drawn_view[2];
/* view_gen: +1 at each change of the view; drawn_view: its value when each
   frame buffer was drawn */

/* r32x_init: clear COMM+6, the master-to-slave "draw your half" flag */
void r32x_init(void) { MARS_COMM(6) = 0; }

/* r32x_target: aim the camera at the player (S.px, S.pz, S.facing): load
 * the map if it changed; if the camera is new or more than a cell away
 * (a teleport, a new level), jump there; else glide there over GLIDE
 * frames, turning the shorter way. Called by plat.c's view_render. */
void r32x_target(void)
{
    s32 tx = ((s32)S.px * 64 + 32) * 16, tz = ((s32)S.pz * 64 + 32) * 16, ta = (s32)(S.facing & 3) * 256;
    if (!M.loaded || M.map != S.map) map_load(S.map);
    view_gen++;
    /* (64 * 16 + 8: one cell, with a little slack) */
    if (!have_cam || (tx - cam_x > 64 * 16 + 8) || (cam_x - tx > 64 * 16 + 8) || (tz - cam_z > 64 * 16 + 8) ||
        (cam_z - tz > 64 * 16 + 8)) {
        cam_x = to_x = tx;
        cam_z = to_z = tz;
        cam_a = to_a = ta;
        glide_start = -1000;
        have_cam = 1;
        return;
    }
    while (ta - cam_a > 512) ta -= 1024;        /* the shorter way round */
    while (ta - cam_a < -512) ta += 1024;
    if (tx == cam_x && tz == cam_z && ta == cam_a) return;     /* already there */
    from_x = cam_x;
    from_z = cam_z;
    from_a = cam_a;
    to_x = tx;
    to_z = tz;
    to_a = ta;
    glide_start = -1;                   /* (from the next frame) */
}

/* r32x_step: move the camera along a glide to where it is at frame (the
 * 68000's frame count); returns 1 if it moved (the view must be redrawn).
 * Called each frame by plat.c's wait_vblank. */
int r32x_step(int frame)
{
    int el, k;
    if (glide_start == -1000) return 0;     /* no glide */
    if (glide_start == -1) glide_start = frame;
    el = (u16)(frame - glide_start);    /* frames since it began (the count wraps at 16 bits) */
    if (el >= GLIDE) {                  /* over: at the target */
        cam_x = to_x;
        cam_z = to_z;
        cam_a = to_a & 1023;
        glide_start = -1000;
    } else {
        k = el * 256 / GLIDE;           /* the time, 0..255 for 0..1 */
        k = (k * k * (768 - 2 * k)) >> 16;      /* ease in and out */
        /* (256 x (3 t^2 - 2 t^3): a smoothstep, still 0..256) */
        cam_x = from_x + ((to_x - from_x) * k >> 8);
        cam_z = from_z + ((to_z - from_z) * k >> 8);
        cam_a = from_a + ((to_a - from_a) * k >> 8);
    }
    view_gen++;
    return 1;
}

/* r32x_changed: 1 if frame buffer buf's picture of the view is out of date */
int r32x_changed(int buf) { return M.loaded && drawn_view[buf] != view_gen; }

/* ---- drawing */
/* The per-column arrays below are written by the master (walls) and read
 * by both SH-2s (each draws its half of the columns). */
static s32 fx, fz, rx, rz, ex, ez;      /* forward, right (x 16384); the eye (x 16) */
static s32 col_iz[VW];                  /* the column's nearest wall: 1/depth (x 2^30 / 16 units), 0 none */
static s32 col_uz[VW];                  /* its u / depth (u in texels x 256, same scale) */
static u8 col_tex[VW];                  /* its texture */

/* tex_map_of: texture t, or what the map swaps it for once both power
 * couplings are destroyed (S.couplings >= 2: the portal closes) */
static u8 tex_map_of(u8 t)
{
    int i;
    if (S.couplings >= 2)
        for (i = 0; i < M.n_swaps; i++)
            if (M.swaps[2 * i] == t) return M.swaps[2 * i + 1];
    return t;
}

/* a point (world units) into view space: depth and lateral (units x 16) */
static void to_view(s32 wx, s32 wz, s32 *d, s32 *l)
{
    s32 dx = wx * 16 - ex, dz = wz * 16 - ez;
    *d = (dx * fx + dz * fz) >> 14;
    *l = (dx * rx + dz * rz) >> 14;
}

/* (x >> 14) for |x| < 2^30, by logical shifts: the SH-2 shifts right arithmetically a bit at a time)
 * (adding 2^30 makes x positive; after the shift that bias is 2^16) */
#define ASR14(x) ((s32)(((u32)(x) + 0x40000000u) >> 14) - 0x10000)
#define IZ_ONE (1 << 30)                /* iz = IZ_ONE / depth (depth: units x 16, at least NEAR x 16) */
static u8 col_ub[VW];                   /* the wall's u at its left end (u = col_ub + col_uz / col_iz) */

/* a wall A..B (view space, u in texels at each end) into the columns
 * da, la: A's depth and lateral (units x 16); ua its u; the same for B;
 * tex the texture. A must be the left end (seen from the front). Clips the
 * wall at the near plane, projects its ends to columns, and for each
 * column between them where it is nearer than what the column has, stores
 * its 1/depth, u/depth (both stepped linearly across the screen, which
 * gives perspective-correct u), its texture and its base u.
 * Called by walls(). */
static void wall_span(s32 da, s32 la, s32 ua, s32 db, s32 lb, s32 ub, u8 tex)
{
    s32 x0, x1, iza, izb, uza, uzb, diz, duz, x, base, n;
    s32 ua8 = ua << 8, ub8 = ub << 8;   /* u x 256 */
    /* the near plane (t: how far along, x 4096) */
    if (da < NEAR * 16) {
        s32 t = hdiv((NEAR * 16 - da) << 12, db - da);
        la += ((lb - la) * t) >> 12;
        ua8 += ((ub8 - ua8) * t) >> 12;
        da = NEAR * 16;
    } else if (db < NEAR * 16) {
        s32 t = hdiv((NEAR * 16 - db) << 12, da - db);
        lb += ((la - lb) * t) >> 12;
        ub8 += ((ua8 - ub8) * t) >> 12;
        db = NEAR * 16;
    }
    /* the screen: x = 160 + 160 l / d */
    x0 = VW / 2 + hdiv(la * FOCAL, da);
    x1 = VW / 2 + hdiv(lb * FOCAL, db);
    if (x0 >= x1 || x1 <= 0 || x0 >= VW) return;          /* (from behind, or off screen) */
    base = (ua8 < ub8 ? ua8 : ub8) >> 8;   /* u relative to the smaller end keeps u/depth small */
    iza = hdiv(IZ_ONE, da);
    izb = hdiv(IZ_ONE, db);
    uza = (ua8 - (base << 8)) * (iza >> 8);
    uzb = (ub8 - (base << 8)) * (izb >> 8);
    n = x1 - x0;
    diz = hdiv(izb - iza, n);           /* the steps per column */
    duz = hdiv(uzb - uza, n);
    if (x0 < 0) {                       /* starts left of the screen: skip ahead */
        iza += diz * -x0;
        uza += duz * -x0;
        x0 = 0;
    }
    if (x1 > VW) x1 = VW;
    for (x = x0; x < x1; x++, iza += diz, uza += duz)
        if (iza > col_iz[x]) {          /* nearer than the column's wall so far */
            col_iz[x] = iza;
            col_uz[x] = uza;
            col_tex[x] = tex;
            col_ub[x] = (u8)base;
        }
}

/* walls: every wall of the map into the columns (col_iz etc., which must
 * be cleared first). Skips open doors, walls behind the eye or wholly
 * outside the 90 degree view, and one-sided walls seen from behind.
 * Called by r32x_draw on the master. */
static void walls(void)
{
    int k;
    u8 swapped = S.couplings >= 2 && M.n_swaps;
    for (k = 0; k < n_w; k++) {
        const struct wall *w = &W[k];
        s32 dax, daz, dbx, dbz, da, la, db, lb, cross;
        u8 t;
        if (w->lock < 64 && (S.l_flags[w->lock] & L_OPEN)) continue;  /* an open door: not drawn */
        dax = w->ax * 16 - ex;
        daz = w->az * 16 - ez;
        dbx = w->bx * 16 - ex;
        dbz = w->bz * 16 - ez;
        da = ASR14(dax * fx + daz * fz);
        db = ASR14(dbx * fx + dbz * fz);
        if (da < NEAR * 16 && db < NEAR * 16) continue;    /* wholly behind the near plane */
        la = ASR14(dax * rx + daz * rz);
        lb = ASR14(dbx * rx + dbz * rz);
        /* (both ends outside the same side of the view) */
        if (la > da && lb > db) continue;
        if (-la > da && -lb > db) continue;
        t = (swapped ? tex_map_of(w->tex) : w->tex) & (MAX_TEX - 1);
        /* the side it is seen from (two-sided ones from both) */
        /* (the cross product of eye-to-A, in units, with A-to-B) */
        cross = (dax >> 4) * (w->bz - w->az) - (daz >> 4) * (w->bx - w->ax);
        if (cross <= 0) {               /* from behind */
            if (!w->two) continue;
            wall_span(db, lb, w->ua + w->du, da, la, w->ua, t);    /* B is the left end from this side */
        } else
            wall_span(da, la, w->ua, db, lb, w->ua + w->du, t);
    }
}

/* ---- the frame buffer's rows 8..183 */
/* ROW(y): view row y's first byte in the frame buffer (row 8 + y) */
#define ROW(y) (MARS_FB8 + 0x200 + (8 + (y)) * 320)

/* floor_ceiling: fill columns x0..x1-1 of every view row with its band's
 * shade (the ceiling above the horizon, the floor below; band 0 at the
 * horizon). Writes long words, 16 pixels per pass: x0 must be a multiple
 * of 4 and x1 - x0 a multiple of 16 (0, 160 and 320 are). Called by part. */
static void floor_ceiling(int x0, int x1)
{
    int y;
    for (y = 0; y < VH; y++) {
        int r = y < HORIZON ? HORIZON - 1 - y : y - HORIZON, band = r / 11;    /* r: rows from the horizon */
        u32 c = (u32)(SHADE0 + (y < HORIZON ? 0 : 8) + (band > 7 ? 7 : band)), i;
        volatile u32 *d = (volatile u32 *)ROW(y) + x0 / 4;
        c |= c << 8;                    /* the colour index in all 4 bytes */
        c |= c << 16;
        for (i = 0; i < (u32)(x1 - x0) / 4; i += 4) {
            d[i] = c;
            d[i + 1] = c;
            d[i + 2] = c;
            d[i + 3] = c;
        }
    }
}

/* a column's scale: pixels a unit (16.16) and texels a pixel (16.16), at depth d (units x 16)
 * (not used: draw_walls and things_draw work the scale out themselves) */
#define SCALE(d) hdiv(FOCAL * 16 * 65536 / 16, (d) >> 4 ? (d) >> 4 : 1)

/* draw_walls: each column x0..x1-1 with a wall: its texture column,
 * scaled to the wall's height at that depth (the wall from 64 units high
 * down to the floor, the eye EYE units up), over the floor and ceiling
 * already drawn. A texture is 64 x 64, stored column by column (64 bytes
 * a column); its bytes index the texture's 32 colours. Called by part. */
static void draw_walls(int x0, int x1)
{
    int x;
    for (x = x0; x < x1; x++) {
        s32 iz = col_iz[x], sc, top, y0, y1, v, vstep, u, izs;
        const u8 *src, *rm;
        volatile u8 *p;
        if (!iz) continue;              /* no wall in this column */
        /* sc = 2560 / d (d = IZ_ONE / iz) in 16.16 = 2560 x iz / 2^14 */
        sc = (s32)(((u32)iz >> 6) * 160 >> 4);
        top = (HORIZON << 16) - (64 - EYE) * sc;       /* the wall's top row, 16.16 */
        y0 = (top + 0xFFFF) >> 16;                      /* the first whole row at or below the top (rounded up) */
        y1 = ((HORIZON << 16) + EYE * sc + 0xFFFF) >> 16;       /* the floor's row */
        if (y0 < 0) y0 = 0;
        if (y1 > VH) y1 = VH;
        if (y0 >= y1) continue;
        vstep = hdiv64(1, 0, sc);                       /* texels per pixel, 16.16 */
        v = muldiv((y0 << 16) - top, vstep, 65536);    /* the texel row at y0, 16.16 */
        izs = iz >> 8;
        u = col_ub[x] + (hdiv(col_uz[x], izs ? izs : 1) >> 8);    /* the texel column: (u/z) / (1/z) */
        src = tex_data[col_tex[x]] + (u & 63) * 64;
        rm = tex_rm[col_tex[x]];
        p = ROW(y0) + x;
        y1 -= y0;                       /* now the number of rows */
        while (y1 >= 2) {               /* two rows per pass */
            p[0] = rm[src[(v >> 16) & 63] & 31];
            v += vstep;
            p[320] = rm[src[(v >> 16) & 63] & 31];
            v += vstep;
            p += 640;
            y1 -= 2;
        }
        if (y1) *p = rm[src[(v >> 16) & 63] & 31];    /* an odd last row */
    }
}

/* ---- the things */
/* A "thing" is anything drawn as a sprite: scenery and the entities
 * (monsters, items...). A billboard always faces the camera; a decal lies
 * flat on a wall, drawn like a short wall. */
struct thing {
    s32 d, l;                           /* the centre (billboards) */
    s32 ax, az, bx, bz;                 /* (decals) the quad's ends, world */
    u8 spr, decal;                      /* the map's sprite; 1 a decal */
};
static struct thing th[MAX_THINGS];     /* this picture's things */
static u8 order[MAX_THINGS];            /* their indexes, far to near */
static int nth;                         /* how many */

/* canvas column c of sprite spr at screen column x: its top row at top
   (16.16), sc pixels a canvas row, vstep canvas rows a pixel
   A sprite is a 64 x 64 canvas stored by column: for each column, its
   first opaque row t and length len (spr_cols), the offset of its len
   bytes in the sprite's data (spr_coff, big-endian); each byte indexes the
   sprite's 16 colours, 255 being see-through. Called by things_draw. */
static void spr_column(int x, u8 spr, int c, s32 top, s32 sc, s32 vstep)
{
    const u8 *ci = spr_cols_p[spr] + c * 2, *co = spr_coff_p[spr] + c * 2, *data, *rm = spr_rm[spr];
    u8 t = ci[0], len = ci[1];
    s32 y0, y1, v;
    volatile u8 *p;
    if (!len) return;                   /* an empty column */
    data = spr_data_p[spr] + ((co[0] << 8) | co[1]) - t;  /* (so that data[row] is canvas row row) */
    y0 = (top + t * sc + 0xFFFF) >> 16;
    y1 = (top + (t + len) * sc + 0xFFFF) >> 16;
    if (y0 < 0) y0 = 0;
    if (y1 > VH) y1 = VH;
    if (y0 >= y1) return;
    v = muldiv((y0 << 16) - top, vstep, 65536);
    p = ROW(y0) + x;
    for (; y0 < y1; y0++, p += 320, v += vstep) {
        int r = v >> 16;
        u8 k;
        if (r < t) r = t;               /* (rounding can step just outside the column's rows) */
        if (r >= t + len) r = t + len - 1;
        k = data[r];
        if (k != 255) *p = rm[k & 15];
    }
}

/* things_list: make th[] and order[]: the map's visible scenery, then the
 * entities that are on the map (F_LINKED, from S), each with its view
 * position, skipping what is too far, behind, or outside the view; then
 * sort them far to near (insertion sort on d). Called by r32x_draw. */
static void things_list(void)
{
    int i, j, n = n_sc + n_ents;
    nth = 0;
    for (i = 0; i < n && nth < MAX_THINGS; i++) {   /* i < n_sc scenery, then entity i - n_sc */
        s32 x, z, dx, dz;
        u16 info = 0;
        u8 sp;
        struct thing *t = &th[nth];
        if (i < n_sc) {
            const struct scen *c = &SC[i];
            if (c->hid >= 0 && (S.static_hidden[(u16)c->hid >> 3] & (1 << (c->hid & 7)))) continue;  /* removed in play */
            x = c->x;
            z = c->z;
            sp = c->spr;
            info = c->info;
        } else {
            int e = i - n_sc;
            u8 d = S.e_def[e];
            if (!(S.e_flags[e] & F_LINKED) || d == 0xFF) continue;    /* not on the map, or no kind */
            x = (s32)S.e_x[e] * 64 + 32;        /* its cell's centre */
            z = (s32)S.e_z[e] * 64 + 32;
            sp = blk[M.def_spr + d];            /* its kind's sprite */
        }
        if (sp >= MAX_SPR || sp >= M.n_spr) continue;
        dx = x * 16 - ex;
        dz = z * 16 - ez;
        t->d = ASR14(dx * fx + dz * fz);
        if (t->d < NEAR * 16 - 64 * 16 || t->d > 1536 * 16) continue;    /* well behind, or beyond 1536 units */
        t->l = ASR14(dx * rx + dz * rz);
        t->spr = sp;
        if (info & 0x78) {              /* a decal: bits 3..6 the wall it is on */
            s32 s = (info & 0x200) ? -32 : 32;  /* half its width, the sign giving the facing */
            t->decal = 1;
            if (info & 0x08) { t->ax = x - s; t->az = z; t->bx = x + s; t->bz = z; }
            else if (info & 0x10) { t->ax = x + s; t->az = z; t->bx = x - s; t->bz = z; }
            else if (info & 0x40) { t->ax = x; t->az = z - s; t->bx = x; t->bz = z + s; }
            else { t->ax = x; t->az = z + s; t->bx = x; t->bz = z - s; }
            /* info $800 (the original's $8000000): seen from both sides. From
               behind (the walls' cross-product test, the camera at ex, ez in
               1/16 units) it goes from B to A, its picture mirrored, as the
               original draws it. */
            if ((info & 0x800) &&
                (t->ax * 16 - ex) * (t->bz - t->az) - (t->az * 16 - ez) * (t->bx - t->ax) <= 0) {
                s32 q = t->ax; t->ax = t->bx; t->bx = q;
                q = t->az; t->az = t->bz; t->bz = q;
            }
            if (t->l - 32 * 16 > t->d + 32 * 16 || -t->l - 32 * 16 > t->d + 32 * 16) continue;  /* (outside the view) */
        } else {
            t->decal = 0;
            t->d -= 16 * 16;            /* (the billboard 16 units nearer, as the original draws it) */
            if (t->d < NEAR * 16) continue;
            if (t->l - 32 * 16 > t->d || -t->l - 32 * 16 > t->d) continue;     /* (outside the view) */
        }
        nth++;
    }
    /* far to near */
    for (i = 0; i < nth; i++) order[i] = (u8)i;
    for (i = 1; i < nth; i++) {
        u8 o = order[i];
        for (j = i; j > 0 && th[order[j - 1]].d < th[o].d; j--) order[j] = order[j - 1];
        order[j] = o;
    }
}

/* the things over columns xa..xb-1
 * Far to near, so nearer things paint over farther ones; a column of a
 * thing is drawn only where it is nearer than that column's wall
 * (col_iz). Billboards are 64 x 64 units, standing on the floor, centred
 * on their position. Called by part, on both SH-2s. */
static void things_draw(int xa, int xb)
{
    int i;
    for (i = 0; i < nth; i++) {
        struct thing *t = &th[order[i]];
        if (!t->decal) {
            /* sc: pixels a unit (16.16); vstep: units (canvas texels) a pixel */
            s32 sc = hdiv(FOCAL * 16 * 4096, t->d) << 4, vstep = hdiv64(1, 0, sc);
            s32 cx = (VW / 2 << 16) + muldiv(t->l, FOCAL << 16, t->d);    /* the centre's column, 16.16 */
            s32 left = cx - 32 * sc, top = (HORIZON << 16) - (64 - EYE) * sc;
            s32 x0 = (left + 0xFFFF) >> 16, x1 = (left + 64 * sc + 0xFFFF) >> 16, iz = hdiv(IZ_ONE, t->d), x, c;
            if (x0 < xa) x0 = xa;
            if (x1 > xb) x1 = xb;
            if (x0 >= x1) continue;
            c = muldiv((x0 << 16) + 0x8000 - left, vstep, 65536);       /* canvas column, 16.16 */
            for (x = x0; x < x1; x++, c += vstep) {
                if (iz < col_iz[x] || (c >> 16) > 63) continue;    /* behind the wall, or past the canvas */
                spr_column(x, t->spr, c >> 16, top, sc, vstep);
            }
        } else {
            /* a decal: as a wall (its own depth per column), the sprite's columns along it */
            s32 dax = t->ax * 16 - ex, daz = t->az * 16 - ez, dbx = t->bx * 16 - ex, dbz = t->bz * 16 - ez;
            s32 da = (dax * fx + daz * fz) >> 14, la = ASR14(dax * rx + daz * rz);
            s32 db = (dbx * fx + dbz * fz) >> 14, lb = ASR14(dbx * rx + dbz * rz);
            s32 x0, x1, iza, izb, diz, x, cza, czb, dcz;
            if (da < NEAR * 16 || db < NEAR * 16) continue;    /* (not clipped: dropped if either end is too near) */
            x0 = VW / 2 + hdiv(la * FOCAL, da);
            x1 = VW / 2 + hdiv(lb * FOCAL, db);
            if (x0 >= x1 || x1 <= 0 || x0 >= VW) continue;
            iza = hdiv(IZ_ONE, da);
            izb = hdiv(IZ_ONE, db);
            cza = 0;                    /* canvas column / depth, as u/depth for walls */
            czb = 64 * (izb >> 8);
            diz = hdiv(izb - iza, x1 - x0);
            dcz = hdiv(czb - cza, x1 - x0);
            if (x0 < xa) {
                iza += diz * (xa - x0);
                cza += dcz * (xa - x0);
                x0 = xa;
            }
            if (x1 > xb) x1 = xb;
            for (x = x0; x < x1; x++, iza += diz, cza += dcz) {
                s32 sc, top;
                int c;
                if (iza + (iza >> 6) < col_iz[x]) continue;     /* (on its wall: a little in front) */
                c = hdiv(cza, (iza >> 8) ? iza >> 8 : 1);      /* the canvas column here */
                if (c < 0 || c > 63) continue;
                sc = (s32)(((u32)iza >> 6) * 160 >> 4);         /* (as in draw_walls) */
                top = (HORIZON << 16) - (64 - EYE) * sc;
                spr_column(x, t->spr, c, top, sc, hdiv64(1, 0, sc));
            }
        }
    }
}

/* ---- the slave SH-2 draws the right half (r32x_slave_poll, between its sound) */

/* part: draw columns x0..x1-1 of the view: floor and ceiling, walls, things */
static void part(int x0, int x1)
{
    floor_ceiling(x0, x1);
    draw_walls(x0, x1);
    things_draw(x0, x1);
}

#define SPLIT 160                       /* the master draws columns 0..159, the slave 160..319 */

/* (the job's signal: COMM 6 = 1 from the master, 0 from the slave when
   done - emulators wake an SH-2 polling a COMM register when it changes)
   r32x_slave_poll: on the slave, if the master has asked, draw the right
   half of the picture and say so. Called by slave_main (sound32x.c). */
void r32x_slave_poll(void)
{
    if (MARS_COMM(6) != 1) return;
    CCR = 0x11;                         /* (the master's new columns and things: not this cache's old ones) */
    /* (CCR $11: purge this SH-2's cache and keep it on) */
    part(SPLIT, VW);
    MARS_COMM(6) = 0;                   /* done: the master may go on */
}

#ifdef R32X_BENCH
/* the benchmark build: frames taken by 30 passes of each phase: [0] walls,
   [1] floor and ceiling, [2] wall columns, [3] the slave's half, [4] a
   whole picture; [5] 1 when done (plat.c then times the sprites into [4]) */
volatile u32 bench_res[6];
static void bench(void)
{
    int k, ph;
    for (ph = 0; ph < 5; ph++) {
        u16 f0 = MARS_COMM(4);
        for (k = 0; k < 30; k++) {
            if (ph == 0) { memset(col_iz, 0, sizeof col_iz); walls(); }
            if (ph == 1) floor_ceiling(0, VW);
            if (ph == 2) draw_walls(0, VW);
            if (ph == 3) { MARS_COMM(6) = 1; while (MARS_COMM(6)) {} }
            if (ph == 4) { memset(col_iz, 0, sizeof col_iz); walls(); things_list(); MARS_COMM(6) = 1; part(0, SPLIT); while (MARS_COMM(6)) {} }
        }
        *(volatile u32 *)UNCACHED(&bench_res[ph]) = (u16)(MARS_COMM(4) - f0);
    }
    *(volatile u32 *)UNCACHED(&bench_res[5]) = 1;
}
#endif

/* r32x_draw: draw the view as the camera has it into rows 8..183 of the
 * frame buffer the SH-2s can write now (plat.c chose and waited for it),
 * and note it as frame buffer buf's. The master works out the walls and
 * the things, asks the slave for the right half (COMM+6), draws the left
 * half, and waits for the slave. Called by plat.c's wait_vblank. */
void r32x_draw(int buf)
{
    if (!M.loaded) return;
    {
        int a = (int)(cam_a & 1023);    /* the unit vectors, x 16384, from the sine table */
        fx = SIN[a];
        fz = -SIN[(a + 256) & 1023];
        rx = SIN[(a + 256) & 1023];
        rz = SIN[a];
        ex = cam_x - ASR14(BACK * 16 * fx);    /* the eye: BACK units behind the camera */
        ez = cam_z - ASR14(BACK * 16 * fz);
    }
    memset(col_iz, 0, sizeof col_iz);   /* no walls yet */
    walls();                            /* the nearest wall of each column */
    things_list();                      /* the sprites to draw, sorted */
    MARS_COMM(6) = 1;                   /* the slave: the right half */
    part(0, SPLIT);                     /* the left half */
    while (MARS_COMM(6)) {}             /* wait for the slave */
    drawn_view[buf] = view_gen;
#ifdef R32X_BENCH
    if (!bench_res[5]) bench();
#endif
}
