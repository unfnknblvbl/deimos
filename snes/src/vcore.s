; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

; Doom RPG for the SNES: the 3D view's walls in assembler: view/view.c's
; project_walls and column_params, the same arithmetic (the tests compare
; the pictures with the C on the build machine, byte for byte).
;
; vc_walls(): from the globals view.c sets (vc_*): every wall of the cell's
; visibility list projected, the nearest per column kept (col_hq, col_w),
; then each column's texture column and height (col_h, col_tex, col_t).
;
; Multiplications are 8 x 16 bits on the SNES's multiplier ($4202/3);
; divisions 32 / 16 bits, 16 quotient bits at a time.
;
; The view's size (view.inc, as view.h): the comments give the sums for
; the full 128-wide view, where VIEW_W = 2 x the focal length = 128 and
; the focal length VIEW_FOCAL = 64; at another width (the SNES game's 104)
; 128 is VIEW_W and 64 VIEW_FOCAL throughout, as view.c's SX_NUM, TIMES_W,
; HQ_NUM and CANVAS_OF say. At 128 wide the code is the original, shifts
; and all (.if VIEW_W = 128).
; ca65. Licence: GPL-2.0-or-later.

.p816
.smart -
.include "view.inc"
.export _vc_walls, _col_hq, _col_w
.export _vc_vx, _vc_vz, _vc_f, _vc_pvs, _vc_walls_at, _vc_lflags
.export smul8_16, sdiv32_16, udiv32_16, screen_col, mr, dn, ddv
.import _col_h, _col_tex, _col_t, _tex_map, _tex_addr, _block_bank

NEAR = 1
MAX_VW = 64

; W_TOP: the highest set bit of VIEW_W (7 at 128), for mul_w
.if VIEW_W >= 128
W_TOP = 7
.elseif VIEW_W >= 64
W_TOP = 6
.elseif VIEW_W >= 32
W_TOP = 5
.elseif VIEW_W >= 16
W_TOP = 4
.else
W_TOP = 3
.endif

; mul_w src: A = A x VIEW_W, 16 bits (wrapping), A holding the word at src
; on entry (a16): shift and add, from the top bit down (at 128 wide: seven
; asl, the original's << 7). Clobbers the carry.
.macro mul_w src
.repeat W_TOP, I
    asl
.if (VIEW_W >> (W_TOP - 1 - I)) & 1
    clc
    adc src
.endif
.endrepeat
.endmacro

; set_hq_num: dn = 1024 VIEW_FOCAL, a thing's hq at depth 1 (65536 at 128
; wide: the original's stz / lda #1 / sta), so hq = dn / d (a16)
.macro set_hq_num
.if VIEW_W = 128
    stz dn
    lda #1
    sta dn + 2
.else
    lda #.loword(1024 * VIEW_FOCAL)
    sta dn
    lda #.hiword(1024 * VIEW_FOCAL)
    sta dn + 2
.endif
.endmacro

; SHL_F: the routine screen_col and screen_col_r call for dn = |l| x the
; focal length x 256 (l in t1+2): shl14 at 128 wide (<< 14), else shl_f
.if VIEW_W = 128
SHL_F = shl14
.else
SHL_F = shl_f
.endif

.segment "ZEROPAGE"
mk:     .res 2                  ; smul8_16: mk (|mk| < 256) * mv -> mr
mv:     .res 2
mr:     .res 4
msgn:   .res 2
ms0:    .res 2
ms1:    .res 2
lfp:    .res 2                  ; (lfp): S.l_flags
dn:     .res 4                  ; (u/s)div32_16: dn / ddv -> dn, rem
ddv:    .res 2
rem:    .res 2
dw:     .res 2
qhi:    .res 2
t0:     .res 4
t1:     .res 4
wp:     .res 3                  ; [wp]: the wall's 8 words
pp:     .res 3                  ; [pp]: the visibility list
ax_:    .res 2
az_:    .res 2
bx_:    .res 2
bz_:    .res 2
ua_:    .res 2
ub_:    .res 2
q_da:   .res 2
q_la:   .res 2
q_db:   .res 2
q_lb:   .res 2
q_x0:   .res 2
q_x1:   .res 2
q_c:    .res 4
acc:    .res 4                  ; hq, 16.8
step:   .res 4
h0:     .res 2
kk:     .res 2
nn:     .res 2
nvw:    .res 2
cx:     .res 2

.segment "BSS"
_vc_vx:      .res 2             ; the camera (world units)
_vc_vz:      .res 2
_vc_f:       .res 2             ; facing 0..3
_vc_pvs:     .res 4             ; block address of the cell's wall list
_vc_walls_at: .res 4            ; block address of the walls (8 words each)
_vc_lflags:  .res 2             ; S.l_flags
_col_hq:     .res 2 * VIEW_W
_col_w:      .res VIEW_W
vw_da:  .res 2 * MAX_VW
vw_la:  .res 2 * MAX_VW
vw_dd:  .res 2 * MAX_VW
vw_dl:  .res 2 * MAX_VW
vw_tex: .res 2 * MAX_VW
vw_ua:  .res 2 * MAX_VW
vw_du:  .res 2 * MAX_VW

.segment "CODE"
.a16
.i16

; ---------------------------------------------------------------- maths
; mr = mk * mv (signed; |mk| < 256)
smul8_16:
    stz msgn
    lda mk
    bpl :+
    eor #$FFFF
    inc
    inc msgn
:   sta ms0
    lda mv
    bpl :+
    eor #$FFFF
    inc
    inc msgn
:   sta ms1
    sep #$20
    .a8
    lda ms0
    sta f:$004202
    lda ms1
    sta f:$004203
    nop
    nop
    rep #$20
    .a16
    lda f:$004216
    sta mr
    stz mr + 2
    sep #$20
    .a8
    lda ms1 + 1
    sta f:$004203
    nop
    nop
    rep #$20
    .a16
    lda f:$004216
    clc
    adc mr + 1
    sta mr + 1
    lda msgn
    and #1
    beq :+
    jsr neg_mr
:   rts

neg_mr:
    lda mr
    eor #$FFFF
    clc
    adc #1
    sta mr
    lda mr + 2
    eor #$FFFF
    adc #0
    sta mr + 2
    rts

neg_dn:
    lda dn
    eor #$FFFF
    clc
    adc #1
    sta dn
    lda dn + 2
    eor #$FFFF
    adc #0
    sta dn + 2
    rts

; 16 quotient bits: dw = (rem:dw) / ddv, rem = the remainder (rem < ddv on entry)
div16:
    lda rem
.repeat 16
    asl dw
    rol a
    bcs :+
    cmp ddv
    bcc :++
:   sbc ddv
    inc dw
:
.endrepeat
    sta rem
    rts

; dn = dn / ddv (dn < 2^24, 1 <= ddv < 256): the SNES's 16/8 divider, a
; byte of the quotient at a time
hdiv24_8:
    lda dn + 2
    and #$00FF
    jsr hdiv
    sta qhi                     ; q2
    lda t1
    xba
    and #$FF00
    sta t0
    lda dn + 1
    and #$00FF
    ora t0
    jsr hdiv
    sta t0 + 2                  ; q1
    lda t1
    xba
    and #$FF00
    sta t0
    lda dn
    and #$00FF
    ora t0
    jsr hdiv
    sta dn                      ; q0
    lda t0 + 2
    xba
    ora dn
    sta dn
    lda qhi
    sta dn + 2
    rts

; A = A / ddv (16 / 8 bits), t1 = the remainder
hdiv:
    sta f:$004204
    sep #$20
    .a8
    lda ddv
    sta f:$004206
    rep #$20
    .a16
    nop
    nop
    nop
    nop
    nop
    nop
    lda f:$004216
    sta t1
    lda f:$004214
    rts

; dn = dn / ddv (unsigned 32 / 16), rem
udiv32_16:
    lda dn + 2
    cmp ddv
    bcc @small
    stz rem
    sta dw
    jsr div16
    lda dw
    sta qhi
    bra @lo
@small:
    sta rem
    stz qhi
@lo:
    lda dn
    sta dw
    jsr div16
    lda dw
    sta dn
    lda qhi
    sta dn + 2
    rts

; dn = dn / ddv (signed, truncated towards 0)
sdiv32_16:
    stz msgn
    lda dn + 2
    bpl :+
    jsr neg_dn
    inc msgn
:   lda ddv
    bpl :+
    eor #$FFFF
    inc
    sta ddv
    inc msgn
:   jsr udiv32_16
    lda msgn
    and #1
    beq :+
    jsr neg_dn
:   rts

; A = the column of a view-space point (d in t1 > 0, l in t1+2):
; clamp(((16384 + (l << 14) / d) + 127) >> 8, 0, 128)
; (at any width: clamp(((F << 8) + l F 256 / d + 127) >> 8, 0, VIEW_W), F =
; VIEW_FOCAL)
screen_col:
    lda t1
    sta ddv
    lda t1 + 2
    bmi @neg
    cmp ddv
    bcs @right                  ; l >= d: at or past the right edge
    ; l >= 0: (l << 14) as dn
    jsr SHL_F
    lda dn + 2
    cmp ddv
    bcs @right                  ; the quotient has more than 16 bits: far right
    jsr udiv32_16
    lda dn
    clc
    adc #VIEW_FOCAL * 256 + 127
    bcs @right
    xba
    and #$00FF
    cmp #VIEW_W + 1
    bcc :+
@right:
    lda #VIEW_W
:   rts
@neg:
    eor #$FFFF
    inc
    sta t1 + 2
    cmp ddv
    bcs @left                   ; -l >= d: at or past the left edge
    jsr SHL_F
    lda dn + 2
    cmp ddv
    bcs @left
    jsr udiv32_16
    lda #VIEW_FOCAL * 256 + 127
    sec
    sbc dn
    bcc @left                   ; below 0
    xba
    and #$00FF
    rts
@left:
    lda #0
    rts

; dn = (t1+2) << 14 (t1+2 >= 0)
shl14:
    lda t1 + 2
    lsr
    lsr
    sta dn + 2
    lda t1 + 2
    and #3
    xba
    asl
    asl
    asl
    asl
    asl
    asl
    sta dn
    rts

.if VIEW_W <> 128
; dn = (t1+2) x VIEW_FOCAL x 256 (t1+2 >= 0): shl14 at any width (the
; product's 24 bits, a byte up). Clobbers mk, mv, mr, msgn, ms0, ms1.
shl_f:
    lda t1 + 2
    sta mv
    lda #VIEW_FOCAL
    sta mk
    jsr smul8_16                ; mr = l x F (< 2^23)
    sep #$20
    .a8
    stz dn
    lda mr + 2
    sta dn + 3
    rep #$20
    .a16
    lda mr
    sta dn + 1
    rts
.endif

; ---------------------------------------------------------------- the camera
; (A, X) world point -> t1 = depth, t1+2 = lateral
to_view:
    sec
    sbc _vc_vx
    sta t0                      ; rx
    txa
    sec
    sbc _vc_vz
    sta t0 + 2                  ; rz
    lda _vc_f
    asl
    tax
    jmp (@tab,x)
@tab: .addr @n, @e, @s, @w
@n: lda t0 + 2                  ; d = -rz, l = rx
    eor #$FFFF
    inc
    sta t1
    lda t0
    sta t1 + 2
    rts
@e: lda t0                      ; d = rx, l = rz
    sta t1
    lda t0 + 2
    sta t1 + 2
    rts
@s: lda t0 + 2                  ; d = rz, l = -rx
    sta t1
    lda t0
    eor #$FFFF
    inc
    sta t1 + 2
    rts
@w: lda t0                      ; d = -rx, l = -rz
    eor #$FFFF
    inc
    sta t1
    lda t0 + 2
    eor #$FFFF
    inc
    sta t1 + 2
    rts

; ---------------------------------------------------------------- projection
; the quad (ax_, az_) .. (bx_, bz_): q_da .. q_c, q_x0, q_x1; carry set: nothing on screen
.export project
project:
    lda ax_
    ldx az_
    jsr to_view
    lda t1
    sta q_da
    lda t1 + 2
    sta q_la
    lda bx_
    ldx bz_
    jsr to_view
    lda t1
    sta q_db
    lda t1 + 2
    sta q_lb
    ; both ends right of the view (l >= d) or both left (l <= -d): nothing
    ; on screen (the columns would all be 128, or all 0)
    lda q_la
    sec
    sbc q_da
    bmi @notr
    lda q_lb
    sec
    sbc q_db
    bmi @notr
    sec
    rts
@notr:
    lda q_la
    clc
    adc q_da
    beq :+
    bpl @notl
:   lda q_lb
    clc
    adc q_db
    beq :+
    bpl @notl
:   sec
    rts
@notl:
    lda q_da
    cmp #NEAR
    bpl @a_ok
    lda q_db
    cmp #NEAR
    bpl :+
    sec                         ; both behind
    rts
:   ; A clipped: cla = la + (lb - la) (NEAR - da) / (db - da), cda = NEAR
    lda q_lb
    sec
    sbc q_la
    sta mk
    lda #NEAR
    sec
    sbc q_da
    sta mv
    jsr smul8_16
    jsr mr_dn
    lda q_db
    sec
    sbc q_da
    sta ddv
    jsr sdiv32_16
    lda q_la
    clc
    adc dn
    sta t1 + 2
    lda #NEAR
    sta t1
    jsr screen_col
    sta q_x0
    lda q_db
    sta t1
    lda q_lb
    sta t1 + 2
    jsr screen_col
    sta q_x1
    bra @order
@a_ok:
    lda q_db
    cmp #NEAR
    bpl @both
    ; B clipped: clb = lb + (la - lb) (NEAR - db) / (da - db)
    lda q_la
    sec
    sbc q_lb
    sta mk
    lda #NEAR
    sec
    sbc q_db
    sta mv
    jsr smul8_16
    jsr mr_dn
    lda q_da
    sec
    sbc q_db
    sta ddv
    jsr sdiv32_16
    lda q_lb
    clc
    adc dn
    sta t1 + 2
    lda #NEAR
    sta t1
    jsr screen_col
    sta q_x1
    lda q_da
    sta t1
    lda q_la
    sta t1 + 2
    jsr screen_col
    sta q_x0
    bra @order
@both:
    lda q_da
    sta t1
    lda q_la
    sta t1 + 2
    jsr screen_col
    sta q_x0
    lda q_db
    sta t1
    lda q_lb
    sta t1 + 2
    jsr screen_col
    sta q_x1
@order:
    lda q_x0
    cmp q_x1
    bcc @c                      ; x0 < x1
    beq @none
    ldx q_x1                    ; swap
    sta q_x1
    stx q_x0
@c: ; c = (lb - la) da - (db - da) la
    lda q_lb
    sec
    sbc q_la
    sta mk
    lda q_da
    sta mv
    jsr smul8_16
    lda mr
    sta q_c
    lda mr + 2
    sta q_c + 2
    lda q_db
    sec
    sbc q_da
    sta mk
    lda q_la
    sta mv
    jsr smul8_16
    lda q_c
    sec
    sbc mr
    sta q_c
    lda q_c + 2
    sbc mr + 2
    sta q_c + 2
    ora q_c
    beq @none
    clc
    rts
@none:
    sec
    rts

mr_dn:
    lda mr
    sta dn
    lda mr + 2
    sta dn + 2
    rts

; A = num2 at kk (= 2x + 1 - 128): (dl << 7) - k dd, dd in t0, dl in t0+2 (16 bits)
; (at any width: dl VIEW_W - k dd, k = 2x + 1 - VIEW_W)
num2_k:
    lda kk
    sta mk
    lda t0
    sta mv
    jsr smul8_16
    lda t0 + 2
    mul_w {t0 + 2}              ; dl x VIEW_W
    sec
    sbc mr
    rts

; A = hq at column x (in A): 512 num2 / c, c cut to 16 bits first
.export hq_at
hq_at:
    asl
    inc
    sec
    sbc #VIEW_W
    sta kk                      ; k = 2x + 1 - 128
    lda q_db
    sec
    sbc q_da
    sta t0
    lda q_lb
    sec
    sbc q_la
    sta t0 + 2
    jsr num2_k
    ; dn = num2 << 9 (sign-extended)
    sta dn + 1
    and #$8000
    beq :+
    lda #$FF
:   sep #$20
    .a8
    sta dn + 3
    stz dn
    rep #$20
    .a16
    asl dn
    rol dn + 2
    ; d = c, both halved until d fits 16 bits
    lda q_c
    sta t1
    lda q_c + 2
    sta t1 + 2
@norm:
    lda t1 + 2
    beq @pos16
    cmp #$FFFF
    beq @neg16
@half:
    lda t1 + 2
    cmp #$8000
    ror t1 + 2
    ror t1
    lda dn + 2
    cmp #$8000
    ror dn + 2
    ror dn
    bra @norm
@pos16:
    lda t1
    bmi @half                   ; 32768.. does not fit
    bra @div
@neg16:
    lda t1
    bpl @half
@div:
    ; signs differ (or n = 0): 0
    lda t1
    sta ddv
    beq @zero
    eor dn + 2
    bmi @zero
    lda ddv
    bpl :+
    eor #$FFFF
    inc
    sta ddv
    jsr neg_dn
:   lda dn + 2
    cmp ddv
    bcs @max
    jsr udiv32_16
    lda dn
    rts
@max:
    lda #$FFFF
    rts
@zero:
    lda #0
    rts

; ---------------------------------------------------------------- walls
.a8
.i8
_vc_walls:
    php
    phd
    rep #$30
    .a16
    .i16
    lda #0
    tcd
    ldx #2 * VIEW_W - 2
    lda #0
:   sta _col_hq,x
    dex
    dex
    bpl :-
    ldx #VIEW_W - 1
    sep #$20
    .a8
    lda #$FF
:   sta _col_w,x
    dex
    bpl :-
    rep #$20
    .a16
    stz nvw
    lda _vc_lflags
    sta lfp
    ; [pp] = the list: n, then n wall numbers
    lda _vc_pvs
    sta pp
    sep #$20
    .a8
    lda _vc_pvs + 2
    clc
    adc _block_bank
    sta pp + 2
    rep #$20
    .a16
    lda [pp]
    xba
    sta nn
    ldy #0
@wall:
    lda nn
    bne :+
    jmp @walls_done
:   dec nn
    iny
    iny
    lda nvw
    cmp #MAX_VW
    bcc :+
    jmp @walls_done
:   phy
    lda [pp],y
    xba                         ; wall i: [wp] = walls + 16 i
    asl
    asl
    asl
    asl
    clc
    adc _vc_walls_at
    sta wp
    lda _vc_walls_at + 2
    adc #0
    sep #$20
    .a8
    clc
    adc _block_bank
    sta wp + 2
    rep #$20
    .a16
    ; the wall's line: an open door is not drawn
    ldy #10
    lda [wp],y
    xba
    and #$00FF
    cmp #$00FF
    beq @closed
    tay
    lda (lfp),y
    and #1
    beq @closed
    jmp @skip
@closed:
    lda [wp]
    xba
    sta ax_
    ldy #2
    lda [wp],y
    xba
    sta az_
    ldy #4
    lda [wp],y
    xba
    sta bx_
    ldy #6
    lda [wp],y
    xba
    sta bz_
    ldy #12
    lda [wp],y
    xba
    and #$00FF
    sta ua_
    ldy #14
    lda [wp],y
    xba
    sta ub_
    ; facing away? (ax - vx)(bz - az) - (az - vz)(bx - ax) <= 0
    lda bz_
    sec
    sbc az_
    sta mk
    lda ax_
    sec
    sbc _vc_vx
    sta mv
    jsr smul8_16
    lda mr
    sta t1
    lda mr + 2
    sta t1 + 2
    lda bx_
    sec
    sbc ax_
    sta mk
    lda az_
    sec
    sbc _vc_vz
    sta mv
    jsr smul8_16
    lda t1
    sec
    sbc mr
    sta t1
    lda t1 + 2
    sbc mr + 2
    bmi @back
    ora t1
    bne @front
@back:
    ldy #12                     ; two-sided? then from the other side
    lda [wp],y
    xba
    and #$0100
    bne :+
    jmp @skip
:   lda ax_
    ldx bx_
    sta bx_
    stx ax_
    lda az_
    ldx bz_
    sta bz_
    stx az_
    lda ua_
    ldx ub_
    sta ub_
    stx ua_
@front:
    jsr project
    bcc :+
    jmp @skip
:   ; the wall's record
    lda nvw
    asl
    tax
    lda q_da
    sta vw_da,x
    lda q_la
    sta vw_la,x
    lda q_db
    sec
    sbc q_da
    sta vw_dd,x
    lda q_lb
    sec
    sbc q_la
    sta vw_dl,x
    ldy #8
    lda [wp],y
    xba
    and #$00FF
    sta vw_tex,x
    lda ua_
    sta vw_ua,x
    lda ub_
    sec
    sbc ua_
    sta vw_du,x
    jsr hq_ramp
    ; the columns: keep the nearest
    ldx q_x0
@zx:
    txa
    asl
    tay
    lda acc + 1                 ; hq = acc >> 8
    cmp _col_hq,y
    beq @znext
    bcc @znext
    sta _col_hq,y
    sep #$20
    .a8
    lda nvw
    sta _col_w,x
    rep #$20
    .a16
@znext:
    lda acc
    clc
    adc step
    sta acc
    lda acc + 2
    adc step + 2
    sta acc + 2
    inx
    cpx q_x1
    bcc @zx
    inc nvw
@skip:
    ply
    jmp @wall
@walls_done:
    jsr columns
    pld
    plp
    rts

; acc, step for the quad's columns q_x0 .. q_x1 - 1 (acc: hq 16.8, from 128)
.export hq_ramp
hq_ramp:
    lda q_x0
    jsr hq_at
    sta h0
    sta acc + 1
    sep #$20
    .a8
    lda #128
    sta acc
    stz acc + 3
    rep #$20
    .a16
    stz step
    stz step + 2
    lda q_x1
    sec
    sbc q_x0
    cmp #2
    bcc @one
    dec
    pha
    lda q_x1
    dec
    jsr hq_at
    stz msgn                    ; dn = |h1 - h0| << 8
    sec
    sbc h0
    bcs :+
    eor #$FFFF
    inc
    inc msgn
:   sta dn + 1
    sep #$20
    .a8
    stz dn
    stz dn + 3
    rep #$20
    .a16
    pla
    sta ddv
    jsr hdiv24_8
    lda msgn
    beq :+
    jsr neg_dn
:   lda dn
    sta step
    lda dn + 2
    sta step + 2
@one:
    rts

; ---------------------------------------------------------------- columns
; per column: t8 = m2 / num2, u, the texture column, the height
columns:
    stz cx
@col:
    lda cx
    tax
    asl
    tay
    lda _col_w,x
    and #$00FF
    cmp #$00FF
    beq @none
    lda _col_hq,y
    cmp #8
    bcs :+
@none:
    lda #0
    sta _col_h,y
    jmp @next
:   lda _col_w,x
    and #$00FF
    asl
    sta t1                      ; the wall's word index
    lda cx
    asl
    inc
    sec
    sbc #VIEW_W
    sta kk                      ; k = 2x + 1 - 128
    ; m2 = k da - (la << 7), 16 bits (la x VIEW_W)
    ldx t1
    lda vw_da,x
    sta mv
    lda kk
    sta mk
    jsr smul8_16
    ldx t1
    lda vw_la,x
    mul_w {vw_la,x}             ; la x VIEW_W
    sta t1 + 2
    lda mr
    sec
    sbc t1 + 2
    sta dw                      ; m2 (kept in dw for t8)
    ; num2 = (dl << 7) - k dd (dl x VIEW_W)
    lda vw_dd,x
    sta mv
    lda kk
    sta mk
    jsr smul8_16
    ldx t1
    lda vw_dl,x
    mul_w {vw_dl,x}             ; dl x VIEW_W
    sec
    sbc mr
    sta ddv                     ; num2
    jsr t8_of
    ; u = (ua + ((t8 du) >> 8)) & 63
    sta mv
    ldx t1
    lda vw_du,x
    sta mk
    jsr smul8_16
    ldx t1
    lda mr + 1
    clc
    adc vw_ua,x
    and #63
    xba
    lsr
    lsr                         ; u * 64
    sta t0
    ; t = tex_map[tex]
    lda vw_tex,x
    tax
    lda _tex_map,x
    and #$00FF
    sta t0 + 2
    ldx cx
    sep #$20
    .a8
    sta _col_t,x
    rep #$20
    .a16
    asl
    asl
    tax                         ; tex_addr[t] + u * 64
    lda cx
    asl
    asl
    tay
    lda _tex_addr,x
    clc
    adc t0
    sta _col_tex,y
    lda _tex_addr + 2,x
    adc #0
    sta _col_tex + 2,y
    lda cx
    asl
    tay
    lda _col_hq,y
    clc
    adc #8
    bcc :+
    lda #4096                   ; (hq + 8 in 17 bits)
    sta _col_h,y
    bra @next
:   lsr
    lsr
    lsr
    lsr
    sta _col_h,y
@next:
    inc cx
    lda cx
    cmp #VIEW_W
    bcs :+
    jmp @col
:   rts

; A = t8 of m2 (dw) / num2 (ddv), as view.c's t8_of
.export t8_of
t8_of:
    lda ddv
    bpl :+
    eor #$FFFF
    inc
    sta ddv
    lda dw
    eor #$FFFF
    inc
    sta dw
:   lda dw
    beq @zero
    bmi @zero
    cmp ddv
    bcs @max                    ; (both positive here)
@norm:
    lda ddv
    cmp #256
    bcc @div
    lsr ddv
    lsr dw
    bra @norm
@div:
    lda ddv
    beq @zero
    lda dw
    xba
    and #$FF00
    sta f:$004204
    sep #$20
    .a8
    lda ddv
    sta f:$004206
    rep #$20                    ; 16 cycles for the quotient
    .a16
    nop
    nop
    nop
    nop
    nop
    nop
    lda f:$004214
    cmp #256
    bcs @max
    rts
@max:
    lda #255
    rts
@zero:
    lda #0
    rts

; ================================================================ things
; vc_things(): view.c's props, decals and things: the ones in the cells
; that can be seen, projected, sorted far to near and drawn column by
; column over the walls (draw.s's draw_spr).
.export _vc_things, _vc_vis, _vc_scenery, _vc_nscen, _vc_def_spr, _vc_spr_cols, _vc_spr_coff
.export _vc_hidden, _vc_eflags, _vc_edef, _vc_ex, _vc_ez, _vc_nents
.import _spr_addr, _draw_spr, _ds_x, _ds_top, _ds_len, _ds_s, _ds_h, _ds_data

MAX_PT = 64
V_MAX_SPR = 64

.segment "BSS"
_vc_vis:      .res 4            ; block address of the visible cells' list ($FFFFFFFF: all)
_vc_scenery:  .res 4            ; the props: x, z, sprite, info, cell (words)
_vc_nscen:    .res 2
_vc_def_spr:  .res 4
_vc_spr_cols: .res 4
_vc_spr_coff: .res 4
_vc_hidden:   .res 2            ; S.static_hidden
_vc_eflags:   .res 2            ; S.e_flags, e_def, e_x, e_z
_vc_edef:     .res 2
_vc_ex:       .res 2
_vc_ez:       .res 2
_vc_nents:    .res 2
vis:     .res 128
pt_hq:   .res 2 * MAX_PT
pt_d:    .res 2 * MAX_PT
pt_l:    .res 2 * MAX_PT
pt_x0:   .res 2 * MAX_PT
pt_x1:   .res 2 * MAX_PT
pt_spr:  .res 2 * MAX_PT
pt_dec:  .res 2 * MAX_PT
pt_ax:   .res 2 * MAX_PT
pt_az:   .res 2 * MAX_PT
pt_bx:   .res 2 * MAX_PT
pt_bz:   .res 2 * MAX_PT
pt_ord:  .res 2 * MAX_PT        ; (word indices, x 2)

.segment "ZEROPAGE"
npt:    .res 2
sp_:    .res 3                  ; [sp_]: a table in the block
s_x:    .res 2
s_z:    .res 2
s_spr:  .res 2
s_info: .res 2
s_d:    .res 2
s_l:    .res 2
s_k:    .res 2
s_n:    .res 2
s_p:    .res 2                  ; the thing (x 2)
s_hq:   .res 2
rnd:    .res 2                  ; screen_col's rounding
hp:     .res 2                  ; (hp): a byte array of S
.if VIEW_W <> 128
bb_q:   .res 2                  ; draw_billboard (any width): k d / VIEW_W, rounded down
bb_r:   .res 2                  ; and its remainder (0 .. VIEW_W - 1)
bb_a:   .res 2                  ; 2d / VIEW_W: the whole part's step a column
bb_b:   .res 2                  ; and the remainder's
bb_neg: .res 2                  ; bit 15: k d at the first column < 0
.endif

.segment "CODE"
.a16
.i16

; [sp_] = the block address in (A = low word, X = high word)
set_sp:
    sta sp_
    txa
    sep #$20
    .a8
    clc
    adc _block_bank
    sta sp_ + 2
    rep #$20
    .a16
    rts

.a8
.i8
_vc_things:
    php
    rep #$30
    .a16
    .i16
    jsr build_vis
    stz npt
    ; the props
    lda _vc_scenery
    ldx _vc_scenery + 2
    jsr set_sp
    stz s_k
@scen:
    lda s_k
    cmp _vc_nscen
    bcc :+
    jmp @ents
:   asl
    sta s_n
    asl
    asl
    adc s_n                     ; 10 k
    sta s_n
    tay
    lda [sp_],y
    xba
    sta s_x
    iny
    iny
    lda [sp_],y
    xba
    sta s_z
    ldy s_n                     ; a billboard (not a decal) in a cell that
    iny                         ; can't be seen: nothing more to do
    iny
    iny
    iny
    iny
    iny
    lda [sp_],y
    xba
    sta s_info
    and #$78
    bne @look
    jsr cell_vis
    bne @look
    jmp @snext
@look:
    ldy s_n
    iny
    iny
    iny
    iny
    lda [sp_],y
    xba
    sta s_spr
    ldy s_n
    tya
    clc
    adc #8
    tay
    lda [sp_],y
    xba
    bmi @shown                  ; (no cell: always there)
    pha                         ; HIDE took this cell's props away?
    lsr
    lsr
    lsr
    tay
    lda _vc_hidden
    sta hp
    lda (hp),y
    sta t0
    pla
    and #7
    tax
    lda t0
:   dex
    bmi :+
    lsr
    bra :-
:   and #1
    bne @snext
@shown:
    lda s_info
    and #$78
    beq @bb
    jsr add_decal
    bra @snext
@bb:
    jsr add_billboard
@snext:
    lda _vc_scenery
    ldx _vc_scenery + 2
    jsr set_sp
    inc s_k
    jmp @scen
@ents:
    stz s_k
@ent:
    lda s_k
    cmp _vc_nents
    bcc :+
    jmp @sort
:   tay
    lda _vc_eflags
    sta hp
    lda (hp),y
    and #4                      ; F_LINKED
    beq @enext
    lda _vc_edef
    sta hp
    lda (hp),y
    and #$00FF
    cmp #$00FF
    beq @enext
    pha
    lda _vc_ex
    sta hp
    lda (hp),y
    and #$00FF
    xba
    lsr
    lsr
    ora #32
    sta s_x
    lda _vc_ez
    sta hp
    lda (hp),y
    and #$00FF
    xba
    lsr
    lsr
    ora #32
    sta s_z
    lda _vc_def_spr
    ldx _vc_def_spr + 2
    jsr set_sp
    ply
    lda [sp_],y
    and #$00FF
    sta s_spr
    jsr add_billboard
@enext:
    inc s_k
    jmp @ent
@sort:
    jsr sort_things
    ; far to near
    stz s_k
@draw:
    lda s_k
    cmp npt
    bcs @done
    asl
    tax
    lda pt_ord,x
    sta s_p
    tax
    lda pt_dec,x
    beq :+
    jsr draw_decal
    bra @dn
:   jsr draw_billboard
@dn:
    inc s_k
    bra @draw
@done:
    plp
    rts

; the cells whose things can show
build_vis:
    lda _vc_vis + 2
    cmp #$FFFF
    bne @list
    lda #$FFFF
    ldx #126
:   sta vis,x
    dex
    dex
    bpl :-
    rts
@list:
    lda #0
    ldx #126
:   sta vis,x
    dex
    dex
    bpl :-
    lda _vc_vis
    ldx _vc_vis + 2
    jsr set_sp
    ldy #0
@c: lda [sp_],y
    xba
    cmp #$FFFF
    beq @end
    pha
    lsr
    lsr
    lsr
    tax
    pla
    and #7
    phy
    tay
    sep #$20
    .a8
    lda bit8,y
    ora vis,x
    sta vis,x
    rep #$20
    .a16
    ply
    iny
    iny
    bne @c
    inc sp_ + 2                 ; (the list crossed a bank)
    bra @c
@end:
    rts

.segment "RODATA"
bit8: .byte 1, 2, 4, 8, 16, 32, 64, 128
.segment "CODE"

; A = the column of (t1 = d > 0, t1+2 = l) rounded with rnd:
; clamp((16384 + (l << 14) / d + rnd) >> 8, 0, 128)
; (at any width: (F << 8) + l F 256 / d, clamped to 0 .. VIEW_W)
screen_col_r:
    lda rnd
    cmp #127
    bne :+
    jmp screen_col
:   lda t1
    sta ddv
    lda t1 + 2
    bmi @neg
    cmp ddv
    bcs @right
    jsr SHL_F
    lda dn + 2
    cmp ddv
    bcs @right
    jsr udiv32_16
    lda dn
    clc
    adc #VIEW_FOCAL * 256
    bcs @right
    adc rnd
    bcs @right
    xba
    and #$00FF
    cmp #VIEW_W + 1
    bcc :+
@right:
    lda #VIEW_W
:   rts
@neg:
    eor #$FFFF
    inc
    sta t1 + 2
    cmp ddv
    bcs @left
    jsr SHL_F
    lda dn + 2
    cmp ddv
    bcs @left
    jsr udiv32_16
    lda #VIEW_FOCAL * 256
    clc
    adc rnd
    sec
    sbc dn
    bcc @left
    xba
    and #$00FF
    rts
@left:
    lda #0
    rts

; a billboard at (s_x, s_z), sprite s_spr
; Z clear: the cell of (s_x, s_z) can't be seen
cell_vis:
    lda s_z                     ; the cell: (z >> 6) << 5 | x >> 6
    asl
    asl
    and #$FF00
    lsr
    lsr
    lsr
    sta t0
    lda s_x
    lsr
    lsr
    lsr
    lsr
    lsr
    lsr
    ora t0
    pha
    lsr
    lsr
    lsr
    tax
    pla
    and #7
    tay
    sep #$20
    .a8
    lda vis,x
    and bit8,y
    rep #$20
    .a16
    rts

add_billboard:
    lda npt
    cmp #MAX_PT
    bcc :+
    rts
:   lda s_spr
    cmp #V_MAX_SPR
    bcc :+
    rts
:   lda s_z                     ; the cell: (z >> 6) << 5 | x >> 6
    asl
    asl
    and #$FF00
    lsr
    lsr
    lsr
    sta t0
    lda s_x
    lsr
    lsr
    lsr
    lsr
    lsr
    lsr
    ora t0
    pha
    lsr
    lsr
    lsr
    tax
    pla
    and #7
    tay
    sep #$20
    .a8
    lda vis,x
    and bit8,y
    rep #$20
    .a16
    bne :+
    rts
:   lda s_x
    ldx s_z
    jsr to_view
    lda t1
    sec
    sbc #16
    sta s_d
    sta t1
    cmp #4
    bpl :+
    rts
:   lda t1 + 2
    sta s_l
    ; l - 32 > d || l + 32 < -d: off to a side
    sec
    sbc #32
    cmp s_d
    beq :+
    bmi :+
    rts
:   lda s_d
    eor #$FFFF
    inc
    sta t0
    lda s_l
    clc
    adc #32
    cmp t0
    bpl :+
    rts
:   ; x0 = screen_x(d, l - 32) >> 8, x1 = (screen_x(d, l + 32) + 255) >> 8
    stz rnd
    lda s_d
    sta t1
    lda s_l
    sec
    sbc #32
    sta t1 + 2
    jsr screen_col_r
    sta t0 + 2
    lda #255
    sta rnd
    lda s_d
    sta t1
    lda s_l
    clc
    adc #32
    sta t1 + 2
    jsr screen_col_r
    cmp t0 + 2
    beq @no
    bcs :+
@no:
    rts
:   pha
    lda npt
    asl
    tax
    pla
    sta pt_x1,x
    lda t0 + 2
    sta pt_x0,x
    lda s_d
    sta pt_d,x
    lda s_l
    sta pt_l,x
    lda s_spr
    sta pt_spr,x
    stz pt_dec,x
    ; hq = 65536 / d (at any width: 1024 VIEW_FOCAL / d)
    set_hq_num
    lda s_d
    sta ddv
    phx
    jsr udiv32_16
    plx
    lda dn
    sta pt_hq,x
    inc npt
    rts

; a decal at (s_x, s_z), sprite s_spr, info s_info
add_decal:
    lda npt
    cmp #MAX_PT
    bcc :+
    rts
:   lda s_spr
    cmp #V_MAX_SPR
    bcc :+
    rts
:   lda npt
    asl
    tax
    lda #32                     ; t0 = 32 sgn
    sta t0
    lda s_info
    and #$200
    beq :+
    lda #$FFE0
    sta t0
:   lda s_info                  ; $800 (the original's $8000000): seen from both
    and #$800                   ; sides; then the sign is chosen so that the
    beq @one                    ; camera is in front (B to A from behind, the
    lda s_info                  ; picture mirrored, as the original draws it):
    and #$08                    ; t0 = 32 if the camera's side D > 0, else -32
    beq @s1
    lda _vc_vz                  ; $08: D = vz - z
    sec
    sbc s_z
    bra @sd
@s1: lda s_info
    and #$10
    beq @s2
    lda s_z                     ; $10: D = z - vz
    sec
    sbc _vc_vz
    bra @sd
@s2: lda s_info
    and #$40
    beq @s3
    lda s_x                     ; $40: D = x - vx
    sec
    sbc _vc_vx
    bra @sd
@s3: lda _vc_vx                 ; else: D = vx - x
    sec
    sbc s_x
@sd: beq @sn                    ; D <= 0: -32
    bmi @sn
    lda #32
    bra @st
@sn: lda #$FFE0
@st: sta t0
@one:
    lda s_info
    and #$08
    beq @b
    lda s_x                     ; ax = x - 32 sgn, bx = x + 32 sgn
    sec
    sbc t0
    sta pt_ax,x
    lda s_x
    clc
    adc t0
    sta pt_bx,x
    lda s_z
    sta pt_az,x
    sta pt_bz,x
    bra @proj
@b: lda s_info
    and #$10
    beq @c
    lda s_x
    clc
    adc t0
    sta pt_ax,x
    lda s_x
    sec
    sbc t0
    sta pt_bx,x
    lda s_z
    sta pt_az,x
    sta pt_bz,x
    bra @proj
@c: lda s_info
    and #$40
    beq @d
    lda s_z
    sec
    sbc t0
    sta pt_az,x
    lda s_z
    clc
    adc t0
    sta pt_bz,x
    lda s_x
    sta pt_ax,x
    sta pt_bx,x
    bra @proj
@d: lda s_z
    clc
    adc t0
    sta pt_az,x
    lda s_z
    sec
    sbc t0
    sta pt_bz,x
    lda s_x
    sta pt_ax,x
    sta pt_bx,x
@proj:
    phx
    lda s_x
    ldx s_z
    jsr to_view
    plx
    lda t1                      ; dm < -32: behind
    cmp #$FFE0
    bpl :+
    rts
:   ; lm - 32 > dm + 32 || lm + 32 < -dm - 32
    lda t1 + 2
    sec
    sbc #32
    sta t0
    lda t1
    clc
    adc #32
    sta t0 + 2
    lda t0
    cmp t0 + 2
    beq :+
    bmi :+
    rts
:   lda t0 + 2
    eor #$FFFF
    inc
    sta t0 + 2                  ; -dm - 32
    lda t1 + 2
    clc
    adc #32
    cmp t0 + 2
    bpl :+
    rts
:   lda #1
    sta pt_dec,x
    lda s_spr
    sta pt_spr,x
    lda #$FFFF
    sta pt_hq,x
    lda t1
    cmp #2
    bmi @hq
    set_hq_num                  ; dn = 65536 (1024 VIEW_FOCAL)
    lda t1
    sta ddv
    phx
    jsr udiv32_16
    plx
    lda dn
    sta pt_hq,x
@hq:
    inc npt
    rts

; insertion sort of pt_ord by pt_hq (far first; equal ones keep their order)
sort_things:
    ldx #0
:   cpx npt
    bcs :+
    txa
    asl
    sta t0
    phx
    txa
    asl
    tax
    lda t0
    sta pt_ord,x
    plx
    inx
    bra :-
:   lda #1
    sta s_k
@i: lda s_k
    cmp npt
    bcs @done
    asl
    tax
    lda pt_ord,x
    sta s_p                     ; v
    tay
    lda pt_hq,y
    sta s_hq
    stx s_n                     ; j (x 2)
@j: ldx s_n
    beq @put
    lda pt_ord - 2,x
    tay
    lda pt_hq,y
    cmp s_hq
    beq @put
    bcc @put
    sta t0
    tya
    sta pt_ord,x
    dex
    dex
    stx s_n
    bra @j
@put:
    ldx s_n
    lda s_p
    sta pt_ord,x
    inc s_k
    bra @i
@done:
    rts

.if VIEW_W = 128
; the thing s_p (a billboard)
draw_billboard:
    ldx s_p
    lda pt_hq,x
    sta s_hq
    lda pt_x0,x
    sta cx
@x: ldx s_p
    lda cx
    cmp pt_x1,x
    bcc :+
    rts
:   asl
    tay
    lda s_hq
    cmp _col_hq,y
    bcc @next                   ; behind the wall
    ; c = ((2x + 1 - 128) d >> 7) - l + 32
    lda cx
    asl
    inc
    sec
    sbc #128
    sta mk
    lda pt_d,x
    sta mv
    jsr smul8_16
    asl mr
    rol mr + 2
    ldx s_p
    lda mr + 1
    sec
    sbc pt_l,x
    clc
    adc #32
    cmp #64
    bcs @next                   ; (unsigned: below 0 too)
    tay
    lda s_hq
    jsr sprite_col
@next:
    inc cx
    bra @x
.else
; the thing s_p (a billboard), at any width: its canvas column is c =
; (k d) / VIEW_W rounded down - l + 32 (k = 2x + 1 - VIEW_W: view.c's
; CANVAS_OF), stepped from column to column instead of a product and a
; division per column: k d grows by 2d a column, kept as a whole part bb_q
; and a remainder bb_r (0 .. VIEW_W - 1), with 2d = VIEW_W bb_a + bb_b.
; Clobbers A, X, Y, cx, s_hq, the maths' variables, bb_*; draws through
; sprite_col.
draw_billboard:
    ldx s_p
    lda pt_hq,x
    sta s_hq
    lda pt_x0,x
    sta cx
    ; N = k d at the first column, signed
    asl
    inc
    sec
    sbc #VIEW_W
    sta mk
    lda pt_d,x
    sta mv
    jsr smul8_16                ; mr = N
    lda mr + 2
    sta bb_neg                  ; (bit 15: N < 0)
    bpl :+
    jsr neg_mr                  ; |N|
:   jsr mr_dn
    lda #VIEW_W
    sta ddv
    jsr udiv32_16               ; dn = |N| / VIEW_W (< 2^15), rem the rest
    lda dn
    sta bb_q
    lda rem
    sta bb_r
    lda bb_neg
    bpl @step
    ; N < 0: rounded down, q = -(|N| / W) - 1 and r = W - rem, unless rem = 0
    lda bb_q
    eor #$FFFF
    inc
    sta bb_q
    lda bb_r
    beq @step
    dec bb_q
    lda #VIEW_W
    sec
    sbc bb_r
    sta bb_r
@step:
    ; the step 2d (d >= 4, 2d < 65536) = VIEW_W a + b
    ldx s_p
    lda pt_d,x
    asl
    sta dn
    stz dn + 2
    lda #VIEW_W
    sta ddv
    jsr udiv32_16
    lda dn
    sta bb_a
    lda rem
    sta bb_b
@x: ldx s_p
    lda cx
    cmp pt_x1,x
    bcc :+
    rts
:   asl
    tay
    lda s_hq
    cmp _col_hq,y
    bcc @next                   ; behind the wall
    lda bb_q                    ; c = q - l + 32
    sec
    sbc pt_l,x
    clc
    adc #32
    cmp #64
    bcs @next                   ; (unsigned: below 0 too)
    tay
    lda s_hq
    jsr sprite_col
@next:
    lda bb_r                    ; the next column: k d + 2d, q += a, r += b
    clc
    adc bb_b
    cmp #VIEW_W
    bcc :+
    sbc #VIEW_W                 ; (r went past VIEW_W - 1: carry 1 into q)
    inc bb_q
:   sta bb_r
    lda bb_q
    clc
    adc bb_a
    sta bb_q
    inc cx
    bra @x
.endif

; the thing s_p (a decal): a quad like a wall's
draw_decal:
    ldx s_p
    lda pt_ax,x
    sta ax_
    lda pt_az,x
    sta az_
    lda pt_bx,x
    sta bx_
    lda pt_bz,x
    sta bz_
    jsr project
    bcc :+
    rts
:   jsr hq_ramp
    lda q_x0
    sta cx
@x: lda cx
    cmp q_x1
    bcc :+
    rts
:   lda acc + 1
    beq @next
    sta s_hq
    lsr
    lsr
    lsr
    lsr
    lsr
    lsr
    clc
    adc s_hq                    ; h + (h >> 6), 17 bits
    bcs @front
    sta t0
    lda cx
    asl
    tay
    lda t0
    cmp _col_hq,y
    bcc @next
@front:
    lda cx                      ; t8 of m2 / num2 at this column
    asl
    inc
    sec
    sbc #VIEW_W
    sta kk
    sta mk
    lda q_da
    sta mv
    jsr smul8_16
    lda q_la
    mul_w q_la                  ; la x VIEW_W
    sta t0
    lda mr
    sec
    sbc t0
    sta dw
    lda q_db
    sec
    sbc q_da
    sta t0
    lda q_lb
    sec
    sbc q_la
    sta t0 + 2
    jsr num2_k
    sta ddv
    jsr t8_of
    lsr
    lsr
    tay
    ldx s_p
    lda s_hq
    jsr sprite_col
@next:
    lda acc
    clc
    adc step
    sta acc
    lda acc + 2
    adc step + 2
    sta acc + 2
    inc cx
    bra @x

; canvas column Y of the thing s_p's sprite at column cx, hq in A
sprite_col:
    clc
    adc #8
    bcc :+
    lda #4096
    bra :++
:   lsr
    lsr
    lsr
    lsr
    bne :+
    rts
:   sta _ds_h
    ldx s_p
    lda pt_spr,x
    sta t0                      ; spr
    xba
    lsr                         ; spr * 128
    sta t0 + 2
    tya
    asl
    clc
    adc t0 + 2                  ; + c * 2
    sta t1
    clc
    adc _vc_spr_cols
    sta sp_
    lda _vc_spr_cols + 2
    adc #0
    tax
    lda sp_
    jsr set_sp
    lda [sp_]                   ; top, len
    sta t1 + 2
    xba
    and #$00FF
    bne :+
    rts
:   sep #$20
    .a8
    sta _ds_len
    lda t1 + 2
    sta _ds_top
    lda t0
    sta _ds_s
    lda cx
    sta _ds_x
    rep #$20
    .a16
    lda t1
    clc
    adc _vc_spr_coff
    sta sp_
    lda _vc_spr_coff + 2
    adc #0
    tax
    lda sp_
    jsr set_sp
    lda [sp_]
    xba
    sta t1                      ; the column's offset
    lda t0
    asl
    asl
    tax
    lda _spr_addr,x
    clc
    adc t1
    sta _ds_data
    lda _spr_addr + 2,x
    adc #0
    sta _ds_data + 2
    jsr _draw_spr
    rts
