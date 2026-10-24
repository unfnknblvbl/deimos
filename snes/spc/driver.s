; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

; Doom RPG for the SNES: the sound driver, on the SPC700 (wla-spc700).
;
; Plays the Mega Drive game's music and sound effects streams (tools/build_music.py's
; format) with the instruments tools/snes_sound.py makes: two players, the
; music (voices 0-5, and 6 for the noise drums) and the effects (voices 4,
; 5 and 6: while an effect plays the music goes on without them, as on the
; Mega Drive). Time runs in 1/60 s ticks (timer 0: 133 x 125 us).
;
; The main CPU (snes/src/sound.s) talks through the ports: it writes a
; command to port 1, its argument to port 2, then a new count to port 0;
; the driver answers with the count on port 0. Port 1 reads 1 while an
; effect plays.  Commands: 1 play song (arg), 2 stop the music, 3 play
; effect (arg), 4 the music's volume (arg: 0..127).
;
; Memory: the driver at $0200, the tables from $0E00 (snes_sound.py),
; the streams from $2000.
;
; Where it fits: tools/snes_sound.py assembles this file and builds the
; sound engine image around it (the tables below); the SNES's CPU
; (snes/src/sound.s) uploads it to the SPC700's RAM through the IPL ROM's
; loader and jumps to start. The 32X, 3DS and GBA ports run a C copy of
; this driver (32x/sh2/sound32x.c, 3ds/src/sound3ds.c, gba/src/sound.c),
; so their behaviour follows this file.
;
; ARAM layout:
;   $0000-$00DF  the variables below (direct page 0)
;   $00F0-$00FF  the SPC700's I/O registers (ports, DSP address/data,
;                timers)
;   $0100-$01EF  the stack (from $01EF down)
;   $0200        this driver
;   $0E00 PITCH  384 words: pitch of the top octave (octave 8), per
;                semitone (12) in 32 steps of 1/32 semitone
;   $1100 VOLTAB 128 bytes: DSP volume for an attenuation of 0.75 dB a
;                step
;   $1180 INST   8 bytes per instrument: sample number, ADSR1, ADSR2,
;                base attenuation (the instrument's TL), release GAIN, 0, 0, 0
;   $11D0 NOISE  4 bytes per noise drum type: noise clock, attenuation
;                (in the PSG's 2 dB steps), ADSR1, ADSR2
;   $1200        the sample directory (DIR = $12), $1300 the samples
;   $2000 STREAMS  u16 x 3 the songs' addresses, u16 the number of
;                effects, u16 per effect its address, then the streams
;
; A stream: a "loops" byte (non-zero: start again at the end), then
; commands. $00-$7F: wait that many ticks. $FF: the end. Otherwise the
; top 5 bits are the command and the low 3 bits the voice v (0..6):
;   $80+v note on: note, attenuation (v = 6: a noise drum: type, extra
;         attenuation)
;   $88+v note off      $90+v instrument (one byte)
;   $98+v pitch bend (1/32 semitones, signed)
;   $A0+v attenuation   $A8+v pan (bit 7 left on, bit 6 right on)
;   $B0+v the note changes (only while one sounds)
; Several commands run in one tick until a wait.
;
; Jargon: "player" 0 runs the music, 1 the effects; "pv" = player * 7 +
; voice indexes the per-voice arrays; a voice is "free" for a player if
; it may write the DSP for it (see free).
; Licence: GPL-2.0-or-later.

.MEMORYMAP
    SLOTSIZE $0C00
    DEFAULTSLOT 0
    SLOT 0 START $0200
.ENDME
.ROMBANKSIZE $0C00
.ROMBANKS 1

; the tables' addresses (tools/snes_sound.py puts them there)
.DEFINE PITCH   $0E00
.DEFINE VOLTAB  $1100
.DEFINE INST    $1180
.DEFINE NOISE   $11D0
.DEFINE DIRPAGE $12             ; the sample directory's page ($1200)
.DEFINE STREAMS $2000
.DEFINE SILENT  10              ; the silent sample (the noise voice's)

; DSP registers
; (DSPA: the DSP register number to access; DSPD: its data. PORT0-2: the
; ports shared with the CPU, $2140-$2142 on its side: a read gets what
; the CPU wrote, a write is what the CPU reads)
.DEFINE DSPA $F2
.DEFINE DSPD $F3
.DEFINE PORT0 $F4
.DEFINE PORT1 $F5
.DEFINE PORT2 $F6

.ENUM $00
    ptrl    dsb 2               ; per player (0 music, 1 effects): the stream
    ptrh    dsb 2
    startl  dsb 2               ; its start (looping)
    starth  dsb 2
    wait    dsb 2               ; ticks left to wait (0: run now)
    loops   dsb 2               ; non-zero: the stream loops
    active  dsb 2               ; 1 while the player plays
    note    dsb 14              ; per player and voice (p * 7 + v)
                                ; (the note sounding, in semitones; $FF none)
    bend    dsb 14              ; pitch bend, 1/32 semitones (signed)
    att     dsb 14              ; attenuation (0.75 dB steps)
    pan     dsb 14              ; bit 7 left on, bit 6 right on
    patch   dsb 14              ; instrument + 1 (0: none)
    pl      db                  ; the player being run
    pv      db                  ; p * 7 + v
    vv      db                  ; the voice
    tmp     dsb 4               ; scratch
    sp_     dsw 1               ; the stream pointer while running
    lastcnt db                  ; the last command count seen on port 0
    mvol    db                  ; the music's volume (attenuation added)
    nonbits db                  ; (not used)
.ENDE

.BANK 0 SLOT 0
.ORG 0

; start: the entry point (the CPU's upload jumps here). Sets the stack,
; clears the variables, sets the DSP up (all voices off, no echo, the
; main volume, the sample directory), starts timer 0 at 60 Hz, then
; falls into the main loop. Never returns.
start:
    mov x, #$EF
    mov sp, x                   ; the stack at $01EF down
    mov a, #0
    mov x, #0
-   mov (x)+, a                 ; clear $00-$DF (the variables)
    cmp x, #$E0
    bne -
    ; the DSP: everything off, no echo
    mov DSPA, #$5C              ; KOFF
    mov DSPD, #$FF
    mov DSPA, #$6C              ; FLG: echo writes off, noise clock 0
    mov DSPD, #$20
    mov DSPA, #$0C              ; main volume (as loud as the Mega Drive's)
    mov DSPD, #$26
    mov DSPA, #$1C
    mov DSPD, #$26
    mov DSPA, #$2C              ; echo volume 0
    mov DSPD, #0
    mov DSPA, #$3C
    mov DSPD, #0
    mov DSPA, #$4D              ; EON
    mov DSPD, #0
    mov DSPA, #$3D              ; NON
    mov DSPD, #0
    mov DSPA, #$2D              ; PMON
    mov DSPD, #0
    mov DSPA, #$5D              ; DIR
    mov DSPD, #DIRPAGE
    mov DSPA, #$5C              ; KOFF: release the key-offs
    mov DSPD, #0
    ; the timer: 60 Hz
    mov $FA, #133               ; T0TARGET: 133 steps of 125 us (16.6 ms)
    mov $F1, #$01               ; CONTROL: timer 0 on
    ; (only timer 0 runs; bits 4-5 clear, so the ports are not reset.
    ; Bit 7 is clear too, which unmaps the IPL ROM at $FFC0 (RAM shows
    ; there instead). Nothing here uses that area either way)
    mov a, PORT0
    mov lastcnt, a              ; (the count the CPU left: no command yet)

; main: the loop. A new count on port 0 means a command (port 1) with its
; argument (port 2, in y): run it, then echo the count to say it is done.
; Then, if timer 0 has ticked, one tick of each player, and port 1 tells
; the CPU whether an effect is still playing. (Several ticks since the
; last look count as one.)
main:
    ; a command?
    mov a, PORT0
    cmp a, lastcnt
    beq @tick
    mov lastcnt, a
    mov a, PORT1                ; the command
    mov y, PORT2                ; its argument
    cmp a, #1
    bne +
    call !play_song             ; 1: song y
    bra @ack
+   cmp a, #2
    bne +
    call !stop_music            ; 2: silence the music
    bra @ack
+   cmp a, #3
    bne +
    call !play_sfx              ; 3: effect y
    bra @ack
+   cmp a, #4
    bne @ack
    mov mvol, y                 ; 4: the music's volume
@ack:
    mov a, lastcnt
    mov PORT0, a                ; done: the count back to the CPU
@tick:
    mov a, $FD                  ; timer 0 ticks since the last read
    beq main
    mov x, #0
    call !run_player            ; the music's tick
    mov x, #1
    call !run_player            ; the effects' tick
    mov a, active+1
    mov PORT1, a                ; 1 while an effect plays
    bra main

; ------------------------------------------------------------------ players
; y = song: the music player from it
; (the song's address from the word table at STREAMS; the music stopped
; first). Clobbers a, x, y, tmp and the voice variables.
play_song:
    mov a, y
    asl a
    mov x, a                    ; x = 2 y: its word in the table
    mov a, !STREAMS+x
    mov tmp, a
    mov a, !STREAMS+1+x
    mov tmp+1, a
    call !stop_music            ; release the old song's notes
    mov x, #0
    jmp !start_player           ; (it returns for us)

; stop_music: the music player stops; its voices 0-5 are released
; (keyoff, unless an effect owns them) and marked silent. Voice 6 (the
; noise drums) is left alone. Clobbers a, x, y, pl, pv, vv.
stop_music:
    mov active, #0
    mov x, #0
-   mov pv, x                   ; player 0: pv = voice
    mov vv, x
    mov pl, #0
    call !keyoff                ; let the note fade
    mov x, vv
    mov a, #$FF
    mov note+x, a               ; no note
    inc x
    cmp x, #6
    bne -
    ret

; y = effect: the effects player from it
; (the effects' word table starts at STREAMS + 8, after the 3 songs and
; the count; a new effect cuts the one playing). Clobbers a, x, y, tmp.
play_sfx:
    mov a, y
    asl a
    mov x, a
    mov a, !STREAMS+8+x
    mov tmp, a
    mov a, !STREAMS+9+x
    mov tmp+1, a
    mov x, #1
    ; (its voices off first)
    push x
    call !sfx_off               ; key off voices 4-6
    pop x
    jmp !start_player

; x = player, tmp = the stream: loops flag, then the commands
; Sets the player's pointers (ptr, start: just past the loops byte), no
; wait, active; and its 7 voices: no note, no bend, no attenuation, no
; instrument, panned both sides ($C0). Clobbers a, x, y, tmp.
start_player:
    mov y, #0
    mov a, [tmp]+y
    mov loops+x, a
    incw tmp                    ; past the loops byte
    mov a, tmp
    mov ptrl+x, a
    mov startl+x, a
    mov a, tmp+1
    mov ptrh+x, a
    mov starth+x, a
    mov a, #0
    mov wait+x, a
    mov a, #1
    mov active+x, a
    ; the voices' state
    mov a, x
    mov y, #7
    mul ya
    mov x, a                    ; x = player * 7: its first voice
    mov y, #7                   ; 7 voices
-   mov a, #$FF
    mov note+x, a
    mov a, #0
    mov bend+x, a
    mov att+x, a
    mov patch+x, a
    mov a, #$C0
    mov pan+x, a
    inc x
    dbnz y, -
    ret

; one tick of player x
; Nothing if it is not playing; counts its wait down; when the wait is
; over, runs its stream's commands up to the next wait (or the end) and
; saves the stream pointer. In: x. Sets pl, pv, vv; clobbers a, x, y,
; tmp, sp_.
run_player:
    mov pl, x
    mov a, active+x
    beq rp_done
    mov a, wait+x
    beq rp_run
    dec a
    mov wait+x, a
    bne rp_done                 ; still waiting
rp_run:
    mov a, ptrl+x               ; the stream pointer into sp_
    mov sp_, a
    mov a, ptrh+x
    mov sp_+1, a
rp_cmd:
    call !next_byte
    bmi rp_op                   ; bit 7 set: a command
    mov x, pl                   ; $01-$7F: wait
    mov wait+x, a
    mov a, sp_
    mov ptrl+x, a
    mov a, sp_+1
    mov ptrh+x, a
rp_done:
    ret
rp_op:
    cmp a, #$FF
    bne +
    jmp !song_end               ; $FF: the stream's end
+   push a
    and a, #7
    mov vv, a                   ; the voice: low 3 bits
    mov y, pl                   ; pv = player * 7 + voice
    mov a, y
    mov y, #7
    mul ya
    clrc
    adc a, vv
    mov pv, a
    pop a
    and a, #$F8                 ; the command: top 5 bits
    cmp a, #$80
    bne +
    call !c_on                  ; $80: note on
    bra rp_cmd2
+   cmp a, #$88
    bne +
    call !c_off                 ; $88: note off
    bra rp_cmd2
+   cmp a, #$90
    bne +
    call !next_byte             ; $90: the instrument (stored + 1)
    inc a
    mov x, pv
    mov patch+x, a
    bra rp_cmd2
+   cmp a, #$98
    bne +
    call !next_byte             ; $98: the pitch bend
    mov x, pv
    mov bend+x, a
    mov a, note+x
    cmp a, #$FF
    beq rp_cmd2                 ; (no note: kept for the next one)
    call !set_pitch
    bra rp_cmd2
+   cmp a, #$A0
    bne +
    call !next_byte             ; $A0: the attenuation
    mov x, pv
    mov att+x, a
    call !set_volume
    bra rp_cmd2
+   cmp a, #$A8
    bne +
    call !next_byte             ; $A8: the pan
    mov x, pv
    mov pan+x, a
    call !set_volume
    bra rp_cmd2
+   cmp a, #$B0
    bne rp_cmd2                 ; (an unknown command: ignored, no operand)
    call !next_byte             ; $B0: a new note without a new key-on
    mov x, pv
    mov y, a
    mov a, note+x
    cmp a, #$FF
    beq rp_cmd2                 ; (only while a note sounds)
    mov a, y
    mov note+x, a
    call !set_pitch
rp_cmd2:
    jmp !rp_cmd

; song_end: player pl's stream ended: a looping one starts again from
; its start; otherwise the player stops (the effects player also keys
; its voices off).
song_end:
    mov x, pl
    mov a, loops+x
    beq +
    mov a, startl+x             ; back to the start
    mov sp_, a
    mov a, starth+x
    mov sp_+1, a
    jmp !rp_cmd
+   mov a, #0
    mov active+x, a
    cmp x, #1
    bne +
    call !sfx_off               ; the effect's notes off
+   ret

; the effects' voices off (and the noise quiet)
; DSP KOFF = $70 (voices 4, 5, 6), then KOFF = 0 so later key-ons work.
; Clobbers a.
sfx_off:
    mov a, #$70                 ; KOFF voices 4, 5, 6
    mov DSPA, #$5C
    mov DSPD, a
    mov DSPD, #0                ; (DSPA still $5C: KOFF cleared)
    ret

; next_byte: a = the next byte of the stream at sp_ (sp_ + 1), flags
; N and Z from it. Clobbers y.
next_byte:
    mov y, #0
    mov a, [sp_]+y
    incw sp_
    cmp a, #0                   ; (flags from the byte)
    ret

; ------------------------------------------------------------------ voices
; may player pl write voice vv? Z set: yes. (not the music on 4..6 while an
; effect plays)
free:
    mov a, pl
    bne @yes
    mov a, vv
    cmp a, #4
    bcc @yes
    mov a, active+1
    beq @yes
    mov a, #1                   ; (Z clear)
    ret
@yes:
    mov a, #0
    ret

; a = DSP register of voice vv (r + vv * 16)
; In: a = r (0..15). Clobbers tmp+3.
vreg:
    push a
    mov a, vv
    xcn a                       ; swap nibbles: vv * 16
    mov tmp+3, a
    pop a
    or a, tmp+3
    ret

; c_on: note on for voice vv of player pl (pv): reads the note and its
; attenuation from the stream. Voice 6 is the noise drum (noise_hit).
; The note is remembered even when the voice is not free or has no
; instrument; otherwise the instrument's sample and ADSR, the volume and
; the pitch are set and the voice keyed on. Clobbers a, x, y, tmp.
c_on:
    call !next_byte
    mov tmp+2, a                ; note / noise type
    call !next_byte
    mov x, pv
    mov y, a                    ; attenuation
    mov a, vv
    cmp a, #6
    bne +
    jmp !noise_hit
+   mov a, tmp+2
    mov note+x, a
    mov a, y
    mov att+x, a
    call !free
    bne @ret                    ; an effect has this voice
    mov a, patch+x
    beq @ret                    ; no instrument yet
    dec a
    ; the instrument: sample, ADSR
    asl a
    asl a
    asl a
    mov y, a                    ; y = instrument * 8: its INST entry
    mov a, #$04                 ; SRCN
    call !vreg
    mov DSPA, a
    mov a, !INST+y
    mov DSPD, a
    mov a, #$05                 ; ADSR1
    call !vreg
    mov DSPA, a
    mov a, !INST+1+y
    mov DSPD, a
    mov a, #$06                 ; ADSR2
    call !vreg
    mov DSPA, a
    mov a, !INST+2+y
    mov DSPD, a
    mov x, pv
    call !set_volume            ; its volume
    mov x, pv
    call !set_pitch             ; its pitch
    ; key on (the noise flag off for this voice)
    ; (the NON register is not touched here: only voice 6 ever has noise)
    mov a, vv
    mov x, a
    mov a, !bits+x
    mov DSPA, #$4C              ; KON
    mov DSPD, a
@ret:
    ret

; a voice's bit in the DSP's KON, KOFF, NON registers
bits: .db 1, 2, 4, 8, 16, 32, 64, 128

; c_off: note off for voice vv (not the noise voice: its drums just end):
; no note, and the voice released (keyoff).
c_off:
    mov a, vv
    cmp a, #6
    beq @ret
    mov x, pv
    mov a, #$FF
    mov note+x, a
    call !keyoff
@ret:
    ret

; the note of voice vv fades with its instrument's release (GAIN)
; (switching ADSR1 to GAIN mode makes the DSP follow the GAIN register,
; a falling slope); with no instrument the voice is keyed off at once.
; Nothing if the voice is not free. In: pl, pv, vv. Clobbers a, x, y,
; tmp+3.
keyoff:
    call !free
    bne @ret
    mov x, pv
    mov a, patch+x
    beq @koff
    dec a
    asl a
    asl a
    asl a
    mov y, a                    ; its INST entry
    mov a, #$07                 ; GAIN: the release
    call !vreg
    mov DSPA, a
    mov a, !INST+4+y
    mov DSPD, a
    mov a, #$05                 ; ADSR1: GAIN mode
    call !vreg
    mov DSPA, a
    mov DSPD, #0
    ret
@koff:
    mov a, vv
    mov x, a
    mov a, !bits+x
    mov DSPA, #$5C              ; KOFF this voice
    mov DSPD, a
    mov DSPD, #0                ; then KOFF cleared
@ret:
    ret

; the pitch of voice vv from note+x and bend+x (x = pv)
; s = note + bend / 32 (semitones), split into octave (s / 12) and
; semitone; the table gives the top octave's pitch for (semitone, bend &
; 31), halved once per octave below 8; octaves above 8 get the highest
; pitch, $3FFF. Writes the voice's DSP pitch registers. Nothing if the
; voice is not free. Clobbers a, x, y, tmp.
set_pitch:
    call !free
    bne @ret
    mov x, pv
    ; s = note + (bend >> 5), fine = bend & 31
    mov a, bend+x
    mov tmp+1, a
    and a, #31
    mov tmp+2, a                ; fine
    mov a, tmp+1
    lsr a
    lsr a
    lsr a
    lsr a
    lsr a
    mov y, a
    mov a, tmp+1
    bpl +
    mov a, y
    or a, #$F8                  ; (an arithmetic shift)
    mov y, a
+   mov a, y
    clrc
    adc a, note+x               ; s
    mov y, #0                   ; (ya = s)
    mov x, #12
    div ya, x                   ; a = octave, y = semitone
    mov tmp+3, a
    ; the pitch table's word: (semitone * 32 + fine) * 2, + PITCH ($0E00)
    mov a, y
    mov y, #64
    mul ya
    mov tmp, a
    mov tmp+1, y
    mov a, tmp+2
    asl a
    clrc
    adc a, tmp
    mov tmp, a
    mov a, tmp+1
    adc a, #$0E                 ; (with the carry from the low byte)
    mov tmp+1, a
    mov y, #0
    mov a, [tmp]+y
    mov tmp+2, a
    inc y
    mov a, [tmp]+y
    mov tmp+1, a                ; pitch (tmp+2 low, tmp+1 high) for octave 8
    ; shift right 8 - octave times (octave > 8: the highest)
    mov a, tmp+3
    cmp a, #9
    bcc +
    mov tmp+2, #$FF
    mov tmp+1, #$3F
    bra @put
+   mov a, #8
    setc
    sbc a, tmp+3
    beq @put
    mov y, a
-   lsr tmp+1                   ; halve the 16-bit pitch: one octave down
    ror tmp+2
    dbnz y, -
@put:
    mov a, #$02                 ; pitch low
    call !vreg
    mov DSPA, a
    mov a, tmp+2
    mov DSPD, a
    mov a, #$03                 ; pitch high
    call !vreg
    mov DSPA, a
    mov a, tmp+1
    mov DSPD, a
@ret:
    ret

; the volume of voice vv: VOLTAB[instrument's TL + att (+ the music's)], by pan
; The attenuation sum is held to 127 (silence end of the table). Each
; side gets that volume if its pan bit is set, else 0. Nothing if the
; voice is not free or has no instrument. Clobbers a, x, y, tmp,
; tmp+3.
set_volume:
    call !free
    bne @ret
    mov x, pv
    mov a, patch+x
    beq @ret
    dec a
    asl a
    asl a
    asl a
    mov y, a                    ; its INST entry
    mov a, !INST+3+y            ; the instrument's own attenuation
    clrc
    adc a, att+x
    bcs @max
    mov y, pl
    bne +                       ; (the effects: not the music's volume)
    clrc
    adc a, mvol
    bcs @max
+   cmp a, #128
    bcc +
@max:
    mov a, #127
+   mov y, a
    mov a, !VOLTAB+y
    mov tmp, a
    ; left: pan bit 7, right: bit 6
    mov a, #$00                 ; VOL (L)
    call !vreg
    mov DSPA, a
    mov a, pan+x
    and a, #$80
    beq +
    mov a, tmp
+   mov DSPD, a
    mov a, #$01                 ; VOL (R)
    call !vreg
    mov DSPA, a
    mov a, pan+x
    and a, #$40
    beq +
    mov a, tmp
+   mov DSPD, a
@ret:
    ret

; noise drum tmp+2 (type), y = the stream's extra attenuation (effects)
; Voice 6 plays the DSP's noise: FLG's noise clock from the drum's entry
; (with $20: echo writes kept off), its volume from the drum's
; attenuation (plus y for an effect; held to 15 PSG steps, then turned
; into 0.75 dB steps for VOLTAB), the silent sample (the noise replaces
; it), the drum's ADSR, noise on (NON) and key on. Nothing if the voice
; is not free. Clobbers a, x, y.
noise_hit:
    call !free
    bne @ret
    mov a, tmp+2
    and a, #7
    asl a
    asl a
    mov x, a                    ; x = type * 4: its NOISE entry
    mov a, !NOISE+x             ; the noise clock
    or a, #$20
    mov DSPA, #$6C              ; FLG
    mov DSPD, a
    mov a, pl                   ; effects: + y PSG steps
    beq +
    mov a, y
+   clrc
    adc a, !NOISE+1+x           ; PSG steps (2 dB) -> 0.75 dB steps: x 8 / 3
    cmp a, #15
    bcc +
    mov a, #15
+   mov y, #8
    mul ya
    mov y, #0
    push x
    mov x, #3
    div ya, x
    pop x
    mov y, a
    mov a, !VOLTAB+y
    mov DSPA, #$60              ; voice 6 volume
    mov DSPD, a
    mov DSPA, #$61
    mov DSPD, a
    mov DSPA, #$64              ; SRCN: the silent loop
    mov DSPD, #SILENT
    mov DSPA, #$65              ; ADSR1
    mov a, !NOISE+2+x
    mov DSPD, a
    mov DSPA, #$66              ; ADSR2
    mov a, !NOISE+3+x
    mov DSPD, a
    mov DSPA, #$3D              ; NON: voice 6
    mov DSPD, #$40
    mov DSPA, #$4C              ; KON: voice 6
    mov DSPD, #$40
@ret:
    ret
