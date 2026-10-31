/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the Game Boy Color: the 3D view's drawing (view/view.h's
 * platform part) and the picture made into tiles. Bank 0: it reads the
 * textures and sprites from any ROM bank.
 *
 * The picture: 128 x 96 pixels (the view's 88 rows from row 4: the
 * horizon falls between two rows of tiles), a byte a pixel, column by
 * column (128 bytes apart), in the cartridge RAM's banks 4 (columns
 * 0..63) and 5, from $A000. A pixel is 0 (the floor or the ceiling: the
 * backdrop) or family * 4 + shade (shade 1..3; gb_res.py's per map
 * tables say which for each colour).
 *
 * The tiles (vd_tiles): 192 Game Boy tiles (16 x 12, column by column)
 * in bank 6 from $A000, then (VD_ATTR) the map's attributes, row by row:
 * per tile its palette, 2 + its family (most of a few of its pixels), + 3
 * below the horizon (palettes 2..4 have the ceiling as colour 0, 5..7 the
 * floor); then the same with VRAM bank 1's bit.
 *
 * The loops are in assembler (unrolled): a column's colours are looked up
 * into the high RAM (HV: $FFB3.., by texture row), from the remap there
 * (HR: $FF93.., 32 colours), then scaled down the column.
 *
 * The view's variables are in bank 3 (view_ram.h), in place while the
 * view runs; this switches to 4, 5 or 6 for a moment and back.
 *
 * Who calls what: view/view.c's view_render (run by gfx.c's gfx_view)
 * calls draw_walls once, then draw_spr for each sprite column, back to
 * front; gfx.c then calls vd_tiles. view.c's divisions and products come
 * here too (gb_div_l16, gb_div_u16, gb_mul_w: view_plat.h).
 *
 * The high RAM ($FF80-$FFFE, always fast to reach with ldh) holds:
 *   $FF93..$FFB2  HR: the remap in use (texture or sprite colour -> pixel
 *                 value), so a lookup is "ldh a, (c)" with c = $93 + colour
 *   $FFB3..$FFF2  HV: the column's 64 texture rows already remapped, so the
 *                 scaler's lookup is "ldh a, (c)" with c = $B3 + row
 *   $FFF3         tile8's row counter
 * Fixed point: the texture row v is 8.8 (row in the high byte); the
 * scalers keep its high byte as c = $B3 + row and its low byte in e.
 * Licence: GPL-2.0-or-later.
 */
#include <gb/gb.h>
#include <string.h>
#include "plat.h"
#include "../../view/view.h"
#include "../../view/vdata.h"
#include "view_ram.h"
#include "vdraw.h"
#include "view_plat.h"

#define FB_BANK 4                       /* the picture: cartridge RAM banks 4 (columns 0..63) and 5 */
#define TILE_BANK 6                     /* the tiles and their palettes */
#define VIEW_BANK 3                     /* view.c's variables (VR) */
#define FB_TOP 4                        /* the view's row 0 in the picture */
#define FB_COL(x) ((u8 *)0xA000 + (u16)((x) & 63) * 128)    /* column x's first byte (its bank mapped) */
#define HR ((u8 *)0xFF93)               /* the remap (32) */

u8 vd_conv_map = 0xFF;                  /* the map whose remaps hold pixel values */
static u8 buf[192];                     /* a texture column; a column of tiles */
/* the assembler routines' arguments (globals: the routines are __naked):
   vs_dst destination (or HV row), vs_v texture row (8.8), vs_dv its step
   per pixel (8.8), vs_src source address, vs_sp the saved stack pointer,
   vs_n a row count */
static u16 vs_dst, vs_v, vs_dv, vs_src, vs_sp;
static u8 vs_n;
static u8 hr_is = 0xFF;                 /* the remap in HR (texture slot, 0x80 + sprite) */
static u32 hv_tex = 0xFFFFFFFFUL;       /* the texture column whose colours HV holds */

void vd_conv_remaps(void) BANKED;     /* (vdraw_map.c) */

/* ---- the assembler */

/* 64 texture rows (vs_src, the ROM bank in place) -> HV through HR.
   In: vs_src. Out: HV[0..63] = HR[texel]. Clobbers a, c, de, hl */
static void lut64(void) __naked
{
    __asm
    ld  a, (_vs_src)
    ld  l, a
    ld  a, (_vs_src + 1)
    ld  h, a
    ld  de, #0xFF00 + 0xB3              /* (de: HV) */
    .rept 64
    ld  a, (hl+)                        /* (the texel: a colour 0..31) */
    add a, #0x93
    ld  c, a
    ldh a, (c)                          /* (HR[colour]: its pixel value) */
    ld  (de), a
    inc e
    .endm
    ret
    __endasm;
}

/* vs_n sprite rows (vs_src) -> HV from row vs_dst's low byte; 255 stays (a hole).
   In: vs_src, vs_dst (the first row), vs_n (1..64). Clobbers a, b, c, de, hl */
static void lut_spr(void) __naked
{
    __asm
    ld  a, (_vs_src)
    ld  l, a
    ld  a, (_vs_src + 1)
    ld  h, a
    ld  a, (_vs_dst)
    add a, #0xB3
    ld  e, a
    ld  d, #0xFF
    ld  a, (_vs_n)
    ld  b, a
1$:
    ld  a, (hl+)
    cp  #0xFF                           /* (a hole: kept as 255) */
    jr  z, 2$
    add a, #0x93
    ld  c, a
    ldh a, (c)
2$:
    ld  (de), a
    inc e
    dec b
    jr  nz, 1$
    ret
    __endasm;
}

/* the scaling: vs_n rows from vs_dst, texture row vs_v (8.8) stepping by vs_dv.
   The scalers' registers: e = v's fraction, c = $B3 + v's row (HV's
   address), d / b = dv's low / high byte, hl = the destination */
static void scale_setup(void) __naked
{
    __asm
    ld  a, (_vs_v)
    ld  e, a
    ld  a, (_vs_v + 1)
    add a, #0xB3
    ld  c, a
    ld  a, (_vs_dv)
    ld  d, a
    ld  a, (_vs_dv + 1)
    ld  b, a
    ld  a, (_vs_dst)
    ld  l, a
    ld  a, (_vs_dst + 1)
    ld  h, a
    ret
    __endasm;
}

/* a wall's column: vs_n (1..88) pixels from vs_dst, pixel = HV[v >> 8], v
   from vs_v stepping by vs_dv. An unrolled run of 88 bodies of 8 bytes;
   the entry is computed so exactly vs_n of them run: the address is
   pushed, scale_setup called, and the ret jumps there; the last body's ret
   returns to the caller. Clobbers a, bc, de, hl */
static void scale_wall(void) __naked
{
    __asm
    ld  a, (_vs_n)                      ; the way in: 88 - n bodies of 8 bytes skipped
    ld  b, a
    ld  a, #88
    sub b
    ld  l, a
    ld  h, #0
    add hl, hl
    add hl, hl
    add hl, hl
    ld  de, #wall_bodies
    add hl, de
    push hl                             /* (the entry: the ret below goes there) */
    call _scale_setup
    ret
wall_bodies:
    .rept 88                            /* (a body: the pixel stored, v += dv) */
    ldh a, (c)
    ld  (hl+), a
    ld  a, e
    add a, d
    ld  e, a
    ld  a, c
    adc a, b
    ld  c, a
    .endm
    ret
    __endasm;
}

/* a sprite's column: as scale_wall, but a 255 pixel (a hole) is not
   stored, leaving what is behind it. Bodies of 13 bytes. Clobbers a, bc,
   de, hl */
static void scale_spr(void) __naked
{
    __asm
    ld  a, (_vs_n)                      ; 13 bytes a body
    ld  b, a
    ld  a, #88
    sub b
    ld  l, a
    ld  h, #0
    ld  e, l
    ld  d, h
    add hl, hl
    add hl, de                          ; x3
    add hl, hl
    add hl, hl                          ; x12
    add hl, de                          ; x13
    ld  de, #spr_bodies
    add hl, de
    push hl
    call _scale_setup
    ret
spr_bodies:
    .rept 88
    ldh a, (c)
    inc a                               /* (255 + 1 = 0: a hole) */
    jr  z, .+4                          /* (skips the dec and the store) */
    dec a
    ld  (hl), a
    inc hl
    ld  a, e
    add a, d
    ld  e, a
    ld  a, c
    adc a, b
    ld  c, a
    .endm
    ret
    __endasm;
}

/* a tile: 8 columns (128 apart) of 8 rows from vs_src (its last row's,
   the first column's) -> 16 bytes ending at vs_dst (pushed: the stack
   pointer borrowed, the interrupts off meanwhile). Each pixel's bits 0, 1
   (its shade) go to the low and high bit planes; the family bits (2, 3)
   are left for the palette. The rows are done last to first, so push
   writes each row's two bytes (low plane first) below the previous.
   Clobbers a, bc, de, hl; $FFF3 */
static void tile8(void) __naked
{
    __asm
    di
    ld  (_vs_sp), sp
    ld  a, (_vs_dst)
    ld  l, a
    ld  a, (_vs_dst + 1)
    ld  h, a
    ld  sp, hl
    ld  a, (_vs_src)
    ld  l, a
    ld  a, (_vs_src + 1)
    ld  h, a
    ld  bc, #128                        /* (the next column, 128 bytes on) */
    ld  a, #8
1$:
    ldh (0xF3), a                       ; (the rows left: $FFF3, past HV)
    .rept 8                             /* (the row's 8 pixels, left to right) */
    ld  a, (hl)
    rrca                                /* (bit 0 into the low plane, e) */
    rl  e
    rrca                                /* (bit 1 into the high plane, d) */
    rl  d
    add hl, bc
    .endm
    push de                             /* (the row's two bytes) */
    ld  a, h
    sub #4                              /* (hl - 1024: back 8 columns) */
    ld  h, a
    dec hl                              /* (and up a row) */
    ldh a, (0xF3)
    dec a
    jr  nz, 1$
    ld  a, (_vs_sp)
    ld  l, a
    ld  a, (_vs_sp + 1)
    ld  h, a
    ld  sp, hl
    ei
    ret
    __endasm;
}

/* ---- s32 / s16 (view_plat.h): unsigned 32 / 16 by shifts and
   subtractions, 16 steps a half (the divisor at most $8000) */
static u16 gd_lo, gd_hi, gd_d;          /* the dividend's halves (then the quotient's), the divisor */

/* gd_hi:gd_lo / gd_d -> gd_hi:gd_lo (the remainder, in de, is dropped).
   Restoring division: each step shifts a dividend bit into the remainder
   de and, when de >= the divisor (bc), subtracts it and sets the quotient
   bit (inc l, into the bit just shifted out). A zero high half skips its
   16 steps. The divisor must be at most $8000 so de never overflows.
   Clobbers a, bc, de, hl */
static void div_u32_u16(void) __naked
{
    __asm
    ld  a, (_gd_d)
    ld  c, a
    ld  a, (_gd_d + 1)
    ld  b, a
    ld  de, #0
    ld  a, (_gd_hi)
    ld  l, a
    ld  a, (_gd_hi + 1)
    ld  h, a
    or  l
    jp  z, 1$                           /* (high half 0: its quotient bits are 0) */
    .rept 16
    add hl, hl
    rl  e
    rl  d
    ld  a, e
    sub c
    ld  a, d
    sbc a, b
    jr  c, .+7                          /* (de < divisor: skip the 5 bytes below) */
    ld  d, a
    ld  a, e
    sub c
    ld  e, a
    inc l
    .endm
    ld  a, l
    ld  (_gd_hi), a
    ld  a, h
    ld  (_gd_hi + 1), a
1$:
    ld  a, (_gd_lo)
    ld  l, a
    ld  a, (_gd_lo + 1)
    ld  h, a
    .rept 16
    add hl, hl
    rl  e
    rl  d
    ld  a, e
    sub c
    ld  a, d
    sbc a, b
    jr  c, .+7
    ld  d, a
    ld  a, e
    sub c
    ld  e, a
    inc l
    .endm
    ld  a, l
    ld  (_gd_lo), a
    ld  a, h
    ld  (_gd_lo + 1), a
    ret
    __endasm;
}

/* view_plat.h's DIV_L16: n / d, signed, truncated toward zero (as C's /),
   through the unsigned division of the magnitudes */
s32 gb_div_l16(s32 n, s16 d)
{
    u8 neg = 0;
    u32 un;
    if (n < 0) {
        un = (u32)0 - (u32)n;
        neg = 1;
    } else
        un = (u32)n;
    if (d < 0) {
        gd_d = (u16)0 - (u16)d;
        neg ^= 1;
    } else
        gd_d = (u16)d;
    gd_lo = (u16)un;
    gd_hi = (u16)(un >> 16);
    div_u32_u16();                      /* (|n| / |d|) */
    un = ((u32)gd_hi << 16) | gd_lo;
    return neg ? (s32)((u32)0 - un) : (s32)un;
}

/* view_plat.h's DIV_T8: (m << 8) / n for m < n (the quotient fits 8
   bits): 8 steps of restoring division, the remainder in a (its ninth
   bit in the carry), the quotient shifted into e */
static u8 d8_m, d8_n, d8_q;
static void div8(void) __naked
{
    __asm
    ld  a, (_d8_n)
    ld  b, a
    ld  a, (_d8_m)
    ld  e, #0
    sla e
    add a, a                            ; (r = 2 r: a carry means r >= 256 > n)
    jr  c, 1$
    cp  b
    jr  c, 2$
1$:
    sub b
    inc e
2$:
    sla e
    add a, a                            ; (r = 2 r: a carry means r >= 256 > n)
    jr  c, 3$
    cp  b
    jr  c, 4$
3$:
    sub b
    inc e
4$:
    sla e
    add a, a                            ; (r = 2 r: a carry means r >= 256 > n)
    jr  c, 5$
    cp  b
    jr  c, 6$
5$:
    sub b
    inc e
6$:
    sla e
    add a, a                            ; (r = 2 r: a carry means r >= 256 > n)
    jr  c, 7$
    cp  b
    jr  c, 8$
7$:
    sub b
    inc e
8$:
    sla e
    add a, a                            ; (r = 2 r: a carry means r >= 256 > n)
    jr  c, 9$
    cp  b
    jr  c, 10$
9$:
    sub b
    inc e
10$:
    sla e
    add a, a                            ; (r = 2 r: a carry means r >= 256 > n)
    jr  c, 11$
    cp  b
    jr  c, 12$
11$:
    sub b
    inc e
12$:
    sla e
    add a, a                            ; (r = 2 r: a carry means r >= 256 > n)
    jr  c, 13$
    cp  b
    jr  c, 14$
13$:
    sub b
    inc e
14$:
    sla e
    add a, a                            ; (r = 2 r: a carry means r >= 256 > n)
    jr  c, 15$
    cp  b
    jr  c, 16$
15$:
    sub b
    inc e
16$:
    ld  a, e
    ld  (_d8_q), a
    ret
    __endasm;
}

u8 gb_div8(u8 m, u8 n)
{
    d8_m = m;
    d8_n = n;
    div8();
    return d8_q;
}

/* view_plat.h's DIV_U16: a / b */
u16 gb_div_u16(u16 a, u16 b)          /* (b at most $8000) */
{
    gd_hi = 0;
    gd_lo = a;
    gd_d = b;
    div_u32_u16();                      /* (a 16-bit dividend: the high half's steps skipped) */
    return gd_lo;
}

/* ---- s16 * s16 (view_plat.h): unsigned 16 x 16 -> 32, shift and add */
static u16 gm_a, gm_b, gm_lo, gm_hi;    /* the factors; the product's halves */

/* gm_a * gm_b -> gm_hi:gm_lo. Shift and add from the right: 16 times, if
   the multiplier's low bit (c) is set, hl += gm_b; then the 33 bits
   carry:hl:bc shift right, the product's low bits entering bc as the
   multiplier's bits leave. Clobbers a, bc, de, hl */
static void mul_u16(void) __naked
{
    __asm
    ld  a, (_gm_a)
    ld  c, a
    ld  a, (_gm_a + 1)
    ld  b, a
    ld  a, (_gm_b)
    ld  e, a
    ld  a, (_gm_b + 1)
    ld  d, a
    ld  hl, #0
    or  a
    .rept 16
    bit 0, c
    jr  z, .+5                          /* (bit clear: to the or, carry 0) */
    add hl, de                          /* (bit set: add; its carry shifts in) */
    jr  .+3
    or  a
    rr  h
    rr  l
    rr  b
    rr  c
    .endm
    ld  a, c
    ld  (_gm_lo), a
    ld  a, b
    ld  (_gm_lo + 1), a
    ld  a, l
    ld  (_gm_hi), a
    ld  a, h
    ld  (_gm_hi + 1), a
    ret
    __endasm;
}

/* view_plat.h's MUL_W: a * b, signed, 32 bits */
s32 gb_mul_w(s16 a, s16 b)
{
    u8 neg = 0;
    u32 r;
    if (a < 0) {
        gm_a = (u16)0 - (u16)a;
        neg = 1;
    } else
        gm_a = (u16)a;
    if (b < 0) {
        gm_b = (u16)0 - (u16)b;
        neg ^= 1;
    } else
        gm_b = (u16)b;
    mul_u16();                          /* (|a| * |b|) */
    r = ((u32)gm_hi << 16) | gm_lo;
    return neg ? (s32)((u32)0 - r) : (s32)r;
}

/* ---- the columns */
/* column x's view row 0 in the picture, its bank (4 or 5) mapped: the
   view's RAM (bank 3) is not in place until SWITCH_RAM(VIEW_BANK) */
static u8 *col_at(u8 x)
{
    SWITCH_RAM(FB_BANK + (x >> 6));
    return FB_COL(x) + FB_TOP;
}

/* the texture's (or sprite's) bytes at block offset a, n of them: from
   the ROM where they are (vs_src, its bank in place), or copied when they
   cross a bank */
static void src_at(u32 a, u8 n)
{
    if (((u16)a & 0x3FFF) > 0x4000 - n) {   /* (crosses into the next bank) */
        far_copy(a, buf, n);
        vs_src = (u16)buf;
    } else {
        SWITCH_ROM((u8)(DATA_BANK0 + (u8)(a >> 14)));     /* (left mapped: the caller switches back) */
        vs_src = 0x4000 + ((u16)a & 0x3FFF);
    }
}

/* draw_walls' state for dw_cols (globals: the routine is __naked):
   dw_x, dw_x1 the columns left (dw_x .. dw_x1 - 1); dw_ph, dw_pt, dw_ptex,
   dw_rm the addresses of VR.col_h, col_t, col_tex and tex_rm (bank 3);
   dw_b the ROM bank to put back; dw_h the column's height, dw_q its
   (7 h + 8) >> 4, dw_k0 the rows above the view's top, dw_y0 / dw_y1 the
   view rows the wall covers, dw_tex its texture column's block offset */
static u8 dw_x, dw_x1, dw_b, dw_y0, dw_y1;
static u16 dw_ph, dw_pt, dw_ptex, dw_rm, dw_h, dw_q, dw_k0;
static u32 dw_tex;

/* the texture column hv_tex when its 64 bytes cross into the next bank:
   copied (src_at), and its ROM bank mapped otherwise. Called by dw_cols */
static void dw_far(void)
{
    src_at(hv_tex, 64);
}

/* the columns dw_x .. dw_x1 - 1 into the picture: as the C this replaced
   (kept in the comments), a column at a time:
     h = col_h[x]; no wall: the whole column 0
     t = col_t[x]; another texture: its remap into HR (and HV forgotten)
     another texture column: its 64 texels remapped into HV (lut64)
     y0 = 44 - (7 h + 8) / 16, dv = 16384 / h, k0 = -y0 if y0 < 0 (the
     rows above the view skipped), y1 = min(y0 + h, 88), v = dv / 2 + k0 dv;
     rows 0 .. y0 - 1 and y1 .. 87 the backdrop (0), the rest scaled
   Entered with bank 3 (VR) mapped; leaves it so, the ROM bank dw_b. */
static void dw_cols(void) __naked
{
    __asm
00$:
    ld  a, (_dw_x)
    ld  c, a                            ; (c = x)
    ld  a, (_dw_x1)
    cp  c
    jp  z, 99$                          ; (every column done)
    ld  a, (_dw_ph)                     ; h = col_h[x]
    ld  l, a
    ld  a, (_dw_ph + 1)
    ld  h, a
    ld  e, c
    ld  d, #0
    add hl, de
    add hl, de
    ld  a, (hl+)
    ld  (_dw_h), a
    ld  b, a
    ld  a, (hl)
    ld  (_dw_h + 1), a
    or  b
    jp  z, 50$                          ; (no wall)
    ld  a, (_dw_pt)                     ; t = col_t[x]
    ld  l, a
    ld  a, (_dw_pt + 1)
    ld  h, a
    add hl, de
    ld  b, (hl)
    ld  a, (_hr_is)
    cp  b
    jr  z, 10$
    ld  a, b                            ; another texture: HR = tex_rm[t]
    ld  (_hr_is), a
    ld  l, b
    ld  h, #0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    ld  a, (_dw_rm)
    ld  e, a
    ld  a, (_dw_rm + 1)
    ld  d, a
    add hl, de
    ld  de, #0xFF93
    ld  b, #32
1$:
    ld  a, (hl+)
    ld  (de), a
    inc e
    dec b
    jr  nz, 1$
    ld  a, #0xFF                        ; (HV's texture column: none)
    ld  (_hv_tex), a
    ld  (_hv_tex + 1), a
    ld  (_hv_tex + 2), a
    ld  (_hv_tex + 3), a
10$:
    ld  a, (_dw_ptex)                   ; dw_tex = col_tex[x]
    ld  l, a
    ld  a, (_dw_ptex + 1)
    ld  h, a
    ld  e, c
    ld  d, #0
    sla e
    rl  d
    sla e
    rl  d
    add hl, de
    ld  de, #_dw_tex
    ld  b, #4
2$:
    ld  a, (hl+)
    ld  (de), a
    inc de
    dec b
    jr  nz, 2$
    ld  hl, #_dw_tex                    ; the same as HV holds?
    ld  de, #_hv_tex
    ld  b, #4
3$:
    ld  a, (de)
    cp  (hl)
    jr  nz, 4$
    inc hl
    inc de
    dec b
    jr  nz, 3$
    jr  20$                             ; (yes: HV kept)
4$:
    ld  hl, #_dw_tex                    ; hv_tex = dw_tex
    ld  de, #_hv_tex
    ld  b, #4
5$:
    ld  a, (hl+)
    ld  (de), a
    inc de
    dec b
    jr  nz, 5$
    ld  a, (_dw_tex + 1)                ; its 64 bytes cross a bank? ((a & $3FFF) > $3FC0)
    and #0x3F
    cp  #0x3F
    jr  nz, 6$
    ld  a, (_dw_tex)
    cp  #0xC1
    jr  c, 6$
    call _dw_far                        ; (yes: copied)
    jr  7$
6$:
    ld  a, (_dw_tex + 1)                ; the bank: data_bank0 + (a >> 14)
    rlca
    rlca
    and #3
    ld  b, a
    ld  a, (_dw_tex + 2)
    add a, a
    add a, a
    or  b
    ld  b, a
    ld  a, (_data_bank0)
    add a, b
    ldh (__current_bank), a
    ld  (#_rROMB0), a
    ld  a, (_dw_tex)                    ; vs_src = $4000 + (a & $3FFF)
    ld  (_vs_src), a
    ld  a, (_dw_tex + 1)
    and #0x3F
    or  #0x40
    ld  (_vs_src + 1), a
7$:
    call _lut64                         ; (the 64 texels remapped into HV)
    ld  a, (_dw_b)                      ; (the ROM bank back)
    ldh (__current_bank), a
    ld  (#_rROMB0), a
20$:
    ld  a, (_dw_h)                      ; q = (7 h + 8) >> 4
    ld  l, a
    ld  a, (_dw_h + 1)
    ld  h, a
    ld  d, h
    ld  e, l
    add hl, hl
    add hl, hl
    add hl, hl
    ld  a, l
    sub e
    ld  l, a
    ld  a, h
    sbc a, d
    ld  h, a
    ld  de, #8
    add hl, de
    srl h
    rr  l
    srl h
    rr  l
    srl h
    rr  l
    srl h
    rr  l
    ld  a, l
    ld  (_dw_q), a
    ld  a, h
    ld  (_dw_q + 1), a
    or  a                               ; q <= 44: y0 = 44 - q, k0 = 0
    jr  nz, 21$
    ld  a, l
    cp  #45
    jr  nc, 21$
    ld  b, a
    ld  a, #44
    sub b
    ld  (_dw_y0), a
    xor a
    ld  (_dw_k0), a
    ld  (_dw_k0 + 1), a
    jr  22$
21$:
    ld  a, l                            ; else y0 = 0, k0 = q - 44
    sub #44
    ld  (_dw_k0), a
    ld  a, h
    sbc a, #0
    ld  (_dw_k0 + 1), a
    xor a
    ld  (_dw_y0), a
22$:
    ld  a, (_dw_h)                      ; y1 = min(h - q + 44, 88)
    ld  l, a
    ld  a, (_dw_h + 1)
    ld  h, a
    ld  a, (_dw_q)
    ld  e, a
    ld  a, (_dw_q + 1)
    ld  d, a
    ld  a, l
    sub e
    ld  l, a
    ld  a, h
    sbc a, d
    ld  h, a
    ld  de, #44
    add hl, de
    ld  a, h
    or  a
    jr  nz, 23$
    ld  a, l
    cp  #89
    jr  c, 24$
23$:
    ld  a, #88
24$:
    ld  (_dw_y1), a
    xor a                               ; dv = 16384 / h (div_u32_u16: gd_lo)
    ld  (_gd_hi), a
    ld  (_gd_hi + 1), a
    ld  (_gd_lo), a
    ld  a, #0x40
    ld  (_gd_lo + 1), a
    ld  a, (_dw_h)
    ld  (_gd_d), a
    ld  a, (_dw_h + 1)
    ld  (_gd_d + 1), a
    call _div_u32_u16
    ld  a, (_gd_lo)
    ld  (_vs_dv), a
    ld  a, (_gd_lo + 1)
    ld  (_vs_dv + 1), a
    ld  a, (_dw_k0)                     ; v = dv / 2 + k0 dv
    ld  b, a
    ld  a, (_dw_k0 + 1)
    or  b
    jr  nz, 25$
    ld  hl, #0
    jr  26$
25$:
    ld  a, (_dw_k0)
    ld  (_gm_a), a
    ld  a, (_dw_k0 + 1)
    ld  (_gm_a + 1), a
    ld  a, (_vs_dv)
    ld  (_gm_b), a
    ld  a, (_vs_dv + 1)
    ld  (_gm_b + 1), a
    call _mul_u16
    ld  a, (_gm_lo)
    ld  l, a
    ld  a, (_gm_lo + 1)
    ld  h, a
26$:
    ld  a, (_vs_dv + 1)
    srl a
    ld  d, a
    ld  a, (_vs_dv)
    rra
    ld  e, a
    add hl, de
    ld  a, l
    ld  (_vs_v), a
    ld  a, h
    ld  (_vs_v + 1), a
    call 60$                            ; (the picture's bank; hl = the column's row 0)
    ld  a, (_dw_y0)                     ; rows 0 .. y0 - 1: the backdrop
    or  a
    jr  z, 28$
    ld  b, a
    xor a
27$:
    ld  (hl+), a
    dec b
    jr  nz, 27$
28$:
    ld  a, l                            ; vs_dst = row y0 (hl)
    ld  (_vs_dst), a
    ld  a, h
    ld  (_vs_dst + 1), a
    ld  a, (_dw_y0)
    ld  b, a
    ld  a, (_dw_y1)
    sub b                               ; vs_n = y1 - y0
    ld  (_vs_n), a
    ld  e, a                            ; (rows y1 .. 87: the backdrop, after the wall)
    ld  d, #0
    add hl, de
    ld  a, (_dw_y1)
    ld  b, a
    ld  a, #88
    sub b
    jr  z, 30$
    ld  b, a
    xor a
29$:
    ld  (hl+), a
    dec b
    jr  nz, 29$
30$:
    ld  a, (_vs_n)
    or  a
    call nz, _scale_wall                ; (the wall's pixels)
    jr  51$
50$:                                    ; no wall: the whole column 0
    call 60$
    ld  b, #88
    xor a
52$:
    ld  (hl+), a
    dec b
    jr  nz, 52$
51$:
    ld  a, #3                           ; (VR back for the next column)
    ld  (#_rRAMB), a
    ld  hl, #_dw_x
    inc (hl)
    jp  00$
60$:                                    ; the picture's bank 4 + x / 64 mapped; hl = column x's row 0
    ld  a, (_dw_x)
    ld  c, a
    rlca
    rlca
    and #1
    add a, #4
    ld  (#_rRAMB), a
    ld  a, c
    and #63
    srl a                               ; (x & 63) * 128 + $A004
    ld  h, a
    ld  a, #0
    rra
    or  #4
    ld  l, a
    ld  a, h
    add a, #0xA0
    ld  h, a
    ret
99$:
    ret
    __endasm;
}

/* view.h's hook: the walls of view_draw's columns (VR.strip_x0 ..
   strip_x1 - 1; VR.col_h, col_t, col_tex, computed by view.c) into the
   picture, the rest of each column the backdrop (0) (dw_cols). A new
   map's remaps are made into pixel values first. Runs with bank 3 mapped
   and leaves it so; the ROM bank is restored */
void draw_walls(void)
{
    if (vd_conv_map != (u8)VR.cur_map_loaded) {
        vd_conv_remaps();               /* (vdraw_map.c: the remaps -> pixel values) */
        hr_is = 0xFF;                   /* (HR's copy is stale) */
    }
    dw_x = VR.strip_x0;
    dw_x1 = VR.strip_x1;
    dw_b = _current_bank;
    dw_ph = (u16)VR.col_h;
    dw_pt = (u16)VR.col_t;
    dw_ptex = (u16)VR.col_tex;
    dw_rm = (u16)VR.tex_rm;
    dw_cols();
}

/* view.h's hook: one sprite column (VR.ds_*: screen column ds_x, height
   ds_h, data rows ds_top .. ds_top + ds_len - 1 at block offset ds_data,
   remap spr_rm[ds_s]) over the picture; 255 texels are holes. Called
   after draw_walls, back to front. Runs with bank 3 mapped */
static u16 spr_h, spr_dv = 0;          /* the last sprite height, 16384 / it */
void draw_spr(void)
{
    u8 b = _current_bank, s = VR.ds_s, top = VR.ds_top, n = VR.ds_len, end;
    u16 h = VR.ds_h, dv, v;
    s16 y0, y, y1;
    if (n > 64) n = 64;                 /* (HV holds 64 rows) */
    end = top + n;
    if (hr_is != 0x80 + s) {
        memcpy(HR, VR.spr_rm[s], 16);
        hr_is = 0x80 + s;
    }
    hv_tex = 0xFFFFFFFFUL;              /* (HV is about to hold the sprite's rows) */
    src_at(VR.ds_data, n);
    vs_dst = top;
    vs_n = n;
    lut_spr();                          /* (HV[top..end - 1] = the remapped rows) */
    SWITCH_ROM(b);
    /* the rows whose texture row is top .. end - 1: v(y) = dv / 2 + (y - y0) dv */
    y0 = VIEW_HALF_H - (s16)((h * 7 + 8) >> 4);
    if (h != spr_h) {                   /* (a billboard's columns share their height) */
        spr_h = h;
        spr_dv = gb_div_u16(16384, h);
    }
    dv = spr_dv;
    {
        u16 a = (u16)top << 8, b = (u16)end << 8, h2 = dv >> 1;
        s16 k = a > h2 ? (s16)gb_div_u16(a - h2 + dv - 1, dv) : 0;   /* the first y - y0 at top */
        s16 m = (s16)gb_div_u16(b - h2 + dv - 1, dv);                 /* ... past end - 1 */
        y = y0 + k;
        y1 = y0 + m;
        if (y1 > y0 + (s16)h) y1 = y0 + (s16)h;
    }
    if (y < 0) y = 0;
    if (y1 > VIEW_H) y1 = VIEW_H;
    v = (dv >> 1) + (u16)(y - y0) * dv;
    vs_v = v;
    vs_dv = dv;
    if (y >= y1) return;
    vs_dst = (u16)(col_at(VR.ds_x) + y);
    vs_n = (u8)(y1 - y);
    scale_spr();                        /* (the sprite's pixels, holes skipped) */
    SWITCH_RAM(VIEW_BANK);
}

/* the picture's rows 0..3 and 92..95: the backdrop, once (draw_walls only
   writes rows 4..91). Called by gfx.c's init_common; leaves bank 1 mapped */
void vd_init(void)
{
    u8 b, x;
    for (b = 0; b < 2; b++) {
        SWITCH_RAM(FB_BANK + b);
        for (x = 0; x < 64; x++) {
            u8 *c = FB_COL(x);
            memset(c, 0, FB_TOP);
            memset(c + FB_TOP + VIEW_H, 0, 96 - FB_TOP - VIEW_H);
        }
    }
    SWITCH_RAM(1);
}

/* ---- the picture -> tiles (bank 6) */
/* the tile's colour family (0..2): a tile has one palette, so its pixels
   can only come from one family. Picked from five fixed pixels (t[col *
   128 + row]); backdrop pixels (0) do not count; ties go to the lower
   family */
static u8 fam_of(const u8 *t)        /* t: the tile's first column, first row */
{
    /* five of its pixels: the family most of them have */
    u8 cnt[3], v;
    cnt[0] = cnt[1] = cnt[2] = 0;
    if ((v = t[1 * 128 + 1])) cnt[v >> 2]++;
    if ((v = t[6 * 128 + 2])) cnt[v >> 2]++;
    if ((v = t[3 * 128 + 4])) cnt[v >> 2]++;
    if ((v = t[5 * 128 + 6])) cnt[v >> 2]++;
    if ((v = t[2 * 128 + 6])) cnt[v >> 2]++;
    return cnt[1] > cnt[0] ? (cnt[2] > cnt[1] ? 2 : 1) : (cnt[2] > cnt[0] ? 2 : 0);
}

/* the picture (banks 4, 5) -> 192 tiles in bank 6 from $A000, column of
   tiles by column of tiles (tile tx * 12 + ty), then at VD_ATTR the BG
   attributes for $9800 (row by row, 16 a row) and 192 bytes on the same
   with bit 3 (VRAM bank 1) for $9C00. Called by gfx.c's gfx_view; leaves
   bank 3 mapped */
void vd_tiles(void)
{
    u8 tx;
    for (tx = 0; tx < 16; tx++) vd_tiles_col(tx);
}

/* vd_tiles for column of tiles tx (0..15) alone: its 12 tiles at
   $A000 + tx * 192 in bank 6 and its attributes; leaves bank 3 mapped */
void vd_tiles_col(u8 tx)
{
    u8 ty, f[12];
    u8 *c0;
    SWITCH_RAM(FB_BANK + (tx >> 3));
    c0 = FB_COL(tx * 8);
    for (ty = 0; ty < 12; ty++) {
        vs_src = (u16)(c0 + ty * 8 + 7);    /* (the tile's last row, first column) */
        vs_dst = (u16)(buf + ty * 16 + 16); /* (the end of its 16 bytes in buf) */
        tile8();                        /* (8 x 8 pixels -> a Game Boy tile) */
        f[ty] = fam_of(c0 + ty * 8);
    }
    SWITCH_RAM(TILE_BANK);
    memcpy((u8 *)0xA000 + (u16)tx * 192, buf, 192);
    for (ty = 0; ty < 12; ty++) {
        u8 *a = (u8 *)0xA000 + VD_ATTR + ty * 16 + tx;
        a[0] = 2 + f[ty] + (ty >= 6 ? 3 : 0);   /* (tile rows 6..11 are below the horizon: picture row 48) */
        a[192] = a[0] | 0x08;           /* (the second picture's: VRAM bank 1) */
    }
    SWITCH_RAM(VIEW_BANK);
}
