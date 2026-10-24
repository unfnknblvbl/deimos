; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

; Doom RPG for the SNES with the SuperFX: the main CPU's side of the GSU.
; While the GSU runs it has the ROM and its RAM, so the CPU waits in work
; RAM: gsu_init copies the waiting routine there, and the stubs the CPU
; jumps to on an interrupt meanwhile (the GSU gives it the vectors $0108,
; NMI, and $010C, IRQ): the NMI only counts the frame then.
; ca65. Licence: GPL-2.0-or-later.
;
; Built only for the SuperFX build. Callers: fx.c (gsu_run, fx_copy,
; fx_romcopy), ui.c and the tests (gsu_init), video.s (view_upload jumps to
; fx_out; view_busy and the video modes read _fbu_left; nmi_video calls
; WFBU), gfx.s (nmi_gfx subtracts fbu_n from its budget).
;
; GSU registers used (at $00:3000-$303F, reached with long addresses):
;   $301E/$301F R15: the GSU's program counter; writing the high byte
;               starts the GSU
;   $3030 SFR   status; bit 5 GO is set while the GSU runs
;   $3034 PBR   the GSU's program bank
;   $3037 CFGR  bit 7: no IRQ to the CPU at STOP
;   $3038 SCBR  the PLOT screen's base (not used: 0)
;   $3039 CLSR  bit 0: 21 MHz (else 10.7 MHz)
;   $303A SCMR  bit 4 RON, bit 3 RAN: the ROM / the RAM belong to the GSU
;               (the CPU cannot read them meanwhile); the other bits are
;               PLOT's screen mode, left 0
;   $303C RAMBR the GSU's RAM bank (0: $70)
;
; The view's picture (FB_BYTES: 12288 at 128 x 88, 7488 at the SNES
; game's tiled 104 x 72, view.inc) goes from the GSU's RAM to work RAM
; ($7F:0000) by one DMA (fx_out), then up to VRAM in parts of at most
; 4096 bytes, one part per vertical blank (WFBU: three at 128 x 88, two
; at 104 x 72), so drawing the next picture can start at once.

.p816
.smart -
.include "banks.inc"
.include "view.inc"
.export _gsu_init, _gsu_run, _fx_copy, _fxp_dst, _fxp_src, _fxp_n
.export _fx_out, _fbu_left, WFBU, fbu_n, _fx_romcopy, _fxp_bank
.import _frame_count

GSU_BANK = $44                  ; the GSU's program (tools/snes_rom.py --fx)

.segment "LOWRAM"
; the routines run from work RAM (copied by gsu_init): the NMI while the
; GSU runs, the picture's upload, the wait for the GSU
; (their work RAM addresses: wram_code + the routine's place in the template)
WNMI = wram_code + (nmi_tpl - tpl_start)
WFBU = wram_code + (fbu_tpl - tpl_start)
WGO  = wram_code + (go_tpl - tpl_start)
gsu_entry: .res 2               ; gsu_run's entry point (for WGO)
_fbu_left: .res 2               ; the picture's bytes still to go up
fbu_src:   .res 2               ; ($7F:fbu_src)
fbu_dst:   .res 2               ; (VRAM word address)
fbu_n:     .res 2               ; bytes sent in this vblank
_fxp_dst:  .res 2               ; fx_copy, fx_romcopy: the destination in the GSU's RAM ($70)
_fxp_src:  .res 2               ; the source address (bank $7E, or fxp_bank)
_fxp_n:    .res 2               ; bytes
_fxp_bank: .res 2               ; fx_romcopy's source bank (low byte used)

.segment "ZEROPAGE"
zrp:       .res 3               ; fx_romcopy's source pointer (long)

.segment "LOWRAM"

.segment "CODE"

; void gsu_init(void)
; Sets the GSU up (speed, no IRQ, its RAM bank, its program bank), writes
; the interrupt stubs at $00:0108 and $00:010C, copies the work RAM
; routines (tpl_start..tpl_end) to wram_code, and clears the upload state.
; Clobbers A, X. Called once at start-up (ui.c, test_fx.c, test_view.c).
.a8
.i8
_gsu_init:
    php
    sep #$20
    rep #$10
    .a8
    .i16
    lda #1
    sta f:$003039               ; CLSR: 21 MHz
    lda #$80
    sta f:$003037               ; CFGR: no IRQ at STOP (the CPU waits on SFR)
    lda #0
    sta f:$003038               ; SCBR
    sta f:$00303A               ; SCMR: the ROM and the RAM the CPU's
    sta f:$00303C               ; RAMBR: bank $70
    lda #GSU_BANK
    sta f:$003034               ; PBR
    ; the work RAM stubs
    ; (while the GSU has the ROM, the CPU's NMI and IRQ vectors read as
    ; $0108 and $010C: these bytes in low RAM are what then runs)
    lda #$5C                    ; $0108: jml WNMI
    sta f:$000108
    lda #<WNMI
    sta f:$000109
    lda #>WNMI
    sta f:$00010A
    lda #$00
    sta f:$00010B
    lda #$40                    ; $010C: rti
    sta f:$00010C
    ldx #0
:   lda f:CODE_BANK * $10000 + tpl_start,x  ; copy the templates into work RAM
    sta f:wram_code,x
    inx
    cpx #tpl_end - tpl_start
    bne :-
    rep #$20
    .a16
    lda #0
    sta f:_fbu_left
    sta f:fbu_n
    plp
    rts

; jsl here from the NMI (a16 i16): the picture's next piece (fbu_n: its size)
; (Not referenced: video.s's nmi_video calls WFBU directly.)
fbu_nmi:
    jml f:WFBU

; void fx_out(void): the GSU's picture to work RAM ($7F:0000, once the
; last one is up), its upload started (the vblanks do it: WFBU)
; In: the picture at FB_BANK:FB_ADDR ($70:0200); the GSU stopped.
; Out: the copy at $7F:0000; fbu_src = 0, fbu_dst = 0 (VRAM word 0),
; _fbu_left = FB_BYTES. Clobbers A. Uses DMA channel 7. Reached through
; video.s's view_upload (a jmp), so it returns to view_upload's caller.
.a8
.i8
_fx_out:
    php
    sep #$20
    rep #$10
    .a8
    .i16
:   lda f:_fbu_left
    ora f:_fbu_left + 1
    bne :-                      ; wait: the previous picture is still going up
    lda #$00
    sta f:$002181               ; WMADD = $01:0000, i.e. $7F:0000
    sta f:$002182
    lda #$01
    sta f:$002183
    lda #$00                    ; A -> B, one register
    sta f:$004370
    lda #$80                    ; $2180
    sta f:$004371               ; (WMDATA: work RAM through its port)
    lda #FB_BANK
    sta f:$004374
    rep #$20
    .a16
    lda #FB_ADDR
    sta f:$004372
    lda #FB_BYTES
    sta f:$004375
    sep #$20
    .a8
    lda #$80
    sta f:$00420B               ; run channel 7: FB_BYTES bytes
    rep #$20
    .a16
    lda #0
    sta f:fbu_src
    sta f:fbu_dst
    lda #FB_BYTES
    sta f:_fbu_left
    plp
    rts

; void gsu_run(u16 entry): the GSU from entry (in its program bank) to its STOP
; In: A/X = entry (cc65 fastcall). Returns when the GSU has stopped. NMIs
; meanwhile go to WNMI (frame count and the picture's upload only).
; Clobbers A.
_gsu_run:
    sta f:gsu_entry
    txa
    sta f:gsu_entry + 1
    jsl f:WGO                   ; start it and wait, from work RAM
    rts

; void fx_copy(void): fxp_n bytes from work RAM ($7E:fxp_src) to the GSU's
; RAM ($70:fxp_dst) (the GSU stopped)
; Clobbers A, X, Y. Called by fx.c's put.
_fx_copy:
    php
    rep #$30
    .a16
    .i16
    lda f:$7E0000 + _fxp_n
    beq @done                   ; nothing to copy
    dec                         ; MVN: A = count - 1
    ldx _fxp_src
    ldy _fxp_dst
    phb                         ; (MVN leaves the data bank at $70)
    mvn #$7E, #$70
    plb
@done:
    plp
    rts
    .a8
    .i8

; void fx_romcopy(void): fxp_n bytes (even) from the ROM (fxp_bank:fxp_src)
; to the GSU's RAM ($70:fxp_dst) (the GSU stopped)
; A word at a time (an odd count copies one byte more). Clobbers A, X, Y,
; zrp. Called by fx.c's fx_map_load (the map's walls).
_fx_romcopy:
    php
    rep #$30
    .a16
    .i16
    lda f:_fxp_src
    sta zrp
    lda f:_fxp_bank
    sep #$20
    .a8
    sta zrp + 2
    rep #$20
    .a16
    lda f:_fxp_dst
    tax                         ; X = the destination
    ldy #0                      ; Y = the bytes done
@l:
    tya
    cmp f:_fxp_n
    bcs @done                   ; all copied
    lda [zrp],y
    sta f:$700000,x
    inx
    inx
    iny
    iny
    bra @l
@done:
    plp
    rts
    .a8
    .i8

; ---- copied to work RAM (long addresses only)
; These run while the GSU may own the ROM, so they must not touch it: every
; address is a long one into RAM or the registers, and they never call
; ROM code.

; WNMI: the NMI while the GSU runs (entered through the stub at $0108, so
; with whatever widths and banks the CPU had in WGO's wait loop).
; Counts the frame and sends the picture's next part; nothing else
; (no sound, no other uploads). Saves A, X, Y.
tpl_start:
nmi_tpl:
    rep #$30
    .a16
    .i16
    pha
    phx
    phy
    lda f:$7E0000 + _frame_count
    inc
    sta f:$7E0000 + _frame_count
    jsl f:WFBU                  ; the picture's next part
    sep #$20
    .a8
    lda f:$004210               ; RDNMI: acknowledge
    rep #$30
    .a16
    ply
    plx
    pla
    rti

; WFBU: the picture's next part up (in the vertical blank; called with jsl
; by WNMI and by video.s's nmi_video).
; In: _fbu_left, fbu_src, fbu_dst. Out: up to 4096 bytes of $7F:fbu_src
; sent to VRAM's high bytes at fbu_dst; fbu_n = the bytes sent (0 if
; none); fbu_src, fbu_dst advanced, _fbu_left reduced.
; Clobbers A; DMA channel 0.
fbu_tpl:
    php
    rep #$30
    .a16
    .i16
    lda f:_fbu_left
    bne :+
    sta f:fbu_n                 ; nothing to send: fbu_n = 0
    plp
    rtl
:   cmp #4096
    bcc :+
    lda #4096                   ; at most 4 KB a vblank
:   sta f:fbu_n
    sta f:$004305               ; DAS0: the byte count
    lda f:fbu_src
    sta f:$004302               ; A1T0: the source address
    lda f:fbu_dst
    sta f:$002116               ; VMADD
    sep #$20
    .a8
.if FB_TILED
    lda #$80                    ; VRAM high bytes (Mode 7's tiles), as they are (view.inc's FB_TILED)
.else
    lda #$84                    ; VRAM high bytes (Mode 7's tiles), 8-bit rotation
.endif
    sta f:$002115               ; (VMAIN: step after $2119, the address remapped)
    lda #$00
    sta f:$004300               ; DMAP0: one register
    lda #$19
    sta f:$004301               ; BBAD0: VMDATAH ($2119)
    lda #$7F
    sta f:$004304               ; A1B0: bank $7F
    lda #$01
    sta f:$00420B               ; run channel 0
    rep #$20
    .a16
    lda f:fbu_src
    clc
    adc f:fbu_n
    sta f:fbu_src
    lda f:fbu_dst
    clc
    adc f:fbu_n                 ; (a byte per VRAM word)
    sta f:fbu_dst
    lda f:_fbu_left
    sec
    sbc f:fbu_n
    sta f:_fbu_left
    plp
    rtl

; WGO: start the GSU at gsu_entry and wait (in work RAM) until it stops,
; then give the ROM and its RAM back to the CPU. Called with jsl by
; gsu_run. Clobbers A.
go_tpl:
    php
    sep #$20
    .a8
    lda #$18                    ; SCMR: RON | RAN, the GSU's
    sta f:$00303A
    rep #$20
    .a16
    lda f:gsu_entry
    sta f:$00301E               ; R15: the GSU starts
    sep #$20
    .a8
:   lda f:$003030               ; SFR
    and #$20                    ; GO
    bne :-                      ; until the GSU's STOP clears it
    lda #0
    sta f:$00303A               ; the ROM and the RAM back to the CPU
    plp
    rtl
tpl_end:

.segment "LOWRAM"
wram_code: .res tpl_end - tpl_start ; the templates' copy (WNMI, WFBU, WGO)
