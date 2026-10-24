; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

; Doom RPG for the SNES: getting pictures to the PPU.
;
;  - an upload queue: VRAM and CGRAM copies the NMI makes during vblank
;    (up to VQ_BUDGET bytes a frame; the view's picture goes this way too)
;  - the sprite table (OAM): a shadow copy in work RAM, sent each frame
;  - Mega Drive tiles (the original game's graphics as the Mega Drive game stores
;    them: 4 bits a pixel, two pixels a byte) to SNES tiles (bitplanes)
;  - the text layer's tiles: a box of characters drawn into sprite tiles
;
; Callers: ui.c (everything), video.s (view_upload, cgram_load queue
; copies; nmi_video calls nmi_gfx), fx.c / gsu_cpu.s in the SuperFX build.
; The C side is declared in gfx.h.
;
; Why a queue: VRAM, CGRAM and OAM can only be written while the screen
; is not being drawn (the vertical blank, or forced blank). C queues
; copies at any time; the NMI (nmi_gfx) sends them by DMA, VQ_BUDGET bytes
; a frame at most so the work fits in the vertical blank.
;
; DMA: all copies use DMA channel 0 (fill7f uses channel 7). A channel's
; registers: $43x0 DMAP (mode), $43x1 BBAD (the B-bus register, $21xx),
; $43x2-4 the source (address, bank), $43x5-6 the byte count (0 = 65536);
; writing bit x of $420B (MDMAEN) runs channel x at once.
;
; Bank $7F work RAM used here: $7F3000-$7F5FFF the text layer's tiles
; (TXT), $7FD000- the font as SNES tiles (FONT, made by ui.c with
; md_tiles).
;
; ca65. Licence: GPL-2.0-or-later.

.p816
.include "banks.inc"
.smart -
.export _vq_add, _vq_src, _vq_dst, _vq_len, _vq_mode, _vq_flush, _vq_busy, nmi_gfx
.export _oam, _oam_dirty, _gfx_init, _md_tiles, _mt_src, _mt_dst, _mt_n
.export _txt_render, _txt_chars, _font_ready

VQ_N      = 16                  ; entries (a power of two); the queue holds VQ_N - 1
VQ_BUDGET = 4700                ; bytes a vblank
CONV      = TABLES1 + $9000     ; the conversion tables (tools/snes_tables.py)
                                ; (16 tables of 256 bytes: at ((k * 4 + j) * 256 + b),
                                ; bitplane k of the MD byte b's two pixels placed
                                ; where they go in an SNES tile row, for byte j of the row)
FONT      = $7FD000             ; the font as SNES tiles (96 x 32 bytes)
TXT       = $7F3000             ; the text layer's tiles (384 x 32 bytes)

.segment "LOWRAM"
; the queue (in bank-0 RAM: the NMI reads it): kind (1 VRAM words, 2 VRAM
; high bytes, 3 CGRAM), VMAIN, destination, source (long), length
; (one array per field; byte fields indexed by the slot, word fields by
; the slot * 2)
vq_kind:  .res VQ_N             ; 1 VRAM words, 2 VRAM high bytes, 3 CGRAM
vq_vmain: .res VQ_N             ; VMAIN ($2115) for kind 2
vq_dst_:  .res 2 * VQ_N         ; VRAM word address, or CGRAM colour index
vq_srcl:  .res 2 * VQ_N         ; source address (low 16 bits)
vq_srcb:  .res VQ_N             ; source bank
vq_len_:  .res 2 * VQ_N         ; bytes (at most 4096)

.segment "BSS"
_vq_src:  .res 4                ; vq_add's arguments
_vq_dst:  .res 2
_vq_len:  .res 2
_vq_mode: .res 2                ; low byte: 0 VRAM words ($2118/9), 1 VRAM high bytes ($2119,
                                ; Mode 7 tiles), 2 CGRAM; high byte: VMAIN for 1
_oam:     .res 544              ; the OAM's shadow: 128 sprites x 4 bytes (x, y, tile,
                                ; attributes), then 32 bytes of 2 bits each (x bit 8, size)
_oam_dirty: .res 1              ; nonzero: nmi_gfx sends _oam at the next vblank
_mt_src:  .res 4                ; md_tiles' arguments: source (long), destination (bank $7F), tiles
_mt_dst:  .res 2
_mt_n:    .res 2
_txt_chars: .res 16 * 24        ; the text layer (rows of 24 characters)
_font_ready: .res 1             ; set by ui.c once the font is at FONT (gfx_init clears it)

.segment "LOWRAM"
vq_head:  .res 1                ; the next slot to send (head == tail: empty)
vq_tail:  .res 1                ; the next slot to fill
vq_left:  .res 2                ; bytes the current vblank may still send

.segment "ZEROPAGE"
gp:       .res 3                ; md_tiles: the source pointer (long)
gq:       .res 3                ; md_tiles: the destination; txt_render: scratch
gk:       .res 2                ; md_tiles: tiles left; txt_render: the column c
gr:       .res 2                ; md_tiles: the row in the tile; txt_render: the row r

.segment "CODE"

; ---------------------------------------------------------------- the queue
; void vq_add(void): queue a copy (vq_src, vq_dst, vq_len, vq_mode); waits
; while the queue is full. Copies bigger than 4 KB are split.
; In: _vq_src (long), _vq_dst (VRAM word address or CGRAM colour), _vq_len
; (bytes; 0 queues nothing), _vq_mode (see above).
; Out: the queue's new entries. _vq_src, _vq_dst and _vq_len are used up
; (advanced past the copy, _vq_len 0). Clobbers A, X, Y.
; If the queue is full it waits for the NMI to empty it, so it must not be
; called with NMIs off (use vq_flush then).
.a8
.i8
_vq_add:
    php
    rep #$30
    .a16
    .i16
@more:                          ; per part of at most 4096 bytes:
    lda _vq_len
    bne :+
    plp                         ; all queued
    rts
:   ; wait for room
@wait:
    sep #$20
    .a8
    lda vq_tail
    inc
    and #VQ_N - 1
    cmp vq_head
    rep #$20
    .a16
    beq @wait                   ; tail + 1 == head: full, the NMI will make room
    lda vq_tail
    and #$00FF
    tax                         ; X = the slot (byte fields)
    asl
    tay                         ; Y = the slot * 2 (word fields)
    lda _vq_mode
    sep #$20
    .a8
    inc
    sta vq_kind,x               ; 1 VRAM words, 2 VRAM high bytes, 3 CGRAM
    xba                         ; the mode's high byte: VMAIN
    sta vq_vmain,x
    lda _vq_src + 2
    sta vq_srcb,x
    rep #$20
    .a16
    lda _vq_src
    sta vq_srcl,y
    lda _vq_dst
    sta vq_dst_,y
    lda _vq_len
    cmp #4097
    bcc :+
    lda #4096                   ; at most 4 KB an entry
:   sta vq_len_,y
    ; the rest, after this part
    pha
    clc
    adc _vq_src
    sta _vq_src
    lda _vq_src + 2
    adc #0                      ; (carry into the bank)
    sta _vq_src + 2
    lda _vq_mode
    and #$00FF
    cmp #1
    beq @bytes                  ; high bytes: a VRAM word per byte
    lda 1,s                     ; words (VRAM, CGRAM): the destination moves len / 2
    lsr
    bra :+
@bytes:
    lda 1,s
:   clc
    adc _vq_dst
    sta _vq_dst
    lda _vq_len
    sec
    sbc 1,s
    sta _vq_len
    pla
    sep #$20
    .a8
    lda vq_tail
    inc
    and #VQ_N - 1
    sta vq_tail                 ; publish the entry (the NMI may send it from now on)
    rep #$20
    .a16
    jmp @more

; u8 vq_busy(void): 1 while copies wait
; Out: A = 1 or 0, X = 0. Called from C (8-bit registers).
_vq_busy:
    lda vq_head
    cmp vq_tail
    beq :+
    lda #1
    ldx #0
    rts
:   lda #0
    ldx #0
    rts

; void vq_flush(void): with the screen off, make the copies now
; (forced blank: the PPU may be written at any time). Sends the whole queue
; with no budget. Clobbers A, X, Y.
_vq_flush:
    php
    rep #$30
    .a16
    .i16
    phb
    pea $8080
    plb
    plb                         ; data bank $80: the hardware registers (as in the NMI)
:   lda #$FFFF
    sta vq_left                 ; no budget
    jsr vq_run                  ; send entries
    sep #$20
    .a8
    lda vq_head
    cmp vq_tail
    rep #$20
    .a16
    bne :-                      ; until the queue is empty
    plb
    plp
    rts

; in the NMI: a16 i16, data bank $80
; Called by video.s's nmi_video. Sends the sprite table if it changed, then
; the queued copies, VQ_BUDGET bytes at most (less, in the SuperFX build,
; what the picture's upload took this vblank).
; Clobbers A, X, Y; uses DMA channel 0.
.a16
.i16
nmi_gfx:
    sep #$20
    .a8
    lda f:$7E0000 + _oam_dirty
    beq @nooam                  ; the sprite table did not change
    lda #0
    sta f:$7E0000 + _oam_dirty
    stz $2102                   ; OAMADDL/H: OAM address 0
    stz $2103
    lda #$00                    ; one register, A -> B
    sta $4300
    lda #$04                    ; $2104
    sta $4301                   ; (OAMDATA)
    rep #$20
    .a16
    lda #.loword(_oam)
    sta $4302
    lda #544
    sta $4305
    sep #$20
    .a8
    lda #$7E
    sta $4304
    lda #1
    sta $420B                   ; run channel 0: the whole sprite table
@nooam:
    rep #$20
    .a16
    lda #VQ_BUDGET
.ifdef SUPERFX
    sec                         ; less what the picture took (gsu_cpu.s)
    sbc f:fbu_n
.endif
    sta vq_left
    ; (fall through)

; copies from the queue while the budget lasts (at least one)
; In: vq_left = the budget (bytes); a16 i16, data bank $80.
; Out: the sent entries removed (vq_head advanced), vq_left reduced.
; An entry bigger than what is left waits for the next vblank, unless
; vq_left is still the full VQ_BUDGET (then it goes anyway, so a big entry
; can never block the queue). Clobbers A, X, Y; DMA channel 0.
vq_run:
    sep #$20
    .a8
    lda vq_head
    cmp vq_tail
    bne :+
    rep #$20                    ; empty: done
    .a16
    rts
    .a8
:   rep #$20
    .a16
    and #$00FF
    tax                         ; X = the slot (byte fields)
    asl
    tay                         ; Y = the slot * 2 (word fields)
    lda vq_len_,y
    cmp vq_left
    bcc :+
    beq :+                      ; it fits
    lda vq_left                 ; over the budget: only if nothing went yet
    cmp #VQ_BUDGET
    beq :+
    rts
:   lda vq_left
    sec
    sbc vq_len_,y
    bcs :+
    lda #0                      ; (went over: nothing left)
:   sta vq_left
    lda vq_srcl,y
    sta $4302                   ; A1T0: the source address
    lda vq_len_,y
    sta $4305                   ; DAS0: the byte count
    sep #$20
    .a8
    lda vq_srcb,x
    sta $4304                   ; A1B0: the source bank
    lda vq_kind,x
    cmp #3
    beq @cg
    cmp #2
    beq @hi
    lda #$80                    ; VRAM words: increment after $2119
    sta $2115
    rep #$20
    .a16
    lda vq_dst_,y
    sta $2116                   ; VMADD: the VRAM word address
    sep #$20
    .a8
    lda #$01                    ; two registers
    sta $4300                   ; (DMAP mode 1: $2118, $2119, $2118, ...)
    lda #$18
    sta $4301                   ; BBAD: VMDATAL ($2118)
    bra @go
@hi:                            ; VRAM high bytes only (Mode 7's tile pixels)
    lda vq_vmain,x
    sta $2115                   ; VMAIN as asked (the view: $84, see video.s)
    rep #$20
    .a16
    lda vq_dst_,y
    sta $2116
    sep #$20
    .a8
    lda #$00
    sta $4300                   ; DMAP mode 0: one register
    lda #$19
    sta $4301                   ; BBAD: VMDATAH ($2119)
    bra @go
@cg:                            ; CGRAM: colours, two bytes each
    lda vq_dst_,y
    sta $2121                   ; CGADD: the first colour
    lda #$00                    ; one register, written repeatedly
    sta $4300
    lda #$22
    sta $4301                   ; BBAD: CGDATA ($2122)
@go:
    lda #1
    sta $420B                   ; run channel 0 (the CPU waits until it is done)
    lda vq_head
    inc
    and #VQ_N - 1
    sta vq_head                 ; the entry is sent
    rep #$20
    .a16
    lda vq_left
    beq :+
    jmp vq_run                  ; budget left: the next entry
:   rts

; ---------------------------------------------------------------- tiles
; void gfx_init(void): the conversion tables, the sprite table cleared
; (the conversion tables are in the ROM, made by tools/snes_tables.py:
; nothing to set up for them here.)
; Out: every sprite at y 224 (below the screen), tile 0, small; _oam_dirty
; set; _font_ready 0; the upload queue emptied. Clobbers A, X.
; Called once at start-up (ui.c, the tests).
.a8
.i8
_gfx_init:
    php
    rep #$30
    .a16
    .i16
    ; the sprites off screen
    ldx #0
:   lda #$E000                  ; x 0, y 224
    sta _oam,x                  ; (bytes: x = $00, y = $E0)
    lda #0
    sta _oam + 2,x              ; tile 0, attributes 0
    inx
    inx
    inx
    inx
    cpx #512
    bcc :-                      ; 128 sprites
    lda #0
:   sta _oam,x                  ; the high table: x bit 8 clear, the small size
    inx
    inx
    cpx #544
    bcc :-
    sep #$20
    .a8
    lda #1
    sta _oam_dirty
    stz _font_ready
    stz vq_head
    stz vq_tail
    plp
    rts

; void md_tiles(void): mt_n tiles from mt_src (Mega Drive, long) to
; mt_dst (SNES 4bpp, in bank $7F)
; A Mega Drive tile: 8 rows of 4 bytes, each byte two pixels (the left in
; the high nibble), 32 bytes. An SNES 4bpp tile: 32 bytes, rows of
; bitplanes 0 and 1 first (2 bytes a row), then rows of bitplanes 2 and 3.
; Each SNES plane byte is the OR of four CONV lookups, one per MD byte of
; the row.
; In: _mt_src, _mt_dst, _mt_n. Out: the tiles at $7F:mt_dst.
; Clobbers A, X, Y, gp, gq, gk, gr, mb0-mb3, mbp. Called by ui.c.
.a8
.i8
_md_tiles:
    php
    phb
    rep #$30
    .a16
    .i16
    lda _mt_src
    sta gp
    lda _mt_src + 2
    sta gp + 2                  ; (gp + 2: the bank; the high byte lands in gq, set next)
    lda _mt_dst
    sta gq
    lda _mt_n
    sta gk
    pea $7F7F
    plb
    plb                         ; data bank $7F: the destination
@tile:                          ; per tile:
    lda gk
    bne :+
    plb                         ; all done
    plp
    rts
:   ldy #0
    stz gr                      ; row
@row:                           ; per row of the tile:
    sep #$20
    .a8
    lda #0                      ; B = 0 (tax below)
    xba
    ; the row's four bytes, then each plane
    lda [gp],y
    sta f:$7E0000 + mb0
    iny
    lda [gp],y
    sta f:$7E0000 + mb1
    iny
    lda [gp],y
    sta f:$7E0000 + mb2
    iny
    lda [gp],y
    sta f:$7E0000 + mb3
    iny
    phy                         ; (keep the source index)
    ldy gq                      ; Y = the destination row
.repeat 4, K                    ; per bitplane K: OR the four bytes' bits together
    lda f:$7E0000 + mb0
    tax
    lda f:CONV + (K * 4 + 0) * 256,x
    sta f:$7E0000 + mbp
    lda f:$7E0000 + mb1
    tax
    lda f:CONV + (K * 4 + 1) * 256,x
    ora f:$7E0000 + mbp
    sta f:$7E0000 + mbp
    lda f:$7E0000 + mb2
    tax
    lda f:CONV + (K * 4 + 2) * 256,x
    ora f:$7E0000 + mbp
    sta f:$7E0000 + mbp
    lda f:$7E0000 + mb3
    tax
    lda f:CONV + (K * 4 + 3) * 256,x
    ora f:$7E0000 + mbp
    .if K = 0                   ; planes 0, 1 at row * 2 + 0, 1; planes 2, 3 at + 16, 17
    sta 0,y
    .elseif K = 1
    sta 1,y
    .elseif K = 2
    sta 16,y
    .else
    sta 17,y
    .endif
.endrepeat
    ply
    rep #$20
    .a16
    inc gq
    inc gq                      ; the next row: 2 bytes on
    inc gr
    lda gr
    cmp #8
    bcs :+
    jmp @row                    ; 8 rows
:   lda gq                      ; next tile: the planes 2-3 half is filled
    clc
    adc #16
    sta gq
    tya                         ; the source: 32 bytes on
    clc
    adc gp
    sta gp
    bcc :+
    sep #$20
    .a8
    inc gp + 2
    rep #$20
    .a16
:   dec gk
    jmp @tile

.segment "LOWRAM"
mb0: .res 1                     ; md_tiles: the row's four MD bytes
mb1: .res 1
mb2: .res 1
mb3: .res 1
mbp: .res 1                     ; md_tiles: the plane byte being built

.segment "CODE"
; ---------------------------------------------------------------- text
; void txt_render(void): the text layer's characters (txt_chars, 16 rows
; of 24) into its sprite tiles. The layer is shown as 16x16 sprites, each
; holding 2 x 2 characters: sprite s = (c >> 1) + 12 (r >> 1). In the
; tile area the sprites are laid 8 to a row of 16 tiles (as a 16x16
; sprite needs: tiles t, t + 1, t + 16, t + 17), so sprite s starts at
; tile (s % 8) * 2 + (s / 8) * 32, and character (c, r) is tile
; (s % 8) * 2 + (s / 8) * 32 + (r & 1) * 16 + (c & 1) of TXT.
; Characters 32..127 from the font; 'a'..'z' shown as capitals; 0 is
; see-through (font tile 0); space and control characters are the opaque
; blank (font tile 95).
; In: _txt_chars, the font at FONT. Out: 384 tiles at TXT ($7F3000).
; Clobbers A, X, Y, gq, gk, gr. Called by ui.c (which then queues TXT
; for upload to VRAM).
.a8
.i8
_txt_render:
    php
    phb
    rep #$30
    .a16
    .i16
    stz gr                      ; r
@r: stz gk                      ; c
@c: lda gr                      ; the character
    asl
    asl
    asl                         ; r * 8
    sta gq
    asl                         ; r * 16
    clc
    adc gq                      ; r * 24
    adc gk
    tax
    lda _txt_chars,x
    and #$00FF
    beq @font                   ; 0: see-through (the font's space, cleared)
    cmp #'a'
    bcc :+
    cmp #'z' + 1
    bcs :+
    sbc #31                     ; (carry clear: - 32)
:   sec
    sbc #33
    bcs :+
    lda #94                     ; a space or a control character: the opaque blank
:   inc                         ; font tile = character - 32
    cmp #96
    bcc :+
    lda #95                     ; (past the font: the blank)
:
@font:
    asl                         ; font tile * 32
    asl
    asl
    asl
    asl
    clc
    adc #.loword(FONT)
    tax                         ; source
    ; the destination tile: sprite s = (c >> 1) + 12 (r >> 1)
    lda gr
    lsr
    sta gq
    asl
    adc gq                      ; (r >> 1) * 3
    asl
    asl                         ; * 12
    sta gq
    lda gk
    lsr
    clc
    adc gq                      ; s
    pha
    and #7
    asl                         ; (s % 8) * 2
    sta gq
    pla
    lsr
    lsr
    lsr
    xba
    lsr
    lsr
    lsr                         ; (s / 8) * 32
    clc
    adc gq
    sta gq
    lda gr
    and #1
    beq :+
    lda #16
:   clc
    adc gq
    sta gq
    lda gk
    and #1
    clc
    adc gq                      ; the tile
    asl
    asl
    asl
    asl
    asl                         ; * 32
    clc
    adc #.loword(TXT)
    tay                         ; Y = the destination
    lda #31                     ; 32 bytes: one tile
    mvn #$7F, #$7F              ; (also sets the data bank to $7F)
    pea $7E7E
    plb
    plb                         ; data bank back to $7E
    inc gk
    lda gk
    cmp #24
    bcs :+
    jmp @c                      ; the next column
:   inc gr
    lda gr
    cmp #16
    bcs :+
    jmp @r                      ; the next row
:   plb
    plp
    rts

; void fill7f(u16 at, u16 len): zeros into bank $7F (DMA from a zero in the ROM)
; Uses the WRAM port: $2181-$2183 (WMADD) the work RAM address ($7F:at),
; then DMA channel 7 from one fixed zero byte in the ROM to $2180 (WMDATA),
; which steps the address itself. (DMA cannot copy work RAM to work RAM,
; hence a byte in the ROM.) len 0 means 65536 bytes.
; In: A/X = len, at on the C stack. Clobbers A, X, fl_len.
.import popax
.ifdef SUPERFX
.import fbu_n
.endif
.export _fill7f
.a8
.i8
_fill7f:
    sta fl_len                  ; (data bank $7E: the caller's)
    stx fl_len + 1
    jsr popax                   ; at, off the C stack into A/X
    php
    sta f:$002181               ; WMADDL
    txa
    sta f:$002182               ; WMADDM
    lda #1
    sta f:$002183               ; WMADDH: bank $7F
    lda #$08
    sta f:$004370               ; DMAP7: fixed source address, one register
    lda #$80
    sta f:$004371               ; BBAD7: WMDATA ($2180)
    rep #$20
    .a16
    lda #.loword(zero1)
    sta f:$004372
    lda fl_len
    sta f:$004375
    sep #$20
    .a8
    lda #CODE_BANK
    sta f:$004374
    lda #$80
    sta f:$00420B               ; run channel 7
    plp
    rts

zero1: .byte 0                  ; the DMA's source (in the code bank's ROM)

.segment "LOWRAM"
fl_len: .res 2                  ; fill7f's length
