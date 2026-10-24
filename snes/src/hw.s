; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

; Doom RPG for the SNES: the hardware side the C code calls (vblank, the
; pad, DMA) and the NMI's body. ca65.
; C calling convention (cc65 fastcall): the last argument in A (low) / X
; (high), the others on the C stack; 8-bit A/X/Y on entry and exit.
; Licence: GPL-2.0-or-later.
;
; The variables are in LOWRAM ($0200-$0FFF): that RAM is the same at
; $00:xxxx, $7E:xxxx and $80:xxxx, so both C (data bank $7E) and the NMI
; (data bank $80) reach it with plain absolute addresses.
; The pad is read by the hardware's auto-joypad read (enabled in $4200 by
; the video code); wait_vblank copies the result once per frame.

.p816
.smart -
.import nmi_video, nmi_sound
.export nmi_body, _wait_vblank, _frame_count, _pad, _pad_edge, _set_backdrop

.segment "LOWRAM"
_frame_count: .res 2             ; NMIs (frames, 60 a second) since start; wraps at 65536
_pad:         .res 2             ; buttons held (SNES order: B Y Sel Start U D L R | A X L R)
                                 ; (bit 15 B down to bit 4 R: $4218-$4219 as a word)
_pad_edge:    .res 2             ; newly pressed since the last read by wait_vblank
pad_prev:     .res 2             ; _pad at the previous wait_vblank
backdrop:     .res 2             ; colour 0 (BGR555) set_backdrop asked for
nmi_ready:    .res 1             ; nonzero once set_backdrop was called: the NMI then writes colour 0 every frame

.segment "CODE"

; NMI body: a16 i16, data bank $80, direct page 0
; Called (jsl) by crt0.s's NMI handler, which saved every register.
; Does the vertical blank's work: the video uploads (video.s), the sound
; driver's commands (sound.s), the backdrop colour, then counts the frame.
; Out: _frame_count + 1 (what wait_vblank waits for). Clobbers A, X, Y.
.a16
.i16
nmi_body:
    jsr nmi_video               ; DMA the waiting picture, colours, tiles to the PPU (video.s)
    jsr nmi_sound               ; talk to the SPC700: music and queued effects (sound.s)
    sep #$20
    .a8
    lda nmi_ready
    beq @skip                   ; set_backdrop never called: leave colour 0 alone
    stz $2121                   ; CGADD: colour 0 (the backdrop)
    lda backdrop
    sta $2122                   ; CGDATA: low byte, then high byte
    lda backdrop + 1
    sta $2122
@skip:
    rep #$20
    .a16
    inc _frame_count
    rtl

; void wait_vblank(void): the next frame; the pad read then
; Waits until the NMI has counted a frame (so it returns just after an
; NMI's work, early in the vertical blank), then waits for the auto-joypad
; read to finish and reads pad 1.
; Out: _pad, _pad_edge (buttons that went down since the previous call),
; pad_prev. Clobbers A. Needs NMIs enabled, or it waits for ever.
; Called by C everywhere it waits for a frame (ui.c, the tests).
.a8
.i8
_wait_vblank:
    php
    rep #$20
    .a16
    lda f:$7E0000 + _frame_count   ; (LOWRAM is bank-0 RAM, the same as $7E:0000..)
@w: cmp f:$7E0000 + _frame_count
    beq @w                      ; until the NMI changes it
    ; the pad: auto-read finishes a few lines after vblank starts
@j: sep #$20
    .a8
    lda f:$004212               ; HVBJOY: bit 0 is set while the auto-joypad read runs
    and #$01
    bne @j
    rep #$20
    .a16
    lda f:$004218               ; JOY1L/JOY1H: pad 1's buttons (1 = pressed)
    sta _pad
    eor pad_prev                ; the bits that changed ...
    and _pad                    ; ... and are now down
    sta _pad_edge
    lda _pad
    sta pad_prev
    plp
    rts

; void set_backdrop(u16 bgr555)
; In: A/X = the colour (A low byte, X high byte; BGR555: bits 0-4 red,
; 5-9 green, 10-14 blue). Out: backdrop; nmi_ready = 1, so from the next
; NMI on colour 0 is written every frame. Clobbers A.
; Used by the test programs (test_main.c, test_rules.c).
.a8
.i8
_set_backdrop:
    sta backdrop
    stx backdrop + 1
    lda #1
    sta nmi_ready
    rts


; u16 ticks(void): scanlines since start (frame_count * 262 + the V counter), for profiling
; 262 lines a frame (NTSC). The result wraps at 65536 lines (about 4
; seconds). Out: A = low byte, X = high byte. Clobbers tick_v, tick_t.
; Retries if an NMI counted a frame while the counter was being read.
.export _ticks
.a8
.i8
_ticks:
    php
    rep #$30
    .a16
    .i16
@again:
    lda f:$7E0000 + _frame_count
    pha
    sep #$20
    .a8
    lda f:$002137               ; latch the counters (SLHV: reading latches H and V)
    lda f:$00213F               ; (resets the low/high toggle) (STAT78)
    lda f:$00213D               ; OPVCT: the V counter's low 8 bits ...
    xba
    lda f:$00213D               ; ... then its 9th bit (bit 0 of the second read)
    and #1
    xba                         ; A (16-bit) = the line, 0-261
    rep #$20
    .a16
    ; lines since the NMI (it counts the frame at line 225)
    sec
    sbc #225
    bcs :+
    adc #262                    ; before line 225: the last NMI was in the previous frame
:   sta f:$7E0000 + tick_v
    pla
    cmp f:$7E0000 + _frame_count
    bne @again                  ; an NMI came in between: read again
    ; frame * 262 = frame * 256 + frame * 6
    pha
    xba
    and #$FF00
    sta f:$7E0000 + tick_t
    pla
    asl                         ; frame * 2
    pha
    asl                         ; frame * 4
    clc
    adc 1,s                     ; + frame * 2 (on the stack) = frame * 6
    plx                         ; (drop it)
    clc
    adc f:$7E0000 + tick_t      ; + frame * 256
    adc f:$7E0000 + tick_v      ; + the line (no clc: a carry from the add above adds 1)
    plp
    sep #$30
    .a8
    .i8
    xba
    tax                         ; X = the high byte
    xba                         ; A = the low byte
    rts

.segment "LOWRAM"
tick_v: .res 2                  ; ticks: lines since the last NMI
tick_t: .res 2                  ; ticks: frame * 256 (16 bits)
