; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

; Doom RPG for the SNES with the SuperFX: the things for the GSU, per
; picture (view/view.c's view_render_cam does the same in C on the build
; machine): the map's scenery that is not hidden, then the linked entities.
; ca65. Licence: GPL-2.0-or-later.
;
; Built only for the SuperFX build. view/view.c (VIEW_FX, on the SNES)
; points the _fxl_ pointers at the rules' state arrays and calls fx_list;
; the result, fx_th[] (8 bytes a thing: s16 x, s16 z, u8 sprite, u8 0,
; u16 info; struct fx_th in view/view.h) and fx_nth, is then copied into
; the GSU's RAM by fx.c's fx_draw. Written in assembler for speed: it runs
; for every picture.
;
; "Scenery" are the map's fixed props (fx_scen, made at map load); a prop
; can be taken away (picked up, destroyed), which the rules record as a bit
; in S.static_hidden. "Linked" entities are those placed on the map (the
; rules' F_LINKED flag).

.p816
.smart -
.export _fx_list
.export _fxl_flags, _fxl_def, _fxl_x, _fxl_z, _fxl_defspr, _fxl_hidden
.import _fx_th, _fx_nth, _fx_scen, _fx_scen_hid, _fx_nscen, _n_ents
.import _fxl_px, _fxl_pz, _fxl_lim, _fxl_over   ; (view/view.c: the reach, and "full")

F_LINKED = $04                  ; core/rules.h: the entity is on the map
MAX_TH   = 255                  ; fx_th's size (FX_MAX_TH)

.segment "ZEROPAGE"
_fxl_flags:  .res 2             ; S.e_flags
_fxl_def:    .res 2             ; S.e_def
_fxl_x:      .res 2             ; S.e_x
_fxl_z:      .res 2             ; S.e_z
_fxl_defspr: .res 2             ; the sprite of each def
_fxl_hidden: .res 2             ; S.static_hidden
cnt:         .res 2             ; low byte: scenery left; high byte: scratch

.segment "RODATA"
bits: .byte 1, 2, 4, 8, 16, 32, 64, 128 ; bit n's mask

.segment "CODE"
; void fx_list(void): fx_th / fx_nth
; In: the _fxl_ pointers (direct page, so (dp),y reaches the arrays in
; bank $7E); fx_scen, fx_scen_hid, fx_nscen; n_ents (the entity count).
; fxl_px, fxl_pz (the player's cell) and fxl_lim: only the things whose
; cell is less than fxl_lim cells away on x and on z (256: all).
; Out: fx_th[0..fx_nth-1]: first every prop not hidden, then every linked
; entity with a def, those within reach (at most 255 things in all; the
; props are not counted against the limit); fxl_over 1 if a linked entity
; within reach found fx_th full (view_render_cam then tries nearer).
; Clobbers A, X, Y, cnt.
.a8
.i8
_fx_list:
    php
    rep #$30
    .a16
    .i16
    ldx #0                      ; fx_th offset
    stz _fxl_over
    ; scenery
    ldy #0                      ; its offset (8 a thing)
    lda _fx_nscen
    and #$00FF
    sta cnt
    beq @ents                   ; no props
@scen:                          ; per prop:
    phy
    tya
    lsr
    lsr
    tay                         ; (fx_scen_hid: 2 a thing)
    lda _fx_scen_hid,y          ; its bit number in S.static_hidden
    ply
    cmp #$8000                  ; (-1: never hidden)
    bcs @shown
    pha
    lsr
    lsr
    lsr                         ; the byte: bit number / 8
    phy
    tay
    sep #$20
    .a8
    lda (_fxl_hidden),y
    ply
    sta cnt + 1                 ; (the byte; cnt's high byte is free: < 256 things)
    rep #$20
    .a16
    pla
    and #7                      ; the bit within it
    phy
    tay
    sep #$20
    .a8
    lda bits,y
    ply
    and cnt + 1
    rep #$20
    .a16
    bne @hidden                 ; taken away: skip it
@shown:                         ; within reach? (its cell: world / 64)
    lda _fx_scen,y
    lsr
    lsr
    lsr
    lsr
    lsr
    lsr
    jsr dist
    bcs @hidden                 ; too far on x
    lda _fx_scen + 2,y
    lsr
    lsr
    lsr
    lsr
    lsr
    lsr
    jsr distz
    bcs @hidden                 ; too far on z
    ; copy its 8 bytes to fx_th
    lda _fx_scen,y
    sta _fx_th,x
    lda _fx_scen + 2,y
    sta _fx_th + 2,x
    lda _fx_scen + 4,y
    sta _fx_th + 4,x
    lda _fx_scen + 6,y
    sta _fx_th + 6,x
    txa
    clc
    adc #8                      ; the next fx_th slot
    tax
@hidden:
    tya
    clc
    adc #8                      ; the next prop
    tay
    sep #$20
    .a8
    dec cnt
    rep #$20
    .a16
    bne @scen                   ; (the flags are still those of the 8-bit dec)
@ents:                          ; the entities: Y = the entity's index
    ldy #0
@ent:
    cpy _n_ents
    bcs @done                   ; every entity seen
    sep #$20
    .a8
    lda (_fxl_flags),y
    and #F_LINKED
    beq @next                   ; not on the map
    lda (_fxl_def),y
    cmp #$FF
    beq @next                   ; no def ($FF): nothing to draw
    rep #$20
    .a16
    lda (_fxl_x),y
    and #$00FF
    jsr dist
    bcs @next                   ; too far on x
    lda (_fxl_z),y
    and #$00FF
    jsr distz
    bcs @next                   ; too far on z
    cpx #MAX_TH * 8
    bcc @room
    lda #1                      ; fx_th is full: say so, and stop
    sta _fxl_over
    bra @done
@room:
    sep #$20
    .a8
    lda (_fxl_def),y
    phy
    rep #$20
    .a16
    and #$00FF
    tay
    sep #$20
    .a8
    lda (_fxl_defspr),y         ; the def's sprite
    sta _fx_th + 4,x
    stz _fx_th + 5,x
    ply
    rep #$20
    .a16
    lda (_fxl_x),y
    and #$00FF
    xba
    lsr
    lsr
    ora #32                     ; x = cell * 64 + 32: the cell's centre in world units
    sta _fx_th,x
    lda (_fxl_z),y
    and #$00FF
    xba
    lsr
    lsr
    ora #32                     ; z likewise
    sta _fx_th + 2,x
    stz _fx_th + 6,x            ; info 0
    txa
    clc
    adc #8
    tax
@next:
    rep #$20
    .a16
    iny
    bra @ent
@done:
    txa
    lsr
    lsr
    lsr                         ; fx_th's bytes / 8 = things
    sep #$20
    .a8
    sta _fx_nth
    plp
    rts

; dist / distz: A (16-bit) = a cell's x / z -> carry set if |A - fxl_px|
; (|A - fxl_pz|) >= fxl_lim: out of reach. Clobbers A.
.a16
.i16
dist:
    sec
    sbc _fxl_px
    bra absd
distz:
    sec
    sbc _fxl_pz
absd:
    bpl :+
    eor #$FFFF
    inc
:   cmp _fxl_lim
    rts
