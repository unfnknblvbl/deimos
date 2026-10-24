; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

; Doom RPG for the SNES: the 3D view's reads of the campaign's block
; (view/vdata.h): the block is in the ROM from $C20000, big-endian.
; cc65 fastcall: the u32 address in A, X, sreg, sreg+1; results in A/X
; (and sreg). ca65. Licence: GPL-2.0-or-later.
;
; The "campaign block" is the Mega Drive game's blob of the whole game (every map's
; walls, textures, sprites, level data and strings; tools/md_campaign.py
; in the Mega Drive game). tools/snes_rom.py puts it in the ROM at the start of a
; bank and writes that bank number into the cartridge header (HEADER_AT +
; 6, i.e. $FFB6). Its offsets are relative to its start, so a block
; offset becomes a ROM address by adding the block's bank to bits 16-23.
;
; Callers: view/view.c through view/vdata.h (vd_u8, vd_u16, vd_u32),
; vdata.c (vd_words, vd_pack), campaign.c (block_bank), ui.c (rom_u8,
; rom_u16le for the resources bank). Also built in the SuperFX build.
;
; cc65 fastcall: the last argument arrives in registers (a u32 in A = bits
; 0-7, X = bits 8-15, sreg = bits 16-23, sreg+1 = bits 24-31). The
; registers are 8-bit throughout (the C runtime's widths).

.p816
.include "banks.inc"
.smart -
.export _vd_u8, _vd_u16, _vd_u32, _block_bank, _campaign_init
.importzp sreg

.segment "BSS"
_block_bank: .res 1             ; the campaign block's first bank (tools/snes_rom.py): $C2.. plain, $4x SuperFX

.segment "ZEROPAGE"
vp: .res 3                      ; 24-bit pointer for [vp] and [vp],y

.segment "CODE"
.a8
.i8
; void campaign_init(void): where the block is (the header's $FFB6)
; In: the cartridge header (read with a long address, so any data bank).
; Out: _block_bank. Clobbers A. Called once at start-up (ui.c, test_view.c).
_campaign_init:
    lda f:HEADER_AT + 6         ; the header byte snes_rom.py set to the block's bank
    sta _block_bank
    rts

.i8
; set vp = $C20000 + the address in A/X/sreg
; (more exactly: the block's bank, _block_bank, plus the offset.)
; In: A = offset bits 0-7, X = bits 8-15, sreg = bits 16-23 (sreg+1, bits
; 24-31, is ignored: the block is under 16 MB). Out: vp. Clobbers A, flags.
; Called by vd_u8, vd_u16, vd_u32.
setp:
    sta vp
    stx vp + 1
    lda sreg
    clc
    adc _block_bank             ; offset's bank part + the block's first bank
    sta vp + 2
    rts

; u8 vd_u8(u32 a): the byte at block offset a.
; Out: A = the byte, X = 0 (cc65 returns a u8 widened in A/X). Clobbers vp.
_vd_u8:
    jsr setp                    ; vp = the ROM address
    lda [vp]
    ldx #0
    rts

; u16 vd_u16(u32 a): the big-endian word at block offset a.
; Out: A = low byte (the second in the ROM), X = high byte (the first).
; Clobbers Y, vp. The word must not straddle a bank boundary.
_vd_u16:
    jsr setp                    ; vp = the ROM address
    ldy #1
    lda [vp],y                  ; the low byte: keep it on the stack
    pha
    lda [vp]                    ; the high byte, returned in X
    tax
    pla
    rts

; u32 vd_u32(u32 a): the big-endian long at block offset a.
; Out: A = bits 0-7, X = bits 8-15, sreg = bits 16-23, sreg+1 = bits 24-31.
; Clobbers Y, vp.
_vd_u32:
    jsr setp                    ; vp = the ROM address
    ldy #0
    lda [vp],y                  ; byte 0: bits 24-31
    sta sreg + 1
    iny
    lda [vp],y                  ; byte 1: bits 16-23
    sta sreg
    iny
    lda [vp],y                  ; byte 2: bits 8-15
    tax
    iny
    lda [vp],y                  ; byte 3: bits 0-7
    rts

; u8 rom_u8(u32 a), u16 rom_u16le(u32 a): the ROM (or anything) at a long address
; Unlike the vd_ reads, a is a full 24-bit address (not a block offset),
; and rom_u16le reads a little-endian word (the SNES's own resources,
; tools/snes_ui_res.py, are little-endian). Used by ui.c for the cartridge
; header and the resources bank.
; Out: rom_u8: A = the byte, X = 0. rom_u16le: A = low byte, X = high byte.
; Clobbers Y (rom_u16le), vp.
.export _rom_u8, _rom_u16le
_rom_u8:
    sta vp                      ; vp = the address in A/X/sreg
    stx vp + 1
    lda sreg
    sta vp + 2
    lda [vp]
    ldx #0
    rts

_rom_u16le:
    sta vp                      ; vp = the address in A/X/sreg
    stx vp + 1
    lda sreg
    sta vp + 2
    ldy #1
    lda [vp],y                  ; the second byte is the high one
    tax
    lda [vp]                    ; the first, the low one
    rts
