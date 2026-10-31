| Deimos - Doom RPG on retro consoles
| Copyright (C) 2026 unfnknblvbl and contributors
| SPDX-License-Identifier: GPL-2.0-or-later
| Free software under the GNU General Public License, version 2 or (at your
| option) any later version, with NO WARRANTY: see the LICENSE file.

| The Mega Drive game - the live first-person renderer, in 68000 assembler.
|
| Draws the view the way the original J2ME engine does: camera 16 units
| behind the cell centre, eye 36 units up, walls 64 high, 90 degree field
| of view (focal length = half the view width), perspective-correct
| textures, billboards and wall decals. The floor and ceiling are left
| transparent (colour 0): plane B behind the view holds their shading
| (src/backdrop.c).
|
|  1. visibility: the walls that can be seen from this cell and facing
|     (worked out offline, tools/build_render_data.py) are projected; each
|     screen column keeps the nearest one (a 1/z buffer, col_iz)
|  2. things (props, decals, monsters, items) are projected
|  3. the 15 colours of this view are chosen from what is actually on
|     screen (weighted by area), and every texture and sprite gets a remap
|     into them (render_pal.s)
|     [Now out of date: render_pal.s is gone. Today src/area.c picks the
|     area's three 15-colour palettes (area_select, called below) and
|     fills the remap tables tex_hi3 ... ; this file then gives each 8x8
|     tile of the view one of the three palettes (tile_pals) and draws
|     its pixels through that palette's remap.]
|  4. the frame buffer (VDP tiles in column order: one byte = two pixels,
|     high nibble left; one screen row down = +4 bytes; COLBYTES = 512
|     bytes per 8-pixel tile column) is cleared, walls are drawn a
|     byte-column at a time (compiled scalers, tools/gen_scalers.py, or
|     the span loops),
|     things on top, far to near
|  5. one DMA into the hidden VRAM buffer; the frame interrupt flips the
|     plane A name table and loads the palette together.
|
| Detail (cstep): every column (1), or every second column drawn two
| pixels wide (2: low detail and the in-between pictures of smooth
| movement).
|
| The view is VIEW_W x VIEW_H = 208 x 128 pixels (it was 256 x 160 until
| 0.1.0; md.h places it on the screen). The focal length F = HALF_W = 104
| is half the width, so the field of view is still 90 degrees, and the
| horizon is row HALF_H = 64: the same picture as before, at 80% size.
|
| Geometry, for a wall from A to B in view space (depth d, lateral l) and
| the screen column x (k = (x - F) / F is the ray's slope):
|   num(x) = F (lb - la) - (x - F) (db - da)
|   c      = (lb - la) da - (db - da) la
|   depth(x) = F c / num(x)              (1/depth is linear in x)
|   u(x)   = 64 m(x) / num(x),  m(x) = (x - F) da - F la
| A wall 64 units high at depth d is 64 F / d pixels high, its top 28/64
| of that above the horizon (the eye is 36 units up); a screen row y is
| the canvas row 28 + (y + 0.5 - HALF_H) d / F of a billboard at depth d.
|
| The arithmetic (divisions, rounding, clamps) was that of the C version
| this replaced (0.0.5's src/render.c); with F = 128 it was shifts, now
| that F is 104 some are multiplications and divisions (marked "F").
|
| C entry points: render_view, render_between, render_reset, render_busy.
| Internal routines pass values in registers (documented per routine) and
| may clobber d0-d7/a0-a6 unless they say otherwise; the C entry points
| save what C expects (d2-d7/a2-a6).
|
| Where it fits: ui.c calls render_view after every action and
| render_between for the in-between pictures of smooth movement (render.h
| has the C prototypes). This file calls into:
|   - area.c (area_select): the palettes and remap tables of the area;
|   - the generated compiled scalers (build/gen/scalers.s, made by
|     tools/gen_scalers.py): one routine per wall height;
|   - vdp.c (vdp_dma_queue): the picture's upload, queued for the
|     interrupts (dma.s, crt0.s);
|   - the level's tables, found by pack.c in the campaign block: r_walls,
|     r_pvs_index / r_pvs, r_vis_index / r_vis, r_tex_addr, r_spr_*,
|     r_scenery, r_sin, r_tex_map (build/gen/render_data.h lists them).
|
| Words used below:
|   view space   coordinates relative to the camera: depth d (forward)
|                and lateral l (to the right), in world units.
|   PVS          "potentially visible set": the build's list of walls that
|                can be seen from a cell and facing (r_pvs).
|   iz           inverse depth, 2^28 / depth (bigger = nearer); the walls'
|                is kept per column in col_iz, as a depth buffer.
|   byte-column  one byte of the frame buffer holds two pixels, so walls
|                are drawn two screen columns at a time (or one byte-column
|                with both nibbles from the same texel in low detail).
|   scaler       a routine that copies one texture column to one screen
|                column of a given height, fully unrolled.
|   thing        anything drawn over the walls: a billboard (monster,
|                item, prop: a sprite facing the camera) or a decal quad.
|   tile palette each 8x8 tile of the view uses one of three 16-colour
|                palettes (palettes 0-2); tpal holds the choice per tile.
|
| Fixed-point formats used throughout:
|   8.8    screen positions (x or y in pixels times 256)
|   16.16  texture rows (CP_V0, CP_DV)
|   x 16384 (2.14) the camera axes and r_sin
|   2^28 / depth  the inverse depth "iz" of walls and things
|
| Licence: GPL-2.0-or-later.

| The view's size and place (the same as md.h's VIEW_*: keep them equal)
    .equ    VIEW_TW, 26                 | view width in 8-pixel tile columns
    .equ    VIEW_TH, 16                 | view height in 8-pixel tile rows
    .equ    VIEW_W, VIEW_TW * 8         | view size in pixels: 208
    .equ    VIEW_H, VIEW_TH * 8         |   x 128
    .equ    VIEW_TILES, VIEW_TW * VIEW_TH   | 416 tiles
    .equ    VIEW_BYTES, VIEW_TILES * 32 | 208 x 128 pixels at 4 bits: 13312
    .equ    VIEW_ROW0, 3                | its first screen tile row (md.h)
    .equ    VIEW_COL0, 3                | its first screen tile column (md.h)
    .equ    HALF_W, VIEW_W / 2          | screen centre column = focal length F (104)
    .equ    HALF_H, VIEW_H / 2          | horizon row (eye level): 64
    .if     HALF_W & 7
    .error  "wall_iz needs the focal length HALF_W to be a multiple of 8"
    .endif
    .equ    EYE_Z, 36                   | eye height in world units
    .equ    CAM_BACK, 16                | camera distance behind cell centre
    .equ    NEAR, 1                     | near clip depth (world units)
    .equ    MAX_VW, 96                  | most projected walls per picture (an in-between
                                        | picture of smooth movement takes two views' walls:
                                        | up to 70 on the destroyed Junction)
    .equ    MAX_PT, 96                  | most things per picture
| The game state's field offsets (S_*), generated by tools/gen_cdata.py.
| (This line is a C preprocessor directive, not a comment.)
#include "../build/gen/state.inc"
    .equ    R_MAX_TEX, 48               | texture slots (render_data.h)
    .equ    R_MAX_SPR, 64               | sprite slots (render_data.h)
    .equ    MAX_SCALER_H, 320           | tallest wall with its own scaler
                                        | (tools/gen_scalers.py MAX_H; taller
                                        | walls are drawn by the span loops)
    .equ    TILE_BLANK, 1408            | (md.h: the font's space, all clear)
    .equ    COLBYTES, VIEW_H * 4        | one 8-pixel tile column of the frame buffer: 512
    .equ    COLSHIFT, 9                 | log2(COLBYTES): offset >> COLSHIFT = tile column
    .if     COLBYTES - (1 << COLSHIFT)
    .error  "seg_init needs COLBYTES = 1 << COLSHIFT"
    .endif
    .equ    NT_W, 32                    | entries in a name table row (32x32 planes)
    .equ    NT_WORDS, NT_W * VIEW_TH    | the view's rows of the name table: 512 words
    .equ    VRAM_BUF0, 0x0000           | view tiles, buffer 0 (md.h)
    .equ    VRAM_BUF1, 0x5000           | view tiles, buffer 1
    .equ    BUF1_TILE, VRAM_BUF1 / 32   | buffer 1's first tile number (640)
    .equ    VRAM_PLANE_A0, 0xC000       | name table showing buffer 0
    .equ    VRAM_PLANE_A1, 0xE000       | name table showing buffer 1

| struct vwall (16 bytes): a projected wall
| One per wall that survives projection (n_vw of them, in vw).
| Distances are in world units in view space (depth, lateral).
    .equ    VW_DA, 0                    | A in view space (unclipped)
    .equ    VW_LA, 2
    .equ    VW_DD, 4                    | B - A
    .equ    VW_DL, 6
    .equ    VW_C, 8                     | dl * da - dd * la
                                        | (long; the "c" of the formulas
                                        | at the top, 0 = edge-on wall)
    .equ    VW_TEX, 12                  | texture slot
    .equ    VW_UA, 13                   | texture u at A (signed byte)
    .equ    VW_DU, 14                   | B - A (signed byte)
                                        | (byte 15 unused)
    .equ    VW_SIZE, 16

| struct colp (32 bytes): one screen column's wall
    .equ    CP_H, 0                     | height in pixels (word)
    .equ    CP_FINE, 2                  | y0, y1, v0, dv worked out
    .equ    CP_Y0, 3                    | wall rows y0..y1-1
    .equ    CP_Y1, 4
    .equ    CP_T, 5                     | texture slot
    .equ    CP_U, 6                     | texture column
    .equ    CP_TOP8, 8                  | exact top, 8.8 (long)
    .equ    CP_H8, 12                   | exact height, 8.8 (long)
    .equ    CP_TEX, 16                  | texture column address (0 = no wall)
    .equ    CP_V0, 20                   | texture row at y0, 16.16
    .equ    CP_DV, 24                   | texture rows per screen row, 16.16
    .equ    CP_SIZE, 32

| struct colp (above): one entry per screen column in cp (208 x 32 bytes),
| filled by column_params (CP_H .. CP_TEX) and, lazily, by col_fine
| (CP_Y0, CP_Y1, CP_V0, CP_DV, with CP_FINE set once done).

| struct pthing (24 bytes): a thing to draw
| One per billboard or decal in pt (n_pt of them); pt_order lists them
| far to near after sort_things.
    .equ    PT_IZ8, 0                   | 2^28 / depth, for sorting and depth tests
    .equ    PT_D, 4                     | billboard: depth (16 nearer) and lateral
    .equ    PT_L, 6
    .equ    PT_X0, 8                    | screen columns
    .equ    PT_X1, 10
    .equ    PT_SPR, 12
    .equ    PT_DECAL, 13
    .equ    PT_AX, 14                   | decal quad (world)
    .equ    PT_AZ, 16
    .equ    PT_BX, 18
    .equ    PT_BZ, 20
    .equ    PT_SIZE, 24

| struct robj (C, 6 bytes): x, z (words), sprite (byte)

    .bss
    .even

| Work RAM used by the renderer (about 50 KB of the 64 KB). Globals are
| read by the tests (fb, prof, rphase, render_frames), by area.c (the
| remap tables, and cp as scratch between pictures) and by tpal users.
    .global fb, render_frames, prof, tpal, cp
    .global tex_hi3, tex_lo3, spr_hi3, spr_lo3
fb:         .space  VIEW_BYTES          | the picture (the tests read it)
                                        | (VDP tile order: VIEW_TW tile columns
                                        | of COLBYTES bytes, see the top)
col_iz:     .space  4*VIEW_W            | per column: the nearest wall's 2^28/depth (0 = none)
cp:         .space  CP_SIZE*VIEW_W      | per column: struct colp
vw:         .space  VW_SIZE*MAX_VW      | the projected walls
pt:         .space  PT_SIZE*MAX_PT      | the things to draw
render_frames: .space 2                 | frames the last picture took
prof:       .space  16                  | time per phase (lines), for tuning
                                        | (8 words, see render_core)
    .global rphase
rphase:     .space  2                   | high byte: the last phase done
tpal:       .space  2*VIEW_TILES        | per tile (x * VIEW_TH + y): its palette << 8
ntbuf:      .space  2*NT_WORDS          | the view's name table rows (palettes in),
                                        | whole 32-entry rows: the border columns blank
col_r0:     .space  VIEW_W / 8          | per tile column: its first tile row drawn in,
col_r1:     .space  VIEW_W / 8          |   the row after its last (0, 0: none)
vx:         .space  2                   | camera
vz:         .space  2
fwd_x:      .space  2                   | view axes, x 16384
fwd_z:      .space  2
rgt_x:      .space  2
rgt_z:      .space  2
vis_a:      .space  4                   | cells whose things can show
vis_b:      .space  4                   | (pointers to 128-byte bitmaps)
vis_buf_a:  .space  128                 | (vis_of builds them here)
vis_buf_b:  .space  128
prof_t0:    .space  4                   | ticks at the last prof_mark
frame0:     .space  2                   | frame_count when render_core began
pw_ax:      .space  2                   | project_walls / draw_decal scratch
pw_az:      .space  2
pw_bx:      .space  2
pw_bz:      .space  2
pw_ua:      .space  2
pw_ub:      .space  2
pw_da:      .space  2
pw_la:      .space  2
pw_db:      .space  2
pw_lb:      .space  2
pw_sxa:     .space  4
pw_sxb:     .space  4
pw_x0:      .space  2
pw_x1:      .space  2
pw_iz:      .space  4
dc_vw:      .space  VW_SIZE             | draw_decal's wall
col_w:      .space  VIEW_W              | per column: its wall's slot in vw, $FF = none
| remaps, colour -> slot << 4 (hi) or slot (lo), for each of the area's
| three palettes (src/area.c makes them): a texture's tables are 32 bytes
| (32 colours), a sprite's 16, and those of its palette p are p * 256
| bytes on (tex_boff / spr_boff: where each one's palette 0 table is)
tex_hi3:    .space  32*R_MAX_TEX*3
tex_lo3:    .space  32*R_MAX_TEX*3
spr_hi3:    .space  16*R_MAX_SPR*3
spr_lo3:    .space  16*R_MAX_SPR*3
pt_order:   .space  MAX_PT              | pt indices, far to near
n_vw:       .space  1                   | walls in vw
n_pt:       .space  1                   | things in pt
cstep:      .space  1                   | 1 or 2 (see the top)
back_buf:   .space  1                   | the VRAM buffer the next picture goes to
inited:     .space  1                   | set after the first picture
cur_slots:  .space  1                   | (not used)
keep_mode:  .space  1                   | render_core: 0 choose a palette, 1 keep it, 2 palette only
                                        | (only 2 is tested now: 0 and 1
                                        | draw alike)
both_mode:  .space  1                   | draw_sprite_col's "both" for this detail
    .even

    .text

| ======================================================================
| arithmetic
| ======================================================================

| udiv16: d0.l = d0.l / d1.w, unsigned (d1.w != 0), any quotient. Uses d2.
| The 68000's divu.w only gives a 16-bit quotient, so a dividend whose
| high word is >= the divisor is done as two divisions (long division in
| base 65536): first the high word, then the remainder with the low word.
| The remainder is discarded. Flags undefined.
udiv16:
    move.l  d0, d2
    clr.w   d2
    swap    d2                          | high word of the dividend
    cmp.w   d1, d2
    bcc.s   1f                          | quotient would overflow 16 bits
    divu.w  d1, d0                      | quotient fits 16 bits
    swap    d0
    clr.w   d0                          | drop the remainder (high word)
    swap    d0
    rts
1:  divu.w  d1, d2                      | high word: remainder, low: q1
    move.w  d2, -(sp)                   | keep q1
    move.w  d0, d2                      | remainder : low word
    divu.w  d1, d2                      | low word: q0
    move.w  (sp)+, d0
    swap    d0
    move.w  d2, d0                      | q1 : q0
    rts

| udiv32: d0.l = d0.l / d1.l, unsigned, the C version's way: d1 = 0 gives
| $FFFFFFFF; a divisor over 16 bits is shifted down (with the dividend)
| until it fits. Uses d1, d2.
| (So the result is approximate when the divisor is over 16 bits; it is
| kept that way to match the old C renderer bit for bit.) Global so other
| assembler files can use it.
    .global udiv32
udiv32:
    tst.l   d1
    bne.s   1f
    moveq   #-1, d0                     | division by zero: all ones
    rts
1:  cmpi.l  #0xFFFF, d1                 | divisor fits a word?
    bls.s   udiv16                      | yes: exact 32 / 16 division
    lsr.l   #1, d1                      | no: halve both and try again
    lsr.l   #1, d0
    bra.s   1b

| sdiv32: d0.l = d0.l / d1.l, signed (truncating; |d1| over 16 bits as
| udiv32). Uses d1-d3.
| d3.b records the sign of the result (flipped once per negative operand).
sdiv32:
    moveq   #0, d3
    tst.l   d0
    bpl.s   1f
    neg.l   d0                          | |dividend|
    not.b   d3
1:  tst.l   d1
    bpl.s   2f
    neg.l   d1                          | |divisor|
    not.b   d3
2:  bsr.s   udiv32                      | unsigned quotient
    tst.b   d3
    beq.s   3f
    neg.l   d0                          | negative result
3:  rts

| shdiv: d0.l = (d0.l << d4.w) / d1.l, signed, keeping as many bits of the
| dividend as fit (bit 31 stays clear); what is left of the shift comes off
| the divisor. A divisor shifted to 0 gives +-$7FFFFFFF. Uses d1-d4.
| Used for 2^28 / depth style results, where the dividend is a small
| number shifted far left. A zero dividend skips the shifting (the result
| is then dividend / (d1 >> d4), still 0 unless that divisor is 0).
shdiv:
    moveq   #0, d3                      | d3.b: sign of the result
    tst.l   d0
    bpl.s   1f
    neg.l   d0
    not.b   d3
1:  tst.l   d1
    bpl.s   2f
    neg.l   d1
    not.b   d3
2:  tst.l   d0
    beq.s   5f
3:  cmpi.w  #8, d4                      | a byte at a time while it fits
    bcs.s   4f
    cmpi.l  #0x00400000, d0             | would << 8 reach bit 30?
    bcc.s   4f
    lsl.l   #8, d0
    subq.w  #8, d4
    bra.s   3b
4:  tst.w   d4                          | then a bit at a time
    beq.s   5f
    cmpi.l  #0x40000000, d0             | would << 1 reach bit 31?
    bcc.s   5f
    add.l   d0, d0
    subq.w  #1, d4
    bra.s   4b
5:  lsr.l   d4, d1                      | the rest of the shift: divisor
    bne.s   6f
    move.l  #0x7FFFFFFF, d0             | divisor gone to 0: "infinite"
    bra.s   7f
6:  bsr.s   udiv32
7:  tst.b   d3
    beq.s   8f
    neg.l   d0                          | apply the sign
8:  rts

| mul32: d0.l = d0.l * d1.l, the low 32 bits (signed or unsigned alike).
| Uses d1-d3.
| (a_hi * b_hi only affects bits 32 and up, so it is not needed.)
mul32:
    move.l  d0, d2
    swap    d2
    mulu.w  d1, d2                      | a_hi * b_lo
    move.l  d1, d3
    swap    d3
    mulu.w  d0, d3                      | a_lo * b_hi
    add.w   d3, d2                      | the cross terms (low 16 bits count)
    swap    d2
    clr.w   d2                          | shifted up 16 bits
    mulu.w  d1, d0                      | a_lo * b_lo
    add.l   d2, d0
    rts

| mulu32_16: d0.l = d0.l * d1.w (unsigned word), the low 32 bits. Uses d2.
mulu32_16:
    move.l  d0, d2
    swap    d2
    mulu.w  d1, d2                      | a_hi * b
    swap    d2
    clr.w   d2                          | << 16
    mulu.w  d1, d0                      | a_lo * b
    add.l   d2, d0
    rts

| ======================================================================
| camera
| ======================================================================

| to_view: world point (d0.w, d1.w) -> d0.w depth, d1.w lateral (rounded,
| x 16384 axes). Uses d2, d3.
| depth = (r . fwd + 8192) >> 14, lateral = (r . rgt + 8192) >> 14, with
| r = point - camera (vx, vz). Reads vx, vz, fwd_*, rgt_*.
to_view:
    sub.w   vx, d0                      | rx
    sub.w   vz, d1                      | rz
    move.w  d0, d2
    muls.w  fwd_x, d2
    move.w  d1, d3
    muls.w  fwd_z, d3
    add.l   d3, d2
    addi.l  #8192, d2                   | round (half of 16384)
    asr.l   #8, d2
    asr.l   #6, d2                      | depth
    muls.w  rgt_x, d0
    muls.w  rgt_z, d1
    add.l   d0, d1
    addi.l  #8192, d1
    asr.l   #8, d1
    asr.l   #6, d1                      | lateral
    move.w  d2, d0
    rts

| set_camera: at (d0.w, d1.w) world units looking along angle d2.w (1024
| per turn, 0 = north, clockwise): forward = (sin a, -cos a), right =
| (cos a, sin a). Uses a0.
| Out: vx, vz, fwd_x, fwd_z, rgt_x, rgt_z (2.14 fixed point). r_sin is a
| table of 1024 words; cos a = sin (a + 256), i.e. +512 bytes.
| Clobbers d0-d2, a0.
set_camera:
    move.w  d0, vx
    move.w  d1, vz
    lea     r_sin, a0
    andi.w  #1023, d2                   | angle modulo one turn
    add.w   d2, d2                      | word index
    move.w  0(a0,d2.w), d0              | sin
    addi.w  #512, d2                    | + a quarter turn
    andi.w  #2047, d2
    move.w  0(a0,d2.w), d1              | cos
    move.w  d0, fwd_x
    move.w  d1, rgt_x
    move.w  d0, rgt_z
    neg.w   d1
    move.w  d1, fwd_z
    rts

| screen_x: d0.l = the screen x (8.8) of the view-space point (depth d0.w,
| lateral d1.w): F * 256 + (l * F * 256) / d. Uses d1-d3.
| (The focal length F = HALF_W pixels: x = F + F l / d, times 256. The
| product fits: |l| * 26624 < 2^31.)
screen_x:
    move.w  d0, d2                      | d2 = depth
    move.w  d1, d0
    muls.w  #HALF_W << 8, d0            | lateral * F * 256 (F)
    move.w  d2, d1
    ext.l   d1
    bsr     sdiv32                      | / depth
    addi.l  #HALF_W << 8, d0            | + screen centre
    rts

| clip_ab: the segment A (pw_da, pw_la) - B (pw_db, pw_lb) clipped to depth
| >= NEAR, then both ends on screen: pw_sxa <= pw_sxb (8.8) and the column
| range d6.w (x0) .. d7.w (x1, exclusive), clamped to the view.
| -> Z set (beq) when nothing is in front of the camera. Uses d0-d7.
| The columns are the 8.8 ends rounded up ((sx + 255) >> 8): a column is
| covered when its left edge is inside the segment.
clip_ab:
    move.w  pw_da, d4                   | cda
    move.w  pw_la, d5                   | cla
    move.w  pw_db, d6                   | cdb
    move.w  pw_lb, d7                   | clb
    cmpi.w  #NEAR, d4
    bge.s   1f                          | A in front: check B
    cmpi.w  #NEAR, d6
    blt     9f                          | both behind
    | cla += (lb - la) * (NEAR - da) / (db - da)
    move.w  d7, d0
    sub.w   d5, d0
    moveq   #NEAR, d1
    sub.w   d4, d1
    muls.w  d1, d0
    move.w  d6, d1
    sub.w   d4, d1
    divs.w  d1, d0
    add.w   d0, d5
    moveq   #NEAR, d4                   | A moved onto the near plane
    bra.s   2f
1:  cmpi.w  #NEAR, d6
    bge.s   2f                          | both in front: no clipping
    | clb += (la - lb) * (NEAR - db) / (da - db)
    move.w  d5, d0
    sub.w   d7, d0
    moveq   #NEAR, d1
    sub.w   d6, d1
    muls.w  d1, d0
    move.w  d4, d1
    sub.w   d6, d1
    divs.w  d1, d0
    add.w   d0, d7
    moveq   #NEAR, d6                   | B moved onto the near plane
2:  move.w  d6, -(sp)
    move.w  d7, -(sp)
    move.w  d4, d0
    move.w  d5, d1
    bsr.s   screen_x                    | A's screen x
    move.l  d0, pw_sxa
    move.w  (sp)+, d1
    move.w  (sp)+, d0
    bsr   screen_x                      | B's screen x
    move.l  pw_sxa, d1
    cmp.l   d0, d1                      | sxa > sxb: swap
    ble.s   3f
    exg     d0, d1
3:  move.l  d1, pw_sxa
    move.l  d0, pw_sxb
    move.l  #255, d2                    | round both up to whole columns
    add.l   d2, d1
    asr.l   #8, d1
    add.l   d2, d0
    asr.l   #8, d0
    tst.l   d1                          | both clamped to 0..VIEW_W (as longs:
    bpl.s   4f                          | a far end can be beyond a word)
    moveq   #0, d1
4:  cmpi.l  #VIEW_W, d1
    ble.s   41f
    move.l  #VIEW_W, d1
41: tst.l   d0
    bpl.s   42f
    moveq   #0, d0
42: cmpi.l  #VIEW_W, d0
    ble.s   5f
    move.l  #VIEW_W, d0
5:  move.w  d1, d6                      | x0
    move.w  d0, d7                      | x1 (exclusive)
    moveq   #1, d0                      | (Z clear)
    rts
9:  moveq   #0, d0                      | (Z set)
    rts

| ======================================================================
| walls
| ======================================================================

| wall_iz: d0.l = 2^28 / depth at column d0.w of the wall a3 (vwall):
| shdiv(F dl - (x - F) dd, (F / 8) c, 25). Uses d0-d4.
| (From depth(x) = F c / num(x): 2^28 / depth = num(x) * 2^28 / (F c)
| = num(x) * 2^25 / ((F / 8) c); F = 104 is 8 x 13. With F = 128 this was
| num << 21 / c. (F / 8) c fits: |c| < 2^25 for walls inside a map.)
wall_iz:
    move.w  d0, -(sp)                   | (x)
    move.l  VW_C(a3), d0
    moveq   #HALF_W / 8, d1
    bsr     mul32                       | (F / 8) c: the divisor (F)
    move.w  (sp)+, d1
    move.l  d0, -(sp)
    move.w  d1, d0
    subi.w  #HALF_W, d0                 | x - F
    muls.w  VW_DD(a3), d0
    move.w  VW_DL(a3), d1
    muls.w  #HALF_W, d1                 | F dl (F)
    sub.l   d0, d1                      | num(x)
    move.l  d1, d0
    move.l  (sp)+, d1
    moveq   #25, d4
    bra     shdiv                       | (tail call) num << 25 / ((F / 8) c)

| project_walls: a0 = line flags, a1 / a2 = two visibility lists (u16
| count, then wall ids, sorted). Each wall of either list once: projected,
| and kept per column where it is the nearest.
| (Two lists because an in-between picture of smooth movement is seen
| from between two cells: the walls visible from either.) The lists are
| merged like a sorted merge, so a wall in both is projected once. At
| most MAX_VW walls are kept.
| Out: vw[0 .. n_vw-1], col_iz (per column the nearest wall's iz, 0 =
| none) and col_w (per column the slot in vw of that wall, $FF = none).
| Clobbers d0-d7, a3-a6.
project_walls:
    lea     col_iz, a3                  | col_iz = 0, col_w = $FF
    moveq   #0, d0
    move.w  #VIEW_W/4-1, d1             | 4 longs (4 columns) a pass
1:  move.l  d0, (a3)+
    move.l  d0, (a3)+
    move.l  d0, (a3)+
    move.l  d0, (a3)+
    dbra    d1, 1b
    lea     col_w, a3
    moveq   #-1, d0
    moveq   #VIEW_W/16-1, d1            | 16 bytes (columns) a pass
2:  move.l  d0, (a3)+
    move.l  d0, (a3)+
    move.l  d0, (a3)+
    move.l  d0, (a3)+
    dbra    d1, 2b
    clr.b   n_vw
    move.w  (a1)+, d6                   | walls left in list a
    move.w  (a2)+, d7                   | and in list b
| merge loop: take the smaller head of the two lists
pw_next:
    cmpi.b  #MAX_VW, n_vw               | vw full?
    bcc     pw_done
    tst.w   d6
    bne.s   1f
    tst.w   d7                          | list a empty: b only
    beq     pw_done                     | both empty: done
    bra.s   pw_b
1:  tst.w   d7
    beq.s   pw_a                        | list b empty: a only
    move.w  (a1), d0
    cmp.w   (a2), d0
    bcs.s   pw_a                        | a's head smaller
    bhi.s   pw_b                        | b's head smaller
    addq.l  #2, a2                      | in both: once
    subq.w  #1, d7
pw_a:
    move.w  (a1)+, d0
    subq.w  #1, d6
    bra.s   pw_got
pw_b:
    move.w  (a2)+, d0
    subq.w  #1, d7
pw_got:
    movem.l d6-d7/a0-a2, -(sp)
    bsr.s   project_one                 | project wall d0
    movem.l (sp)+, d6-d7/a0-a2
    bra.s   pw_next
pw_done:
    rts

| project_one: wall d0.w (index into r_walls: ax az bx bz tex line
| ua|two_sided<<8 ub), a0 = line flags
| r_walls records are 8 words (16 bytes): +0 ax, +2 az, +4 bx, +6 bz
| (world units), +9 texture slot (byte), +10 the door/line number ($FF =
| none), +12 bit 0 = two-sided, +13 ua, +14 ub (texture u at A and B).
| A wall whose line is open (line flags bit 0) is skipped; a one-sided
| wall seen from behind is skipped; a two-sided one is flipped.
| Out: a new vwall in vw[n_vw] (n_vw + 1), and col_iz / col_w updated
| for the columns where it is the nearest. Clobbers d0-d7, a1, a3-a6
| (a1 is saved around its use); pw_* scratch.
project_one:
    movea.l r_walls, a4
    andi.l  #0xFFFF, d0
    lsl.l   #4, d0                      | 16 bytes per wall
    adda.l  d0, a4                      | a4 = the wall's record
    move.w  10(a4), d0                  | its door line: open = not drawn
    cmpi.w  #0xFF, d0
    beq.s   1f                          | no line
    btst    #0, 0(a0,d0.w)              | line open?
    bne     po_skip
1:  move.w  (a4), d4                    | ax
    move.w  2(a4), d5                   | az
    move.w  4(a4), d6                   | bx
    move.w  6(a4), d7                   | bz
    moveq   #0, d0
    move.b  13(a4), d0
    move.w  d0, pw_ua                   | ua = w[6] & $FF
    move.w  14(a4), pw_ub
    | one-sided unless flagged: (ax - vx) (bz - az) - (az - vz) (bx - ax) <= 0
    | (the 2D cross product: its sign says which side of the wall the
    | camera is on; > 0 = the front)
    move.w  d4, d0
    sub.w   vx, d0
    move.w  d7, d1
    sub.w   d5, d1
    muls.w  d1, d0
    move.w  d5, d1
    sub.w   vz, d1
    move.w  d6, d2
    sub.w   d4, d2
    muls.w  d2, d1
    sub.l   d1, d0
    bgt.s   2f                          | front side: draw as is
    btst    #0, 12(a4)                  | two-sided: seen from behind, swapped
    beq     po_skip
    exg     d4, d6
    exg     d5, d7
    move.w  pw_ua, d0
    move.w  pw_ub, pw_ua
    move.w  d0, pw_ub
2:  move.w  d4, d0
    move.w  d5, d1
    bsr     to_view                     | A in view space
    move.w  d0, pw_da
    move.w  d1, pw_la
    move.w  d6, d0
    move.w  d7, d1
    bsr     to_view                     | B in view space
    move.w  d0, pw_db
    move.w  d1, pw_lb
    bsr     clip_ab                     | near clip, screen columns d6..d7
    beq     po_skip
    cmp.w   d6, d7
    ble     po_skip                     | x1 <= x0
    | the vwall
    moveq   #0, d0
    move.b  n_vw, d0
    lsl.w   #4, d0
    lea     vw, a3
    adda.w  d0, a3                      | a3 = vw[n_vw]
    move.w  pw_da, d0
    move.w  d0, VW_DA(a3)
    move.w  pw_la, d1
    move.w  d1, VW_LA(a3)
    move.w  pw_db, d2
    sub.w   d0, d2
    move.w  d2, VW_DD(a3)               | dd
    move.w  pw_lb, d3
    sub.w   d1, d3
    move.w  d3, VW_DL(a3)               | dl
    muls.w  d0, d3                      | dl * da
    muls.w  d1, d2                      | dd * la
    sub.l   d2, d3
    move.l  d3, VW_C(a3)
    beq     po_skip                     | c = 0: seen edge-on, nothing to draw
    moveq   #0, d0                      | (through r_tex_map: the portal's swaps)
    move.b  9(a4), d0
    move.l  a1, -(sp)
    lea     r_tex_map, a1               | pack.c: slot -> slot drawn now
    move.b  0(a1,d0.w), VW_TEX(a3)
    movea.l (sp)+, a1
    move.w  pw_ua, d0
    move.b  d0, VW_UA(a3)
    move.w  pw_ub, d1
    sub.w   d0, d1
    move.b  d1, VW_DU(a3)
    | only every cstep-th column is used
    | (x0 rounded up to a multiple of cstep; if that leaves no column the
    | vwall is still counted, but no column points at it)
    moveq   #0, d5
    move.b  cstep, d5
    cmpi.b  #1, d5
    beq.s   3f
    move.w  d5, d0
    subq.w  #1, d0
    add.w   d0, d6
    not.w   d0
    and.w   d0, d6                      | x0 = (x0 + cstep - 1) & -cstep
    cmp.w   d6, d7
    bgt.s   3f
    addq.b  #1, n_vw
    rts
| iz is linear in x along a wall, so it is worked out exactly at the two
| ends and stepped by a constant diz in between.
3:  move.w  d6, d0                      | iz at x0
    bsr     wall_iz
    move.l  d0, pw_iz
    moveq   #0, d1                      | diz
    move.w  d7, d0
    sub.w   d6, d0
    cmpi.w  #1, d0
    ble.s   4f                          | one column: no step needed
    move.w  d7, d0                      | (iz(x1 - 1) - iz) / (x1 - 1 - x0)
    subq.w  #1, d0
    bsr     wall_iz
    sub.l   pw_iz, d0
    move.w  d7, d1
    subq.w  #1, d1
    sub.w   d6, d1
    ext.l   d1
    bsr     sdiv32
    move.l  d0, d1                      | d1 = diz per column
4:  move.l  pw_iz, d0
    | keep per column where nearest
    move.b  n_vw, d4                    | d4 = this wall's slot
    lea     col_iz, a5
    lea     col_w, a6
    move.w  d6, d2
    adda.w  d2, a6                      | a6 = &col_w[x0]
    add.w   d2, d2
    add.w   d2, d2
    adda.w  d2, a5                      | a5 = &col_iz[x0]
    move.w  d7, d3
    sub.w   d6, d3                      | columns in range
    cmpi.b  #1, d5
    bne.s   6f
    subq.w  #1, d3                      | (dbra count)
5:  cmp.l   (a5)+, d0                   | nearer (bigger iz) than so far?
    ble.s   51f
    move.l  d0, -4(a5)                  | yes: this wall owns the column
    move.b  d4, (a6)
51: addq.l  #1, a6
    add.l   d1, d0                      | iz of the next column
    dbra    d3, 5b
    addq.b  #1, n_vw
    rts
6:  | every second column: diz * 2
    add.l   d1, d1
    moveq   #8, d2                      | col_iz step (bytes)
    addq.w  #1, d3
    lsr.w   #1, d3                      | ceil(n / 2)
62: subq.w  #1, d3                      | (dbra count; label unused)
    move.w  d2, d6
    lsr.w   #2, d6                      | col_w step
7:  cmp.l   (a5), d0                    | nearer than so far?
    ble.s   71f
    move.l  d0, (a5)
    move.b  d4, (a6)
71: adda.w  d2, a5
    adda.w  d6, a6
    add.l   d1, d0
    dbra    d3, 7b
    addq.b  #1, n_vw
po_skip:
    rts

| ======================================================================
| columns
| ======================================================================

| column_params: every cstep-th column's wall height, top, texture column.
| Along one wall num and m change by a constant per column, so they are
| stepped instead of multiplied; the per-column division for u uses one
| divs.w when it fits (the same result as sdiv32).
| In: col_iz, col_w, vw, cstep. Out: cp[x] for x = 0, cstep, 2 cstep ...:
| CP_H, CP_TOP8, CP_H8, CP_T, CP_U, CP_TEX, CP_FINE = 0 (or, for a column
| with no wall, CP_TEX = 0 and CP_Y0 = CP_Y1 = HALF_H).
| Register use in the loop: a2 = &cp[x], a3 = &col_iz[x], a4 = &col_w[x],
| a5 = the current vwall, a1 = num(x), a6 = m(x), d7 = x, d6 = cstep,
| d5 = iz >> 8 (= 2^20 / depth). Clobbers d0-d7, a0-a6.
    .bss
cp_dnum:    .space  4                   | per cstep columns: num change, m change
cp_dm:      .space  4
cp_texb:    .space  4                   | the wall's texture: r_tex + tex * 4096
                                        | (address of texel 0: 64 columns
                                        | of 64 bytes, column-major)
cp_cur:     .space  1                   | the wall num / m belong to ($FF none)
    .even
    .text
column_params:
    lea     cp, a2
    lea     col_iz, a3
    lea     col_w, a4
    moveq   #0, d7                      | x
    moveq   #0, d6
    move.b  cstep, d6
    st      cp_cur                      | no wall yet: recompute num / m
cpm_loop:
    move.l  (a3), d5
    asr.l   #8, d5                      | iz
    moveq   #0, d0
    move.b  (a4), d0                    | the column's wall slot
    cmpi.b  #0xFF, d0
    beq.s   1f                          | no wall here
    tst.l   d5
    bgt.s   2f
1:  move.b  #HALF_H, CP_Y0(a2)          | no wall
    move.b  #HALF_H, CP_Y1(a2)
    clr.l   CP_TEX(a2)
    st      cp_cur
    bra     cpm_next
2:  cmp.b   cp_cur, d0
    beq.s   3f                          | same wall as the last column
    | a new wall: num and m at x, their steps, the texture
    move.b  d0, cp_cur
    lsl.w   #4, d0
    lea     vw, a5
    adda.w  d0, a5                      | a5 = the vwall
    move.w  d7, d4
    subi.w  #HALF_W, d4
    move.w  VW_DL(a5), d1               | num = F dl - (x - F) dd
    muls.w  #HALF_W, d1                 | (F)
    move.w  d4, d0
    muls.w  VW_DD(a5), d0
    sub.l   d0, d1
    movea.l d1, a1
    move.w  d4, d0                      | m = (x - F) da - F la
    muls.w  VW_DA(a5), d0
    move.w  VW_LA(a5), d2
    muls.w  #HALF_W, d2                 | (F)
    sub.l   d2, d0
    movea.l d0, a6
    move.w  d6, d0                      | steps: -cstep dd, cstep da
    muls.w  VW_DD(a5), d0
    neg.l   d0
    move.l  d0, cp_dnum
    move.w  d6, d0
    muls.w  VW_DA(a5), d0
    move.l  d0, cp_dm
    moveq   #0, d2
    move.b  VW_TEX(a5), d2
    move.b  d2, CP_T(a2)
    move.w  d2, d3
    lsl.w   #2, d3
    lea     r_tex_addr, a0
    move.l  0(a0,d3.w), d3              | r_tex_addr[tex]
    move.l  d3, cp_texb
3:  | u = sdiv32(m << 8, num): 0..256 along the wall
    move.l  a6, d0
    asl.l   #8, d0
    move.l  a1, d1
    move.w  d1, d2
    ext.l   d2
    cmp.l   d1, d2
    bne.s   4f                          | num not a signed word
    tst.w   d1
    beq.s   4f                          | (num = 0: sdiv32 handles it)
    move.l  d0, d3
    divs.w  d1, d3                      | fast path: one 32 / 16 division
    bvs.s   4f                          | quotient too big
    move.w  d3, d0
    ext.l   d0
    bra.s   5f
4:  bsr     sdiv32                      | slow path
5:  tst.l   d0
    bpl.s   6f
    moveq   #0, d0                      | clamp u to 0..255
6:  cmpi.l  #255, d0
    ble.s   7f
    move.l  #255, d0
7:  move.b  VW_DU(a5), d1               | u = (ua + (u du >> 8)) & 63
    ext.w   d1
    muls.w  d1, d0
    asr.l   #8, d0
    move.b  VW_UA(a5), d1
    ext.w   d1
    add.w   d1, d0
    andi.l  #63, d0                     | textures repeat every 64 texels
    move.b  d0, CP_U(a2)
    move.b  VW_TEX(a5), CP_T(a2)
    lsl.w   #6, d0                      | the texture column
    add.l   cp_texb, d0
    move.l  d0, CP_TEX(a2)
    | h8 = min(iz, $FFFFF) * F / 64, h = (h8 + 128) >> 8
    | (d5 = 2^20 / depth; the wall is 64 units at focal length F, so its
    | height is 64 F / depth pixels = d5 * F / 64 / 256: h8 is it in 8.8.
    | With F = 128 that was 2 d5.) d5 * F by one mulu.w when d5 fits a
    | word (depth 16 or more), else from d5 >> 4 (the last 4 bits lost
    | only for the nearest walls).
    cmpi.l  #0xFFFFF, d5
    ble.s   8f
    move.l  #0xFFFFF, d5                | cap: very near walls
8:  cmpi.l  #0xFFFF, d5
    bhi.s   81f
    move.w  d5, d1
    mulu.w  #HALF_W, d1                 | d5 * F (F)
    bra.s   82f
81: move.l  d5, d1
    lsr.l   #4, d1
    mulu.w  #HALF_W, d1
    lsl.l   #4, d1                      | (d5 >> 4) * F * 16
82: lsr.l   #6, d1                      | h8 = d5 * F / 64
    move.l  d1, CP_H8(a2)
    move.l  d1, d2                      | (kept for top8)
    addi.l  #128, d1
    asr.l   #8, d1                      | rounded to whole pixels
    move.w  d1, CP_H(a2)
    | top8 = (HALF_H << 8) - (h8 * 7 >> 4)
    | (the eye is 36 units up a 64-unit wall: the top is 28/64 = 7/16 of
    | the height above the horizon)
    move.l  d2, d3
    lsl.l   #3, d2
    sub.l   d3, d2                      | h8 * 7 = * 8 - * 1
    lsr.l   #4, d2
    move.l  #HALF_H << 8, d3
    sub.l   d2, d3
    move.l  d3, CP_TOP8(a2)
    clr.b   CP_FINE(a2)                 | rows / v not worked out yet
    adda.l  cp_dnum, a1                 | on to the next column of this wall
    adda.l  cp_dm, a6
cpm_next:
    add.w   d6, d7                      | x += cstep
    move.w  d6, d0
    lsl.w   #5, d0
    adda.w  d0, a2                      | cp: 32 bytes a column
    move.w  d6, d0
    lsl.w   #2, d0
    adda.w  d0, a3                      | col_iz: 4 bytes a column
    adda.w  d6, a4                      | col_w: 1 byte a column
    cmpi.w  #VIEW_W, d7
    bcs     cpm_loop
    rts

| col_fine: rows and texture stepping of the column a2 (colp), for the
| columns not drawn by a scaler. Uses d0-d4.
| Done at most once per column (CP_FINE marks it) and only on demand,
| since most columns are drawn by a scaler, which does not need these.
| Out: CP_Y0 / CP_Y1 = the first and the after-last screen row of the
| wall, clamped to 0..VIEW_H (y0 <= y1); CP_DV = texture rows per screen row
| (16.16) = 64 / h; CP_V0 = the texture row at the centre of row y0
| (16.16). Rows are covered when their centre (y + 0.5) is inside.
col_fine:
    tst.b   CP_FINE(a2)                 | already done?
    bne   9f
    tst.l   CP_TEX(a2)                  | no wall?
    beq   9f
    st      CP_FINE(a2)
    move.l  CP_TOP8(a2), d3
    move.l  d3, d0
    addi.l  #255, d0
    asr.l   #8, d0                      | y0
    move.l  d3, d1
    add.l   CP_H8(a2), d1
    addi.l  #255, d1
    asr.l   #8, d1                      | y1
    tst.l   d0
    bpl.s   1f
    moveq   #0, d0                      | clamp y0 >= 0
1:  cmpi.l  #VIEW_H, d1
    ble.s   2f
    move.l  #VIEW_H, d1                 | clamp y1 <= VIEW_H
2:  cmpi.l  #VIEW_H, d0
    ble.s   3f
    move.l  #VIEW_H, d0                 | clamp y0 <= VIEW_H
3:  cmp.l   d0, d1
    bge.s   4f
    move.l  d0, d1                      | y1 >= y0
4:  move.b  d0, CP_Y0(a2)
    move.b  d1, CP_Y1(a2)
    move.l  d0, d4
    | dv = 2^30 / h8
    | (= 64 * 65536 / h: 64 texture rows over h pixels, in 16.16)
    move.l  #1 << 30, d0
    move.l  CP_H8(a2), d1
    bsr     udiv32
    move.l  d0, CP_DV(a2)
    | off = (y0 << 8) + 128 - top8 (rows below the top, 8.8)
    lsl.l   #8, d4
    addi.l  #128, d4
    sub.l   d3, d4
    cmpi.l  #0x10000, d4                | fits a word?
    bcc.s   5f
    move.w  d4, d1                      | v0 = dv * off >> 8
    bsr     mulu32_16
    lsr.l   #8, d0
    bra.s   6f
5:  lsr.l   #2, d4                      | (off >> 2) * (dv >> 6)
    lsr.l   #6, d0
    move.l  d4, d1
    bsr     mul32
6:  move.l  d0, CP_V0(a2)
9:  rts

| v_at: d0.l = the texture row of the column a2 at screen row d0.w
| (>= y0): v0 + dv * (y - y0). Uses d1, d2.
| (16.16, as CP_V0; col_fine must have run for this column.)
v_at:
    moveq   #0, d1
    move.b  CP_Y0(a2), d1
    sub.w   d1, d0
    move.w  d0, d1
    move.l  CP_DV(a2), d0
    bsr     mulu32_16
    add.l   CP_V0(a2), d0
    rts

| ======================================================================
| the inner loops: register interfaces
| a texture row v is kept as 16.16 with its words swapped (fraction in
| the high word, integer in the low word) so it can index directly; the
| step is added with add.l (fraction, sets X) + addx.w.
| ======================================================================

| Every pixel is looked up in the remap tables of its tile's palette: the
| loops below run a tile row at a time, with the tile's palette (tpal: the
| palette << 8) in the high byte of the texel register d4, so the lookup
| 0(remap, d4.w) lands in that palette's table (256 bytes apart).

| seg_init: a0 = a frame buffer address, d1.w = rows (> 0) -> a3 = its
| tile's tpal entry, d4.w = the rows of the first run: to the tile's end
| (1..8), or all of them when every tile they cross has the same palette
| (most do: a tile column takes its wall's palette). Uses d4.
| Frame buffer offset = (x >> 3) * COLBYTES + y * 4 + ((x & 7) >> 1);
| COLBYTES is 512, so the tile column is offset >> COLSHIFT and the rest
| (offset & 511) >> 2 is the row y. tpal is laid out the same way:
| VIEW_TH words per tile column, so a3 = tpal + (x >> 3) * 2 VIEW_TH +
| (y >> 3) * 2.
| The span loops below call this, then walk down a3 one tile at a time.
| Keeps d0, d1 (d0 and a3 are saved around the look-ahead).
seg_init:
    move.l  a0, d4                      | offset in fb: x >> 3 tile columns of
    subi.l  #fb, d4                     | COLBYTES bytes, then y rows of 4
    move.w  d4, -(sp)
    lsr.w   #8, d4
    lsr.w   #COLSHIFT - 8, d4           | x >> 3 (offset >> COLSHIFT)
    mulu.w  #2*VIEW_TH, d4              | (x >> 3) * 2 VIEW_TH
    lea     tpal, a3
    adda.w  d4, a3
    move.w  (sp)+, d4
    andi.w  #COLBYTES - 1, d4           | y * 4 + (x & 7) >> 1
    lsr.w   #2, d4                      | y
    move.w  d4, -(sp)
    lsr.w   #3, d4
    add.w   d4, d4
    adda.w  d4, a3
    move.w  (sp)+, d4
    andi.w  #7, d4
    subq.w  #8, d4
    neg.w   d4                          | to the tile's end
    cmp.w   d1, d4
    bcc.s   9f                          | (all in this tile)
    | the tiles below, while the same palette
    movem.l d0/a3, -(sp)
    move.w  (a3)+, d0                   | the first tile's palette
1:  cmp.w   (a3)+, d0                   | next tile down the same?
    bne.s   8f
    addq.w  #8, d4                      | yes: 8 more rows in the run
    cmp.w   d1, d4
    bcs.s   1b
    move.w  d1, d4                      | all the same
    movem.l (sp)+, d0/a3
    rts
8:  movem.l (sp)+, d0/a3
    andi.w  #7, d4                      | (back to the first tile's)
    bne.s   9f
    moveq   #8, d4                      | (a full first tile: 8 rows)
9:  rts

| The three span loops below share one shape:
|   - d0 (v) and d2 (dv) are turned into the swapped form: d0 = fraction
|     : integer, d2 = fraction : 0, d3 = the integer step (word). Then
|     "add.l d2, d0 / addx.w d3, d0" adds the fraction (the carry goes
|     to X) and then the integer part plus X: a 16.16 add in 2 steps.
|   - a4 counts the rows left; d1 the rows in the current run of tiles
|     (the first from seg_init, then 8 at a time); at the start of each
|     run the tile's palette word (palette << 8) is loaded into d4 from
|     (a3)+, so the remap lookup 0(a2, d4.w) uses that palette's table.
|   - a0 moves down 4 bytes (one screen row) a pixel.

| span1: a0 = frame buffer, a1 = texture column, a2 / a6 = remaps (slot
| << 4 / slot), d0 = v, d2 = dv, d1.w = rows. Both pixels of each byte
| from one texel. Uses d0-d5, a3, a4.
| (Used where both screen columns of a byte show the same texture column,
| and in low detail.) Out: a0 past the last row.
span1:
    tst.w   d1
    ble.s   9f                          | no rows
    bsr     seg_init                    | a3 = tpal entry, d4 = first run
    movea.w d1, a4                      | rows left
    move.w  d4, d1                      | the first tile row's
    swap    d0                          | v: fraction : integer
    swap    d2
    moveq   #0, d3
    move.w  d2, d3                      | d3 = integer step
    clr.w   d2                          | d2 = fraction : 0
0:  move.w  (a3)+, d4                   | the tile's palette
    cmpa.w  d1, a4
    bcc.s   2f
    move.w  a4, d1                      | last run: only the rows left
2:  suba.w  d1, a4
    subq.w  #1, d1                      | (dbra count)
1:  move.b  0(a1,d0.w), d4              | texel (palette in the high byte)
    move.b  0(a2,d4.w), d5              | left pixel's slot << 4
    or.b    0(a6,d4.w), d5              | right pixel's slot
    move.b  d5, (a0)
    addq.l  #4, a0                      | next row
    add.l   d2, d0                      | v += dv (fraction, sets X)
    addx.w  d3, d0                      | (integer + X)
    dbra    d1, 1b
    moveq   #8, d1                      | later runs: one tile (8 rows)
    cmpa.w  #0, a4                      | rows left?
    bne.s   0b
9:  rts

| span2: a0 = frame buffer, a1 / a5 = texture columns (left / right
| pixel), a2 / a6 = remaps (slot << 4 / slot), d0 / d5 = v, d2 / d6 = dv,
| d1.w = rows. (d4's low byte: the texel, then its slot, below 16.) Uses
| d0-d7, a3, a4.
| (Two different wall columns in one byte: each pixel has its own
| texture column and v.)
span2:
    tst.w   d1
    ble.s   9f
    bsr     seg_init                    | a3 = tpal entry, d4 = first run
    movea.w d1, a4                      | rows left
    move.w  d4, d1
    swap    d0                          | left v and dv: swapped form
    swap    d2
    moveq   #0, d3
    move.w  d2, d3
    clr.w   d2
    swap    d5                          | right v and dv: the same
    swap    d6
    moveq   #0, d7
    move.w  d6, d7
    clr.w   d6
0:  move.w  (a3)+, d4                   | the tile's palette << 8
    cmpa.w  d1, a4
    bcc.s   2f
    move.w  a4, d1
2:  suba.w  d1, a4
    subq.w  #1, d1
1:  move.b  0(a1,d0.w), d4              | left texel
    move.b  0(a2,d4.w), (a0)            | its slot << 4
    move.b  0(a5,d5.w), d4              | right texel
    move.b  0(a6,d4.w), d4              | its slot (move.b leaves d4's high
                                        | byte, the palette, as it was)
    or.b    d4, (a0)
    addq.l  #4, a0
    add.l   d2, d0                      | left v += dv
    addx.w  d3, d0
    add.l   d6, d5                      | right v += dv
    addx.w  d7, d5
    dbra    d1, 1b
    moveq   #8, d1
    cmpa.w  #0, a4
    bne.s   0b
9:  rts

| span_half: one pixel of each byte: *a0 = (*a0 & d5.b) | remap a2[texel].
| a1 = texture column, d0 = v, d2 = dv, d1.w = rows. Uses d0-d4, d6, a3, a4.
| d5.b = $0F keeps the right pixel (writing the left), $F0 keeps the left.
| a2 must be the matching remap (hi for the left pixel, lo for the right).
span_half:
    tst.w   d1
    ble.s   9f
    bsr     seg_init                    | a3 = tpal entry, d4 = first run
    movea.w d1, a4
    move.w  d4, d1
    swap    d0
    swap    d2
    moveq   #0, d3
    move.w  d2, d3
    clr.w   d2
0:  move.w  (a3)+, d4                   | the tile's palette << 8
    cmpa.w  d1, a4
    bcc.s   2f
    move.w  a4, d1
2:  suba.w  d1, a4
    subq.w  #1, d1
1:  move.b  0(a1,d0.w), d4              | texel
    move.b  (a0), d6
    and.b   d5, d6                      | keep the other pixel
    or.b    0(a2,d4.w), d6              | add this one
    move.b  d6, (a0)
    addq.l  #4, a0
    add.l   d2, d0
    addx.w  d3, d0
    dbra    d1, 1b
    moveq   #8, d1
    cmpa.w  #0, a4
    bne.s   0b
9:  rts

| where each texture's / sprite's palette-0 remap table is (from tex_hi3 ...)
| Byte offsets, one word per slot, following area.c's layout: textures in
| groups of 8 (768 bytes a group: 3 palettes x 256), 32 bytes each;
| sprites in groups of 16, 16 bytes each. Palette p's table is p * 256
| bytes further, which the loops reach through the palette << 8 in d4.
tex_boff:
    .set    k, 0
    .rept   R_MAX_TEX
    .word   (k >> 3) * 768 + (k & 7) * 32
    .set    k, k + 1
    .endr
spr_boff:
    .set    k, 0
    .rept   R_MAX_SPR
    .word   (k >> 4) * 768 + (k & 15) * 16
    .set    k, k + 1
    .endr

| ======================================================================
| drawing the walls
| ======================================================================

| draw_walls: every wall column into fb, from cp (column_params).
| Full detail (cstep 1): two screen columns a = x, b = x + 1 share one
| frame buffer byte (a = high nibble). If both have walls of nearly the
| same height (within 3 pixels) one compiled scaler of their average
| height draws the pair; otherwise (dwf_split) the rows both cover are
| drawn with span1 / span2 and the rows only one covers with span_half.
| Low detail (cstep 2, dw_mid): one column per byte, both nibbles from the
| same texel, with the doubling scalers or span1.
| In: cp, tpal, the remap tables, fb (cleared). Out: fb. Clobbers d0-d7,
| a0-a6. prof+14 counts the pairs drawn by a scaler (for tuning).
draw_walls:
    cmpi.b  #2, cstep
    beq     dw_mid
    | ---- full detail: a byte (two columns, a and b) at a time
    moveq   #0, d7                      | x
dwf_loop:
    lea     cp, a2
    move.w  d7, d0
    lsl.w   #5, d0
    adda.w  d0, a2                      | a
    lea     CP_SIZE(a2), a3             | b
    move.w  d7, d0                      | column address: fb + (x >> 3) * COLBYTES + (x & 7) >> 1
    lsr.w   #3, d0
    mulu.w  #COLBYTES, d0
    move.w  d7, d1
    andi.w  #7, d1
    lsr.w   #1, d1
    add.w   d1, d0
    lea     fb, a4
    adda.l  d0, a4                      | the column (kept in a4)
    | both walls with near heights: one scaler for the pair
    move.l  CP_TEX(a2), d0
    beq     dwf_split                   | a has no wall
    move.l  CP_TEX(a3), d1
    beq     dwf_split                   | b has no wall
    move.w  CP_H(a2), d2
    beq     dwf_split                   | zero height: no scaler
    cmpi.w  #MAX_SCALER_H, d2
    bhi     dwf_split                   | too tall for a scaler
    move.w  d2, d3
    sub.w   CP_H(a3), d3
    addq.w  #3, d3
    cmpi.w  #6, d3                      | |a.h - b.h| <= 3? (unsigned test
    bhi     dwf_split                   | of the difference + 3 <= 6)
    add.w   CP_H(a3), d2                | (a->h + b->h + 1) >> 1
    addq.w  #1, d2
    lsr.w   #1, d2
    cmpi.w  #MAX_SCALER_H, d2           | (b's may be a little over)
    bhi     dwf_split
| the scaler's registers (tools/gen_scalers.py): a0 / a3 = the texture
| columns of a / b, a1 = the frame buffer column, a2 = a's hi remap, a4 =
| b's lo remap, a5 = this tile column's VIEW_TH tpal words
    movea.l a4, a1
    movea.l d0, a0
    movea.l a3, a5                      | (keep b)
    movea.l d1, a3
    lea     tex_boff, a6
    moveq   #0, d0
    move.b  CP_T(a5), d0
    add.w   d0, d0
    move.w  0(a6,d0.w), d0
    lea     tex_lo3, a4
    adda.w  d0, a4                      | lo of b
    moveq   #0, d0
    move.b  CP_T(a2), d0
    add.w   d0, d0
    move.w  0(a6,d0.w), d0
    lea     tex_hi3, a2
    adda.w  d0, a2                      | hi of a
    move.w  d7, d0                      | the tile column's palettes
    lsr.w   #3, d0
    mulu.w  #2*VIEW_TH, d0
    lea     tpal, a5
    adda.w  d0, a5
    lsl.w   #2, d2                      | one long per height
    lea     scaler_tab, a6
    movea.l 0(a6,d2.w), a6              | the scaler for this height
    jsr     (a6)                        | draw the pair (scalers.s)
    addq.w  #1, prof+14                 | (count: pairs by scaler)
    bra     dwf_next
| The pair cannot share a scaler: work out each column's rows (col_fine),
| then draw the rows both cover (c0 .. c1) a byte at a time, and the rest
| of each column a nibble at a time.
dwf_split:
    exg     a2, a3                      | col_fine(a), col_fine(b)
    bsr     col_fine
    exg     a2, a3
    bsr     col_fine
    | c0 = max(a.y0, b.y0), c1 = min(a.y1, b.y1)
    moveq   #0, d4
    moveq   #0, d5
    move.b  CP_Y0(a2), d4
    move.b  CP_Y0(a3), d0
    cmp.b   d0, d4
    bcc.s   1f
    move.b  d0, d4
1:  move.b  CP_Y1(a2), d5
    move.b  CP_Y1(a3), d0
    cmp.b   d0, d5
    bls.s   2f
    move.b  d0, d5
2:  cmp.w   d4, d5
    bls   5f                            | no shared rows
    tst.l   CP_TEX(a2)
    beq   5f                            | a has no wall
    tst.l   CP_TEX(a3)
    beq   5f                            | b has no wall
    movem.w d4-d5, -(sp)                | (c0, c1)
    move.w  d4, d0
    bsr     v_at                        | a's texture row at c0
    move.l  d0, -(sp)                   | v of a at c0
    move.l  CP_TEX(a2), d0
    cmp.l   CP_TEX(a3), d0
    bne.s   3f
    | same texture column: span1
    move.l  (sp)+, d0
    movem.w (sp), d4-d5
    movem.l a2-a4, -(sp)
    move.w  d5, d1
    sub.w   d4, d1                      | rows = c1 - c0
    lsl.w   #2, d4
    lea     0(a4,d4.w), a0              | fb at row c0
    movea.l CP_TEX(a2), a1
    move.l  CP_DV(a2), d2
    moveq   #0, d3
    move.b  CP_T(a2), d3
    add.w   d3, d3
    lea     tex_boff, a2
    move.w  0(a2,d3.w), d3
    lea     tex_lo3, a6
    adda.w  d3, a6
    lea     tex_hi3, a2
    adda.w  d3, a2
    bsr     span1                       | both pixels from one texel
    movem.l (sp)+, a2-a4
    bra.s   4f
3:  | two texture columns: span2
    exg     a2, a3
    move.w  4(sp), d0                   | c0
    bsr     v_at                        | b's texture row at c0
    exg     a2, a3
    move.l  d0, d5                      | v of b
    move.l  (sp)+, d0                   | v of a
    movem.w (sp), d1/d4                 | c0, c1
    movem.l d7/a2-a4, -(sp)             | (span2 uses d7)
    sub.w   d1, d4
    exg     d1, d4                      | d1 = rows, d4 = c0
    lsl.w   #2, d4
    lea     0(a4,d4.w), a0
    movea.l CP_TEX(a2), a1
    movea.l CP_TEX(a3), a5
    move.l  CP_DV(a2), d2
    move.l  CP_DV(a3), d6
    lea     tex_boff, a6
    moveq   #0, d3
    move.b  CP_T(a2), d3
    add.w   d3, d3
    move.w  0(a6,d3.w), d4              | a's tables
    moveq   #0, d3
    move.b  CP_T(a3), d3
    add.w   d3, d3
    move.w  0(a6,d3.w), d3              | b's
    lea     tex_lo3, a6
    adda.w  d3, a6
    lea     tex_hi3, a2
    adda.w  d4, a2
    bsr     span2                       | each pixel its own texel
    movem.l (sp)+, d7/a2-a4
4:  movem.w (sp)+, d4-d5
    bra.s   6f
5:  moveq   #0, d4                      | c0 = c1 = 0
    moveq   #0, d5
6:  | rows where only one of the two is wall
    | (d6: high byte = the keep mask, low byte = 0 for the hi remap, 1
    | for the lo remap; see dwf_half)
    movem.w d4-d5, -(sp)
    move.l  #0x0F00, d6                 | a: high nibble (keep the low one)
    bsr.s   dwf_half
    exg     a2, a3
    movem.w (sp), d4-d5
    move.l  #0xF001, d6                 | b: low nibble
    bsr.s   dwf_half
    exg     a2, a3
    addq.l  #4, sp                      | drop c0, c1
dwf_next:
    addq.w  #2, d7                      | next pair of columns
    cmpi.w  #VIEW_W, d7
    bcs     dwf_loop
    rts

| dwf_half: the rows of the column a2 outside the shared span c0..c1
| (d4.w, d5.w): span_half with keep mask d6 >> 8, remap hi (d6.b = 0) or
| lo (1); a4 = the frame buffer column
| Two pieces at most: y0 .. min(c0, y1) above the shared rows, and
| c1 .. y1 below them (when c0 >= c1 there are no shared rows and the
| whole column y0 .. y1 is the first piece). Keeps a2-a4, d4-d6.
dwf_half:
    tst.l   CP_TEX(a2)
    beq     9f                          | no wall in this column
    moveq   #0, d3
    move.b  CP_Y1(a2), d3               | e = (c1 > c0 && c0 < y1) ? c0 : y1
    cmp.w   d4, d5
    bls.s   1f
    cmp.w   d3, d4
    bcc.s   1f
    move.w  d4, d3
1:  moveq   #0, d0
    move.b  CP_Y0(a2), d0
    cmp.w   d0, d3
    bls.s   2f
    | rows y0 .. e
    movem.l d4-d6/a2-a4, -(sp)
    move.w  d3, d1
    sub.w   d0, d1
    lsl.w   #2, d0
    lea     0(a4,d0.w), a0
    movea.l CP_TEX(a2), a1
    move.l  CP_V0(a2), d0               | v at y0
    bsr.s   dwf_span                    | draw rows y0 .. e
    movem.l (sp)+, d4-d6/a2-a4
2:  cmp.w   d4, d5                      | rows c1 .. y1
    bls.s   9f
    moveq   #0, d3
    move.b  CP_Y1(a2), d3
    cmp.w   d5, d3
    bls.s   9f                          | column ends before c1
    movem.l d4-d6/a2-a4, -(sp)
    move.w  d3, d1
    sub.w   d5, d1                      | rows = y1 - c1
    move.w  d1, -(sp)
    move.w  d5, d0
    bsr     v_at                        | v at c1
    move.w  (sp)+, d1
    movem.l (sp), d4-d6
    lsl.w   #2, d5
    lea     0(a4,d5.w), a0              | fb at row c1
    movea.l CP_TEX(a2), a1
    bsr.s   dwf_span                    | draw rows c1 .. y1
    movem.l (sp)+, d4-d6/a2-a4
9:  rts

| (d0 = v, d1 = rows, a0, a1 set; a2 = colp; d6 as dwf_half)
| dwf_span: picks the remap of column a2's texture (hi or lo by d6.b) and
| the keep mask (d6 >> 8), then tail-calls span_half.
dwf_span:
    move.l  CP_DV(a2), d2
    moveq   #0, d3
    move.b  CP_T(a2), d3
    add.w   d3, d3
    lea     tex_boff, a3
    move.w  0(a3,d3.w), d3
    lea     tex_hi3, a3
    tst.b   d6
    beq.s   1f
    lea     tex_lo3, a3
1:  adda.w  d3, a3
    move.w  d6, d5
    lsr.w   #8, d5                      | keep mask
    movea.l a3, a2
    bra     span_half                   | (tail call)

| ---- every second column, both pixels of the byte from one texel
| (cstep 2: only even columns have cp entries; each fills one whole byte.
| Tall walls, or zero height, fall back to col_fine + span1.)
dw_mid:
    moveq   #0, d7                      | x (even)
1:  lea     cp, a2
    move.w  d7, d0
    lsl.w   #5, d0
    adda.w  d0, a2                      | a2 = &cp[x]
    tst.l   CP_TEX(a2)
    beq     9f                          | no wall here
    move.w  d7, d0                      | fb + (x >> 3) * COLBYTES + (x & 7) / 2
    lsr.w   #3, d0
    mulu.w  #COLBYTES, d0
    move.w  d7, d1
    andi.w  #7, d1
    lsr.w   #1, d1
    add.w   d1, d0
    lea     fb, a1
    adda.l  d0, a1                      | a1 = the frame buffer column
    move.w  CP_H(a2), d2
    beq.s   5f                          | zero height: span path
    cmpi.w  #MAX_SCALER_H, d2
    bhi.s   5f                          | too tall: span path
| doubling scaler registers: as the pair scalers, but a0 is the only
| texture column; a2 / a4 = its hi / lo remaps (the same texel ORed into
| both nibbles)
    movea.l CP_TEX(a2), a0              | the doubling scaler: one texel, both pixels
    moveq   #0, d0
    move.b  CP_T(a2), d0
    add.w   d0, d0
    lea     tex_boff, a6
    move.w  0(a6,d0.w), d0
    lea     tex_hi3, a2
    adda.w  d0, a2
    lea     tex_lo3, a4
    adda.w  d0, a4
    move.w  d7, d0                      | the tile column's palettes
    lsr.w   #3, d0
    mulu.w  #2*VIEW_TH, d0
    lea     tpal, a5
    adda.w  d0, a5
    lsl.w   #2, d2                      | one long per height
    lea     dscaler_tab, a6
    movea.l 0(a6,d2.w), a6
    jsr     (a6)                        | draw the column (scalers.s)
    bra.s   9f
5:  movea.l a1, a4
    bsr     col_fine                    | rows y0..y1, v0, dv
    moveq   #0, d0
    move.b  CP_Y0(a2), d0
    moveq   #0, d1
    move.b  CP_Y1(a2), d1
    sub.w   d0, d1                      | rows
    lsl.w   #2, d0
    lea     0(a4,d0.w), a0              | fb at row y0
    movea.l CP_TEX(a2), a1
    move.l  CP_V0(a2), d0
    move.l  CP_DV(a2), d2
    moveq   #0, d3
    move.b  CP_T(a2), d3
    add.w   d3, d3
    lea     tex_boff, a2
    move.w  0(a2,d3.w), d3
    lea     tex_lo3, a6
    adda.w  d3, a6
    lea     tex_hi3, a2
    adda.w  d3, a2
    bsr     span1                       | both nibbles from one texel
9:  addq.w  #2, d7                      | next even column
    cmpi.w  #VIEW_W, d7
    bcs     1b
    rts

| fb_clear: clear the frame buffer (floor and ceiling stay transparent:
| colour 0)
| Fills fb with zeros from the end down, 384 bytes a loop with movem
| (11 registers = 44 bytes per movem.l). 13312 = 34 x 384 + 256; the last
| 256 bytes are done a long at a time. Clobbers d0-d7, a0-a4.
fb_clear:
    lea     fb + VIEW_BYTES, a0
    moveq   #0, d0
    moveq   #0, d1
    moveq   #0, d2
    moveq   #0, d3
    moveq   #0, d4
    moveq   #0, d5
    moveq   #0, d6
    moveq   #0, d7
    movea.l d0, a1
    movea.l d0, a2
    movea.l d0, a3
    movea.l d0, a4
    move.w  #VIEW_BYTES / 384 - 1, d0   | 384 bytes per round (8 x 48)
    moveq   #0, d1
1:  movem.l d1-d7/a1-a4, -(a0)          | 44 bytes ... (see below)
    movem.l d1-d7/a1-a4, -(a0)
    movem.l d1-d7/a1-a4, -(a0)
    movem.l d1-d7/a1-a4, -(a0)
    movem.l d1-d7/a1-a4, -(a0)
    movem.l d1-d7/a1-a4, -(a0)
    movem.l d1-d7/a1-a4, -(a0)
    movem.l d1-d7/a1-a4, -(a0)
    movem.l d1-d4, -(a0)                | 8 x 44 + 16 + 16 = 384
    movem.l d1-d4, -(a0)
    dbra    d0, 1b
    move.w  #(VIEW_BYTES % 384) / 4 - 1, d0
2:  move.l  d1, -(a0)
    dbra    d0, 2b
    rts

| ======================================================================
| things: billboards (props, monsters, items) and wall decals
| ======================================================================

| add_billboard: world (d0.w, d1.w), sprite d2.w. Kept if its cell can be
| seen from this view and it is in front, within the view.
| A billboard is a sprite 64 units wide, standing upright and facing the
| camera. vis_a / vis_b point to 128-byte bitmaps of the 32 x 32 map
| cells (bit (cell & 7) of byte cell >> 3) whose things can be seen.
| The depth is taken 16 units nearer than the thing's centre (as the
| original does); things nearer than 4 units are dropped.
| Out: a new pthing in pt[n_pt] (n_pt + 1) with PT_D, PT_L, PT_X0 /
| PT_X1 (the columns it covers, clamped to the view), PT_SPR, PT_DECAL =
| 0 and PT_IZ8. Clobbers d0-d7, a0, a3.
add_billboard:
    cmpi.b  #MAX_PT, n_pt               | pt full?
    bcc     9f
    move.w  d0, d4                      | cell = (z >> 6) << 5 | x >> 6
    lsr.w   #6, d4
    move.w  d1, d3
    lsr.w   #6, d3
    lsl.w   #5, d3
    or.w    d3, d4
    move.w  d4, d3
    lsr.w   #3, d3                      | byte of the bitmap
    movea.l vis_a, a0
    move.b  0(a0,d3.w), d5
    movea.l vis_b, a0
    or.b    0(a0,d3.w), d5              | visible from either view
    andi.w  #7, d4
    btst    d4, d5                      | the cell's bit
    beq     9f                          | cell not visible: skip
    move.w  d2, d7                      | sprite
    bsr     to_view                     | centre in view space
    subi.w  #16, d0                     | d (16 nearer)
    cmpi.w  #4, d0
    blt     9f                          | behind or too near
    move.w  d0, d5                      | d
    move.w  d1, d6                      | l
    move.w  d6, d2                      | l - 32 > d: off to the right
    subi.w  #32, d2
    cmp.w   d5, d2
    bgt     9f
    move.w  d6, d2                      | l + 32 < -d: off to the left
    addi.w  #32, d2
    move.w  d5, d3
    neg.w   d3
    cmp.w   d3, d2
    blt     9f
    move.w  d5, d0                      | sx0 = screen_x(d, l - 32) >> 8
    move.w  d6, d1
    subi.w  #32, d1
    bsr     screen_x                    | left edge (8.8)
    asr.l   #8, d0
    move.l  d0, d4
    move.w  d5, d0                      | sx1 = (screen_x(d, l + 32) + 255) >> 8
    move.w  d6, d1
    addi.w  #32, d1
    bsr     screen_x                    | right edge (8.8)
    addi.l  #255, d0
    asr.l   #8, d0
    tst.l   d4
    bpl.s   1f
    moveq   #0, d4                      | clamp x0 >= 0
1:  cmpi.l  #VIEW_W, d0
    ble.s   2f
    move.l  #VIEW_W, d0                 | clamp x1 <= VIEW_W
2:  cmp.l   d4, d0
    ble     9f                          | no columns left
    moveq   #0, d1
    move.b  n_pt, d1
    addq.b  #1, n_pt
    mulu.w  #PT_SIZE, d1
    lea     pt, a3
    adda.w  d1, a3                      | a3 = the new pthing
    move.w  d5, PT_D(a3)
    move.w  d6, PT_L(a3)
    move.w  d4, PT_X0(a3)
    move.w  d0, PT_X1(a3)
    move.b  d7, PT_SPR(a3)
    clr.b   PT_DECAL(a3)
    move.l  #1 << 28, d0                | iz8 = 2^28 / d
    move.w  d5, d1
    bsr     udiv16
    move.l  d0, PT_IZ8(a3)
9:  rts

| add_decal: centre (d0.w, d1.w), sprite d2.w, info d3.w (which way it
| faces). Its quad is 64 wide, flat on the wall.
| Info bits (from the scenery table, r_scenery's info >> 16): bit 9
| flips the direction (s = -1, else +1); bit 3 or bit 4: the quad runs
| along x (A and B at x -+ 32 s, in opposite orders); bit 6 or neither:
| along z. The A -> B order sets which side shows (draw_decal treats the
| quad as a one-sided wall).
| Kept unless its centre is more than 32 units behind the camera or
| outside the 90 degree view by more than 32 units.
| Out: a new pthing (PT_AX .. PT_BZ, PT_DECAL = 1, PT_SPR, PT_X0 = 0,
| PT_X1 = VIEW_W, PT_IZ8 from the centre's depth). Clobbers d0-d7, a3.
add_decal:
    cmpi.b  #MAX_PT, n_pt               | pt full?
    bcc     9f
    move.w  d2, d7                      | sprite
    move.w  d0, d4                      | x
    move.w  d1, d5                      | z
    moveq   #32, d6                     | 32 * sgn
    btst    #9, d3
    beq.s   1f
    moveq   #-32, d6
1:  moveq   #0, d1
    move.b  n_pt, d1
    mulu.w  #PT_SIZE, d1
    lea     pt, a3
    adda.w  d1, a3                      | a3 = pt[n_pt] (kept only if seen)
    move.b  #1, PT_DECAL(a3)            | a decal: 1 one-sided, 2 two-sided
    btst    #11, d3                     | info bit $8000000 (>> 16): seen from both sides
    beq.s   0f                          | (the original draws it from behind with A and B swapped)
    move.b  #2, PT_DECAL(a3)
0:  btst    #3, d3
    beq.s   2f
    move.w  d4, d0                      | facing $08: ax = x - 32s, bx = x + 32s
    sub.w   d6, d0
    move.w  d0, PT_AX(a3)
    move.w  d5, PT_AZ(a3)
    move.w  d4, d0
    add.w   d6, d0
    move.w  d0, PT_BX(a3)
    move.w  d5, PT_BZ(a3)
    bra.s   5f
2:  btst    #4, d3
    beq.s   3f
    move.w  d4, d0                      | $10: ax = x + 32s, bx = x - 32s
    add.w   d6, d0
    move.w  d0, PT_AX(a3)
    move.w  d5, PT_AZ(a3)
    move.w  d4, d0
    sub.w   d6, d0
    move.w  d0, PT_BX(a3)
    move.w  d5, PT_BZ(a3)
    bra.s   5f
3:  btst    #6, d3
    beq.s   4f
    move.w  d4, PT_AX(a3)               | $40: az = z - 32s, bz = z + 32s
    move.w  d5, d0
    sub.w   d6, d0
    move.w  d0, PT_AZ(a3)
    move.w  d4, PT_BX(a3)
    move.w  d5, d0
    add.w   d6, d0
    move.w  d0, PT_BZ(a3)
    bra.s   5f
4:  move.w  d4, PT_AX(a3)               | else: az = z + 32s, bz = z - 32s
    move.w  d5, d0
    add.w   d6, d0
    move.w  d0, PT_AZ(a3)
    move.w  d4, PT_BX(a3)
    move.w  d5, d0
    sub.w   d6, d0
    move.w  d0, PT_BZ(a3)
5:  move.w  d4, d0                      | its centre in view space
    move.w  d5, d1
    bsr     to_view
    cmpi.w  #-32, d0
    blt     9f                          | well behind the camera
    move.w  d0, d5                      | dm
    move.w  d1, d2                      | lm - 32 > dm + 32
    subi.w  #32, d2
    move.w  d5, d3
    addi.w  #32, d3
    cmp.w   d3, d2
    bgt     9f
    move.w  d1, d2                      | lm + 32 < -dm - 32
    addi.w  #32, d2
    move.w  d5, d3
    neg.w   d3
    subi.w  #32, d3
    cmp.w   d3, d2
    blt     9f
    addq.b  #1, n_pt                    | (PT_DECAL set above: 1 or 2)
    move.b  d7, PT_SPR(a3)
    clr.w   PT_X0(a3)
    move.w  #VIEW_W, PT_X1(a3)
    move.l  #1 << 28, d0                | iz8 = dm > 1 ? 2^28 / dm : 2^28
    cmpi.w  #1, d5
    ble.s   6f
    move.w  d5, d1
    ext.l   d1
    bsr     udiv32
6:  move.l  d0, PT_IZ8(a3)
9:  rts

| sort_things: pt_order, far (small iz8) first; stable insertion sort
| (things are drawn in this order, so nearer ones paint over farther).
| In: pt, n_pt. Out: pt_order[0 .. n_pt-1] = indices into pt.
| Clobbers d0-d5, d7, a0, a1.
sort_things:
    lea     pt_order, a0
    moveq   #0, d0
    moveq   #0, d7
    move.b  n_pt, d7
    beq.s   9f                          | no things
1:  move.b  d0, 0(a0,d0.w)              | start as 0, 1, 2, ...
    addq.w  #1, d0
    cmp.w   d7, d0
    bcs.s   1b
    lea     pt, a1
    moveq   #1, d1                      | i
2:  cmp.w   d7, d1
    bcc.s   9f                          | all inserted
    moveq   #0, d2
    move.b  0(a0,d1.w), d2              | v
    move.w  d2, d3
    mulu.w  #PT_SIZE, d3
    move.l  PT_IZ8(a1,d3.w), d4         | its iz8
    move.w  d1, d5                      | j
3:  tst.w   d5
    beq.s   4f
    moveq   #0, d3
    move.b  -1(a0,d5.w), d3
    mulu.w  #PT_SIZE, d3
    cmp.l   PT_IZ8(a1,d3.w), d4         | pt[order[j-1]].iz8 > iz8: move it up
    bge.s   4f
    move.b  -1(a0,d5.w), 0(a0,d5.w)
    subq.w  #1, d5
    bra.s   3b
4:  move.b  d2, 0(a0,d5.w)
    addq.w  #1, d1
    bra.s   2b
9:  rts

| ---- sprite columns
| Sprite data: each sprite is a 64 x 64 canvas, stored column by column;
| r_spr_cols gives per column (2 bytes) the first opaque row (top) and
| how many rows are stored (len, 0 = empty column); the stored bytes are
| colour indices, $80 and up (in practice $FF) = transparent. A sprite is
| 64 world units tall and wide, standing on the floor.

| sprspan: one pixel of each byte: a0 = frame buffer, a1 = data, a2 =
| remap, d0 = r, d2 = dr, d1.w = rows, d5.b = keep mask; texel $FF is
| transparent. A tile row at a time (see span1). Uses d0-d6, a3, a4.
| (The test is bmi: any texel with bit 7 set is skipped.)
sprspan:
    tst.w   d1
    ble.s   9f                          | no rows
    bsr     seg_init                    | a3 = tpal entry, d4 = first run
    movea.w d1, a4                      | rows left
    move.w  d4, d1
    swap    d0                          | r: fraction : integer
    swap    d2
    moveq   #0, d3
    move.w  d2, d3                      | integer step
    clr.w   d2                          | fraction : 0
0:  move.w  (a3)+, d4                   | the tile's palette << 8
    cmpa.w  d1, a4
    bcc.s   3f
    move.w  a4, d1                      | last run: the rows left
3:  suba.w  d1, a4
    subq.w  #1, d1
1:  move.b  0(a1,d0.w), d4              | texel
    bmi.s   2f                          | transparent: leave the pixel
    move.b  (a0), d6
    and.b   d5, d6                      | keep the other pixel
    or.b    0(a2,d4.w), d6              | this pixel's slot
    move.b  d6, (a0)
2:  addq.l  #4, a0                      | next row
    add.l   d2, d0                      | r += dr
    addx.w  d3, d0
    dbra    d1, 1b
    moveq   #8, d1                      | later runs: a tile (8 rows)
    cmpa.w  #0, a4
    bne.s   0b
9:  rts

| sprspan2: both pixels of each byte (a2 / a6 = remaps slot << 4 / slot).
| Uses d0-d5, a3, a4.
| (Other registers as sprspan; a transparent texel leaves the byte.)
sprspan2:
    tst.w   d1
    ble.s   9f
    bsr     seg_init                    | a3 = tpal entry, d4 = first run
    movea.w d1, a4
    move.w  d4, d1
    swap    d0
    swap    d2
    moveq   #0, d3
    move.w  d2, d3
    clr.w   d2
0:  move.w  (a3)+, d4                   | the tile's palette << 8
    cmpa.w  d1, a4
    bcc.s   3f
    move.w  a4, d1
3:  suba.w  d1, a4
    subq.w  #1, d1
1:  move.b  0(a1,d0.w), d4              | texel
    bmi.s   2f                          | transparent
    move.b  0(a2,d4.w), d5              | left pixel
    or.b    0(a6,d4.w), d5              | right pixel (the same texel)
    move.b  d5, (a0)
2:  addq.l  #4, a0
    add.l   d2, d0
    addx.w  d3, d0
    dbra    d1, 1b
    moveq   #8, d1
    cmpa.w  #0, a4
    bne.s   0b
9:  rts

    .bss
sc_dh:      .space  4                   | draw_sprite_col: canvas rows per half
                                        | screen row, 16.16 (d / 2F)
sc_x:       .space  2                   | draw_sprite_col's arguments
sc_d:       .space  2
sc_c:       .space  2
sc_spr:     .space  2
sc_top:     .space  4                   | top << 16, (top + len) << 16
sc_bot:     .space  4
sc_y:       .space  2                   | (not used)
sc_both:    .space  1
    .even
    .text

| draw_sprite_col: sprite d3.w's column d2.w at screen x d0.w, depth d1.w
| (world units, 1..32767), d4.b = 0 one pixel, 1 both pixels of the byte.
| Rows: canvas row r = 28 + (y + 0.5 - HALF_H) d / F (64 rows a canvas).
| (The canvas top is 64 units up, 28 above the eye; r is in 16.16 and
| steps by dr = d / F per screen row = 2 dh, dh = (d << 15) / F; at the
| centre of row y, r = 28 + (2y + 1 - 2 HALF_H) dh. With F = 128, dh was
| d << 8.) The first and last screen rows are first estimated with a
| division, then nudged a row at a time until they match the exact rule:
| row y is drawn when its r falls in top .. top + len. No depth test
| here: the callers do it per column.
| Callers: draw_billboard (via db_col) and draw_decal.
| Clobbers d0-d7, a0-a2 (a3, a4, a6 are kept).
draw_sprite_col:
    move.w  d0, sc_x
    move.w  d1, sc_d
    move.w  d2, sc_c
    move.w  d3, sc_spr
    move.b  d4, sc_both
    movea.l r_spr_cols, a0              | top, len of the column
    lsl.w   #7, d3                      | 128 bytes per sprite
    add.w   d2, d2                      | 2 bytes per column
    add.w   d2, d3
    moveq   #0, d6
    move.b  0(a0,d3.w), d6              | top
    moveq   #0, d7
    move.b  1(a0,d3.w), d7              | len
    beq     9f                          | empty column
    move.l  d6, d0
    swap    d0
    clr.w   d0
    move.l  d0, sc_top                  | top in 16.16
    move.l  d6, d0
    add.w   d7, d0
    swap    d0
    clr.w   d0
    move.l  d0, sc_bot                  | top + len in 16.16
    moveq   #0, d0                      | dh = (d << 15) / F (F)
    move.w  sc_d, d0
    swap    d0
    lsr.l   #1, d0                      | d << 15
    move.w  #HALF_W, d1
    bsr     udiv16
    move.l  d0, sc_dh
    add.l   d0, d0                      | dr = 2 dh
    move.l  d0, -(sp)                   | (dr kept on the stack)
    | y = (((top - 28) * 2F) / d + 2 HALF_H - 1) >> 1, at least 0
    | (the row whose centre is nearest the canvas row top)
    move.w  d6, d0
    subi.w  #28, d0
    muls.w  #2*HALF_W, d0               | (F)
    move.w  sc_d, d1
    ext.l   d1
    bsr     sdiv32
    addi.l  #2*HALF_H-1, d0
    asr.l   #1, d0
    bpl.s   1f
    moveq   #0, d0
1:  move.l  d0, d5                      | y
    | r16 = (28 << 16) + (2y + 1 - 2 HALF_H) dh
    add.l   d0, d0
    subi.l  #2*HALF_H-1, d0
    move.l  sc_dh, d1
    bsr     mul32
    addi.l  #28 << 16, d0
    move.l  d0, d6                      | r16
    move.l  (sp), d4                    | dr
    move.l  sc_top, d3
2:  tst.l   d5                          | back up while the row above is still on the canvas
    ble.s   3f
    move.l  d6, d0
    sub.l   d4, d0
    cmp.l   d3, d0
    blt.s   3f
    move.l  d0, d6
    subq.l  #1, d5
    bra.s   2b
3:  cmpi.l  #VIEW_H, d5                 | forward while above the canvas
    bge.s   4f
    cmp.l   d3, d6
    bge.s   4f
    add.l   d4, d6
    addq.l  #1, d5
    bra.s   3b
4:  | y1 = (((top + len - 28) * 2F) / d + 2 HALF_H - 1) >> 1, at most VIEW_H
    move.w  sc_bot, d0                  | (top + len, sc_bot's high word)
    subi.w  #28, d0
    muls.w  #2*HALF_W, d0               | (F)
    move.w  sc_d, d1
    ext.l   d1
    bsr     sdiv32
    addi.l  #2*HALF_H-1, d0
    asr.l   #1, d0
    cmpi.l  #VIEW_H, d0
    ble.s   5f
    move.l  #VIEW_H, d0
5:  move.l  d0, d7                      | y1
    | r1 = r16 + (y1 - y) dr
    sub.l   d5, d0
    move.l  (sp), d1
    bsr     mul32
    add.l   d6, d0
    move.l  (sp), d4
    move.l  sc_bot, d3
6:  cmp.l   d5, d7                      | back up while the row above is past the end
    ble.s   7f
    move.l  d0, d1
    sub.l   d4, d1
    cmp.l   d3, d1
    blt.s   7f
    move.l  d1, d0
    subq.l  #1, d7
    bra.s   6b
7:  cmpi.l  #VIEW_H, d7                 | forward while before the end
    bge.s   8f
    cmp.l   d3, d0
    bge.s   8f
    add.l   d4, d0
    addq.l  #1, d7
    bra.s   7b
8:  move.l  (sp)+, d2                   | dr
    cmp.l   d5, d7
    ble     9f                          | no rows on screen
    sub.w   d5, d7                      | rows
    | data = r_spr_data + r_spr_off[spr] + r_spr_coff[spr * 64 + c] - top
    | (minus top, so the data can be indexed by the canvas row r itself)
    move.w  sc_spr, d0
    move.w  d0, d1
    lsl.w   #2, d0
    movea.l r_spr_off, a0
    movea.l r_spr_data, a1
    adda.l  0(a0,d0.w), a1              | the sprite's data
    lsl.w   #7, d1                      | (spr * 64 + c) * 2
    move.w  sc_c, d0
    add.w   d0, d0
    add.w   d0, d1
    movea.l r_spr_coff, a0
    moveq   #0, d0
    move.w  0(a0,d1.w), d0
    adda.l  d0, a1                      | the column's data
    move.l  sc_top, d0
    swap    d0
    suba.w  d0, a1
    | the frame buffer: fb + (x >> 3) * COLBYTES + (x & 7) >> 1 + y * 4
    move.w  sc_x, d0
    move.w  d0, d1
    lsr.w   #3, d0
    mulu.w  #COLBYTES, d0
    andi.w  #7, d1
    lsr.w   #1, d1
    add.w   d1, d0
    lsl.w   #2, d5
    add.w   d5, d0
    lea     fb, a0
    adda.l  d0, a0
    move.l  d6, d0                      | r16
    move.w  d7, d1                      | rows
    movem.l a3-a4/a6, -(sp)
    move.w  sc_spr, d3
    add.w   d3, d3
    lea     spr_boff, a2
    move.w  0(a2,d3.w), d3              | the sprite's remap offset
    move.b  sc_both, d4
    beq.s   12f
    lea     spr_hi3, a2                 | both pixels
    adda.w  d3, a2
    lea     spr_lo3, a6
    adda.w  d3, a6
    bsr     sprspan2                    | draw the whole byte
    bra.s   14f
12: btst    #0, sc_x+1                  | one pixel: odd x the low nibble
    beq.s   13f
    lea     spr_lo3, a2
    adda.w  d3, a2
    move.b  #0xF0, d5                   | keep the left pixel
    bsr     sprspan                     | draw the right pixel
    bra.s   14f
13: lea     spr_hi3, a2
    adda.w  d3, a2
    move.b  #0x0F, d5                   | keep the right pixel
    bsr     sprspan                     | draw the left pixel
14: movem.l (sp)+, a3-a4/a6
9:  rts

| bb_col: d0.w = the sprite column at screen x d0.w for billboard a3:
| floor((2x + 1 - 2F) d / 2F) - l + 32
| (the lateral position of the pixel centre at the billboard's depth,
| relative to its left edge; 0..63 is on the sprite). Clobbers d0 only.
| (With F = 128 the division was >> 8; divs.w rounds towards zero, so a
| negative quotient with a remainder is taken one lower. The quotient
| fits a word: |2x + 1 - 2F| < 2F.)
bb_col:
    add.w   d0, d0
    subi.w  #2 * HALF_W - 1, d0         | 2x + 1 - 2F
    muls.w  PT_D(a3), d0
    divs.w  #2 * HALF_W, d0             | / 2F (F)
    swap    d0                          | the remainder
    tst.w   d0
    bpl.s   1f                          | (>= 0: already rounded down)
    swap    d0
    subq.w  #1, d0                      | negative: round down
    bra.s   2f
1:  swap    d0
2:  sub.w   PT_L(a3), d0
    addi.w  #32, d0
    rts

| draw_billboard: a3 = the thing
| Draws a billboard column by column between PT_X0 and PT_X1, skipping
| columns where a wall is nearer (col_iz > PT_IZ8; equal counts as the
| thing in front). Clobbers d0-d7, a0-a2.
draw_billboard:
    cmpi.b  #1, cstep
    beq.s   db_full
    | every second / fourth column: the column at its middle
    | (cstep is 1 or 2 today, so this is the cstep 2 path; each drawn
    | column fills a whole byte, with both_mode set)
    moveq   #0, d7
    move.b  cstep, d7
    move.w  d7, d0
    subq.w  #1, d0
    not.w   d0
    move.w  PT_X0(a3), d6
    and.w   d0, d6                      | x
1:  cmp.w   PT_X1(a3), d6
    bge.s   9f                          | past the right edge
    move.w  d7, d0
    lsr.w   #1, d0
    add.w   d6, d0                      | sample the middle of the cell
    bsr.s   bb_col
    cmpi.w  #63, d0
    bhi.s   2f                          | (unsigned: also < 0)
    move.w  d6, d1
    lsl.w   #2, d1
    lea     col_iz, a0
    move.l  PT_IZ8(a3), d2
    cmp.l   0(a0,d1.w), d2              | behind the wall in this column?
    blt.s   2f
    move.w  d0, d2
    move.w  d6, d0
    move.w  PT_D(a3), d1
    moveq   #0, d3
    move.b  PT_SPR(a3), d3
    move.b  both_mode, d4
    movem.l d6-d7/a3, -(sp)
    bsr     draw_sprite_col             | draw this sprite column
    movem.l (sp)+, d6-d7/a3
2:  add.w   d7, d6
    bra.s   1b
9:  rts
| full detail: each column on its own, but an even column and the next
| one are drawn as one byte when both are visible and show the same
| sprite column (common when the sprite is magnified)
db_full:
    move.w  PT_X0(a3), d6               | x
1:  cmp.w   PT_X1(a3), d6
    bge     9f
    move.w  d6, d0
    bsr   bb_col                        | sprite column at x
    move.w  d0, d7                      | c
    moveq   #0, d5                      | vis
    cmpi.w  #63, d0
    bhi.s   2f                          | off the sprite
    move.w  d6, d1
    lsl.w   #2, d1
    lea     col_iz, a0
    move.l  PT_IZ8(a3), d2
    cmp.l   0(a0,d1.w), d2
    blt.s   2f                          | hidden by a wall
    moveq   #1, d5
2:  | an even column whose neighbour shows the same: both pixels at once
    btst    #0, d6
    bne.s   3f
    move.w  d6, d0
    addq.w  #1, d0
    cmp.w   PT_X1(a3), d0
    bge.s   3f
    tst.b   d5
    beq.s   3f
    move.w  d6, d1
    lsl.w   #2, d1
    lea     col_iz, a0
    move.l  PT_IZ8(a3), d2
    cmp.l   4(a0,d1.w), d2              | x + 1 hidden by a wall?
    blt.s   3f
    bsr   bb_col                      | (d0 = x + 1)
    cmp.w   d7, d0
    bne.s   3f
    moveq   #1, d4
    bsr.s   db_col                      | draw the byte (both pixels)
    addq.w  #2, d6
    bra.s   1b
3:  tst.b   d5
    beq.s   4f
    moveq   #0, d4
    bsr.s   db_col                      | draw one pixel
4:  addq.w  #1, d6
    bra.s   1b
9:  rts
| (column x d6, sprite column d7, both d4)
| db_col: draw_sprite_col for billboard a3, keeping d6, d7, a3.
db_col:
    move.w  d6, d0
    move.w  PT_D(a3), d1
    move.w  d7, d2
    moveq   #0, d3
    move.b  PT_SPR(a3), d3
    movem.l d6-d7/a3, -(sp)
    bsr     draw_sprite_col
    movem.l (sp)+, d6-d7/a3
    rts

    .bss
dc_x:       .space  2                   | draw_decal: current column
dc_x1:      .space  2                   |   end column (exclusive)
dc_iz:      .space  4                   |   iz at dc_x (2^28 / depth)
dc_diz:     .space  4                   |   iz step per drawn column
    .text

| draw_decal: a3 = the thing: its quad as a wall, each column's depth and
| sprite column worked out exactly
| The quad A-B is projected like a wall (to_view, clip_ab, the vwall in
| dc_vw); per drawn column the sprite column u = 64 m / num and the depth
| F c / num are computed, and draw_sprite_col draws it. A column is
| hidden when the wall's iz there beats the decal's iz plus 1/64 (so a
| decal lying on a wall wins the tie). Keeps a3. Clobbers d0-d7, a0-a2.
draw_decal:
    move.l  a3, -(sp)                   | (the pthing, at (sp) throughout)
    move.w  PT_AX(a3), d0
    move.w  PT_AZ(a3), d1
    bsr     to_view                     | A in view space
    move.w  d0, pw_da
    move.w  d1, pw_la
    movea.l (sp), a3
    move.w  PT_BX(a3), d0
    move.w  PT_BZ(a3), d1
    bsr     to_view                     | B in view space
    move.w  d0, pw_db
    move.w  d1, pw_lb
    | a two-sided decal seen from behind (c = dl da - dd la < 0: it faces
    | away) is drawn from B to A instead, like the original (its picture
    | mirrored, as the back of a flat thing would look)
    movea.l (sp), a3
    cmpi.b  #2, PT_DECAL(a3)
    bne.s   7f                          | one-sided: as it is
    move.w  pw_lb, d2                   | dl = lb - la
    sub.w   pw_la, d2
    muls.w  pw_da, d2                   | dl da
    move.w  pw_db, d3                   | dd = db - da
    sub.w   pw_da, d3
    muls.w  pw_la, d3                   | dd la
    sub.l   d3, d2                      | c
    bge.s   7f                          | facing the camera (or edge-on)
    move.w  pw_da, d0                   | swap A and B
    move.w  pw_db, pw_da
    move.w  d0, pw_db
    move.w  pw_la, d0
    move.w  pw_lb, pw_la
    move.w  d0, pw_lb
7:  bsr     clip_ab                     | near clip, columns d6..d7
    beq     dd_out                      | all behind the camera
    lea     dc_vw, a3                   | the quad as a wall
    move.w  pw_da, d0
    move.w  d0, VW_DA(a3)
    move.w  pw_la, d1
    move.w  d1, VW_LA(a3)
    move.w  pw_db, d2
    sub.w   d0, d2
    move.w  d2, VW_DD(a3)
    move.w  pw_lb, d3
    sub.w   d1, d3
    move.w  d3, VW_DL(a3)
    muls.w  d0, d3
    muls.w  d1, d2
    sub.l   d2, d3
    move.l  d3, VW_C(a3)
    beq     dd_out                      | edge-on
    cmp.w   d6, d7
    ble     dd_out                      | no columns
    move.w  d6, dc_x
    move.w  d7, dc_x1
    move.w  d6, d0                      | iz at x0
    bsr     wall_iz
    move.l  d0, dc_iz
    clr.l   dc_diz
    move.w  dc_x1, d0
    sub.w   dc_x, d0
    cmpi.w  #1, d0
    ble.s   1f                          | one column: no step
    move.w  dc_x1, d0
    subq.w  #1, d0
    bsr     wall_iz                     | iz at x1 - 1
    sub.l   dc_iz, d0
    move.w  dc_x1, d1
    subq.w  #1, d1
    sub.w   dc_x, d1
    ext.l   d1
    bsr     sdiv32                      | diz per column
    move.l  d0, dc_diz
1:  | every cstep-th column: start on one
    moveq   #0, d7
    move.b  cstep, d7
    move.w  d7, d6
    subq.w  #1, d6                      | mask
    move.w  dc_x, d0
    move.l  dc_iz, d1
2:  move.w  d0, d2
    and.w   d6, d2
    beq.s   3f                          | on a multiple of cstep
    addq.w  #1, d0
    add.l   dc_diz, d1
    bra.s   2b
3:  move.w  d0, dc_x
    move.l  d1, dc_iz
    move.l  dc_diz, d0                  | the step: diz * cstep
    move.l  d7, d1
    bsr     mul32
    move.l  d0, dc_diz
| one drawn column a pass
dd_loop:
    move.w  dc_x, d6
    cmp.w   dc_x1, d6
    bge     dd_out                      | past the end
    move.l  dc_iz, d5
    ble     dd_next                     | behind
    move.l  d5, d0                      | iz + iz >> 6 < col_iz: hidden (decals win ties)
    asr.l   #6, d0
    add.l   d5, d0
    move.w  d6, d1
    lsl.w   #2, d1
    lea     col_iz, a0
    cmp.l   0(a0,d1.w), d0
    blt     dd_next
    lea     dc_vw, a3
    | num = F dl - (x - F) dd
    move.w  d6, d4
    subi.w  #HALF_W, d4
    move.w  d4, d0
    muls.w  VW_DD(a3), d0
    move.w  VW_DL(a3), d5
    muls.w  #HALF_W, d5                 | (F)
    sub.l   d0, d5                      | num
    | u = shdiv((x - F) da - F la, num, 6)
    move.w  d4, d0
    muls.w  VW_DA(a3), d0
    move.w  VW_LA(a3), d1
    muls.w  #HALF_W, d1                 | (F)
    sub.l   d1, d0
    move.l  d5, d1
    moveq   #6, d4
    bsr     shdiv                       | u = (m << 6) / num
    tst.l   d0
    bmi     dd_next                     | off the sprite (left)
    cmpi.l  #63, d0
    bgt     dd_next                     | off the sprite (right)
    move.w  d0, -(sp)
    | d = (F c) / num
    move.l  VW_C(a3), d0
    moveq   #HALF_W, d1                 | (F; a decal's c is small, F c fits)
    bsr     mul32
    move.l  d5, d1
    bsr     sdiv32
    move.w  (sp)+, d2                   | d2 = sprite column
    cmpi.l  #1, d0
    blt.s   dd_next                     | depth out of range: skip
    cmpi.l  #32000, d0
    bgt.s   dd_next
    move.w  d0, d1                      | depth
    move.w  d6, d0                      | screen x
    movea.l (sp), a3
    moveq   #0, d3
    move.b  PT_SPR(a3), d3
    move.b  both_mode, d4
    bsr     draw_sprite_col             | draw this column of the decal
dd_next:
    moveq   #0, d0
    move.b  cstep, d0
    add.w   d0, dc_x                    | next drawn column
    move.l  dc_diz, d0
    add.l   d0, dc_iz
    bra     dd_loop
dd_out:
    movea.l (sp)+, a3
    rts

| ======================================================================
| a picture
| ======================================================================

| ticks: d0.l = time in lines (frames x 262 + the line counter). Uses d1.
| (frame_count from vdp.c; the VDP's HV counter at $C00008, high byte =
| the line. Read again if a frame interrupt came in between. 262 lines a
| frame on NTSC; the V counter's jump near the end of the frame makes
| this a rough measure, good enough for profiling.)
ticks:
1:  move.w  frame_count, d1
    move.w  0xC00008, d0                | HV counter
    lsr.w   #8, d0                      | V (line)
    cmp.w   frame_count, d1             | a new frame meanwhile?
    bne.s   1b                          | yes: read again
    mulu.w  #262, d1
    andi.l  #0xFF, d0
    add.l   d1, d0
    rts

| prof_mark: prof[d7.w] = the time since the last mark
| Also stores the phase number in rphase (its high byte) for the tests,
| and restarts the clock (prof_t0). Clobbers d0, d1, d7, a0.
prof_mark:
    move.b  d7, rphase                  | (the tests: the phase just done)
    bsr.s   ticks
    move.l  d0, d1
    sub.l   prof_t0, d1                 | lines since the last mark
    move.l  d0, prof_t0
    lea     prof, a0
    add.w   d7, d7
    move.w  d1, 0(a0,d7.w)
    rts

    .bss
rc_flags:   .space  4                   | render_core's arguments
                                        | (the line flags: byte per line,
                                        | bit 0 = open)
rc_pvs_a:   .space  4                   | the two PVS lists (count, ids)
rc_pvs_b:   .space  4
rc_objs:    .space  4                   | the C robj array
rc_nobjs:   .space  2                   | its length
    .text

| render_core: the picture of the camera set up; rc_*, vis_a / vis_b,
| keep_mode and cstep set; the area's palettes chosen (area_select)
| Called by render_view and render_between once the camera (set_camera)
| and the arguments are in place. Does the whole picture: projection,
| things, tile palettes, drawing, name table and the queued upload.
| prof[] phases (lines each): 0 walls projected, 1 things and tile
| palettes, 6 frame buffer cleared, 3 walls drawn, 4 things drawn, 7 name
| table built, 5 upload queued; prof+14 (slot 7, reused) counts scaler
| pairs during draw_walls, before slot 7's mark overwrites it.
| Clobbers d0-d7, a0-a6 (the C entry points save them).
render_core:
    move.w  frame_count, frame0         | for render_frames
    bsr.s   ticks
    move.l  d0, prof_t0                 | start the profiling clock
    tst.b   inited
    bne.s   1f
    st      inited                      | first picture: draw into buffer 1
    move.b  #1, back_buf                | (buffer 0 is on screen)
1:  cmpi.b  #2, keep_mode
    beq     99f                         | palette only: area_select did it
    move.b  cstep, d0                   | how draw_sprite_col draws for this detail
    moveq   #0, d1
    cmpi.b  #2, d0
    bne.s   3f
    moveq   #1, d1                      | low detail: whole bytes
3:  move.b  d1, both_mode
    movea.l rc_flags, a0
    movea.l rc_pvs_a, a1
    movea.l rc_pvs_b, a2
    bsr     project_walls               | walls, col_iz, col_w
    bsr     column_params               | per column height and texture
    moveq   #0, d7
    bsr   prof_mark                     | phase 0 done
    | things: the scenery, then the level's entities
    | r_scenery: 5 words per entry (x, z, sprite, info, cell); cell >= 0
    | is a bit number in the game state's S_STATIC_HIDDEN bitmap (set when
    | a script hid the prop); -1 = always shown. info bits 3-6 ($78) mark
    | a decal, else a billboard.
    clr.b   n_pt
    moveq   #0, d7                      | scenery index
5:  cmp.w   r_n_scenery, d7
    bcc.s   7f                          | all scenery done
    movea.l r_scenery, a0               | x, z, sprite, info, cell (words)
    move.w  d7, d0
    mulu.w  #10, d0
    adda.w  d0, a0
    move.w  8(a0), d0                   | a prop HIDE took away?
    bmi.s   51f                         | (no cell: always shown)
    move.w  d0, d1
    lsr.w   #3, d1                      | byte of the bitmap
    lea     S+S_STATIC_HIDDEN, a1
    btst    d0, 0(a1,d1.w)              | (bit number mod 8 on memory)
    beq.s   51f
    addq.w  #1, d7                      | hidden: skip it
    bra.s   5b
51: move.w  (a0)+, d0                   | x
    move.w  (a0)+, d1                   | z
    move.w  (a0)+, d2
    andi.w  #0xFF, d2                   | sprite slot
    move.w  (a0), d3                    | info
    move.w  d7, -(sp)
    move.w  d3, d4
    andi.w  #0x78, d4
    beq.s   6f
    bsr     add_decal                   | a decal on a wall
    bra.s   61f
6:  bsr     add_billboard               | a prop
61: move.w  (sp)+, d7
    addq.w  #1, d7
    bra.s   5b
| then the C side's things (monsters, items...): struct robj, 6 bytes
7:  moveq   #0, d7
8:  cmp.w   rc_nobjs, d7
    bcc.s   9f
    movea.l rc_objs, a0
    move.w  d7, d0
    mulu.w  #6, d0
    adda.w  d0, a0
    move.w  (a0), d0                    | x
    move.w  2(a0), d1                   | z
    moveq   #0, d2
    move.b  4(a0), d2                   | sprite
    move.w  d7, -(sp)
    bsr     add_billboard
    move.w  (sp)+, d7
    addq.w  #1, d7
    bra.s   8b
9:  bsr     sort_things                 | far to near
    bsr     tile_pals                   | each tile's palette
    moveq   #1, d7
    bsr     prof_mark
    jsr     vdp_dma_wait                | (the last picture's upload: done by now?)
                                        | (fb is the upload's source, so
                                        | it must be sent before clearing)
    bsr     fb_clear
    moveq   #6, d7
    bsr     prof_mark
    clr.w   prof+14                     | scaler pair counter
    bsr     draw_walls
    moveq   #3, d7
    bsr     prof_mark
    moveq   #0, d7                      | things, far to near
16: cmp.b   n_pt, d7
    bcc.s   18f
    lea     pt_order, a0
    moveq   #0, d0
    move.b  0(a0,d7.w), d0
    mulu.w  #PT_SIZE, d0
    lea     pt, a3
    adda.w  d0, a3                      | a3 = the next thing
    move.w  d7, -(sp)
    tst.b   PT_DECAL(a3)
    beq.s   17f
    bsr     draw_decal
    bra.s   171f
17: bsr     draw_billboard
171: move.w (sp)+, d7
    addq.w  #1, d7
    bra.s   16b
18: moveq   #4, d7
    bsr     prof_mark
    bsr     scan_cols                   | which tile rows hold pixels
    bsr     build_nt                    | the name table for them
    moveq   #7, d7
    bsr     prof_mark
    | wait for any earlier flip; then the interrupts upload into the hidden
    | buffer the picture's tiles that have something in them and its name
    | table and flip, with the area's palettes, while the next picture is
    | worked out (up to its fb_clear)
19: cmpi.b  #0xFF, pending_view_buf     | an earlier flip still pending?
    bne.s   19b                         | wait for the frame interrupt
    lea     area_pal, a0                | the area's 3 palettes (area.c)
    lea     pending_cram, a1            | applied with the flip (vdp.c)
    moveq   #47, d0                     | 48 colours
21: move.w  (a0)+, (a1)+
    dbra    d0, 21b
    st      dq_hold                     | (all of it queued first)
| one DMA per tile column: its tiles r0 .. r1-1 (tiles are column-major
| in VRAM, so they are contiguous; 32 bytes = 16 words a tile). Empty
| columns send nothing: build_nt points their tiles at TILE_BLANK.
| vdp_dma_queue(src, vram, words) is C: arguments pushed right to left
| as longs, removed by the caller.
    moveq   #0, d7                      | tile column
23: lea     col_r0, a0
    moveq   #0, d0
    move.b  0(a0,d7.w), d0
    lea     col_r1, a0
    moveq   #0, d1
    move.b  0(a0,d7.w), d1
    sub.w   d0, d1
    ble.s   24f                         | nothing drawn in this column
    lsl.w   #4, d1                      | words
    move.l  d1, -(sp)
    move.w  d7, d2                      | VRAM: (x * VIEW_TH + r0) * 32 of the buffer
    mulu.w  #VIEW_TH, d2
    add.w   d0, d2
    lsl.l   #5, d2
    tst.b   back_buf
    beq.s   20f
    addi.l  #VRAM_BUF1, d2
20: move.l  d2, -(sp)
    move.w  d7, d2                      | fb + x * COLBYTES + r0 * 32
    mulu.w  #COLBYTES, d2
    lsl.w   #5, d0
    add.w   d0, d2
    lea     fb, a0
    adda.l  d2, a0
    move.l  a0, -(sp)
    jsr     vdp_dma_queue               | queue this column's tiles
    lea     12(sp), sp
24: addq.w  #1, d7
    cmpi.w  #VIEW_TW, d7                | every tile column
    bcs.s   23b
| the name table: the view's VIEW_TH whole rows (NT_WORDS words, the
| border columns blank) into the hidden buffer's plane A table, from
| screen row VIEW_ROW0 (64 bytes a 32-entry row)
    move.l  #NT_WORDS, -(sp)
    move.l  #VRAM_PLANE_A0 + VIEW_ROW0 * 2 * NT_W, d0
    tst.b   back_buf
    beq.s   22f
    move.l  #VRAM_PLANE_A1 + VIEW_ROW0 * 2 * NT_W, d0
22: move.l  d0, -(sp)
    pea     ntbuf
    jsr     vdp_dma_queue               | queue the name table
    lea     12(sp), sp
    clr.l   -(sp)                       | then the flip
    moveq   #0, d0
    move.b  back_buf, d0
    move.l  d0, -(sp)                   | (vram = the buffer number)
    clr.l   -(sp)
    jsr     vdp_dma_queue               | words = 0: a flip entry (dma.s)
    lea     12(sp), sp
    clr.b   dq_hold                     | the interrupts may start sending
    eori.b  #1, back_buf                | the next picture: the other buffer
    moveq   #5, d7
    bsr     prof_mark
    move.w  frame_count, d0
    sub.w   frame0, d0
    move.w  d0, render_frames           | frames this picture took
99: rts

| ======================================================================
| the tiles' palettes
| ======================================================================

    .bss
tp_p:       .space  2                   | tile_pals: the thing's palette << 8
    .text

| tile_pals: each tile's palette (tpal: palette << 8), from what is drawn
| at its middle: the wall of the tile column's middle column (else one
| beside it), then the things in front, far to near, over the tiles they
| cover (3 rows and columns of a tile at least; decals stay with their
| wall). Every pixel of a tile is drawn in the tile's palette.
| Wall pass: the column sampled is 8t + 4, else 8t, else 8t + 6 (the
| first with a wall); its texture's palette (area_tex_p, from area.c)
| goes into all VIEW_TH tiles of the tile column; palette 0 if none has a
| wall.
| Thing pass (tp_things): billboards only. A tile takes the thing's sprite
| palette (area_spr_p) when the thing covers at least 3 of its columns
| (or the whole thing, if narrower) and, at the column looked at, at
| least min(3, the thing's rows there) of its rows, and is in front of
| the wall there. Later (nearer) things win.
| In: cp, pt, pt_order, col_iz. Out: tpal. Clobbers d0-d7, a0, a1, a3, a4.
tile_pals:
    movea.l area_tex_p, a4              | the palette of each texture slot
    lea     tpal, a1
    moveq   #0, d7                      | tile column
1:  move.w  d7, d0
    lsl.w   #3, d0
    addq.w  #4, d0
    bsr.s   tp_wall                     | the middle column's wall
    bpl.s   2f
    move.w  d7, d0
    lsl.w   #3, d0
    bsr.s   tp_wall                     | else the first column's
    bpl.s   2f
    move.w  d7, d0
    lsl.w   #3, d0
    addq.w  #6, d0
    bsr.s   tp_wall                     | else column 6's (even: low detail
    bpl.s   2f                          | has entries there)
    moveq   #0, d1                      | no wall: palette 0
2:  moveq   #VIEW_TH-1, d2
3:  move.w  d1, (a1)+                   | the whole tile column
    dbra    d2, 3b
    addq.w  #1, d7
    cmpi.w  #VIEW_TW, d7
    bcs.s   1b
    bra.s   tp_things

| tp_wall: d0.w = a column -> d1.w = its wall's palette << 8 (N clear),
| or -1 (N set: no wall)
| In: a4 = area_tex_p. Clobbers d0, a0.
tp_wall:
    lsl.w   #5, d0
    lea     cp, a0
    adda.w  d0, a0                      | &cp[x]
    tst.l   CP_TEX(a0)
    beq.s   9f
    moveq   #0, d1
    move.b  CP_T(a0), d1
    move.b  0(a4,d1.w), d1              | the texture's palette, 0..2
    lsl.w   #8, d1
    rts
9:  moveq   #-1, d1
    rts

tp_things:
    moveq   #0, d6                      | k: draw order
10: cmp.b   n_pt, d6
    bcc     19f                         | all things done
    lea     pt_order, a0
    moveq   #0, d0
    move.b  0(a0,d6.w), d0
    mulu.w  #PT_SIZE, d0
    lea     pt, a3
    adda.w  d0, a3
    tst.b   PT_DECAL(a3)
    bne     18f                         | decals keep their wall's palette
    movea.l area_spr_p, a0
    moveq   #0, d0
    move.b  PT_SPR(a3), d0
    move.b  0(a0,d0.w), d0              | the sprite's palette
    lsl.w   #8, d0
    move.w  d0, tp_p
    move.w  PT_X0(a3), d7               | its tile columns
    lsr.w   #3, d7
11: move.w  d7, d0
    lsl.w   #3, d0                      | the tile's first column
    cmp.w   PT_X1(a3), d0
    bge     18f                         | past the thing
    move.w  PT_X0(a3), d1               | lo = max(x0, t * 8)
    cmp.w   d0, d1
    bge.s   12f
    move.w  d0, d1
12: move.w  d0, d2                      | hi = min(x1, t * 8 + 8)
    addq.w  #8, d2
    cmp.w   PT_X1(a3), d2
    ble.s   13f
    move.w  PT_X1(a3), d2
13: move.w  d2, d3
    sub.w   d1, d3                      | columns of this tile covered
    cmpi.w  #3, d3
    bge.s   14f
    move.w  PT_X1(a3), d4               | (all of a narrower thing will do)
    sub.w   PT_X0(a3), d4
    cmp.w   d4, d3
    blt     17f                         | too little: tile unchanged
14: addq.w  #4, d0                      | the column looked at: the tile's
    cmp.w   d1, d0                      | middle, within lo .. hi - 1
    bge.s   15f
    move.w  d1, d0
15: cmp.w   d2, d0
    blt.s   16f
    move.w  d2, d0
    subq.w  #1, d0
16: cmpi.b  #2, cstep
    bne.s   161f
    bclr    #0, d0                      | (low detail: the even columns)
161: move.w d0, d1                      | there in front of the wall?
    lsl.w   #2, d1
    lea     col_iz, a0
    move.l  PT_IZ8(a3), d2
    cmp.l   0(a0,d1.w), d2
    blt     17f                         | hidden by the wall there
    bsr     bb_col                      | its sprite column
    cmpi.w  #63, d0
    bhi     17f                         | off the sprite
    moveq   #0, d1
    move.b  PT_SPR(a3), d1
    lsl.w   #7, d1
    add.w   d0, d1
    add.w   d0, d1                      | r_spr_cols index: spr * 128 + c * 2
    movea.l r_spr_cols, a0
    moveq   #0, d4
    move.b  0(a0,d1.w), d4              | top
    moveq   #0, d5
    move.b  1(a0,d1.w), d5              | len
    beq     17f                         | empty column
    add.w   d4, d5                      | bottom
    move.w  d4, d0
    bsr     tp_row                      | screen row of the top
    move.w  d0, d4                      | ya
    move.w  d5, d0
    bsr     tp_row                      | screen row of the bottom
    move.w  d0, d5                      | yb
    move.w  d5, d3                      | rows needed in a tile: 3, or all
    sub.w   d4, d3
    ble     17f
    cmpi.w  #3, d3
    ble.s   162f
    moveq   #3, d3
162: move.w d4, d1                      | tile rows ya >> 3 .. (yb - 1) >> 3
    lsr.w   #3, d1
163: move.w d1, d0
    lsl.w   #3, d0
    cmp.w   d5, d0
    bge.s   17f
    move.w  d4, d2                      | max(ya, ty * 8)
    cmp.w   d0, d2
    bge.s   164f
    move.w  d0, d2
164: addq.w #8, d0                      | min(yb, ty * 8 + 8)
    cmp.w   d5, d0
    ble.s   165f
    move.w  d5, d0
165: sub.w  d2, d0                      | rows of this tile the thing covers
    cmp.w   d3, d0
    blt.s   166f                        | too few
    move.w  d7, d0                      | tpal[t * VIEW_TH + ty] = its palette
    mulu.w  #VIEW_TH, d0
    add.w   d1, d0
    add.w   d0, d0
    lea     tpal, a0
    move.w  tp_p, 0(a0,d0.w)
166: addq.w #1, d1                      | next tile row
    bra.s   163b
17: addq.w  #1, d7                      | next tile column
    bra     11b
18: addq.w  #1, d6                      | next thing
    bra     10b
19: rts

| tp_row: d0.w = a canvas row of the thing a3 -> d0.w its screen row,
| 0..VIEW_H: (((r - 28) * 2F) / d + 2 HALF_H - 1) >> 1 (as
| draw_sprite_col). Uses d0-d3.
| (The estimate only, without draw_sprite_col's exact row adjustment.)
tp_row:
    subi.w  #28, d0
    muls.w  #2*HALF_W, d0               | (F)
    move.w  PT_D(a3), d1
    ext.l   d1
    bsr     sdiv32
    addi.l  #2*HALF_H-1, d0
    asr.l   #1, d0
    bpl.s   1f
    moveq   #0, d0
1:  cmpi.l  #VIEW_H, d0
    ble.s   2f
    move.l  #VIEW_H, d0
2:  rts

| scan_cols: each tile column's first and last tile row with anything
| drawn (col_r0, col_r1): only those are uploaded. A tile row of a column
| is 32 bytes (8 pixel rows, a long each).
| A tile is empty when all its 32 bytes are 0 (colour 0 = transparent).
| Rows in between r0 and r1 are uploaded even if empty.
| Out: col_r0[x], col_r1[x] (0, 0 when the column is empty).
| Clobbers d0-d7, a0-a3.
scan_cols:
    lea     fb, a0
    lea     col_r0, a1
    lea     col_r1, a2
    moveq   #VIEW_TW - 1, d7            | every tile column
1:  movea.l a0, a3                      | from the top
    moveq   #0, d6                      | tile row
2:  movem.l (a3)+, d0-d3                | OR the tile's 8 longs together
    or.l    d1, d0
    or.l    d2, d0
    or.l    d3, d0
    movem.l (a3)+, d1-d4
    or.l    d1, d0
    or.l    d2, d0
    or.l    d3, d0
    or.l    d4, d0
    bne.s   3f                          | something drawn: first row found
    addq.w  #1, d6
    cmpi.w  #VIEW_TH, d6
    bcs.s   2b
    clr.b   (a1)+                       | nothing
    clr.b   (a2)+
    bra.s   6f
3:  move.b  d6, (a1)+                   | r0
    lea     VIEW_H * 4(a0), a3          | from the bottom
    moveq   #VIEW_TH, d6
4:  subq.w  #1, d6
    lea     -32(a3), a3                 | one tile up
    movem.l (a3), d0-d3
    or.l    d1, d0
    or.l    d2, d0
    or.l    d3, d0
    movem.l 16(a3), d1-d4
    or.l    d1, d0
    or.l    d2, d0
    or.l    d3, d0
    or.l    d4, d0
    beq.s   4b                          | empty: keep going up (stops at r0
                                        | at the latest, which is not empty)
    addq.w  #1, d6
    move.b  d6, (a2)+                   | r1 = last row + 1
6:  lea     COLBYTES(a0), a0            | next tile column
    dbra    d7, 1b
    rts

| build_nt: the view's name table rows for the hidden buffer: tile
| x * VIEW_TH + y of the buffer with the tile's palette, or the blank tile
| where nothing was drawn (a column at a time: ntbuf is row-major, NT_W
| entries = 64 bytes a row, the view's tile column x at row entry
| VIEW_COL0 + x); the border columns either side of the view are blank.
| Name table entry: tile number (bits 0-10) | palette << 13 (tpal holds
| palette << 8, shifted 5 more). Buffer 1's tiles start at tile BUF1_TILE
| (VRAM $5000 / 32). Clobbers d0-d7, a0-a5.
build_nt:
    lea     ntbuf, a1                   | the border: blank entries
    move.w  #TILE_BLANK, d5             | (the blank entry, palette 0)
    moveq   #VIEW_TH - 1, d7
0:  moveq   #VIEW_COL0 - 1, d6          | the columns left of the view
1:  move.w  d5, (a1)+
    dbra    d6, 1b
    lea     2 * VIEW_TW(a1), a1         | (the view's columns: below)
    moveq   #NT_W - VIEW_COL0 - VIEW_TW - 1, d6   | the columns right of it
2:  move.w  d5, (a1)+
    dbra    d6, 2b
    dbra    d7, 0b
    lea     tpal, a0
    lea     ntbuf + 2 * VIEW_COL0, a1   | the view's first column
    lea     col_r0, a2
    lea     col_r1, a3
    moveq   #0, d3                      | the buffer's first tile
    tst.b   back_buf
    beq.s   1f
    move.w  #BUF1_TILE, d3              | buffer 1: tiles 640..
1:  moveq   #0, d7                      | x
2:  movea.l a1, a4                      | ntbuf + (VIEW_COL0 + x) * 2
    moveq   #0, d0
    move.b  (a2)+, d0                   | r0
    moveq   #0, d1
    move.b  (a3)+, d1                   | r1
    moveq   #0, d6                      | y
3:  cmp.w   d0, d6                      | above: blank
    bcc.s   4f
    move.w  d5, (a4)
    lea     2 * NT_W(a4), a4
    addq.w  #1, d6
    bra.s   3b
4:  move.w  d3, d2                      | drawn: tile | palette << 13
    add.w   d6, d2
    move.w  d7, d4
    mulu.w  #VIEW_TH, d4
    add.w   d4, d2                      | base + x * VIEW_TH + y
    move.w  d7, d4
    mulu.w  #2 * VIEW_TH, d4
    lea     0(a0,d4.w), a5              | tpal of the column
    move.w  d6, d4
    add.w   d4, d4
    adda.w  d4, a5
5:  cmp.w   d1, d6
    bcc.s   6f
    move.w  (a5)+, d4
    lsl.w   #5, d4
    or.w    d2, d4
    move.w  d4, (a4)
    lea     2 * NT_W(a4), a4
    addq.w  #1, d2
    addq.w  #1, d6
    bra.s   5b
6:  cmpi.w  #VIEW_TH, d6                | below: blank
    bcc.s   7f
    move.w  d5, (a4)
    lea     2 * NT_W(a4), a4
    addq.w  #1, d6
    bra.s   6b
7:  addq.l  #2, a1
    addq.w  #1, d7
    cmpi.w  #VIEW_TW, d7
    bcs.s   2b
    rts

| pvs_of: a0 = the visibility list of cell (d0.w, d1.w) facing d2.w
| r_pvs_index has one word per (cell * 4 + facing), cell = z * 32 + x:
| the list's offset in words from r_pvs. A list is a word count, then
| the wall ids (sorted). Clobbers d0, d1, d2.
pvs_of:
    lsl.w   #5, d1
    add.w   d0, d1                      | cell = z * 32 + x
    lsl.w   #2, d1
    andi.w  #3, d2
    add.w   d2, d1                      | cell * 4 + facing
    add.w   d1, d1                      | word index
    movea.l r_pvs_index, a0
    moveq   #0, d0
    move.w  0(a0,d1.w), d0              | offset in words
    add.l   d0, d0
    movea.l r_pvs, a0
    adda.l  d0, a0
    rts

| vis_of: a0 = a1 = the 128-byte map of the cells whose things can show,
| for cell (d0.w, d1.w) facing d2.w, built in the buffer a1 from the
| map's list of them
| r_vis_index: one word per (cell * 4 + facing): $FFFF = every cell, else
| the word offset of a list in r_vis of cell numbers ended by $FFFF (any
| negative word). The bitmap has bit (cell & 7) of byte cell >> 3.
| Clobbers d0-d2.
vis_of:
    lsl.w   #5, d1
    add.w   d0, d1
    lsl.w   #2, d1
    andi.w  #3, d2
    add.w   d2, d1
    add.w   d1, d1                      | (cell * 4 + facing) * 2
    movea.l r_vis_index, a0
    move.w  0(a0,d1.w), d0
    movea.l a1, a0
    moveq   #-1, d1                     | fill: all set ...
    cmpi.w  #0xFFFF, d0                 | every cell
    beq.s   1f
    moveq   #0, d1                      | ... or all clear
1:  moveq   #31, d2                     | 32 longs = 128 bytes
2:  move.l  d1, (a1)+
    dbra    d2, 2b
    cmpi.w  #0xFFFF, d0
    beq.s   9f
    movea.l r_vis, a1
    add.l   d0, d0
    adda.l  d0, a1                      | the cell list
3:  move.w  (a1)+, d0
    bmi.s   9f                          | end of the list
    move.w  d0, d1
    lsr.w   #3, d1
    bset    d0, 0(a0,d1.w)              | mark the cell (bit d0 & 7)
    bra.s   3b
9:  movea.l a0, a1
    rts

| the camera offset per facing (0 N, 1 E, 2 S, 3 W): CAM_BACK times the
| facing's unit step; cam_xz subtracts it to stand CAM_BACK behind
FX: .word   0, CAM_BACK, 0, -CAM_BACK   | 16 x the facing's x step
FZ: .word   -CAM_BACK, 0, CAM_BACK, 0

| cam_xz: (d0.w, d1.w) = the camera of cell (d0.w, d1.w) facing d2.w:
| 16 units behind the cell centre
| (cell centre = cell * 64 + 32, in world units). Clobbers d2, a0.
cam_xz:
    lsl.w   #6, d0
    addi.w  #32, d0                     | centre x
    lsl.w   #6, d1
    addi.w  #32, d1                     | centre z
    andi.w  #3, d2
    add.w   d2, d2
    lea     FX, a0
    sub.w   0(a0,d2.w), d0              | step back along the facing
    lea     FZ, a0
    sub.w   0(a0,d2.w), d1
    rts

| ======================================================================
| C entry points
| ======================================================================
|
| C arguments are pushed as longs, so after the movem.l of 11 registers
| (44 bytes) and the return address (4), argument n is the long at
| 48 + 4n(sp) and a u8 is its last byte (51, 55, 59 ... (sp)).

| void render_view(u8 gx, u8 gz, u8 facing, const u8 *line_flags,
|                  const struct robj *objs, u8 n_objs, u8 mode)
| The full picture from a cell (render.h). mode: R_LOW (bit 0) low
| detail; R_KEEP (bit 1) sets keep_mode 1, which nothing reads any more
| (the palettes always come from area_select); R_PALETTE_ONLY (bit 2)
| only selects the area's palettes, no picture.
| Stack: 51(sp) gx, 55 gz, 59 facing, 60 line_flags, 64 objs, 71 n_objs,
| 75 mode. Saves d2-d7/a2-a6; returns nothing.
    .global render_view
render_view:
    movem.l d2-d7/a2-a6, -(sp)
    moveq   #0, d0                      | the area's palettes (src/area.c)
| area_select(gx, gz, facing): pushed right to left; each push moves sp
| by 4, so 59(sp) reads facing, then gz, then gx
    move.b  59(sp), d0
    move.l  d0, -(sp)
    move.b  59(sp), d0
    move.l  d0, -(sp)
    move.b  59(sp), d0
    move.l  d0, -(sp)
    jsr     area_select                 | palettes and remap tables
    lea     12(sp), sp
    moveq   #0, d0
    move.b  51(sp), d0
    moveq   #0, d1
    move.b  55(sp), d1
    moveq   #0, d2
    move.b  59(sp), d2
    bsr.s   cam_xz                      | camera position
    moveq   #0, d2
    move.b  59(sp), d2
    lsl.w   #8, d2                      | facing * 256 = angle (1024/turn)
    bsr     set_camera                  | camera axes
    move.b  #1, cstep
    btst    #0, 75(sp)                  | R_LOW
    beq.s   1f
    move.b  #2, cstep
1:  clr.b   keep_mode
    btst    #1, 75(sp)                  | R_KEEP
    beq.s   2f
    move.b  #1, keep_mode
2:  btst    #2, 75(sp)                  | R_PALETTE_ONLY
    beq.s   3f
    move.b  #2, keep_mode
3:  move.l  60(sp), rc_flags
    move.l  64(sp), rc_objs
    moveq   #0, d0
    move.b  71(sp), d0
    move.w  d0, rc_nobjs
    moveq   #0, d0
    move.b  51(sp), d0
    moveq   #0, d1
    move.b  55(sp), d1
    moveq   #0, d2
    move.b  59(sp), d2
    bsr     pvs_of                      | the walls seen from here
    move.l  a0, rc_pvs_a                | (the same list twice: one view)
    move.l  a0, rc_pvs_b
    moveq   #0, d0
    move.b  51(sp), d0
    moveq   #0, d1
    move.b  55(sp), d1
    moveq   #0, d2
    move.b  59(sp), d2
    lea     vis_buf_a, a1
    bsr     vis_of                      | the cells whose things show
    move.l  a0, vis_a
    move.l  a0, vis_b
    bsr     render_core                 | draw and queue the picture
    movem.l (sp)+, d2-d7/a2-a6
    rts

| void render_between(u8 ax, u8 az, u8 af, u8 bx, u8 bz, u8 bf, u16 t,
|                     const u8 *line_flags, const struct robj *objs, u8 n_objs)
| one in-between picture of a step or a turn, t = 0..256: every second
| column, the last palette kept
| (The palettes are those of the end cell (bx, bz, bf), via area_select.)
| A turn (af != bf) turns the camera about cell a's centre, staying
| CAM_BACK behind it; a step (af = bf) moves the camera in a straight line
| from a's camera to b's. Walls and things visible from either cell are
| drawn (two PVS and two vis maps).
| Stack: 51(sp) ax, 55 az, 59 af, 63 bx, 67 bz, 71 bf, 74 t (word), 76
| line_flags, 80 objs, 87 n_objs. Saves d2-d7/a2-a6.
    .global render_between
render_between:
    movem.l d2-d7/a2-a6, -(sp)
    moveq   #0, d0                      | the area's palettes: where it ends
    move.b  71(sp), d0                  | (bf, bz, bx: each push moves sp)
    move.l  d0, -(sp)
    move.b  71(sp), d0
    move.l  d0, -(sp)
    move.b  71(sp), d0
    move.l  d0, -(sp)
    jsr     area_select                 | area_select(bx, bz, bf)
    lea     12(sp), sp
    | da = (bf - af) & 3, 3 -> -1
    | (quarter turns: +1 right, -1 left, 0 a step; 2 is not expected)
    moveq   #0, d6
    move.b  71(sp), d6
    moveq   #0, d0
    move.b  59(sp), d0
    sub.w   d0, d6
    andi.w  #3, d6
    cmpi.w  #3, d6
    bne.s   1f
    moveq   #-1, d6
1:  | a = (af * 256 + ((da * 256 * t) >> 8)) & 1023
    move.w  74(sp), d5                  | t
    move.w  d6, d0
    lsl.w   #8, d0
    muls.w  d5, d0
    asr.l   #8, d0
    moveq   #0, d1
    move.b  59(sp), d1
    lsl.w   #8, d1
    add.w   d1, d0
    andi.w  #1023, d0
    move.w  d0, d4                      | a
    tst.w   d6
    beq.s   2f                          | no turn: a step
    | turning: the camera stays 16 units behind the cell centre
    | (camera = centre - 16 forward = centre + 16 (-sin a, +cos a))
    lea     r_sin, a0
    move.w  d4, d1
    add.w   d1, d1
    move.w  0(a0,d1.w), d2              | sin
    move.w  d4, d1
    addi.w  #256, d1                    | + a quarter turn
    andi.w  #1023, d1
    add.w   d1, d1
    move.w  0(a0,d1.w), d3              | cos
    moveq   #0, d0
    move.b  51(sp), d0
    lsl.w   #6, d0
    addi.w  #32, d0                     | a's centre x
    muls.w  #CAM_BACK, d2
    asr.l   #8, d2
    asr.l   #6, d2                      | 16 sin (from 2.14)
    sub.w   d2, d0
    moveq   #0, d1
    move.b  55(sp), d1
    lsl.w   #6, d1
    addi.w  #32, d1                     | a's centre z
    muls.w  #CAM_BACK, d3
    asr.l   #8, d3
    asr.l   #6, d3                      | 16 cos
    add.w   d3, d1
    move.w  d4, d2
    bsr     set_camera
    bra.s   3f
2:  | stepping: between the two cameras
    | (camera = cam_a + (cam_b - cam_a) t / 256)
    moveq   #0, d0
    move.b  63(sp), d0
    moveq   #0, d1
    move.b  67(sp), d1
    moveq   #0, d2
    move.b  71(sp), d2
    bsr     cam_xz                      | b's camera
    move.w  d0, d6                      | x1
    move.w  d1, d7                      | z1
    moveq   #0, d0
    move.b  51(sp), d0
    moveq   #0, d1
    move.b  55(sp), d1
    moveq   #0, d2
    move.b  59(sp), d2
    bsr     cam_xz                      | a's camera
    sub.w   d0, d6
    muls.w  d5, d6
    asr.l   #8, d6
    add.w   d6, d0
    sub.w   d1, d7
    muls.w  d5, d7
    asr.l   #8, d7
    add.w   d7, d1
    move.w  d4, d2
    bsr     set_camera
3:  move.b  #2, cstep                   | always low detail
    move.b  #1, keep_mode               | (not read: see render_view)
    move.l  76(sp), rc_flags
    move.l  80(sp), rc_objs
    moveq   #0, d0
    move.b  87(sp), d0
    move.w  d0, rc_nobjs
    moveq   #0, d0
    move.b  51(sp), d0
    moveq   #0, d1
    move.b  55(sp), d1
    moveq   #0, d2
    move.b  59(sp), d2
    bsr     pvs_of                      | walls seen from a
    move.l  a0, rc_pvs_a
    moveq   #0, d0
    move.b  63(sp), d0
    moveq   #0, d1
    move.b  67(sp), d1
    moveq   #0, d2
    move.b  71(sp), d2
    bsr     pvs_of                      | walls seen from b
    move.l  a0, rc_pvs_b
    moveq   #0, d0
    move.b  51(sp), d0
    moveq   #0, d1
    move.b  55(sp), d1
    moveq   #0, d2
    move.b  59(sp), d2
    lea     vis_buf_a, a1
    bsr     vis_of                      | cells seen from a
    move.l  a0, vis_a
    moveq   #0, d0
    move.b  63(sp), d0
    moveq   #0, d1
    move.b  67(sp), d1
    moveq   #0, d2
    move.b  71(sp), d2
    lea     vis_buf_b, a1
    bsr     vis_of                      | cells seen from b
    move.l  a0, vis_b
    bsr     render_core                 | draw and queue the picture
    movem.l (sp)+, d2-d7/a2-a6
    rts

| void render_reset(void): after something else used the view's VRAM
| Waits until the DMA queue is empty and no flip is pending, then shows
| plane A name table 0 (VDP register 2 = $C000 >> 10) and makes buffer 1
| the next one drawn. (It does not touch vdp.c's shown_view_buf.)
| Called by ui.c after the title and other full screens. Clobbers
| d0-d1/a0-a1 (C convention).
    .global render_reset
render_reset:
    jsr     vdp_dma_wait                | the queue empty
1:  cmpi.b  #0xFF, pending_view_buf
    bne.s   1b                          | no flip pending
    move.b  #1, back_buf                | the next picture goes to buffer 1
    move.l  #VRAM_PLANE_A0 >> 10, -(sp)
    move.l  #2, -(sp)
    jsr     vdp_set_reg                 | plane A = name table 0
    addq.l  #8, sp
    rts

| u8 render_busy(void): 1 while a finished picture waits for its flip
| Returns dma_busy's non-zero result while transfers are queued, else 1
| while a flip is pending, else 0. Clobbers d0-d1/a0-a1.
    .global render_busy
render_busy:
    jsr     dma_busy                    | DMAs still queued? (vdp.c)
    tst.b   d0
    bne.s   1f                          | yes: busy
    cmpi.b  #0xFF, pending_view_buf
    beq.s   1f                          | no flip pending: d0 = 0
    moveq   #1, d0
1:  rts

