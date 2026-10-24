; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

; Doom RPG for the SNES: the sound, the main CPU's side. At start-up the
; SPC700 gets the driver (snes/spc/driver.s, with its tables and samples,
; from the tables' $A000) and the streams (the resources' spc_streams) through its
; boot ROM's loader; then commands go through the ports (driver.s says
; how). The sound effects the rules ask for (glue.c's queue) are sent from
; the NMI, one at a time: the next starts when the last has ended or has
; had a third of a second (as the Mega Drive game's).
; ca65. Licence: GPL-2.0-or-later.
;
; The ports: $2140-$2143 (APU0-APU3) are four bytes shared with the SPC700
; (each side reads what the other last wrote). The boot ROM's loader
; (the "IPL") protocol used by upload: it signals ready with $AA, $BB on
; ports 0 and 1; the CPU sends a destination address on ports 2-3, a
; nonzero byte on port 1 and a "kick" on port 0 ($CC the first time), and
; waits for the kick to come back; then each byte goes on port 1 with its
; index (low 8 bits) on port 0, each waited for. A new kick (the last
; index + 2 or more, nonzero) starts the next block; with port 1 = 0
; instead, the address on ports 2-3 is jumped to.
;
; Callers: ui.c (sound_init at start-up, music_play, music_stop; nothing
; calls music_volume), hw.s's nmi_body (nmi_sound). The rules queue effects
; with glue.c's sfx().

.p816
.include "banks.inc"
.smart -
.export _sound_init, _music_play, _music_stop, _music_volume, nmi_sound
.import _sfx_q, _sfx_head, _sfx_tail

APU0 = $002140                  ; the four ports to the SPC700
APU1 = $002141
APU2 = $002142
APU3 = $002143
ENGINE = TABLES1 + $A000        ; u16 size, then the driver, its tables and samples (tools/snes_tables.py)
SPC_DRIVER = $0200              ; where they go in the SPC's RAM (and the driver's start)
SPC_STREAMS = $2000             ; where the music and effects streams go
MIN_FRAMES = 20                 ; frames (1/3 s) an effect plays before the next may cut in

.segment "LOWRAM"
cnt:      .res 1                ; the last command's count
busy:     .res 1                ; the main code is sending (the NMI waits)
playing:  .res 1                ; frames since the last effect started
ready:    .res 1                ; nonzero once the driver runs (until then nothing is sent)
up_src:   .res 3                ; upload: the source (long)
up_len:   .res 2                ; upload: bytes
up_dst:   .res 2                ; upload: the SPC address
up_k:     .res 1                ; the loader's byte count

.segment "ZEROPAGE"
sp3:      .res 3                ; a long pointer (the resources' directory, upload's source)

.segment "CODE"

; void sound_init(void): the driver and the streams into the SPC, started
; Must run once, after power-on or reset, while the SPC's boot ROM is
; waiting (it waits for $AA $BB). Clobbers A, X, Y, sp3, up_*.
; Out: cnt, busy, playing, ready = 1.
.a8
.i8
_sound_init:
    php
    sep #$20
    rep #$10
    .a8
    .i16
    stz ready
    ; wait for the boot ROM's $AA $BB
@w: lda f:APU0
    cmp #$AA
    bne @w
    lda f:APU1
    cmp #$BB
    bne @w
    ; the engine: ENGINE+2, its size at ENGINE
    lda f:ENGINE
    sta up_len
    lda f:ENGINE + 1
    sta up_len + 1
    lda #<(ENGINE + 2)
    sta up_src
    lda #>(ENGINE + 2)
    sta up_src + 1
    lda #^ENGINE
    sta up_src + 2
    rep #$20
    .a16
    lda #SPC_DRIVER
    sta up_dst
    sep #$20
    .a8
    lda #$CC                    ; the first block's kick
    sta up_k
    jsr upload                  ; the driver block
    ; the streams: the resources' entry 8
    ; (the resources start with a directory: at 12 + 8 i, entry i's u32
    ; offset and u16 size, little-endian; tools/snes_ui_res.py)
    lda f:HEADER_AT + 7         ; the resources' bank
    sta sp3 + 2
    stz sp3
    stz sp3 + 1
    ldy #12 + 8 * 8
    lda [sp3],y                 ; offset (we take 16 bits: the resources are < 64 KB)
    sta up_src
    iny
    lda [sp3],y
    sta up_src + 1
    lda sp3 + 2
    sta up_src + 2
    ldy #12 + 8 * 8 + 4
    lda [sp3],y
    sta up_len
    iny
    lda [sp3],y
    sta up_len + 1
    rep #$20
    .a16
    lda #SPC_STREAMS
    sta up_dst
    sep #$20
    .a8
    jsr upload                  ; the streams block
    ; run the driver
    rep #$20
    .a16
    lda #SPC_DRIVER
    sta f:APU2                  ; ports 2-3: the address to jump to
    sep #$20
    .a8
    lda #0
    sta f:APU1                  ; port 1 = 0: no more blocks, run it
    lda up_k
    sta f:APU0
:   cmp f:APU0
    bne :-                      ; until the loader has seen it
    ; the driver starts with port 0 as it found it: count from there
    lda up_k
    sta cnt
    stz busy
    stz playing
    lda #1
    sta ready
    plp
    rts

; one block to the boot ROM's loader: up_len bytes from up_src (long) to
; up_dst; up_k is the kick for this block, and on return the next one's
; (a8 i16.) Clobbers A, X, Y, sp3. Called by sound_init.
.a8
.i16
upload:
    rep #$20
    .a16
    lda up_dst
    sta f:APU2                  ; ports 2-3: the block's address in the SPC
    sep #$20
    .a8
    lda #1
    sta f:APU1                  ; port 1 nonzero: a block follows
    lda up_k
    sta f:APU0                  ; the kick
:   cmp f:APU0
    bne :-                      ; wait for its echo
    lda up_src
    sta sp3
    lda up_src + 1
    sta sp3 + 1
    lda up_src + 2
    sta sp3 + 2
    ldy #0
    ldx up_len
    beq @end                    ; an empty block
@b: lda [sp3],y                 ; per byte:
    sta f:APU1
    tya                         ; the byte's index (low 8 bits)
    sta f:APU0
:   cmp f:APU0
    bne :-                      ; wait for the loader's echo of the index
    iny
    dex
    bne @b
@end:
    tya                         ; the next kick: the index + 2 (never 0)
    clc
    adc #2
    bne :+
    inc
:   sta up_k
    rts

; send command a (8-bit), argument x (8-bit) and wait for the answer
; (the driver's commands: 1 play song, 2 stop the music, 3 play effect, 4
; the music's volume; snes/spc/driver.s.) A new count on port 0 says a
; command is there; the driver echoes the count when it has taken it.
; Clobbers A; cnt + 1.
.a8
.i8
send:
    sta f:APU1                  ; port 1: the command
    txa
    sta f:APU2                  ; port 2: its argument
    lda cnt
    inc
    sta cnt
    sta f:APU0                  ; port 0: the new count
:   cmp f:APU0
    bne :-                      ; until the driver echoes it
    rts

; void music_play(u8 id)
; In: A = the song. Clobbers A, X.
_music_play:
    tax
    lda #1
    bra main_send

; void music_stop(void)
_music_stop:
    ldx #0
    lda #2
    bra main_send

; void music_volume(u8 att): the music's volume (0 full, 127 off)
_music_volume:
    tax
    lda #4

; send A, X from the main code: nothing before sound_init; busy keeps the
; NMI's effects off the ports meanwhile
main_send:
    php
    sep #$30
    pha
    lda ready
    bne :+
    pla                         ; no driver yet: drop it
    plp
    rts
:   lda #1
    sta busy
    pla
    jsr send
    stz busy
    plp
    rts

; in the NMI (a16 i16, data bank $80): the effects' queue
; Sends at most one effect a frame from glue.c's sfx_q: when the queue is
; not empty and either no effect is sounding (the driver's port 1, bit 0)
; or the last one has had MIN_FRAMES. Not while sound_init has not run or
; the main code is in the middle of a send. Clobbers A, X.
.a16
.i16
nmi_sound:
    sep #$30
    .a8
    .i8
    lda ready
    beq @done
    lda busy
    bne @done
    lda playing
    cmp #255
    beq :+                      ; (stop counting at 255)
    inc playing
:   lda f:$7E0000 + _sfx_head
    cmp f:$7E0000 + _sfx_tail
    beq @done                   ; the queue is empty
    ; an effect still sounding for less than MIN_FRAMES: wait
    lda f:APU1
    and #1
    beq @go
    lda playing
    cmp #MIN_FRAMES
    bcc @done
@go:
    lda f:$7E0000 + _sfx_head
    tax
    lda f:$7E0000 + _sfx_q,x    ; take the oldest
    pha
    txa
    inc
    and #7
    sta f:$7E0000 + _sfx_head
    pla
    beq @done                   ; (SFX_NONE)
    tax
    lda #3                      ; command 3: play effect X
    jsr send
    stz playing
@done:
    rep #$30
    .a16
    .i16
    rts
