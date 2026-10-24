; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

; Doom RPG for the SNES: the battery-backed RAM (8 KB at SRAM_AT), a byte
; at a time for save.c. ca65. Licence: GPL-2.0-or-later.
;
; The plain build's cartridge has its battery RAM at $30:6000-$7FFF (the
; HiROM SRAM window); the SuperFX build uses the last 8 KB of the GSU's
; RAM at $70:E000 (banks.inc's SRAM_AT). Offsets are masked to 13 bits
; (0-$1FFF), so a bad offset wraps within the 8 KB instead of writing
; elsewhere. save.c lays the blocks out.
; Both routines are called from C with the registers 8-bit; they widen X
; for the 16-bit index and restore the caller's widths with plp.
.p816
.include "banks.inc"
.smart -
.export _sram_rd, _sram_wr
.import popax

.segment "ZEROPAGE"
si: .res 2                      ; the offset (0-$1FFF after masking)
sv: .res 1                      ; the byte to write (sram_wr)

.segment "CODE"
.a8
.i8
; u8 sram_rd(u16 i)
; In: A/X = i (cc65 fastcall: A low byte, X high byte).
; Out: A = the byte at SRAM_AT + (i & $1FFF), X = 0. Clobbers si.
_sram_rd:
    sta si
    stx si + 1
    php
    rep #$30                    ; A, X, Y 16-bit
    .a16
    .i16
    lda si
    and #$1FFF                  ; keep it within the 8 KB
    tax
    sep #$20                    ; A back to 8-bit for the byte read
    .a8
    lda f:SRAM_AT,x             ; long address: whatever the data bank
    plp                         ; caller's widths (X back to 8 bits)
    .a8
    .i8
    ldx #0
    rts

; void sram_wr(u16 i, u8 v)
; In: A = v (the last argument, in registers); i on cc65's C stack.
; Out: the byte written. Clobbers A, X, si, sv; pops i off the C stack.
_sram_wr:
    sta sv
    jsr popax                   ; cc65 runtime: pop i off the C stack into A/X
    sta si
    stx si + 1
    php
    rep #$30
    .a16
    .i16
    lda si
    and #$1FFF                  ; keep it within the 8 KB
    tax
    sep #$20
    .a8
    lda sv
    sta f:SRAM_AT,x             ; into the battery RAM
    plp
    rts
