; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

; Doom RPG for the SNES: reading the ROM beyond the code bank (the
; campaign's block from $C20000 on). C sees one 64 KB bank; these copy
; from anywhere in the ROM into it. ca65.
; Licence: GPL-2.0-or-later.
;
; Callers: src/campaign.c (blk_copy, blk_strcpy, blk_u32 and everything
; built on them: glue.c's level and string loads, ui.c's resources).
;
; How C calls these: cc65's code runs with the 65816 in native mode and A,
; X and Y 8 bits wide (the m and x flags set), and cc65 only knows 16-bit
; pointers, which address the data bank ($7E, work RAM). So C fills in
; the parameter block below (_far_src, _far_dst, _far_len) and then calls
; a routine with no arguments. Each routine saves the flags (php),
; widens the registers for its loop and restores them (plp) on return.
;
; The "long" address used is a 24-bit one: bank byte, then a 16-bit
; address within the bank. The reads go through the 65816's [dp],y
; addressing (a 24-bit pointer in the direct page plus Y).

.p816
.smart -
.export _far_copy, _far_strcpy, _far_u32, _far_src, _far_dst, _far_len
.importzp ptr1

.segment "LOWRAM"
; The parameter block (in low RAM, bank $7E $0200-$0FFF; C sees it as
; ordinary variables, declared in campaign.h).
_far_src: .res 3                 ; a long address: low byte, high byte, bank
          .res 1                 ; (padding: C declares far_src as u8[4])
_far_dst: .res 2                 ; the destination, in bank $7E (the data bank)
_far_len: .res 2                 ; byte count (far_copy) or most bytes (far_strcpy); counted down to 0

.segment "ZEROPAGE"
fp:       .res 3                 ; the 24-bit source pointer for [fp],y (a copy of _far_src)

.segment "CODE"

; void far_copy(void): _far_len bytes from _far_src (long) to _far_dst (bank $7E)
; In:  _far_src, _far_dst, _far_len (0 copies nothing).
; Out: the bytes at _far_dst; _far_len is 0 afterwards.
; Clobbers: A, X, Y, fp. The flags (register widths) are restored by plp.
; The source may cross bank boundaries: [fp],y carries into the bank
; byte, and when Y wraps past $FFFF the pointer's bank is stepped by one.
.a8
.i8
_far_copy:
    php                         ; save the caller's register widths
    rep #$30                    ; A, X, Y 16-bit
    .a16
    .i16
    lda _far_src                ; fp = _far_src (16 bits, then the bank byte)
    sta fp
    sep #$20
    .a8
    lda _far_src + 2
    sta fp + 2
    rep #$20
    .a16
    ldx _far_dst                ; X walks the destination
    ldy #0                      ; Y walks the source
    lda _far_len
    beq @done                   ; nothing to copy
@lp:                            ; per byte:
    sep #$20
    .a8
    lda [fp],y
    sta a:$0000,x               ; (data bank $7E)
    rep #$20
    .a16
    inx
    iny
    bne @same
    sep #$20                    ; y wrapped: the next bank
    .a8
    inc fp + 2
    rep #$20
    .a16
@same:
    dec _far_len                ; (16-bit decrement: A is 16-bit here)
    bne @lp
@done:
    plp                         ; back to the caller's 8-bit registers
    rts

; void far_strcpy(void): like far_copy, up to and with the first NUL
; In:  _far_src, _far_dst, _far_len = the most bytes it copies.
; Out: the string at _far_dst. The NUL is copied when it is met within
;      _far_len bytes; otherwise nothing ends the copy (blk_strcpy in
;      campaign.c writes a NUL after it). _far_len is left at what was
;      not used.
; Clobbers: A, X, Y, fp. Unlike far_copy it does not step the bank when Y
; wraps, so a string must not be longer than 64 KB (they are far shorter).
.a8
.i8
_far_strcpy:
    php
    rep #$30
    .a16
    .i16
    lda _far_src
    sta fp
    sep #$20
    .a8
    lda _far_src + 2
    sta fp + 2
    rep #$20
    .a16
    ldx _far_dst
    ldy #0
@lp:
    lda _far_len
    beq @done                   ; the most bytes copied
    sep #$20
    .a8
    lda [fp],y
    sta a:$0000,x
    beq @done8                  ; it was the NUL: done (the flags are from the lda)
    rep #$20
    .a16
    inx
    iny
    dec _far_len
    bra @lp
@done8:
@done:
    plp
    rts

; u32 far_u32(void): the big-endian long at _far_src (cc65: A/X low word, sreg high word)
; In:  _far_src. A stays 8-bit; only X and Y are widened.
; Out: A = bits 0-7, X = bits 8-15, sreg = bits 16-23, sreg+1 = bits 24-31
;      (cc65's 32-bit return registers). The block stores its numbers
;      big-endian (the Mega Drive game's 68000 format), so the first byte read is the
;      top one.
; Clobbers: Y, fp.
.importzp sreg
_far_u32:
    php
    rep #$10                    ; X, Y 16-bit (A stays 8-bit)
    .i16
    lda _far_src                ; fp = _far_src, a byte at a time
    sta fp
    lda _far_src + 1
    sta fp + 1
    lda _far_src + 2
    sta fp + 2
    ldy #0
    lda [fp],y                  ; byte 0: bits 24-31
    sta sreg + 1
    iny
    lda [fp],y                  ; byte 1: bits 16-23
    sta sreg
    iny
    lda [fp],y                  ; byte 2: bits 8-15, returned in X
    tax                         ; (16-bit tax; plp then cuts X back to 8 bits)
    iny
    lda [fp],y                  ; byte 3: bits 0-7, returned in A
    plp
    rts
