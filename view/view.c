/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the SNES: the 3D view (see view.h).
 *
 * Geometry (given for the default 128 x 88 view; at another size, 128 is
 * VIEW_W, 64 the focal length HW = VIEW_W / 2 and 44 VIEW_HALF_H: the same
 * 90 degree field of view, the picture scaled - see SX_NUM and the others
 * below). A view-space point has a depth d and a lateral offset l; the
 * focal length is half the view's width (64), so screen x = 64 + 64 l / d.
 * For a wall from A to B (da, la) .. (db, lb) and the centre of screen
 * column x, with dd = db - da, dl = lb - la and c = dl da - dd la:
 *   num2(x) = 128 dl - (2x + 1 - 128) dd
 *   m2(x)   = (2x + 1 - 128) da - 128 la
 *   depth   = 64 c / num2 ... so hq = 65536 / depth = 1024 num2 / c / 2
 *   t       = m2 / num2   (0 at A, 1 at B: the texture's u)
 * (In general depth = HW c / (num2 / 2) and hq = 1024 HW / depth, so hq =
 * 512 num2 / c at any size.)
 * hq (12.4 fixed point: the height in pixels times 16) is linear in x, so
 * it is worked out at both ends of the wall and stepped in between; t
 * takes one small division per column (the SNES's 16/8 divider).
 *
 * A column of height h (pixels) starts at row 44 - (28 h + 32) / 64 (the
 * eye is 36 of the 64 units up) and steps through the 64 texture rows by
 * dv = 16384 / h (8.8) from dv / 2: the same for walls and sprites
 * (a sprite's canvas is 64 rows standing on the floor).
 *
 * Who uses it. The game's screen code calls view_map_load when a map
 * starts and view_render after each move or turn (snes/src/ui.c on the
 * SNES; gbc/src/gfx.c on the Game Boy Color; gba/src/gfx.c on the GBA;
 * test/view_host.c on the build machine). The 32X and the 3DS have their
 * own renderers and do not use this file. The view reads the game state
 * S (core/rules.h: the player's cell and facing, the doors' l_flags, the
 * entities, S.static_hidden, S.couplings) and the map pack through
 * view/vdata.h. It writes col_h / col_tex / col_t (per screen column:
 * the wall to draw) and calls the platform's draw_walls once, then
 * draw_spr once per sprite column, far things first, so near ones paint
 * over them.
 *
 * Three builds of the same file:
 *  - the fixed camera (the default: the SNES's cc65 build, the Game Boy
 *    Color, view_host): the camera stands on a cell facing one of 4 ways,
 *    and the map pack's precomputed lists say which walls (pvs) and
 *    which cells' things (vis) can be seen from there. On the SNES
 *    (__CC65__) the wall and thing loops are snes/src/vcore.s's, in
 *    assembler (vc_walls, vc_things), with the same arithmetic as the C
 *    here, which the other builds compile.
 *  - VIEW_FX, the free camera (any position, 256 angles) used by the
 *    SNES's Super FX build (where the GSU, snes/gsu/view.s, does the
 *    drawing and this C is its reference) and by the GBA (VIEW_FX_FAST).
 *  - VIEW_PLAT: the platform supplies faster division and multiply
 *    macros (the Game Boy Color's gbc/src/view_plat.h).
 *
 * Units and fixed point. World units: 64 per map cell (cell x spans
 * 64 x .. 64 x + 63; its centre is 64 x + 32). "hq" is a height in
 * pixels times 16 (12.4 fixed point): 65536 / depth. An "8.8" number has
 * 8 fraction bits. Colours: the map pack holds Mega Drive colours (9 bits:
 * 0000 BBB0 GGG0 RRR0); they become SNES colours (BGR555) and "slots"
 * (indices 32..223 into the picture's palette, view_cgram).
 *
 * Jargon: "view" (in the pvs / vis tables) is a (cell, facing) pair,
 * numbered cell * 4 + facing; "the map pack" is a map's render data in
 * the campaign block (see vdata.h); a "decal" is a sprite flat on a
 * wall, drawn as a quad, a "billboard" one that always faces the camera;
 * "remap" turns a texture's or sprite's own colour index into a palette
 * slot (tex_rm, spr_rm).
 *
 * Licence: GPL-2.0-or-later.
 */
#include "view.h"
#include "vdata.h"
#include "../core/rules.h"

/* HW: half the view's width, the focal length in pixels (view.h's
   VIEW_FOCAL: 64 at the default 128 wide); NEAR: the nearest depth a wall
   is clipped to (world units) */
#define HW VIEW_FOCAL
#define NEAR 1

/* The sums that hold the view's size (the top of the file has them at 128
 * wide: there 2 HW = VIEW_W = 128, HW = 64). With column x's centre k =
 * 2x + 1 - VIEW_W half pixels from the middle:
 *   SX_NUM(l)    l * HW * 256: the numerator of a screen x in 8.8
 *                (screen x = HW + HW l / d)
 *   TIMES_W(v)   v * VIEW_W (= 2 HW), in m2 = k da - 2 HW la
 *   HQ_NUM       1024 HW: hq = HQ_NUM / d for a thing at depth d (64 HW
 *                pixels high, 16 times that in hq)
 *   CANVAS_OF(v) v / VIEW_W rounded down (v = k d): where column x's ray
 *                crosses a billboard's plane, from its centre
 * At the default size they are the original shifts and constants, so the
 * other ports' builds of this file are unchanged. */
#if VIEW_W == 128
#define SX_NUM(l) ((s32)(l) << 14)
#define TIMES_W(v) ((v) << 7)
#define HQ_NUM 65536L
#define CANVAS_OF(v) ((v) >> 7)
#else
#define SX_NUM(l) ((s32)(l) * (HW * 256L))
#define TIMES_W(v) ((v) * VIEW_W)
#define HQ_NUM (1024L * HW)
#define CANVAS_OF(v) floor_w(v)

/* floor_w: v / VIEW_W rounded down (towards minus infinity, as an
 * arithmetic shift would be for a power of two). No side effects. */
static s32 floor_w(s32 v)
{
    return v >= 0 ? v / VIEW_W : -((-v + VIEW_W - 1) / VIEW_W);
}
#endif

/* a platform may do the 32-bit divisions by a 16-bit divisor faster (the
   Game Boy Color: gbc/src/view_plat.h), with the same results */
#ifdef VIEW_PLAT
#include "view_plat.h"
#endif
#ifndef DIV_L16
#define DIV_L16(n, d) ((n) / (d))       /* s32 by a non-zero s16 */
#endif
#ifndef DIV_U16
#define DIV_U16(a, b) ((a) / (b))       /* u16 by a non-zero u16 */
#endif
#ifndef MUL_W
#define MUL_W(a, b) ((s32)(a) * (b))    /* s16 by s16 */
#endif
/* CAM_BACK: the camera stands 16 units behind the cell's centre (the
   original's); MAX_VW: walls kept per view (fixed camera); MAX_PT:
   things kept per view */
#define CAM_BACK 16
#define MAX_VW 64
#define MAX_PT 64

/* the outputs declared in view.h (see there) */
u16 view_cgram[VIEW_SLOTS];
u8 view_n_colours;
u16 view_floor, view_ceiling;
u8 tex_rm[V_MAX_TEX][32];
u8 spr_rm[V_MAX_SPR][16];
u16 col_h[VIEW_W];
u32 col_tex[VIEW_W];
u8 col_t[VIEW_W];
u8 ds_x, ds_top, ds_len, ds_s;
u16 ds_h;
u32 ds_data;

/* the map in play's pack: counts and table addresses (vd_pack) */
static struct vpack P;
u32 tex_addr[V_MAX_TEX];                /* block address of each texture */
u32 spr_addr[V_MAX_SPR];                /* and of each sprite's column data */
u8 tex_map[V_MAX_TEX];                  /* the portal's swaps */
/* (tex_map: wall texture slot -> the slot drawn; the identity, except
   that once both of the Reactor's power couplings are destroyed
   (S.couplings >= 2) the pack's swap pairs apply) */
#ifdef __CC65__
/* the walls are snes/src/vcore.s's there (the same arithmetic) */
/* (col_hq, col_w: as below; vc_*: vcore.s's inputs, set by view_render:
   the camera, its facing, the view's pvs list, the wall table, the doors'
   flags; then for the things: the vis list (0xFFFFFFFF: every cell), the
   scenery, the def -> sprite table, the sprites' column tables, and S's
   entity arrays) */
extern u16 col_hq[VIEW_W];
extern u8 col_w[VIEW_W];
extern s16 vc_vx, vc_vz;
extern u16 vc_f;
extern u32 vc_pvs, vc_walls_at;
extern u8 *vc_lflags;
void vc_walls(void);
extern u32 vc_vis, vc_scenery, vc_def_spr, vc_spr_cols, vc_spr_coff;
extern u16 vc_nscen, vc_nents;
extern u8 *vc_hidden, *vc_eflags, *vc_edef, *vc_ex, *vc_ez;
void vc_things(void);
#else
static u16 col_hq[VIEW_W];              /* the nearest wall's hq (0: none) */
static u8 col_w[VIEW_W];                /* its index in vw[] (0xFF: none) */
#endif

/* ------------------------------------------------------------ colours */
/* the Mega Drive's 8 levels per colour channel, as 0..255 */
static const u8 LEV[8] = {0, 52, 87, 116, 144, 172, 206, 255};
#if !defined(__CC65__) && VIEW_W >= 128
/* 9-bit colour -> slot (0: none yet): needed only while view_map_load
   runs, when col_tex (512 bytes or more) is free - so it lends its room
   (the Game Boy Color's view RAM is one 8 KB bank) */
#define slot_of ((u8 *)col_tex)
#else
static u8 slot_of[512];                 /* 9-bit colour -> slot (0: none yet) */
#endif

/* C9: a Mega Drive colour (0000 BBB0 GGG0 RRR0) as 9 bits: R | G << 3 | B << 6 */
#define C9(c) ((((c) >> 1) & 7) | (((c) >> 2) & 0x38) | (((c) >> 3) & 0x1C0))

/* snes_colour: a Mega Drive colour as an SNES one (0BBBBBGGGGGRRRRR),
 * each channel's level through LEV, kept to 5 bits. No side effects.
 * (Also used by the ports' own code.) */
u16 snes_colour(u16 md)
{
    return (u16)(LEV[(md >> 1) & 7] >> 3) | ((u16)(LEV[(md >> 5) & 7] >> 3) << 5) |
           ((u16)(LEV[(md >> 9) & 7] >> 3) << 10);
}

/* colour_slot: the palette slot (32..223) of Mega Drive colour md: the
 * slot it already has, or a new one (its SNES colour stored in
 * view_cgram, view_n_colours + 1), or if all 192 are taken the slot of
 * the nearest colour already in (squared distance of the 3-bit levels). */
static u8 colour_slot(u16 md)
{
    u16 k = C9(md), i, best = 0, bd = 0xFFFF;
    if (slot_of[k]) return slot_of[k];
    if (view_n_colours < VIEW_SLOTS) {
        view_cgram[view_n_colours] = snes_colour(md);
        slot_of[k] = VIEW_SLOT0 + view_n_colours++;
        return slot_of[k];
    }
    /* (no map has more than about 165 colours; just in case) the nearest */
    for (i = 0; i < 512; i++)
        if (slot_of[i]) {
            s16 dr = (s16)(i & 7) - (s16)(k & 7), dg = (s16)((i >> 3) & 7) - (s16)((k >> 3) & 7),
                db = (s16)(i >> 6) - (s16)(k >> 6);
            u16 d = (u16)(dr * dr + dg * dg + db * db);
            if (d < bd) {
                bd = d;
                best = slot_of[i];
            }
        }
    return (u8)best;
}

/* the map whose pack is loaded (0xFFFF: none yet) */
static u16 cur_map_loaded = 0xFFFF;
#ifdef VIEW_FX
static void fx_scenery_load(void);
#ifndef __CC65__
static void fx_walls_load(void);
#ifdef VIEW_FX_FAST
static void fx_cache_load(void);
#endif
#endif
#endif

#if !defined(VIEW_FX) && !defined(__CC65__)
/* per prop of the map (the first SCEN_CELLS): its cell, | 0x8000 for a
   decal (view_map_load), so view_prepare skips a prop whose cell is not
   seen without reading the prop from the pack */
#define SCEN_CELLS 136
static u16 scen_cell[SCEN_CELLS];
/* the last sprite whose column table was read (0xFF none; view_map_load
   forgets it): per column (top << 8) | length, so a billboard's columns
   read the pack once */
static u8 sc_spr = 0xFF;
static s16 sc_cols[64];
#endif

/* view_map_load: makes `map` the map the view draws. Reads its pack
 * (vd_pack into P), builds the palette from scratch: every texture's 32
 * and every sprite's 16 colours get a slot (tex_rm, spr_rm, view_cgram,
 * view_n_colours), notes each texture's and sprite's block address, and
 * the floor and ceiling colours. The free-camera builds also load their
 * own tables. Called by view_render when S.map changes, or by the port
 * (which then uploads view_cgram). */
void view_map_load(u8 map)
{
    u16 i;
    u8 k;
    vd_pack(map, &P);                   /* the pack's counts and tables */
    view_n_colours = 0;
    for (i = 0; i < 512; i++) slot_of[i] = 0;
    for (i = 0; i < P.n_tex && i < V_MAX_TEX; i++) {
        /* tex_off: u32 per texture, its offset in the texture bank;
           tex_pal: 32 colours (u16) per texture */
        tex_addr[i] = P.tex_bank + vd_u32(P.tex_off + 4 * i);
        for (k = 0; k < 32; k++) tex_rm[i][k] = colour_slot(vd_u16(P.tex_pal + 64 * i + 2 * k));
    }
#if !defined(VIEW_FX) && !defined(__CC65__)
    sc_spr = 0xFF;                      /* (sprite_col's table: another map's) */
    for (i = 0; i < P.n_scenery && i < SCEN_CELLS; i++) {
        s16 w[4];                       /* x, z, sprite slot, info */
        vd_words(P.scenery + 10 * (u32)i, w, 4);
        scen_cell[i] = (((u16)w[1] >> 6) << 5 | ((u16)w[0] >> 6)) | (w[3] & 0x78 ? 0x8000 : 0);
    }
#endif
    for (i = 0; i < P.n_spr && i < V_MAX_SPR; i++) {
        /* the same for the sprites: 16 colours each */
        spr_addr[i] = P.spr_bank + vd_u32(P.spr_off + 4 * i);
        for (k = 0; k < 16; k++) spr_rm[i][k] = colour_slot(vd_u16(P.spr_pal + 32 * i + 2 * k));
    }
    view_floor = snes_colour(P.floor);
    view_ceiling = snes_colour(P.ceiling);
    cur_map_loaded = map;
#ifdef VIEW_FX
    fx_scenery_load();                  /* the scenery list and def -> sprite table */
#ifndef __CC65__
    fx_walls_load();                    /* (the GSU's own on the SNES) */
#ifdef VIEW_FX_FAST
    fx_cache_load();                    /* the walls copied into RAM */
#endif
#endif
#endif
#if defined(VIEW_FX) && defined(__CC65__)
    fx_map_load(&P);                    /* the SNES's: hands the map to the GSU (snes/src/fx.c) */
#endif
}

/* ------------------------------------------------------------ maths */
/* the fixed camera: its world position (vx, vz), and its forward and
   right unit vectors (each component -1, 0 or 1) */
static s16 vx, vz, fwd_x, fwd_z, rgt_x, rgt_z;

/* to_view: world point (wx, wz) in view space: *d its depth (along the
 * forward vector), *l its lateral offset (along the right vector), in
 * world units from the camera. */
static void to_view(s16 wx, s16 wz, s16 *d, s16 *l)
{
    s16 rx = wx - vx, rz = wz - vz;
    *d = rx * fwd_x + rz * fwd_z;
    *l = rx * rgt_x + rz * rgt_z;
}

/* screen x of a view-space point, 8.8
 * HW + HW l / d, as (HW << 8) + SX_NUM(l) / d (at 128 wide: (64 << 8) +
 * (l << 14) / d); d must be non-zero. */
static s32 screen_x(s16 d, s16 l)
{
    return ((s32)HW << 8) + DIV_L16(SX_NUM(l), d);
}

/* t (0..255 along the wall) = m2 / num2 (num2: |num2| < 16384; m2 is
   kept to 16 bits, wrapping, which is exact where it matters: m2 between
   0 and num2), as the SNES works it out: both halved until num2 fits
   8 bits, then its 16/8 division */
static u8 t8_of(s16 m2, s16 num2)
{
    u16 q, m, n;
    if (num2 < 0) {                     /* make the divisor positive */
        num2 = -num2;
        m2 = (s16)(0 - (u16)m2);
    }
    if (m2 <= 0) return 0;              /* before A */
    if (m2 >= num2) return 255;         /* at or past B */
    m = (u16)m2;
    n = (u16)num2;
    while (n > 255) {
        n >>= 1;
        m >>= 1;
    }
    if (!n) return 0;
    q = DIV_U16(m << 8, n);
    return q > 255 ? 255 : (u8)q;
}

/* m2 at column x, to 16 bits (k da - VIEW_W la, k = 2x + 1 - VIEW_W) */
static s16 m2_at(s16 x, s16 da, s16 la)
{
    return (s16)((u16)((2 * x + 1 - VIEW_W) * da) - (u16)TIMES_W(la));
}

/* ------------------------------------------------------------ walls */
/* a wall kept for this view (vw[]): its A end in view space (da, la),
   B - A (dd, dl), c (see the top of the file), its texture slot, and its
   texture columns: ua at A, ua + du at B (0..63, wrapping) */
struct vwall {
    s16 da, la, dd, dl;
    s32 c;
    u8 tex;
    s8 ua, du;
};
static struct vwall vw[MAX_VW];
static u8 n_vw;

/* a projected quad (wall or decal): its view-space ends and columns
   (q_x0 .. q_x1 - 1 are the screen columns it covers), and its c */
static s16 q_da, q_la, q_db, q_lb, q_x0, q_x1;
static s32 q_c;

/* -> 0 when nothing of it is on screen
 * Projects the quad from world point A (ax, az) to B (bx, bz) with the
 * fixed camera. Writes q_da..q_lb (unclipped view-space ends), q_x0,
 * q_x1 and q_c. The part nearer than NEAR is clipped off before the
 * screen columns are found (the q_ ends stay unclipped: the per-column
 * maths uses the whole line). Also 0 if c is 0 (seen edge-on). */
static u8 project(s16 ax, s16 az, s16 bx, s16 bz)
{
    s16 cda, cla, cdb, clb;
    s32 sxa, sxb, t;
    to_view(ax, az, &q_da, &q_la);
    to_view(bx, bz, &q_db, &q_lb);
    if (q_da < NEAR && q_db < NEAR) return 0;       /* wholly behind */
    cda = q_da;
    cla = q_la;
    cdb = q_db;
    clb = q_lb;
    /* clip the end that is behind to depth NEAR, along the line */
    if (cda < NEAR) {
        cla += (s16)(((s32)(q_lb - q_la) * (NEAR - q_da)) / (q_db - q_da));
        cda = NEAR;
    } else if (cdb < NEAR) {
        clb += (s16)(((s32)(q_la - q_lb) * (NEAR - q_db)) / (q_da - q_db));
        cdb = NEAR;
    }
    sxa = screen_x(cda, cla);
    sxb = screen_x(cdb, clb);
    if (sxa > sxb) {                    /* left to right */
        t = sxa;
        sxa = sxb;
        sxb = t;
    }
    sxa = (sxa + 127) >> 8;            /* the first column whose centre is on it */
    sxb = (sxb + 127) >> 8;
    if (sxa < 0) sxa = 0;
    if (sxb > VIEW_W) sxb = VIEW_W;
    if (sxb <= sxa) return 0;
    q_x0 = (s16)sxa;
    q_x1 = (s16)sxb;
    q_c = MUL_W(q_lb - q_la, q_da) - MUL_W(q_db - q_da, q_la);
    return q_c != 0;
}

/* num2 at column x (see the top of the file: VIEW_W dl - k dd), for a
   wall with B - A = (dd, dl) */
static s16 num2_at(s16 x, s16 dd, s16 dl)
{
    return (s16)(VIEW_W * dl - (2 * x + 1 - VIEW_W) * dd);
}

/* hq = 512 num2 / c, c shortened to 16 bits first (and 512 num2 with it)
 * for the quad in q_* at column x; 0 if negative, at most 0xFFFF. */
static u16 hq_at(s16 x)
{
    s32 n = (s32)num2_at(x, q_db - q_da, q_lb - q_la) << 9, d = q_c, h;
    while (d > 32767L || d < -32767L - 1) {     /* until c fits a signed 16-bit divisor */
        d >>= 1;
        n >>= 1;
    }
    h = DIV_L16(n, (s16)d);
    if (h < 0) return 0;
    return h > 0xFFFF ? 0xFFFF : (u16)h;
}

/* hq of the quad's columns q_x0..q_x1-1 as 16.8: start and step */
static s32 q_hq, q_step;

/* hq_ramp: works out hq exactly at the quad's first and last columns and
 * sets q_hq (the first, 16.8, plus 0.5 for rounding) and q_step (the
 * change per column) so the columns between are stepped linearly. */
static void hq_ramp(void)
{
    s16 n = q_x1 - q_x0;
    u16 h0 = hq_at(q_x0);
    q_hq = ((s32)h0 << 8) + 128;
    q_step = 0;
    if (n > 1) q_step = DIV_L16(((s32)hq_at(q_x1 - 1) - h0) << 8, n - 1);
}

#ifndef __CC65__
/* project_walls: the fixed camera's wall pass. pvs: the word offset of
 * this view's list in the pvs table (a count, then wall indices). Each
 * wall: 8 big-endian words in the pack's wall table (16 bytes): ax, az,
 * bx, bz (world units), texture slot, line (the door it belongs to,
 * 0xFF a plain wall), ua | 0x100 if two-sided, ub (texture columns at A
 * and B). Open doors are skipped; a one-sided wall seen from behind is
 * skipped, a two-sided one turned round. Each kept wall goes in vw[] and
 * claims the columns where it is the nearest so far (the largest hq):
 * col_hq / col_w. At most MAX_VW walls. */
static void project_walls(u16 pvs)
{
    u16 n = vd_u16(P.pvs + 2 * (u32)pvs), k;
    s16 w[8], x;
    for (x = 0; x < VIEW_W; x++) {      /* no wall in any column yet */
        col_hq[x] = 0;
        col_w[x] = 0xFF;
    }
    n_vw = 0;
    for (k = 0; k < n && n_vw < MAX_VW; k++) {
        u16 i = vd_u16(P.pvs + 2 * ((u32)pvs + 1 + k));
        s16 ax, az, bx, bz, ua, ub, t;
        struct vwall *v;
        vd_words(P.walls + 16 * (u32)i, w, 8);      /* the wall's 8 words */
        if ((u8)w[5] != 0xFF && (S.l_flags[(u8)w[5]] & L_OPEN)) continue;     /* an open door */
        ax = w[0];
        az = w[1];
        bx = w[2];
        bz = w[3];
        ua = w[6] & 0xFF;
        ub = w[7];
        /* one-sided unless flagged */
        /* (the cross product: <= 0 when the camera is behind the wall) */
        if ((s32)(ax - vx) * (bz - az) - (s32)(az - vz) * (bx - ax) <= 0) {
            if (!(w[6] & 0x100)) continue;
            t = ax; ax = bx; bx = t;
            t = az; az = bz; bz = t;
            t = ua; ua = ub; ub = t;
        }
        if (!project(ax, az, bx, bz)) continue;
        v = &vw[n_vw];
        v->da = q_da;
        v->la = q_la;
        v->dd = q_db - q_da;
        v->dl = q_lb - q_la;
        v->c = q_c;
        v->tex = (u8)w[4];
        v->ua = (s8)ua;
        v->du = (s8)(ub - ua);
        hq_ramp();                                  /* hq along its columns */
        for (x = q_x0; x < q_x1; x++) {
            u16 h = (u16)(q_hq >> 8);
            if (h > col_hq[x]) {                    /* nearer than what the column had */
                col_hq[x] = h;
                col_w[x] = n_vw;
            }
            q_hq += q_step;
        }
        n_vw++;
    }
}

/* column_params: for each screen column, from the nearest wall found
 * (col_w, col_hq): its height col_h (pixels, rounded; 0 if no wall or
 * under half a pixel, col_hq < 8), its texture slot col_t (after the
 * portal's swaps) and col_tex, the block address of the texture column
 * u (0..63; a texture is 64 columns of 64 bytes): u is ua + t * du with
 * t the position along the wall (t8_of, 0..255). */
static void column_params(void)
{
    u8 x = 0;
    /* (m2 and num2 are linear in x: along a run of columns of the same
       wall they are stepped by 2 da and -2 dd, in 16 bits as m2_at and
       num2_at keep them, so the same values without a product a column) */
    while (x < VIEW_W) {
        u8 wi = col_w[x];
        const struct vwall *w;
        u16 m2, n2, dm, dn;
        u32 base;
        u8 t;
        if (wi == 0xFF) {
            col_h[x++] = 0;
            continue;
        }
        w = &vw[wi];
        m2 = (u16)m2_at(x, w->da, w->la);
        n2 = (u16)num2_at(x, w->dd, w->dl);
        dm = (u16)w->da << 1;
        dn = (u16)w->dd << 1;
        t = tex_map[w->tex];
        base = tex_addr[t];
        do {
            u16 hq = col_hq[x];
            if (hq < 8)
                col_h[x] = 0;
            else {
                u8 t8 = t8_of((s16)m2, (s16)n2);
                u8 u = (u8)((w->ua + (((s16)t8 * w->du) >> 8)) & 63);
                col_h[x] = (u16)(((u32)hq + 8) >> 4);
                col_t[x] = t;
                col_tex[x] = base + (u16)u * 64;
            }
            m2 += dm;
            n2 -= dn;
            x++;
        } while (x < VIEW_W && col_w[x] == wi);
    }
}
#endif

/* VIEW_PROF: timing points (the platform's ticks() at each stage of
   view_render into vprof[]); VIEW_LOG: the first 64 sprite columns drawn
   (x, height, canvas column, data address) for debugging */
#ifdef VIEW_PROF
u16 ticks(void);
u16 vprof[6];
#define PROF(i) vprof[i] = ticks()
#else
#define PROF(i)
#endif
#ifdef VIEW_LOG
u16 vlog[64][4];
u8 vlog_n;
#endif

/* the columns view_draw draws: strip_x0 .. strip_x1 - 1 (set by it; the
   whole view unless the picture is drawn a strip at a time) */
u8 strip_x0, strip_x1;

#ifndef __CC65__
/* ------------------------------------------------------------ things */
/* a thing to draw (pt[]): a billboard (an entity or a prop, facing the
   camera) or a decal (a sprite on a wall: a 64-unit quad) */
struct pthing {
    u16 hq;                     /* for sorting (far first) */
    s16 d, l;                   /* billboard: depth (16 nearer) and lateral */
    s16 x0, x1;
    u8 spr, decal;
    s16 ax, az, bx, bz;         /* decal quad (world) */
};
/* (x0 .. x1 - 1: the billboard's screen columns; spr its sprite slot;
   decal 1 for a decal) */
static struct pthing pt[MAX_PT];
static u8 pt_order[MAX_PT];     /* pt's indices, far to near (sort_things) */
static u8 n_pt;
static u8 vis[128];             /* the cells whose things can show */
/* (vis: one bit per map cell, bit c & 7 of byte c >> 3) */

/* add_billboard: a sprite standing at world (wx, wz), if its cell is in
 * vis, it is far enough (4 units, after being brought 16 units nearer,
 * as the original draws things) and some of its 64-unit width is on
 * screen. Adds it to pt[] with its screen columns and hq (65536 / d). */
static void add_billboard(s16 wx, s16 wz, u8 spr)
{
    s16 d, l;
    s32 sx0, sx1;
    struct pthing *p;
    u16 cell = ((u16)(wz >> 6) << 5) | (u16)(wx >> 6);    /* its map cell */
    if (n_pt >= MAX_PT || spr >= V_MAX_SPR) return;
    if (!(vis[cell >> 3] & (1 << (cell & 7)))) return;
    to_view(wx, wz, &d, &l);
    d -= 16;
    if (d < 4) return;
    if (l - 32 > d || l + 32 < -d) return;          /* wholly outside the 90-degree view */
    sx0 = screen_x(d, l - 32) >> 8;                 /* its left edge's column */
    sx1 = (screen_x(d, l + 32) + 255) >> 8;         /* past its right edge, rounded up */
    if (sx0 < 0) sx0 = 0;
    if (sx1 > VIEW_W) sx1 = VIEW_W;
    if (sx1 <= sx0) return;
    p = &pt[n_pt++];
    p->d = d;
    p->l = l;
    p->x0 = (s16)sx0;
    p->x1 = (s16)sx1;
    p->spr = spr;
    p->decal = 0;
    p->hq = (u16)DIV_L16(HQ_NUM, d);
}

/* add_decal: a sprite on a wall at world (x, z). info (the scenery's
 * flags): which way the wall runs and which side faces out (0x08, 0x10:
 * along x; 0x40, else: along z; 0x200 flips it); the quad is 64 units
 * wide, centred on (x, z), A to B (B to A for a two-sided one, 0x800,
 * seen from behind). Kept unless its centre is well behind
 * the camera or outside the view; its hq (for sorting) is from the
 * centre's depth. No vis test. */
static void add_decal(s16 x, s16 z, u8 spr, u16 info)
{
    s16 sgn = (info & 0x200) ? -1 : 1, dm, lm;
    struct pthing *p;
    if (n_pt >= MAX_PT || spr >= V_MAX_SPR) return;
    p = &pt[n_pt];
    if (info & 0x08) { p->ax = x - 32 * sgn; p->az = z; p->bx = x + 32 * sgn; p->bz = z; }
    else if (info & 0x10) { p->ax = x + 32 * sgn; p->az = z; p->bx = x - 32 * sgn; p->bz = z; }
    else if (info & 0x40) { p->ax = x; p->az = z - 32 * sgn; p->bx = x; p->bz = z + 32 * sgn; }
    else { p->ax = x; p->az = z + 32 * sgn; p->bx = x; p->bz = z - 32 * sgn; }
    /* info 0x800 (the original's $8000000): seen from both sides. From
       behind (the walls' cross-product test) it is drawn from B to A, its
       picture mirrored, as the original draws it. */
    if ((info & 0x800) &&
        (s32)(p->ax - vx) * (p->bz - p->az) - (s32)(p->az - vz) * (p->bx - p->ax) <= 0) {
        s16 t = p->ax; p->ax = p->bx; p->bx = t;
        t = p->az; p->az = p->bz; p->bz = t;
    }
    to_view(x, z, &dm, &lm);
    if (dm < -32) return;
    if (lm - 32 > dm + 32 || lm + 32 < -dm - 32) return;
    n_pt++;
    p->decal = 1;
    p->spr = spr;
    p->hq = dm > 1 ? (u16)DIV_L16(HQ_NUM, dm) : 0xFFFF;
    /* projected now, once (the picture may be drawn a strip at a time):
       its view-space ends in ax .. bz, its columns in x0 .. x1 (none:
       x1 = x0) and its c in d (high half), l (low half) */
    if (!project(p->ax, p->az, p->bx, p->bz)) {
        p->x0 = p->x1 = 0;
        return;
    }
    p->ax = q_da;
    p->az = q_la;
    p->bx = q_db;
    p->bz = q_lb;
    p->x0 = q_x0;
    p->x1 = q_x1;
    p->d = (s16)(q_c >> 16);
    p->l = (s16)(u16)q_c;
}

/* sprite spr's canvas column c at screen column x, height from hq
 * Reads the column's opaque span from the pack (spr_cols: per sprite 64
 * pairs (top row, length); spr_coff: per sprite 64 u16 offsets of each
 * column's texels from the sprite's start), sets the ds_* variables and
 * calls the platform's draw_spr. Nothing for an empty column or a height
 * under half a pixel. The free-camera build counts the columns and stops
 * at FX_MAX_SC. */
#ifdef VIEW_FX
static u16 n_sc;                        /* sprite columns drawn (up to FX_MAX_SC) */
#endif
static void sprite_col(u8 x, u16 hq, u8 c, u8 spr)
{
#ifdef FX_DEBUG
    { extern int printf(const char *, ...); printf("col %d %u %d %d\n", x, hq, c, spr); }
#endif
    u8 top, len;
    u16 off;
#ifdef VIEW_FX
    u32 a = P.spr_cols + (u16)spr * 128 + (u16)c * 2;
    if (n_sc >= FX_MAX_SC) return;
    n_sc++;
    top = vd_u8(a);
    len = vd_u8(a + 1);
    if (!len) return;
    off = vd_u16(P.spr_coff + (u16)spr * 128 + (u16)c * 2);
#elif defined(__CC65__)
    u32 a = P.spr_cols + (u16)spr * 128 + (u16)c * 2;
    top = vd_u8(a);
    len = vd_u8(a + 1);
    if (!len) return;
    off = vd_u16(P.spr_coff + (u16)spr * 128 + (u16)c * 2);
#else
    if (sc_spr != spr) {                /* (another sprite: its table) */
        vd_words(P.spr_cols + (u16)spr * 128, sc_cols, 64);
        sc_spr = spr;
    }
    top = (u8)((u16)sc_cols[c] >> 8);
    len = (u8)sc_cols[c];
    if (!len) return;
    off = vd_u16(P.spr_coff + (u16)spr * 128 + (u16)c * 2);
#endif
    ds_x = x;
    ds_h = (hq >> 4) + ((hq & 15) >= 8);       /* ((u32)hq + 8) >> 4 */
    if (!ds_h) return;
    ds_data = spr_addr[spr] + off;
    ds_top = top;
    ds_len = len;
    ds_s = spr;
#ifdef VIEW_LOG
    if (vlog_n < 64) {
        vlog[vlog_n][0] = x;
        vlog[vlog_n][1] = ds_h;
        vlog[vlog_n][2] = c;
        vlog[vlog_n][3] = (u16)ds_data;
        vlog_n++;
    }
#endif
    draw_spr();                         /* the platform draws it */
}

/* draw_billboard: draws billboard p column by column, skipping the
 * columns where a wall is nearer (larger hq). The canvas column is
 * c = (2x + 1 - VIEW_W) d / VIEW_W - l + 32 (rounded down): where the
 * column's ray crosses the sprite's plane, from its left edge (0..63). */
static void draw_billboard(struct pthing *p)
{
    s16 x, c, x1 = p->x1 < strip_x1 ? p->x1 : strip_x1;
    s32 v, dv;
    x = p->x0 > strip_x0 ? p->x0 : strip_x0;    /* (its columns in the strip) */
    if (x >= x1) return;
    v = MUL_W(2 * x + 1 - VIEW_W, p->d);       /* (2x + 1 - VIEW_W) d, stepped by 2 d a column */
    dv = (s32)p->d * 2;
    for (; x < x1; x++, v += dv) {
        if (p->hq < col_hq[x]) continue;
        c = (s16)CANVAS_OF(v) - p->l + 32;
        if (c >= 0 && c <= 63) sprite_col((u8)x, p->hq, (u8)c, p->spr);
    }
}

/* draw_decal: draws decal p as a wall-like quad: projected and hq
 * stepped as a wall is, its canvas column from t (0..255 -> 0..63). A
 * column is drawn unless the wall there is nearer by more than 1/64 (so
 * the decal wins against the wall it sits on). */
static void draw_decal(struct pthing *p)
{
    s16 x, dd, dl;
    if (p->x1 <= p->x0 || p->x1 <= strip_x0 || p->x0 >= strip_x1) return;    /* (nothing in the strip) */
    q_da = p->ax;                       /* (the projection add_decal made) */
    q_la = p->az;
    q_db = p->bx;
    q_lb = p->bz;
    q_x0 = p->x0;
    q_x1 = p->x1;
    q_c = ((s32)p->d << 16) | (u16)p->l;
    dd = q_db - q_da;
    dl = q_lb - q_la;
    hq_ramp();
    for (x = q_x0; x < q_x1; x++, q_hq += q_step) {
        u16 h = (u16)(q_hq >> 8);
        u8 t8;
        if (x < strip_x0) continue;     /* (the strip's columns only) */
        if (x >= strip_x1) break;
        if (!h || (u32)h + (h >> 6) < col_hq[x]) continue;      /* decals win ties */
        t8 = t8_of(m2_at(x, q_da, q_la), num2_at(x, dd, dl));
        sprite_col((u8)x, h, t8 >> 2, p->spr);
    }
}

/* sort_things: pt_order = pt's indices by hq, smallest (farthest) first:
 * an insertion sort, stable (equal hq keep their order). */
static void sort_things(void)
{
    u8 i, j, v;
    for (i = 0; i < n_pt; i++) pt_order[i] = i;
    for (i = 1; i < n_pt; i++) {
        v = pt_order[i];
        j = i;
        while (j > 0 && pt[pt_order[j - 1]].hq > pt[v].hq) {
            pt_order[j] = pt_order[j - 1];
            j--;
        }
        pt_order[j] = v;
    }
}

/* build_vis: fills vis[] for view (cell * 4 + facing) from the pack:
 * vis_index gives the word offset of its list of cells in vis (0xFFFF
 * terminated); a vis_index of 0xFFFF means every cell. */
static void build_vis(u16 view)
{
    u16 at = vd_u16(P.vis_index + 2 * (u32)view), c;
    u8 i;
    if (at == 0xFFFF) {
        for (i = 0; i < 128; i++) vis[i] = 0xFF;
        return;
    }
    for (i = 0; i < 128; i++) vis[i] = 0;
    for (;;) {
        c = vd_u16(P.vis + 2 * (u32)at++);
        if (c == 0xFFFF) break;
        vis[c >> 3] |= 1 << (c & 7);
    }
}

#endif


#ifdef VIEW_FX
/* ------------------------------------------------------------ SuperFX
 * The SuperFX build's view: the GSU (snes/gsu/view.s) draws it; this C is
 * its reference (the build machine's pictures are compared with the GSU's
 * byte for byte). The same arithmetic as above, except: the camera is free
 * (any position, any of 256 angles: fwd and right are 8.8), so moving and
 * turning can be shown in between; every wall and thing of the map is
 * looked at (no pvs, no vis: a wall entirely past either edge of the view
 * is skipped before any division); up to FX_MAX_VW walls, FX_MAX_PT
 * things. The CPU makes the list of things (fx_th) and the doors' state
 * (fx_open), the GSU the rest.
 */
s16 fx_cam[6];                  /* vx, vz, fwd x, fwd z, right x, right z (8.8) */
/* the map's scenery as things (read once: the block is slow to read on the
   SNES), the static index that hides each (-1: none), the sprite per def */
#define FX_MAX_SCEN 128
struct fx_th fx_scen[FX_MAX_SCEN];
s16 fx_scen_hid[FX_MAX_SCEN];
u8 fx_nscen;
u8 fx_def_spr[256];
/* (fx_scen: x, z, sprite slot, info as in the pack's scenery table;
   fx_scen_hid: the cell number whose S.static_hidden bit takes it away;
   fx_def_spr: per def, its sprite slot) */

/* the walls in blocks of 4 x 4 cells (each block's walls together, their
   bounding box): the GSU looks only at the blocks that can be seen (the
   test is exact: a block left out has no wall that would show), so it sees
   the walls in this order, and so does this reference */
#ifndef __CC65__
u16 fx_worder[FX_MAX_WALLS];
u16 fx_nwalls;
struct fx_blk fx_blk[64];

/* fx_walls_load: sorts the map's walls (at most FX_MAX_WALLS) into the
 * 64 blocks (8 x 8 blocks of 4 x 4 cells, 256 world units square) by
 * their midpoint, and finds each block's bounding box. Out: fx_nwalls,
 * fx_blk[] (start, count, box), fx_worder (wall indices, block by
 * block, in wall order within a block). A counting sort: count, then
 * the starts, then place. */
static void fx_walls_load(void)
{
    static u8 blk_of[FX_MAX_WALLS];
    s16 w[4];
    u16 k, at = 0;
    u8 b;
    fx_nwalls = P.n_walls < FX_MAX_WALLS ? P.n_walls : FX_MAX_WALLS;
    for (b = 0; b < 64; b++) {          /* empty blocks, inside-out boxes */
        fx_blk[b].count = 0;
        fx_blk[b].x0 = fx_blk[b].z0 = 32767;
        fx_blk[b].x1 = fx_blk[b].z1 = -32767;
    }
    for (k = 0; k < fx_nwalls; k++) {
        s16 mx, mz;
        struct fx_blk *q;
        vd_words(P.walls + 16 * (u32)k, w, 4);     /* its ends: ax, az, bx, bz */
        /* the midpoint's block (256 units a block), kept to 0..7 */
        mx = (s16)((w[0] + w[2]) >> 1) >> 8;
        mz = (s16)((w[1] + w[3]) >> 1) >> 8;
        if (mx < 0) mx = 0;
        if (mx > 7) mx = 7;
        if (mz < 0) mz = 0;
        if (mz > 7) mz = 7;
        b = (u8)(mz * 8 + mx);
        blk_of[k] = b;
        q = &fx_blk[b];
        q->count++;
        if (w[0] < q->x0) q->x0 = w[0];
        if (w[2] < q->x0) q->x0 = w[2];
        if (w[0] > q->x1) q->x1 = w[0];
        if (w[2] > q->x1) q->x1 = w[2];
        if (w[1] < q->z0) q->z0 = w[1];
        if (w[3] < q->z0) q->z0 = w[3];
        if (w[1] > q->z1) q->z1 = w[1];
        if (w[3] > q->z1) q->z1 = w[3];
    }
    for (b = 0; b < 64; b++) {          /* each block's start in fx_worder */
        fx_blk[b].start = at;
        at += fx_blk[b].count;
        fx_blk[b].count = 0;
    }
    for (k = 0; k < fx_nwalls; k++) {
        struct fx_blk *q = &fx_blk[blk_of[k]];
        fx_worder[q->start + q->count++] = k;
    }
}

#endif

/* fx_scenery_load: reads the map's scenery (5 words each: x, z, sprite
 * slot, info, hiding cell or -1) into fx_scen / fx_scen_hid (at most
 * FX_MAX_SCEN) and the 256-byte def -> sprite table into fx_def_spr. */
static void fx_scenery_load(void)
{
    s16 s[5];
    u16 k;
    fx_nscen = 0;
    for (k = 0; k < P.n_scenery && fx_nscen < FX_MAX_SCEN; k++) {
        struct fx_th *t = &fx_scen[fx_nscen];
        vd_words(P.scenery + 10 * (u32)k, s, 5);
        t->x = s[0];
        t->z = s[1];
        t->spr = (u8)s[2];
        t->pad = 0;
        t->info = (u16)s[3];
        fx_scen_hid[fx_nscen++] = s[4];
    }
    for (k = 0; k < 256; k++) fx_def_spr[k] = vd_u8(P.def_spr + k);
}
u8 *fx_open;                    /* per lock: its flags (L_OPEN: the door is open) */
static u8 fx_swapped = 0xFF;    /* the portal's swaps applied (1), not (0), unknown (0xFF) */
/* this picture's things: the scenery not taken away, then each linked
   entity at its cell's centre (fx_nth of them) */
struct fx_th fx_th[FX_MAX_TH];
u8 fx_nth;
/* the list's rules (the same in snes/src/fx_list.s): a thing is kept only
   within fxl_lim - 1 cells (on x and on z) of the player's cell (fxl_px,
   fxl_pz); fxl_lim 256 keeps all. A linked entity that finds fx_th full
   sets fxl_over, and view_render_cam makes the list again nearer
   (FX_NEAR). Until 0.1.1 the list simply stopped when full: on the maps
   with more than 255 things (the UAC Base, levels 2, 4 and 5) the last
   entities were never drawn, however near. */
u16 fxl_px, fxl_pz, fxl_lim, fxl_over;
static const u8 FX_NEAR[4] = {12, 8, 5, 3};     /* the nearer lists' reach, in cells */

#ifdef __CC65__
void fx_list(void);                     /* snes/src/fx_list.s: fx_th / fx_nth by these rules */
#else
/* fx_dist_ok: world (x, z)'s cell within the list's reach */
static u8 fx_dist_ok(u16 cx, u16 cz)
{
    u16 dx = cx > fxl_px ? cx - fxl_px : fxl_px - cx, dz = cz > fxl_pz ? cz - fxl_pz : fxl_pz - cz;
    return dx < fxl_lim && dz < fxl_lim;
}

/* fx_list_c: fx_th / fx_nth as fx_list.s makes them on the SuperFX: the
   scenery not taken away, then each linked entity with a def, those
   within reach */
static void fx_list_c(void)
{
    u16 k;
    fxl_over = 0;
    fx_nth = 0;
    for (k = 0; k < fx_nscen; k++) {
        s16 h = fx_scen_hid[k];
        if (h >= 0 && (S.static_hidden[(u16)h >> 3] & (1 << (h & 7)))) continue;   /* taken away */
        if (!fx_dist_ok((u16)fx_scen[k].x >> 6, (u16)fx_scen[k].z >> 6)) continue;
        fx_th[fx_nth++] = fx_scen[k];
    }
    for (k = 0; k < n_ents; k++) {
        u8 d = S.e_def[k];
        struct fx_th *t;
        if (!(S.e_flags[k] & F_LINKED) || d == 0xFF) continue;
        if (!fx_dist_ok(S.e_x[k], S.e_z[k])) continue;
        if (fx_nth >= FX_MAX_TH) {
            fxl_over = 1;               /* (full: view_render_cam tries nearer) */
            break;
        }
        t = &fx_th[fx_nth++];
        t->x = (s16)S.e_x[k] * 64 + 32;
        t->z = (s16)S.e_z[k] * 64 + 32;
        t->spr = fx_def_spr[d];
        t->info = 0;
    }
}
#endif

/* sin(a * 2 pi / 256) * 256 */
/* (a quarter turn, 0..64; fx_sin folds the other quarters onto it) */
static const s16 SIN[64 + 1] = {
    0, 6, 13, 19, 25, 31, 38, 44, 50, 56, 62, 68, 74, 80, 86, 92, 98, 104, 109, 115, 121, 126, 132, 137,
    142, 147, 152, 157, 162, 167, 172, 177, 181, 185, 190, 194, 198, 202, 206, 209, 213, 216, 220, 223, 226,
    229, 231, 234, 237, 239, 241, 243, 245, 247, 248, 250, 251, 252, 253, 254, 255, 255, 256, 256, 256};

/* fx_sin: sin of angle a (256 a turn), x 256. */
static s16 fx_sin(u8 a)
{
    if (a < 64) return SIN[a];
    if (a < 128) return SIN[128 - a];
    if (a < 192) return -SIN[a - 128];
    return -SIN[256 - a];
}

/* view_camera: sets fx_cam for a camera standing at world (cx, cz),
 * angle a (0 north, 64 east: facing f is a = 64 f), placed CAM_BACK
 * (16) units behind that point as the fixed camera is. fwd and right
 * are 8.8 unit vectors. Called by view_render, or by the port for the
 * in-between pictures of a step or a turn. */
void view_camera(s16 cx, s16 cz, u8 a)
{
    s16 sn = fx_sin(a), cs = fx_sin((u8)(a + 64));
    fx_cam[2] = sn;             /* forward: facing 0 is -z, 64 is +x */
    fx_cam[3] = -cs;
    fx_cam[4] = cs;             /* right */
    fx_cam[5] = sn;
    fx_cam[0] = cx - (sn >> 4); /* 16 back */
    fx_cam[1] = cz + (cs >> 4);
}

#ifndef __CC65__
/* fx_view's result: depth and lateral offset (world units) */
static s16 fx_d, fx_l;

/* fx_view: world point (wx, wz) in the free camera's view space (the
 * 8.8 products shifted back to whole units) -> fx_d, fx_l. */
static void fx_view(s16 wx, s16 wz)
{
    s16 rx = wx - fx_cam[0], rz = wz - fx_cam[1];
    fx_d = (s16)(((s32)rx * fx_cam[2] + (s32)rz * fx_cam[3]) >> 8);
    fx_l = (s16)(((s32)rx * fx_cam[4] + (s32)rz * fx_cam[5]) >> 8);
}

/* the first column whose centre is right of the point (0..VIEW_W) */
/* (d > 0. Points outside the 90-degree view give 0 or VIEW_W without a
   division; inside, (HW + HW l / d) rounded as project() does) */
static s16 fx_col(s16 d, s16 l)
{
    s16 c;
    if (l >= d) return VIEW_W;          /* right of the view */
    if (l <= -d) return 0;              /* left of it */
    c = (s16)((((s32)HW << 8) + SX_NUM(l) / d + 127) >> 8);
    return c < 0 ? 0 : c;
}

/* the quad's ends in view space; 0: nothing of it can show (both behind,
   or entirely past one edge) - the GSU's first pass over the walls */
static u8 fx_quad_view(s16 ax, s16 az, s16 bx, s16 bz)
{
    fx_view(ax, az);
    q_da = fx_d;
    q_la = fx_l;
    fx_view(bx, bz);
    q_db = fx_d;
    q_lb = fx_l;
    if (q_da < NEAR && q_db < NEAR) return 0;
    if (q_la >= q_da && q_lb >= q_db) return 0;          /* past the right edge */
    if (q_la <= -q_da && q_lb <= -q_db) return 0;        /* past the left */
    return 1;
}

/* the rest of the projection (the second pass)
 * From the q_* ends fx_quad_view left: clips to NEAR, finds the columns
 * q_x0 .. q_x1 - 1 (fx_col) and q_c. Out: 0 if no column or c is 0. */
static u8 fx_quad_rest(void)
{
    s16 cda, cla, cdb, clb, xa, xb;
    cda = q_da;
    cla = q_la;
    cdb = q_db;
    clb = q_lb;
    if (cda < NEAR) {
        cla += (s16)(((s32)(q_lb - q_la) * (NEAR - q_da)) / (q_db - q_da));
        cda = NEAR;
    } else if (cdb < NEAR) {
        clb += (s16)(((s32)(q_la - q_lb) * (NEAR - q_db)) / (q_da - q_db));
        cdb = NEAR;
    }
    xa = fx_col(cda, cla);
    xb = fx_col(cdb, clb);
    if (xa > xb) {
        s16 t = xa;
        xa = xb;
        xb = t;
    }
    if (xb <= xa) return 0;
    q_x0 = xa;
    q_x1 = xb;
    q_c = MUL_W(q_lb - q_la, q_da) - MUL_W(q_db - q_da, q_la);
    return q_c != 0;
}

/* fx_project: both passes: the free camera's project(). */
static u8 fx_project(s16 ax, s16 az, s16 bx, s16 bz)
{
    return fx_quad_view(ax, az, bx, bz) && fx_quad_rest();
}

/* the free camera's kept walls (as vw[]), and one wall's 8 words */
static struct vwall fvw[FX_MAX_VW];
static s16 fwall[8];

#ifdef VIEW_FX_FAST
/* (a faster machine's C: the GBA's) the walls' words kept in RAM, and the
   blocks no wall of which can show left out - the same test as the GSU's
   (exact: a block is left out only when every point of its box is behind
   the camera, or past the same edge of the view, by fx_view's sums) */
static s16 fx_wcache[FX_MAX_WALLS][8];

/* fx_cache_load: copies every wall's 8 words into fx_wcache. */
static void fx_cache_load(void)
{
    u16 k;
    for (k = 0; k < fx_nwalls; k++) vd_words(P.walls + 16 * (u32)k, fx_wcache[k], 8);
}

/* fx_blk_out: 1 if block b can be left out: all 4 corners of its box
 * are behind the camera (depth under NEAR), or all right of the view
 * (L >= D), or all left of it (L + D < 1 unit). D and L are kept in 8.8
 * (not shifted back), so the test needs no division. */
static u8 fx_blk_out(const struct fx_blk *b)
{
    u8 behind = 1, right = 1, left = 1, i;
    for (i = 0; i < 4; i++) {           /* corner i: x from bit 0, z from bit 1 */
        s32 rx = (s32)((i & 1) ? b->x1 : b->x0) - fx_cam[0], rz = (s32)((i & 2) ? b->z1 : b->z0) - fx_cam[1];
        s32 D = rx * fx_cam[2] + rz * fx_cam[3], L = rx * fx_cam[4] + rz * fx_cam[5];
        if (D >= (s32)NEAR * 256) behind = 0;
        if (L - D < 0) right = 0;
        if (L + D >= 256) left = 0;
    }
    return behind | right | left;
}
#endif

/* fx_walls: the free camera's wall pass, as project_walls and
 * column_params together, over every wall of the map in fx_worder's
 * (block) order, with no pvs. It stops after FX_MAX_VW kept walls or
 * FX_MAX_CAND candidates. Lines 64 and up are not doors here (fx_open
 * has 64 entries). Out: col_hq, col_w, then col_h, col_t, col_tex. */
static void fx_walls(void)
{
    u16 k;
    s16 x;
    u8 n = 0;
    for (x = 0; x < VIEW_W; x++) {
        col_hq[x] = 0;
        col_w[x] = 0xFF;
    }
    u16 n_cand = 0;
#ifdef VIEW_FX_FAST
    u8 blk_i = 0;
    u16 blk_end = 0;
#endif
    for (k = 0; k < fx_nwalls && n < FX_MAX_VW && n_cand < FX_MAX_CAND; k++) {
        s16 *w = fwall, ax, az, bx, bz, ua, ub, t;
        struct vwall *v;
#ifdef VIEW_FX_FAST
        while (k >= blk_end && blk_i < 64) {           /* (the next block: its walls, or past them) */
            const struct fx_blk *b = &fx_blk[blk_i++];
            blk_end = b->start + b->count;
            if (b->count && fx_blk_out(b)) k = blk_end;
        }
        if (k >= fx_nwalls) break;
        w = fx_wcache[fx_worder[k]];
#else
        vd_words(P.walls + 16 * (u32)fx_worder[k], w, 8);
#endif
        if ((u8)w[5] < 64 && (fx_open[(u8)w[5]] & L_OPEN)) continue;     /* an open door */
        ax = w[0];
        az = w[1];
        bx = w[2];
        bz = w[3];
        ua = w[6] & 0xFF;
        ub = w[7];
        if ((s32)(ax - fx_cam[0]) * (bz - az) - (s32)(az - fx_cam[1]) * (bx - ax) <= 0) {   /* seen from behind */
            if (!(w[6] & 0x100)) continue;
            t = ax; ax = bx; bx = t;
            t = az; az = bz; bz = t;
            t = ua; ua = ub; ub = t;
        }
        if (!fx_project(ax, az, bx, bz)) continue;
#ifdef FX_DEBUG
        { extern int printf(const char *, ...); printf("vw %d: %d %d %d %d x %d-%d\n", n, q_da, q_la, q_db, q_lb, q_x0, q_x1); }
#endif
        v = &fvw[n];
        v->da = q_da;
        v->la = q_la;
        v->dd = q_db - q_da;
        v->dl = q_lb - q_la;
        v->tex = (u8)w[4];
        v->ua = (s8)ua;
        v->du = (s8)(ub - ua);
        hq_ramp();
        for (x = q_x0; x < q_x1; x++) {
            u16 h = (u16)(q_hq >> 8);
            if (h > col_hq[x]) {
                col_hq[x] = h;
                col_w[x] = n;
            }
            q_hq += q_step;
        }
        n++;
    }
    /* the columns' textures, as column_params */
    for (x = 0; x < VIEW_W; x++) {
        const struct vwall *w;
        u8 t8, t;
        s16 u;
        if (col_w[x] == 0xFF || col_hq[x] < 8) {
            col_h[x] = 0;
            continue;
        }
        w = &fvw[col_w[x]];
        t8 = t8_of(m2_at(x, w->da, w->la), num2_at(x, w->dd, w->dl));
        u = (w->ua + (((s16)t8 * w->du) >> 8)) & 63;
        t = tex_map[w->tex];
        col_h[x] = (u16)(((u32)col_hq[x] + 8) >> 4);
        col_t[x] = t;
        col_tex[x] = tex_addr[t] + (u16)u * 64;
    }
}

struct fpt {
    u16 hq;
    s16 d, l, x0, x1;
    u8 spr, decal;
    s16 ax, az, bx, bz;
};
/* (the free camera's things: the same fields as pthing; fpt_order far
   to near) */
static struct fpt fpt[FX_MAX_PT];
static u8 fpt_order[FX_MAX_PT];
static u8 n_fpt;


/* fx_billboard: add_billboard for the free camera: no vis test, but a
 * billboard hidden behind walls in every one of its columns is left
 * out (the walls are drawn first here). */
static void fx_billboard(s16 wx, s16 wz, u8 spr)
{
    s16 d, l, x0, x1, x;
    u16 hq;
    struct fpt *p;
    if (n_fpt >= FX_MAX_PT || spr >= V_MAX_SPR) return;
    fx_view(wx, wz);
    d = fx_d - 16;
    l = fx_l;
    if (d < 4) return;
    if (l - 32 > d || l + 32 < -d) return;
    x0 = (s16)(screen_x(d, l - 32) >> 8);
    x1 = (s16)((screen_x(d, l + 32) + 255) >> 8);
    if (x0 < 0) x0 = 0;
    if (x1 > VIEW_W) x1 = VIEW_W;
    if (x1 <= x0) return;
    hq = (u16)(HQ_NUM / d);
    for (x = x0; x < x1 && hq < col_hq[x]; x++)
        ;
    if (x == x1) return;                /* behind the walls everywhere */
    p = &fpt[n_fpt++];
    p->d = d;
    p->l = l;
    p->x0 = x0;
    p->x1 = x1;
    p->spr = spr;
    p->decal = 0;
    p->hq = hq;
}

/* fx_decal: add_decal for the free camera (the same quad and tests). */
static void fx_decal(s16 x, s16 z, u8 spr, u16 info)
{
    s16 sgn = (info & 0x200) ? -1 : 1;
    struct fpt *p;
    if (n_fpt >= FX_MAX_PT || spr >= V_MAX_SPR) return;
    p = &fpt[n_fpt];
    if (info & 0x08) { p->ax = x - 32 * sgn; p->az = z; p->bx = x + 32 * sgn; p->bz = z; }
    else if (info & 0x10) { p->ax = x + 32 * sgn; p->az = z; p->bx = x - 32 * sgn; p->bz = z; }
    else if (info & 0x40) { p->ax = x; p->az = z - 32 * sgn; p->bx = x; p->bz = z + 32 * sgn; }
    else { p->ax = x; p->az = z + 32 * sgn; p->bx = x; p->bz = z - 32 * sgn; }
    if ((info & 0x800) &&               /* two-sided, seen from behind: B to A (see add_decal) */
        (s32)(p->ax - fx_cam[0]) * (p->bz - p->az) - (s32)(p->az - fx_cam[1]) * (p->bx - p->ax) <= 0) {
        s16 t = p->ax; p->ax = p->bx; p->bx = t;
        t = p->az; p->az = p->bz; p->bz = t;
    }
    fx_view(x, z);
    if (fx_d < -32) return;
    if (fx_l - 32 > fx_d + 32 || fx_l + 32 < -fx_d - 32) return;
    n_fpt++;
    p->decal = 1;
    p->spr = spr;
    p->hq = fx_d > 1 ? (u16)(HQ_NUM / fx_d) : 0xFFFF;
}

/* fx_things: the free camera's things: each of fx_th[] becomes a decal
 * (info & 0x78: a wall-mounted sprite) or a billboard; they are sorted
 * far to near (insertion sort on hq, as sort_things) and drawn as
 * draw_billboard / draw_decal do. */
static void fx_things(void)
{
    u8 i, j, v;
    n_fpt = 0;
    n_sc = 0;
    for (i = 0; i < fx_nth; i++) {
        const struct fx_th *t = &fx_th[i];
        if (t->info & 0x78) fx_decal(t->x, t->z, t->spr, t->info);
        else fx_billboard(t->x, t->z, t->spr);
    }
    /* far to near */
    for (i = 0; i < n_fpt; i++) fpt_order[i] = i;
    for (i = 1; i < n_fpt; i++) {
        v = fpt_order[i];
        j = i;
        while (j > 0 && fpt[fpt_order[j - 1]].hq > fpt[v].hq) {
            fpt_order[j] = fpt_order[j - 1];
            j--;
        }
        fpt_order[j] = v;
    }
    for (i = 0; i < n_fpt; i++) {
        struct fpt *p = &fpt[fpt_order[i]];
        s16 x, c;
        if (!p->decal) {                /* a billboard */
            for (x = p->x0; x < p->x1; x++) {
                if (p->hq < col_hq[x]) continue;
                c = (s16)CANVAS_OF((s32)(2 * x + 1 - VIEW_W) * p->d) - p->l + 32;
                if (c >= 0 && c <= 63) sprite_col((u8)x, p->hq, (u8)c, p->spr);
            }
        } else {                        /* a decal (it wins ties, as in draw_decal) */
            s16 dd, dl;
            if (!fx_project(p->ax, p->az, p->bx, p->bz)) continue;
            dd = q_db - q_da;
            dl = q_lb - q_la;
            hq_ramp();
            for (x = q_x0; x < q_x1; x++, q_hq += q_step) {
                u16 h = (u16)(q_hq >> 8);
                if (!h || (u32)h + (h >> 6) < col_hq[x]) continue;
                sprite_col((u8)x, h, t8_of(m2_at(x, q_da, q_la), num2_at(x, dd, dl)) >> 2, p->spr);
            }
        }
    }
}

/* fx_draw: one free-camera picture from fx_cam, fx_th and fx_open: the
 * walls, the platform's draw_walls, then the things (draw_spr per
 * column). The C version, for the GBA and the build machine; on the
 * SNES the GSU's (snes/src/fx.c starts it). */
void fx_draw(void)
{
    fx_walls();                         /* col_* for every column */
    draw_walls();                       /* the platform draws them */
    fx_things();
#ifdef FX_DEBUG
    { extern int printf(const char *, ...); printf("nth %d npt %d\n", fx_nth, n_fpt); for (int q = 0; q < VIEW_W; q++) printf("hq %d %u %d\n", q, col_hq[q], col_w[q]); for (int q = 0; q < n_fpt; q++) { struct fpt *p = &fpt[fpt_order[q]]; printf("%d: hq %u d %d l %d x %d-%d spr %d decal %d\n", fpt_order[q], p->hq, p->d, p->l, p->x0, p->x1, p->spr, p->decal); } }
#endif
}
#endif
#endif

/* ------------------------------------------------------------ the view */
/* per facing (0 N, 1 E, 2 S, 3 W): the forward step in x and z */
static const s8 FX[4] = {0, 1, 0, -1};
static const s8 FZ[4] = {-1, 0, 1, 0};

#ifdef VIEW_FX
/* view_render (free-camera build): the camera at the player's cell
 * centre, facing S.facing, then view_render_cam. */
void view_render(void)
{
    view_camera((s16)S.px * 64 + 32, (s16)S.pz * 64 + 32, (u8)((S.facing & 3) << 6));
    view_render_cam();
}

/* view_render_cam: draws a picture from the camera view_camera set.
 * Loads the map if S.map changed, applies or undoes the portal's texture
 * swaps when S.couplings crosses 2, points fx_open at the doors' flags,
 * and makes the things list fx_th: on the SNES by snes/src/fx_list.s
 * (fx_list, from S's arrays), elsewhere here in C (the scenery not taken
 * away, then each linked entity with a def at its cell's centre). Then
 * fx_draw. */
void view_render_cam(void)
{
    u8 i;
#ifdef __CC65__
    extern u16 fx_prof[4];
    u16 ticks(void);
    fx_prof[3] = ticks();               /* (profiling: the picture's start time) */
#endif
    if (cur_map_loaded != S.map) {
        view_map_load(S.map);
        fx_swapped = 0xFF;              /* (forces the swaps to be set again) */
    }
    if (fx_swapped != (S.couplings >= 2)) {
        fx_swapped = S.couplings >= 2;
        for (i = 0; i < V_MAX_TEX; i++) tex_map[i] = i;
        if (fx_swapped)
            for (i = 0; i < P.n_swaps; i++) tex_map[P.swaps[2 * i]] = P.swaps[2 * i + 1];
    }
    fx_open = S.l_flags;                /* (the GSU looks at L_OPEN) */
#ifdef __CC65__
    {
        extern u8 *fxl_flags, *fxl_def, *fxl_x, *fxl_z, *fxl_defspr, *fxl_hidden;
        fxl_flags = S.e_flags;
        fxl_def = S.e_def;
        fxl_x = S.e_x;
        fxl_z = S.e_z;
        fxl_defspr = fx_def_spr;
        fxl_hidden = S.static_hidden;
    }
#endif
    /* the things: all of them, or (more than fx_th holds) the nearest */
    fxl_px = S.px;
    fxl_pz = S.pz;
    fxl_lim = 256;
    for (i = 0;; i++) {
#ifdef __CC65__
        fx_list();                      /* fills fx_th / fx_nth (snes/src/fx_list.s) */
#else
        fx_list_c();
#endif
        if (!fxl_over || i == sizeof FX_NEAR) break;
        fxl_lim = FX_NEAR[i] + 1;
    }
    fx_draw();
}
#else
/* view_render / view_prepare (fixed-camera build): draws the view from
 * the player's cell and facing (view_prepare: all but the drawing, which
 * view_draw does, a strip of columns at a time if wanted). Loads the map if S.map changed; sets the portal's
 * texture swaps; places the camera 16 units behind the cell's centre;
 * the walls (vcore.s's vc_walls on the SNES, else project_walls and
 * column_params) then draw_walls; then the things: the scenery not taken
 * away and every linked entity with a def, those in the view's vis
 * cells, sorted far to near and drawn (vcore.s's vc_things on the
 * SNES). PROF(n) mark the stages for profiling builds. */
#ifdef __CC65__
void view_render(void)
#else
void view_prepare(void)                 /* (everything but the drawing: view_draw draws) */
#endif
{
    u8 f = S.facing & 3, i;
    u16 view = (((u16)S.pz << 5) + S.px) * 4 + f, k;    /* this (cell, facing) */
    s16 s[5];
    if (cur_map_loaded != S.map) view_map_load(S.map);
    for (i = 0; i < V_MAX_TEX; i++) tex_map[i] = i;
    if (S.couplings >= 2)
        for (i = 0; i < P.n_swaps; i++) tex_map[P.swaps[2 * i]] = P.swaps[2 * i + 1];
    /* forward and right: right is forward turned a quarter clockwise */
    fwd_x = FX[f];
    fwd_z = FZ[f];
    rgt_x = -FZ[f];
    rgt_z = FX[f];
    vx = (s16)S.px * 64 + 32 - CAM_BACK * fwd_x;
    vz = (s16)S.pz * 64 + 32 - CAM_BACK * fwd_z;

#ifdef VIEW_LOG
    vlog_n = 0;
#endif
    PROF(0);
#ifdef __CC65__
    vc_vx = vx;
    vc_vz = vz;
    vc_f = f;
    vc_pvs = P.pvs + 2 * (u32)vd_u16(P.pvs_index + 2 * (u32)view);
    vc_walls_at = P.walls;
    vc_lflags = S.l_flags;
    vc_walls();                         /* vcore.s: col_hq ... col_tex */
    PROF(1);
    PROF(2);
    draw_walls();                       /* the platform draws the columns */
    PROF(3);
    {
        u16 at = vd_u16(P.vis_index + 2 * (u32)view);
        vc_vis = at == 0xFFFF ? 0xFFFFFFFFUL : P.vis + 2 * (u32)at;
    }
    vc_scenery = P.scenery;
    vc_nscen = P.n_scenery;
    vc_def_spr = P.def_spr;
    vc_spr_cols = P.spr_cols;
    vc_spr_coff = P.spr_coff;
    vc_hidden = S.static_hidden;
    vc_eflags = S.e_flags;
    vc_edef = S.e_def;
    vc_ex = S.e_x;
    vc_ez = S.e_z;
    vc_nents = n_ents;
    PROF(4);
    vc_things();                        /* vcore.s: lists, sorts and draws them */
    PROF(5);
#else
    project_walls(vd_u16(P.pvs_index + 2 * (u32)view));     /* this view's pvs list */
    PROF(1);
    column_params();
    PROF(2);
    build_vis(view);
    n_pt = 0;
    for (k = 0; k < P.n_scenery; k++) {
        /* (a prop that is not a decal, in a cell not seen: add_billboard
           would drop it - not read at all) */
        if (k < SCEN_CELLS) {
            u16 c = scen_cell[k];
            if (!(c & 0x8000) && !(vis[c >> 3] & (1 << (c & 7)))) continue;
        }
        /* x, z, sprite slot, info, hiding cell (-1: never hidden) */
        vd_words(P.scenery + 10 * (u32)k, s, 5);
        if (s[4] >= 0 && (S.static_hidden[(u16)s[4] >> 3] & (1 << (s[4] & 7)))) continue;
        if (s[3] & 0x78) add_decal(s[0], s[1], (u8)s[2], (u16)s[3]);
        else add_billboard(s[0], s[1], (u8)s[2]);
    }
    for (k = 0; k < n_ents; k++) {
        u8 fl = S.e_flags[k], d = S.e_def[k], x;
        if (!(fl & F_LINKED) || d == 0xFF) continue;
        x = S.e_x[k];
        {
            u16 c = ((u16)S.e_z[k] << 5) | x;   /* (its cell not seen: dropped by add_billboard anyway) */
            if (!(vis[c >> 3] & (1 << (c & 7)))) continue;
        }
        add_billboard((s16)x * 64 + 32, (s16)S.e_z[k] * 64 + 32, vd_u8(P.def_spr + d));   /* at its cell's centre */
    }
    sort_things();                      /* far to near */
    PROF(3);
#endif
}

#ifndef __CC65__
/* view_draw: the prepared view's columns x0 .. x1 - 1 (VIEW_W at most):
 * the walls (the platform's draw_walls, which draws strip_x0 ..
 * strip_x1 - 1), then the things far to near. Drawing the strips of a
 * view one after another gives the same picture as drawing it whole. */
void view_draw(u8 x0, u8 x1)
{
    u8 i;
    strip_x0 = x0;
    strip_x1 = x1;
    draw_walls();
    for (i = 0; i < n_pt; i++) {
        struct pthing *p = &pt[pt_order[i]];
        if (p->decal) draw_decal(p);
        else draw_billboard(p);
    }
}

/* view_render: the whole view (view_prepare, then view_draw of every column) */
void view_render(void)
{
    view_prepare();
    view_draw(0, VIEW_W);
    PROF(4);
}
#endif
#endif
