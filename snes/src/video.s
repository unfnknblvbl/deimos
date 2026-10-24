; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

; Doom RPG for the SNES: the screen.
;
; The game screen (video_game): line 0..7 the message line, 8..183 the 3D
; view (Mode 7 at twice its size: 128x88 -> 256x176), 184..223 the status
; bar. A smaller view (view.inc: the SNES game's 104x72 -> 208x144) is
; centred in that area with a black border: VIEW_PAD_Y lines above and
; below it (the backdrop black there: view_gradient's table) and
; VIEW_PAD_X pixels left and right (the colour window clips the main
; screen to black outside the view on its lines: HDMA channel 4 switches
; CGWSEL per band of lines, win_hdma). Mode 7's centre (M7X, M7Y) puts
; the picture's corner at the view's place. The message line and the status bar are Mode 1 (BG1), switched by
; HDMA; the floor and ceiling are the backdrop (colour 0), shaded per line
; by HDMA (view_gradient's table). BG1's vertical scroll is -8 for Mode 7,
; so in Mode 1 the message line is BG1 map row 31 and the status bar rows
; 22..26 (with the smaller view: row 0, at line 8, and rows 20..24, lines
; 168..207, clear of the lines a TV's overscan hides). Boxes of text and the weapon are sprites.
; The full screen (video_full): Mode 1 everywhere (the title).
;
; VRAM (words): $0000-$3FFF Mode 7 (the map in the low bytes: screen tile
; (tx, ty) is tile tx * 12 + ty at 128 x 88, see below for a smaller
; view; the tiles in the high bytes) - or the
; title's tiles; $4000-$5BFF BG tiles (the font, the faces); $5C00 BG1's
; map (32x32); $6000-$7FFF sprite tiles (16x16 and 32x32).
;
; CGRAM: 0 the backdrop, 1-15 the status bar, 16-31 its faces' second
; palette, 32-223 the view's colours, 224-239 the text boxes (sprite
; palette 6), 240-255 the weapon (sprite palette 7).
;
; The view's picture: view/view.c (or the GSU) draws 128 x 96 bytes, one
; colour index each, in the order of view/view.h's FB_OFF; it goes into
; VRAM's high bytes (Mode 7's tile pixels) with VMAIN $84, whose address
; remapping turns that order into the tiles' own (8 x 8 pixels, a row at a
; time). The map's 16 x 12 tiles are numbered column by column, which is
; what video_init's map holds. A smaller view's picture is tiled
; (view.inc's FB_TILED: VIEW_W x VIEW_H bytes, FB_BYTES): each tile holds
; its pixels column by column, i.e. transposed, so it goes up as it is
; (VMAIN $80) and Mode 7's matrix swaps x and y back (texel x from the
; screen line, texel y from the screen column); the map has the
; picture's strips as its rows (video_init). No rows below the view go
; up: 7488 bytes at 104 x 72, two vertical blanks' worth.
;
; The damage flash: colour math adds a fixed red to the whole screen for
; flash_n frames (nmi_video).
;
; Callers: ui.c and the tests (video_init, video_game, video_full,
; screen_on/off, view_upload, view_busy, cgram_load, flash), video.c (the
; HDMA table grad_hdma), hw.s's nmi_body (nmi_video).

.p816
.smart -
.include "banks.inc"
.include "view.inc"
.export _video_init, _video_game, _video_full, _screen_on, _screen_off
.export _view_upload, _view_busy, nmi_video, _grad_hdma, _cgram_load, _flash
.import _view_cgram, _view_n_colours
.import _vq_add, _vq_src, _vq_dst, _vq_len, _vq_mode, _vq_busy, nmi_gfx
.ifdef SUPERFX
.import _fx_out, _fbu_left, WFBU
.endif

.segment "LOWRAM"
flash_n:   .res 1               ; frames of the damage flash left
flash_on:  .res 1               ; nonzero while the red is being added
mval:      .res 1               ; video_init: the map entry being written

.segment "BSS"
; the backdrop colour per line, for HDMA channel 5 (filled by video.c's
; view_gradient): VIEW_TOP lines black (8 at the full size), 127 +
; (VIEW_LINES - 127) lines (the view, one entry a line: a line count is at
; most 127; 127 + 49 at the full size), 1 line black, the end
_grad_hdma: .res 1 + 4 + 1 + 127 * 4 + 1 + (VIEW_LINES - 127) * 4 + 1 + 4 + 1
.if VIEW_LINES <= 127 || VIEW_LINES > 254
.error "video.s: the gradient's table needs 128 to 254 lines of view"
.endif

.segment "RODATA"
; HDMA table for channel 6, written to BGMODE ($2105): entries of a line
; count and the value, 0 ends it
mode_hdma:
.if VIEW_PAD_Y
    ; (the smaller view: the message line at lines 8-15 and the status bar
    ; at 168-207, inside the part of the screen every TV shows; ui.c's
    ; MSG_ROW and HUD_ROW0)
    .byte VIEW_TOP, $01         ; lines 0 .. VIEW_TOP - 1: Mode 1 (the message line at 8-15)
    .byte 127, $07              ; the view's lines: Mode 7 (127 + the rest)
    .byte VIEW_LINES - 127, $07
    .byte 1, $01                ; from the view's last line + 1: Mode 1 (the status bar; held to the end)
.else
    .byte 8, $01                ; lines 0-7: Mode 1 (the message line)
    .byte 127, $07              ; lines 8-183: Mode 7 (the view; 127 + 49 lines)
    .byte 49, $07
    .byte 1, $01                ; from line 184: Mode 1 (the status bar; held to the end)
.endif
    .byte 0
zero2: .word 0                  ; video_init's DMA source: a zero word
.if VIEW_PAD_X
; HDMA table for channel 4, written to CGWSEL ($2130): $40 clips the main
; screen to black outside the colour window (window 1: the view's
; columns) on the view's lines; 0 elsewhere (the message line, the top
; and bottom borders, the status bar: nothing clipped)
win_hdma:
    .byte VIEW_TOP, $00         ; lines 0 .. VIEW_TOP - 1
    .byte 127, $40              ; the view's lines (127 + the rest)
    .byte VIEW_LINES - 127, $40
    .byte 1, $00                ; from the view's last line + 1: held to the end
    .byte 0
.endif

; the tiled picture's place in the Mode 7 map and its centre (M7X, M7Y).
; With the transposed matrix (B = C = 0.5) and the -8 vertical scroll the
; hardware's sums are texel x = (line + 1 - 8 - M7_Y0) / 2 + M7_X0 (Mode
; 7 counts lines from 1: the screen's line L is its L + 1) and texel y =
; (sx - M7_X0) / 2 + M7_Y0. The picture's tile rows are map columns from
; M7_TX / 8 and its strips map rows from M7_TY / 8 (texel (M7_TX, M7_TY)
; is its corner); that texel must cover lines VIEW_TOP, VIEW_TOP + 1 and
; pixels VIEW_PAD_X, VIEW_PAD_X + 1, so 3 M7_X0 = 4 M7_TX + 2 M7_TY - 2
; VIEW_PAD_Y - VIEW_PAD_X - 2 and M7_Y0 = M7_TY - (VIEW_PAD_X - M7_X0) /
; 2, whole numbers with even halves (at 104 x 72, with the picture from
; map row 1: M7_X0 = -14, M7_Y0 = -11). Map row 0 and the map beyond the
; picture are blank, and texels left of or above the map transparent.
.if FB_TILED
M7_TX = 0                       ; the picture's first map column x 8
M7_TY = 8                       ; its first map row x 8
M7_X0 = (4 * M7_TX + 2 * M7_TY - 2 * VIEW_PAD_Y - VIEW_PAD_X - 2) / 3
M7_Y0 = M7_TY - (VIEW_PAD_X - M7_X0) / 2
.if 3 * M7_X0 <> 4 * M7_TX + 2 * M7_TY - 2 * VIEW_PAD_Y - VIEW_PAD_X - 2 || (VIEW_PAD_X - M7_X0) .mod 2 <> 0 || (VIEW_PAD_Y + 1 - M7_Y0) .mod 2 <> 0
.error "video.s: no whole Mode 7 centre puts the picture at its place for this view size (try another M7_TX, M7_TY)"
.endif
.endif

.segment "CODE"

; void video_init(void): forced blank; VRAM cleared, the Mode 7 map, the
; registers. (the display stays off: screen_on)
; Also: Mode 7's matrix (half size: each 8x8 tile shown as 16x16), BG1's
; map and the sprites' sizes and tiles, the main screen's layers, colour
; math off, the flash off. HDMA is stopped. Clobbers A, X, Y; DMA
; channel 0. Called by ui.c and the tests, before video_game or
; video_full.
.a8
.i8
_video_init:
    php
    sep #$20
    rep #$10
    .a8
    .i16
.ifdef SUPERFX
    lda #0                      ; (a picture still going up: never mind)
    sta f:_fbu_left
    sta f:_fbu_left + 1
.endif
    lda #$80
    sta f:$002100               ; forced blank
    lda #0
    sta f:$00420C               ; HDMAEN: no HDMA
    ; all of VRAM to 0
    lda #$80
    sta f:$002115               ; VMAIN: step after the high byte
    rep #$20
    .a16
    lda #0
    sta f:$002116               ; VMADD: word 0
    lda #$1809                  ; fixed source, two registers ($2118/9)
    sta f:$004300               ; (DMAP0 = $09, BBAD0 = $18)
    lda #.loword(zero2)
    sta f:$004302
    lda #0                      ; 65536 bytes
    sta f:$004305
    sep #$20
    .a8
    lda #$7E                    ; (RODATA runs from work RAM)
    sta f:$004304
    lda #1
    sta f:$00420B               ; run channel 0: 65536 zero bytes
    ; the Mode 7 map: low bytes, increment after $2118
    lda #$00
    sta f:$002115
    rep #$20
    .a16
    lda #0
    sta f:$002116
    sep #$20
    .a8
.if FB_TILED
    ; (the tiled picture, view.inc's FB_TILED: shown transposed) 128 x 128
    ; entries, a row at a time: map row M7_TY / 8 + r holds the picture's
    ; strip r (its 8 columns), its TILE_ROWS tiles in map columns M7_TX /
    ; 8 on (tile r * TILE_ROWS + its row: numbered in this order); tile
    ; 255 elsewhere
    stz mval                    ; the next picture tile
    ldy #0                      ; the map's row
@mrow:
    ldx #0                      ; the map's column
@mcol:
    lda #255                    ; outside the picture: tile 255 (stays blank: the picture is tiles 0 .. FB_STRIPS TILE_ROWS - 1)
    cpy #M7_TY / 8
    bcc @put
    cpy #M7_TY / 8 + FB_STRIPS
    bcs @put
    cpx #M7_TX / 8
    bcc @put
    cpx #M7_TX / 8 + TILE_ROWS
    bcs @put
    lda mval
    inc mval
@put:
    sta f:$002118               ; VMDATAL: the map entry
    inx
    cpx #128
    bne @mcol
    iny
    cpy #128
    bne @mrow
    ; Mode 7: half size and transposed (the matrix: texel x = screen y / 2,
    ; texel y = screen x / 2), nothing outside the map
    ; (the matrix registers are written low byte then high byte; 8.8 fixed point)
    lda #$80
    sta f:$00211A               ; M7SEL: outside the map transparent
    lda #0
    sta f:$00211B               ; M7A = 0
    sta f:$00211B
    lda #$80
    sta f:$00211C               ; M7B = 0.5: half a texel across per line down
    lda #$00
    sta f:$00211C
    lda #$80
    sta f:$00211D               ; M7C = 0.5: half a texel down per pixel across
    lda #$00
    sta f:$00211D
    lda #0
    sta f:$00211E               ; M7D = 0
    sta f:$00211E
    ; the centre (M7_X0, M7_Y0: 13 bits, low byte then high) puts texel
    ; (M7_TX, M7_TY), the picture's corner, at screen x VIEW_PAD_X, line
    ; VIEW_TOP
    lda #<M7_X0
    sta f:$00211F
    lda #>M7_X0 & $1F
    sta f:$00211F
    lda #<M7_Y0
    sta f:$002120
    lda #>M7_Y0 & $1F
    sta f:$002120
.else
    ; 128 x 128 entries, a row at a time; the picture's 16 x 12 tiles in
    ; the top-left corner (numbered down each column), tile 255 elsewhere
    ldy #0                      ; ty
@mrow:
    tya
    sta mval                    ; tx * 12 + ty, from tx = 0
    ldx #0                      ; tx
@mcol:
    lda #255                    ; outside the picture: tile 255 (stays blank: the picture is tiles 0-191)
    cpy #12
    bcs @put
    cpx #16
    bcs @put
    lda mval
@put:
    sta f:$002118               ; VMDATAL: the map entry
    lda mval
    clc
    adc #12                     ; the next column's tile in this row
    sta mval
    inx
    cpx #128
    bne @mcol
    iny
    cpy #128
    bne @mrow
    ; Mode 7: half size (the matrix 0.5), nothing outside the map
    ; (the matrix registers are written low byte then high byte; 8.8 fixed point)
    lda #$80
    sta f:$00211A               ; M7SEL: outside the map transparent
    lda #$80
    sta f:$00211B               ; M7A = $0080 = 0.5: half a texel per pixel across
    lda #$00
    sta f:$00211B
    lda #0
    sta f:$00211C               ; M7B = 0
    sta f:$00211C
    sta f:$00211D               ; M7C = 0
    sta f:$00211D
    lda #$80
    sta f:$00211E               ; M7D = 0.5: half a texel per line down
    lda #$00
    sta f:$00211E
    lda #0
    sta f:$00211F               ; M7X, M7Y = 0: the centre at the map's corner
    sta f:$00211F
    sta f:$002120
    sta f:$002120
.endif
    ; BG1 (Mode 1): map at $5C00 (32x32); sprites: 16x16 / 32x32, tiles at $6000
    lda #$5C
    sta f:$002107               ; BG1SC: map at $5C00 words, 32x32
    lda #$63
    sta f:$002101               ; OBSEL: sizes 16x16 / 32x32, tiles at $6000 words
    lda #$11                    ; BG1 + sprites
    sta f:$00212C               ; TM: the main screen's layers
    lda #0
    sta f:$002130               ; colour math: always (for the flash)
    sta f:$002131               ; CGADSUB: on no layer yet
    stz flash_n
    stz flash_on
    plp
    rts

; void video_game(void): (forced blank) the game screen's modes
; Mode 7 to start with; BG1's tiles and scroll; HDMA channel 6 switches
; BGMODE per band of lines (mode_hdma), channel 5 sets colour 0 per line
; (_grad_hdma, filled by view_gradient); with a border round the view,
; channel 4 switches CGWSEL (win_hdma) and window 1 is set to the view's
; columns. Clobbers A. Called by ui.c when the game screen comes up, and
; by the tests.
.a8
.i8
_video_game:
    php
    sep #$20
    .a8
    lda #$07
    sta f:$002105               ; BGMODE: Mode 7 (HDMA changes it per line)
    lda #$04                    ; BG1 (Mode 1) tiles at $4000
    sta f:$00210B               ; BG12NBA
    lda #0                      ; scroll: x 0, y -8
    sta f:$00210D               ; BG1HOFS (also Mode 7's): written twice, low then high
    sta f:$00210D
    lda #$F8
    sta f:$00210E               ; BG1VOFS = $1FF8: -8 in 13 bits
    lda #$1F
    sta f:$00210E
    ; HDMA: channel 6 BGMODE, channel 5 the backdrop's colour per line
    lda #$00
    sta f:$004360               ; DMAP6: mode 0, one byte to one register
    lda #$05
    sta f:$004361               ; BBAD6: BGMODE ($2105)
    rep #$20
    .a16
    lda #.loword(mode_hdma)
    sta f:$004362
    sep #$20
    .a8
    lda #$7E
    sta f:$004364               ; A1B6: the table's bank
    lda #$03                    ; two registers, each twice: $2121 $2121 $2122 $2122
    sta f:$004350
    lda #$21
    sta f:$004351               ; BBAD5: CGADD ($2121), then CGDATA
    rep #$20
    .a16
    lda #.loword(_grad_hdma)
    sta f:$004352
    sep #$20
    .a8
    lda #$7E
    sta f:$004354
.if VIEW_PAD_X
    ; the side borders: window 1 = the view's columns, the colour window
    ; is window 1 (CGWSEL clips outside it: channel 4)
    lda #VIEW_PAD_X
    sta f:$002126               ; WH0: window 1's left edge
    lda #255 - VIEW_PAD_X
    sta f:$002127               ; WH1: its right edge (inclusive)
    lda #$20
    sta f:$002125               ; WOBJSEL: the colour window = window 1
    lda #$00
    sta f:$004340               ; DMAP4: mode 0, one byte to one register
    lda #$30
    sta f:$004341               ; BBAD4: CGWSEL ($2130)
    rep #$20
    .a16
    lda #.loword(win_hdma)
    sta f:$004342
    sep #$20
    .a8
    lda #$7E
    sta f:$004344               ; A1B4: the table's bank
    lda #$70
    sta f:$00420C               ; HDMAEN: channels 4, 5 and 6 (from the next frame)
.else
    lda #$60
    sta f:$00420C               ; HDMAEN: channels 5 and 6 (from the next frame)
.endif
    plp
    rts

; void video_full(u8 bg1_tiles): (forced blank) Mode 1 everywhere, BG1's
; tiles at bg1_tiles * $1000 words
; In: A = bg1_tiles. Out: HDMA off, Mode 1, no scroll, colour 0 black.
; Clobbers A. Called by ui.c for the title and full-screen pictures.
_video_full:
    php
    sep #$20
    .a8
.ifdef SUPERFX
    pha
    lda #0
    sta f:_fbu_left
    sta f:_fbu_left + 1
    pla
.endif
    sta f:$00210B               ; BG12NBA: BG1's tiles
    lda #0
    sta f:$00420C               ; HDMAEN: off (no mode switching)
.if VIEW_PAD_X
    sta f:$002130               ; CGWSEL: no clipping (HDMA may have left it on)
.endif
    lda #$01
    sta f:$002105               ; BGMODE: Mode 1
    lda #0
    sta f:$00210D               ; the scrolls to 0
    sta f:$00210D
    sta f:$00210E
    sta f:$00210E
    sta f:$002121               ; the backdrop black
    sta f:$002122
    sta f:$002122
    plp
    rts

; void screen_on(void) / screen_off(void): the display (and the NMI, the pad)
; screen_on: INIDISP $0F (display on, full brightness) and NMITIMEN $81
; (the vblank NMI and the auto-joypad read on). screen_off: forced blank
; (the NMI stays on). Clobber A.
_screen_on:
    lda #$0F
    sta f:$002100
    lda #$81
    sta f:$004200
    rts

_screen_off:
    lda #$80
    sta f:$002100
    rts

; void flash(u8 frames): the damage flash (red added to everything)
; In: A = frames. nmi_video does the rest. Called by ui.c.
_flash:
    sta flash_n
    rts

; void view_upload(void): the picture goes up over the next vblanks
; (SuperFX: from a copy in work RAM, while the GSU draws the next: fx_out)
; Plain build: queues the frame buffer ($7F:0000, FB_BYTES bytes) for VRAM's
; high bytes from word 0 (gfx.s's queue; 4 KB parts). Clobbers A, X, Y.
; Called after each picture (ui.c, test_view.c).
.a8
.i8
_view_upload:
.ifdef SUPERFX
    jmp _fx_out
.else
    php
    rep #$30
    .a16
    .i16
    lda #FB_ADDR
    sta _vq_src
    lda #0
    sta _vq_dst
    lda #FB_BANK
    sta _vq_src + 2
    lda #FB_BYTES
    sta _vq_len
.if FB_TILED
    lda #$8001                  ; VRAM high bytes, VMAIN $80 (as it is: the tiles' own order)
.else
    lda #$8401                  ; VRAM high bytes, VMAIN $84 (8-bit rotation)
.endif
    sta _vq_mode
    sep #$30
    .a8
    .i8
    jsr _vq_add                 ; queue it (gfx.s)
    plp
    rts
.endif

; u8 view_busy(void): 1 while the picture (or anything) is still going up
; Out: A = 1 or 0, X = 0. The view must not draw into the frame buffer
; while this is 1 (the plain build draws into the buffer being sent).
_view_busy:
.ifdef SUPERFX
    lda f:_fbu_left
    ora f:_fbu_left + 1
    beq :+
    lda #1
    ldx #0
    rts
:
.endif
    jmp _vq_busy                ; anything in the upload queue (its rts returns to our caller)

; void cgram_load(void): the view's colours (slots 32..)
; Queues view_n_colours colours from view_cgram (view/view.c, BGR555) to
; CGRAM from colour 32 (at least one colour, 2 bytes, if the count is 0).
; Clobbers A, X, Y. Called when a map's colours change (ui.c, the tests).
_cgram_load:
    php
    rep #$30
    .a16
    .i16
    lda #.loword(_view_cgram)
    sta _vq_src
    lda #$007E
    sta _vq_src + 2
    lda #32
    sta _vq_dst
    lda _view_n_colours
    and #$00FF
    asl                         ; 2 bytes a colour
    bne :+
    lda #2
:   sta _vq_len
    lda #2
    sta _vq_mode                ; CGRAM
    sep #$30
    .a8
    .i8
    jsr _vq_add                 ; queue it (gfx.s)
    plp
    rts

; in the NMI: a16 i16, data bank $80
; Called by hw.s's nmi_body. The SuperFX build first sends the next part
; of the GSU's picture (WFBU, gsu_cpu.s); then gfx.s's nmi_gfx sends the
; sprite table and the queued copies; then the damage flash is switched
; on or off. Clobbers A, X, Y.
.a16
.i16
nmi_video:
.ifdef SUPERFX
    jsl f:WFBU                  ; (bank 0 work RAM)
.endif
    jsr nmi_gfx                 ; OAM and the upload queue (gfx.s)
    sep #$20
    .a8
    lda flash_n
    beq @off                    ; no flash (or it just ended)
    dec flash_n
    lda flash_on
    bne @done                   ; already on
    inc flash_on
    lda #$33                    ; add to BG1, BG2, sprites, the backdrop
    sta $2131                   ; CGADSUB
    lda #$20 | 14               ; red
    sta $2132                   ; COLDATA: the fixed colour's red = 14 (bit 5 selects red)
    lda #$40
    sta $2132                   ; green = 0 (bit 6)
    lda #$80
    sta $2132                   ; blue = 0 (bit 7)
    bra @done
@off:
    lda flash_on
    beq @done
    stz flash_on
    stz $2131                   ; CGADSUB: colour math on no layer
    lda #$E0
    sta $2132                   ; the fixed colour back to black
@done:
    rep #$20
    .a16
    rts
