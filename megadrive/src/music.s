| Deimos - Doom RPG on retro consoles
| Copyright (C) 2026 unfnknblvbl and contributors
| SPDX-License-Identifier: GPL-2.0-or-later
| Free software under the GNU General Public License, version 2 or (at your
| option) any later version, with NO WARRANTY: see the LICENSE file.

| The Mega Drive game - the FM sound driver (68000, runs in the frame interrupt).
|
| Two players share the YM2612 and the PSG's noise channel:
|  - the music: the streams tools/build_music.py makes from the original
|    game's MIDI tunes, six FM voices (0-4 melodic, 5 kick and toms) and
|    drums on the noise channel;
|  - the sound effects: the original game's sound clips (and effects in the
|    same instruments), tools/build_sfx.py, in the same stream format. While
|    one plays it has FM voices 4 and 5 and the noise channel: the music
|    goes on in the background for them (its notes are remembered, not
|    sent) and gets them back when the effect ends.
| The stream format is described in tools/build_music.py. Time runs in
| 1/60 s ticks on NTSC and PAL alike (PAL: 6 ticks every 5 frames).
|
| The Z80 is held in reset with its bus taken (src/crt0.s), so the 68000
| writes the YM2612 directly at $A04000 (port 0: channels 1-3 and the
| global registers) and $A04002 (port 1: channels 4-6).
|
| C interface: music_play(id), music_stop(), music_update() (frame
| interrupt), sfx_play(stream), sfx_busy(), fm_init(). Registers: d0-d1/a0-a1
| are free to clobber (C convention); everything else is saved.
|
| Callers: vdp.c vblank_sound calls music_update every frame (from
| crt0.s's frame interrupt, with the H interrupt allowed in). ui.c and
| sound.c call music_play, music_stop, sfx_play and sfx_busy.
|
| Jargon used below:
|   voice     one YM2612 FM channel, numbered 0-5 here (the chip's
|             channels 1-6); "voice 6" in a stream means the PSG noise
|             channel.
|   player    the state of one stream being played (struct below); there
|             are two: mus (the music) and sfxp (the sound effect).
|   tick      1/60 s, the stream's time unit.
|   slot      one of the four FM operators of a voice, in the chip's
|             register order (S1, S3, S2, S4).
|   TL        an operator's "total level", its attenuation (0 = loudest,
|             127 = silent, 0.75 dB a step).
|   patch     an FM instrument: 30 bytes (tools/build_music.py).
|
| Register conventions inside the driver (set by the entry points):
|   a5 = YM (the chip's base address), a2 = the current player,
|   a3 = the stream pointer while commands run, a4 = the voice's patch,
|   d7 = the voice number.
|
| Licence: GPL-2.0-or-later.

    .equ    YM, 0xA04000                | YM2612: +0 address / status, +1
                                        | data (port 0); +2, +3 port 1
    .equ    PSG, 0xC00011               | SN76489 PSG write port
    .equ    NV, 6                       | FM voices
    .equ    V_NOISE, 6                  | the voice number that means the
                                        | PSG noise channel in a stream
    .equ    V_SFX0, 4                   | the effects' first FM voice (4 and 5)

| a player (a2 points at one)
| Per-voice arrays are indexed by voice number: P_PATCH by voice * 4, the
| byte arrays (P_NOTE, P_BEND, P_ATT, P_PAN) by voice, 6 bytes each.
    .equ    P_PTR, 0                    | next stream byte (0 = stopped)
    .equ    P_START, 4                  | the stream (for looping)
    .equ    P_PATCH, 8                  | each voice's instrument (6 longs)
                                        | (0 = none chosen yet)
    .equ    P_NOTE, 32                  | sounding note, 0xFF = none
                                        | (MIDI note number)
    .equ    P_BEND, 38                  | signed 1/32 semitones
    .equ    P_ATT, 44                   | carrier attenuation
                                        | (0-127, added to the patch's TL)
    .equ    P_PAN, 50                   | stereo bits
                                        | (register $B4: $C0 both, $80
                                        | left, $40 right; low bits AMS/FMS)
    .equ    P_WAIT, 56                  | ticks left to wait (0 = run now)
    .equ    P_LOOPS, 57                 | 1: the stream restarts at its end
    .equ    P_ACC, 58                   | PAL tick accumulator
                                        | (in fifths of a tick)
    .equ    P_NATT, 59                  | noise: attenuation (15 = silent)
    .equ    P_NRATE, 60                 |   frames per step, and the count
    .equ    P_NCNT, 61
    .equ    P_NINC, 62                  |   attenuation added per step
    .equ    P_SFX, 63                   | 1: the effects player
    .equ    P_SIZE, 64

    .bss
    .even
mus:        .space P_SIZE               | the music's player
sfxp:       .space P_SIZE               | the sound effects' player
    .even

    .text

| ------------------------------------------------------------------ YM access
| ym0: d0 = register, d1 = value, port 0.  ym1: port 1.  Uses a5 = YM.
| Both wait until the chip's status bit 7 (busy) is clear, then write the
| register number to the address port and the value to the data port.
| Port 0 holds the global registers ($22-$2B) and channels 1-3; port 1
| channels 4-6. In: d0.b, d1.b, a5. Out: nothing. Clobbers only flags.
ym0:
1:  btst    #7, (a5)                    | busy with the last write?
    bne.s   1b
    move.b  d0, (a5)                    | port 0 address
    move.b  d1, 1(a5)                   | port 0 data
    rts
ym1:
1:  btst    #7, (a5)                    | busy with the last write?
    bne.s   1b
    move.b  d0, 2(a5)                   | port 1 address
    move.b  d1, 3(a5)                   | port 1 data
    rts

| ymch: d0 = register base (channel registers / operator register + slot),
| d1 = value, d7 = voice 0..5: adds the channel offset, picks the port
| In the chip, the channel-specific registers add 0, 1 or 2 for the
| channel inside a port, so voice 0-2 = port 0 + d7, voice 3-5 = port 1
| + (d7 - 3). Writes nothing when `locked` says the player a2 may not use
| the voice. In: d0, d1, d7, a2, a5. Clobbers d0 (and d1 when locked),
| flags.
ymch:
    bsr.s   locked                      | may player a2 write voice d7?
    bne.s   9f                          | no: skip the write
    cmpi.b  #3, d7
    bcc.s   1f                          | voices 3-5: port 1
    add.b   d7, d0
    bra     ym0                         | (tail call)
1:  add.b   d7, d0
    subq.b  #3, d0
    bra     ym1                         | (tail call)
9:  rts

| Z clear (bne) when the player a2 must not touch voice d7: the music, while
| an effect has voices 4 and 5
| In: a2, d7, sfxp+P_PTR. Out: Z flag. Clobbers d1 when locked (the
| caller then writes nothing).
locked:
    tst.b   P_SFX(a2)                   | the effects player: always free
    bne.s   1f
    cmpi.b  #V_SFX0, d7                 | voices 0-3: always free
    bcs.s   1f
    tst.l   sfxp+P_PTR                  | no effect playing: free
    beq.s   1f
    moveq   #1, d1                      | (the value is not written anyway)
    rts                                 | (moveq #1 leaves Z clear: locked)
1:  cmp.b   d0, d0                      | Z set: free
    rts

| key on / off: d7 = voice; $28 takes channel codes 0-2 and 4-6
| Register $28: bits 0-2 the channel (0-2 port 0, 4-6 port 1), bits 4-7
| the four operators (1 = key on). keyoff writes 0 in bits 4-7, keyon
| $F (all four operators start their attack).
| In: d7, a2, a5. Clobbers d0, d1, flags. Skipped when `locked`.
keyoff:
    bsr.s   locked                      | may player a2 write voice d7?
    bne.s   9f
    moveq   #0x28, d0                   | key on/off register
    move.b  d7, d1
    cmpi.b  #3, d1
    bcs.s   ym0                         | voices 0-2: codes 0-2
    addq.b  #1, d1                      | voices 3-5: codes 4-6
    bra     ym0
9:  rts
keyon:
    bsr.s   locked                      | may player a2 write voice d7?
    bne.s   9b                          | (keyoff's rts)
    moveq   #0x28, d0                   | key on/off register
    move.b  d7, d1
    cmpi.b  #3, d1
    bcs.s   1f
    addq.b  #1, d1                      | voices 3-5: codes 4-6
1:  ori.b   #0xF0, d1                   | all four operators on
    bra     ym0

| load the instrument a4 into voice d7: 7 operator registers x 4 slots,
| then feedback/algorithm
| Patch layout: 28 bytes, slot by slot, each slot's 7 registers in the
| order $30, $40, ..., $90; then byte 28 = FB/ALG (register $B0); byte 29
| = the carrier mask (used by set_volume, not written to the chip).
| Operator register address = base + slot * 4 (+ channel, added by ymch).
| In: a4 = patch, d7, a2, a5. Clobbers d0, d1, flags; keeps d2-d3/a3.
load_patch:
    movem.l d2-d3/a3, -(sp)
    movea.l a4, a3                      | a3 walks the patch bytes
    moveq   #0, d3                      | slot 0..3
1:  moveq   #0x30, d2                   | DT/MUL, TL, RS/AR, AM/D1R, D2R, D1L/RR, SSG-EG
2:  move.b  d2, d0
    add.b   d3, d0
    add.b   d3, d0
    add.b   d3, d0
    add.b   d3, d0                      | + slot * 4
    move.b  (a3)+, d1
    bsr   ymch                          | write one operator register
    addi.b  #0x10, d2                   | next register group
    cmpi.b  #0xA0, d2                   | $30..$90 done?
    bne.s   2b
    addq.b  #1, d3
    cmpi.b  #4, d3                      | all four slots done?
    bne.s   1b
    move.b  #0xB0, d0                   | feedback / algorithm register
    move.b  28(a4), d1
    bsr   ymch                          | write FB/ALG
    movem.l (sp)+, d2-d3/a3
    rts

| the carriers' TL = the instrument's TL + v_att (voice d7, instrument a4)
| Only the operators marked in the carrier mask (byte 29) change: those
| set the loudness; the modulators set the timbre and are left alone. The
| sum is capped at 127 (silent). TL registers are $40 + slot * 4.
| In: a4, d7, a2 (P_ATT), a5. Clobbers d0, d1, flags; keeps d2-d4.
set_volume:
    movem.l d2-d4, -(sp)
    moveq   #0, d4
    move.b  P_ATT(a2,d7.w), d4          | d4 = the note's attenuation
    move.b  29(a4), d3                  | carrier mask
    moveq   #0, d2                      | slot
1:  btst    d2, d3                      | is this slot a carrier?
    beq.s   2f
    move.w  d2, d0
    mulu    #7, d0                      | 7 patch bytes per slot
    moveq   #0, d1
    move.b  1(a4,d0.w), d1              | the instrument's TL
    add.w   d4, d1
    cmpi.w  #127, d1
    bls.s   3f
    moveq   #127, d1                    | cap at silent
3:  move.b  d2, d0
    lsl.b   #2, d0
    addi.b  #0x40, d0                   | register $40 + slot * 4
    bsr   ymch                          | write the TL
2:  addq.b  #1, d2
    cmpi.b  #4, d2
    bne.s   1b
    movem.l (sp)+, d2-d4
    rts

| frequency registers from v_note and v_bend (voice d7):
| p = note * 32 + bend; octave = p / 384, F-number = music_fnum[p mod 384],
| block = octave - 1 (MIDI 60 = block 4)
| 384 = 12 semitones * 32 steps. music_fnum (tools/build_music.py) holds
| one octave of 11-bit F-numbers for block 4; each block up doubles the
| pitch. The block is clamped to 0..7. Register $A4 (block and F-number
| bits 8-10) must be written before $A0 (bits 0-7): the chip latches $A4
| and applies both when $A0 is written.
| In: d7, a2 (P_NOTE, P_BEND), a5. Clobbers d0, d1, a0, flags; keeps
| d2-d3.
set_freq:
    movem.l d2-d3, -(sp)
    moveq   #0, d2
    move.b  P_NOTE(a2,d7.w), d2
    lsl.w   #5, d2                      | note * 32
    move.b  P_BEND(a2,d7.w), d0
    ext.w   d0                          | signed bend
    add.w   d0, d2                      | p (never negative: notes >= 28)
    ext.l   d2
    divu    #384, d2                    | low word: octave, high word: p mod 384
    move.w  d2, d3
    subq.w  #1, d3                      | block
    bpl.s   1f
    moveq   #0, d3                      | below block 0: clamp
1:  cmpi.w  #7, d3
    bls.s   2f
    moveq   #7, d3                      | above block 7: clamp
2:  swap    d2                          | d2.w = p mod 384
    add.w   d2, d2                      | word index
    lea     music_fnum, a0
    move.w  0(a0,d2.w), d2              | F-number
    move.w  d2, d1
    lsr.w   #8, d1                      | F-number bits 8-10
    lsl.b   #3, d3
    or.b    d3, d1                      | block << 3 | F-number high bits
    move.b  #0xA4, d0                   | (high byte first: it is latched)
    bsr     ymch                        | write block / F-number high
    move.b  #0xA0, d0
    move.b  d2, d1                      | F-number low byte
    bsr     ymch                        | write it: the pitch changes now
    movem.l (sp)+, d2-d3
    rts

| ------------------------------------------------------------------ noise
| type: noise control (white, rate), first attenuation, frames per step,
| attenuation per step
| The PSG noise channel has no envelope, so the driver fades it by hand
| (noise_frame): every "frames per step" frames the attenuation grows by
| "attenuation per step" until it reaches 15 (silent). Attenuation is
| 0-15, 2 dB a step.
| Noise control byte $E0 | bits: bit 2 = 1 white noise (0 = periodic),
| bits 0-1 the shift rate (0 = highest pitch, 1, 2 = lower; 3 = follow
| tone channel 3). So $E4-$E6 are white noise, high to low.
noise_drums:
    .byte   0xE5, 1, 1, 1               | 0 snare
    .byte   0xE4, 3, 1, 3               | 1 closed hi-hat
    .byte   0xE4, 4, 2, 1               | 2 open hi-hat / ride
    .byte   0xE4, 2, 4, 1               | 3 crash
    .byte   0xE5, 0, 1, 1               | 4 shot: a gunshot's crack
    .byte   0xE6, 0, 2, 1               | 5 blast: low rumble (explosions)
    .byte   0xE4, 1, 5, 1               | 6 hiss (the extinguisher, fire)
    .byte   0xE4, 3, 1, 3               | 7 tick (menus, steps)
    .even

| the noise may be written by player a2: the effects always, the music
| while no effect plays (Z set)
| In: a2, sfxp+P_PTR. Out: Z flag (set = may write). Clobbers flags only.
noise_free:
    tst.b   P_SFX(a2)                   | the effects player?
    bne.s   1f                          | yes: always free
    tst.l   sfxp+P_PTR                  | the music: Z set if no effect
    rts
1:  cmp.b   d0, d0                      | Z set: free
    rts

| d1 = noise type: start it
| Looks up noise_drums[d1 & 7], stores the fade state in the player
| (P_NATT, P_NRATE, P_NCNT, P_NINC), and, if the player may use the noise
| channel now, writes the noise control byte and the volume to the PSG.
| For the effects player, d3 (the stream's attenuation byte) is added to
| the drum's starting attenuation, so an effect can play a quieter hit.
| (For the music the byte is ignored.) The state is kept even when the
| channel is busy, so the music's drums fade on silently under an effect.
| In: d1, d3, a2. Clobbers d0, d1, a0, flags.
noise_hit:
    lea     noise_drums, a0
    andi.w  #7, d1
    lsl.w   #2, d1                      | 4 bytes per entry
    adda.w  d1, a0                      | a0 = the drum's entry
    move.b  1(a0), d0                   | starting attenuation
    tst.b   P_SFX(a2)                   | (an effect's stream: d3 = extra attenuation steps)
    beq.s   2f
    add.b   d3, d0
    cmpi.b  #15, d0
    bls.s   2f
    moveq   #15, d0                     | cap at silent
2:  move.b  d0, P_NATT(a2)
    move.b  2(a0), P_NRATE(a2)          | frames per fade step
    move.b  2(a0), P_NCNT(a2)           | (the countdown starts full)
    move.b  3(a0), P_NINC(a2)           | attenuation added per step
    bsr     noise_free                  | may this player use the noise?
    bne.s   1f                          | no: state only
    move.b  (a0), PSG                   | noise control (type and rate);
                                        | this also restarts the noise
    move.b  P_NATT(a2), d0
    ori.b   #0xF0, d0                   | $F0 | att: channel 3 (noise)
    move.b  d0, PSG                     | volume
1:  rts

| once per frame: the noise fades
| Every P_NRATE frames, P_NATT += P_NINC (capped at 15), and the new
| volume is written if the player may use the channel. Nothing happens
| once P_NATT is 15. In: a2. Clobbers d0, flags.
noise_frame:
    cmpi.b  #15, P_NATT(a2)             | already silent?
    bcc.s   9f
    subq.b  #1, P_NCNT(a2)              | count the frames to the next step
    bne.s   9f
    move.b  P_NRATE(a2), P_NCNT(a2)     | reload the count
    move.b  P_NATT(a2), d0
    add.b   P_NINC(a2), d0
    cmpi.b  #15, d0
    bls.s   1f
    moveq   #15, d0                     | cap at silent
1:  move.b  d0, P_NATT(a2)
    bsr     noise_free                  | may this player use the noise?
    bne.s   9f
    move.b  P_NATT(a2), d0
    ori.b   #0xF0, d0                   | channel 3 attenuation command
    move.b  d0, PSG
9:  rts

| ------------------------------------------------------------------ the stream
| one tick of player a2: wait, or run commands up to the next wait
| (the commands are listed in tools/build_music.py and summed up at each
| handler below). A wait of n ticks makes the next n - 1 calls return at
| once; the n-th runs the next commands.
| In: a2, a5. Out: P_PTR, P_WAIT and the voices' state updated; P_PTR = 0
| if a stream without looping ended. Clobbers d0-d3, d7, a0, a3, a4,
| flags (and a2 is put back to sfxp by sfx_done).
| The command handlers below (c_on .. song_end) are branch targets inside
| tick, not subroutines: each reads its operand bytes from (a3)+ and goes
| back to `cmd` for the next command.
tick:
    tst.b   P_WAIT(a2)                  | waiting?
    beq.s   1f
    subq.b  #1, P_WAIT(a2)
    bne.s   9f                          | still waiting: done for this tick
1:  movea.l P_PTR(a2), a3               | a3 = stream pointer
cmd:
    moveq   #0, d0
    move.b  (a3)+, d0                   | the next command byte
    bmi.s   2f                          | $80-$FF: a command
    move.b  d0, P_WAIT(a2)              | $01-$7F: wait
    move.l  a3, P_PTR(a2)               | save the position for next time
9:  rts
2:  cmpi.b  #0xFF, d0
    beq     song_end
    moveq   #7, d7
    and.b   d0, d7                      | voice
    andi.b  #0xF8, d0                   | d0 = the command without its voice
    cmpi.b  #0x80, d0
    beq.s   c_on
    cmpi.b  #0x88, d0
    beq.s   c_off
    cmpi.b  #0x90, d0
    beq     c_patch
    cmpi.b  #0x98, d0
    beq     c_bend
    cmpi.b  #0xA0, d0
    beq     c_att
    cmpi.b  #0xB0, d0
    beq     c_note
    | $A8: stereo
    | (any other unlisted command byte also lands here)
    move.b  (a3)+, d1
    move.b  d1, P_PAN(a2,d7.w)          | remembered, for sfx_done
    move.b  #0xB4, d0                   | register $B4: stereo, AMS, FMS
    bsr     ymch
    bra     cmd

| $80+v n a: key on. FM: retrigger voice v with note n at attenuation a.
| v = 6: noise drum type n (a: the effect's extra attenuation).
c_on:
    move.b  (a3)+, d2                   | note / noise type
    move.b  (a3)+, d3                   | attenuation
    cmpi.b  #V_NOISE, d7
    bne.s   1f
    move.b  d2, d1
    bsr     noise_hit                   | start the noise drum
    bra     cmd
1:  move.b  d2, P_NOTE(a2,d7.w)
    move.b  d3, P_ATT(a2,d7.w)
    bsr     keyoff                      | release the old note first, so the
                                        | key on restarts the envelopes
    move.w  d7, d0
    lsl.w   #2, d0
    movea.l P_PATCH(a2,d0.w), a4        | a4 = this voice's instrument
    move.l  a4, d0
    beq     cmd                         | (no instrument yet)
    bsr     set_volume                  | the carriers' TL for this note
    bsr     set_freq                    | the pitch
    bsr     keyon                       | sound it
    bra     cmd

| $88+v: key off (ignored for the noise, which fades by itself).
c_off:
    cmpi.b  #V_NOISE, d7
    beq     cmd
    st      P_NOTE(a2,d7.w)             | no note sounding (0xFF)
    bsr     keyoff
    bra     cmd

| $90+v p: instrument p for voice v, loaded into the chip now.
c_patch:
    moveq   #0, d0
    move.b  (a3)+, d0
    mulu    #30, d0                     | 30 bytes per patch
    lea     music_patches, a4
    adda.w  d0, a4                      | a4 = the patch
    move.w  d7, d0
    lsl.w   #2, d0
    move.l  a4, P_PATCH(a2,d0.w)        | remembered (set_volume, sfx_done)
    bsr     load_patch                  | write its registers
    bra     cmd

| $98+v b: pitch bend, b signed 1/32 semitones.
c_bend:
    move.b  (a3)+, P_BEND(a2,d7.w)
    cmpi.b  #0xFF, P_NOTE(a2,d7.w)
    beq     cmd                         | no note: just remember it
    bsr     set_freq                    | bend the sounding note
    bra     cmd

c_note:                                 | $B0+v n: a new pitch, no new attack (slides)
    move.b  (a3)+, d0
    cmpi.b  #0xFF, P_NOTE(a2,d7.w)      | no note sounding: ignored
    beq     cmd
    move.b  d0, P_NOTE(a2,d7.w)
    bsr     set_freq                    | change the pitch only
    bra     cmd

| $A0+v a: new carrier attenuation (a volume change during a note).
c_att:
    move.b  (a3)+, P_ATT(a2,d7.w)
    move.w  d7, d0
    lsl.w   #2, d0
    movea.l P_PATCH(a2,d0.w), a4
    move.l  a4, d0
    beq     cmd                         | (no instrument yet)
    bsr     set_volume                  | rewrite the carriers' TL
    bra     cmd

| $FF: end of stream. Loop back to the start (after the header byte), or
| stop the player; an effect that stops hands its voices back.
song_end:
    tst.b   P_LOOPS(a2)
    beq.s   1f
    movea.l P_START(a2), a3             | loop: back to the first command
    bra     cmd
1:  clr.l   P_PTR(a2)                   | stopped
    tst.b   P_SFX(a2)
    bne     sfx_done                    | (tail call for the effects player)
    rts

| an effect has ended: its voices off, the noise quiet, and voices 4 and 5
| back to the music (its instruments and stereo)
| The music's notes on voices 4 and 5 are not restarted; they come back
| at the music's next key on for those voices.
| In: a2 = sfxp, with P_PTR already 0 (so `locked` lets the music write).
| Out: a2 = sfxp again. Clobbers d0, d1, d7, a4, flags.
sfx_done:
    moveq   #V_SFX0, d7
    bsr     keyoff                      | the effect's voice 4 off
    moveq   #V_SFX0+1, d7
    bsr     keyoff                      | and voice 5
    move.b  #15, P_NATT(a2)             | the effect's noise: silent
    move.b  #0xFF, PSG                  | PSG: channel 3 attenuation 15
    lea     mus, a2                     | (the effect is over: the music may write)
    moveq   #V_SFX0, d7
3:  move.w  d7, d0
    lsl.w   #2, d0
    movea.l P_PATCH(a2,d0.w), a4        | the music's instrument for d7
    move.l  a4, d0
    beq.s   4f                          | none chosen yet
    bsr     load_patch                  | put it back in the chip
    bsr     set_volume                  | with the music's volume
4:  move.b  #0xB4, d0
    move.b  P_PAN(a2,d7.w), d1
    bsr     ymch                        | and the music's stereo setting
    addq.b  #1, d7
    cmpi.b  #6, d7                      | voices 4 and 5 done?
    bcs.s   3b
    lea     sfxp, a2                    | the caller expects a2 = sfxp
    rts

| one frame of player a2
| Adds 5 (NTSC, 60 frames/s) or 6 (PAL, 50 frames/s) to the accumulator
| and runs one tick per 5 in it, so the music plays at 60 ticks a second
| on both. Bit 6 of the version register $A10001 is 1 on a PAL console.
| Also fades the noise once a frame. Stops early if the stream ends.
| In: a2, a5. Clobbers what tick clobbers.
run_player:
    tst.l   P_PTR(a2)                   | stopped?
    beq.s   9f
    bsr     noise_frame                 | the noise's fade
    moveq   #5, d0                      | NTSC: one tick a frame
    btst    #6, 0xA10001                | PAL: 6 ticks in 5 frames
    beq.s   1f
    moveq   #6, d0
1:  add.b   d0, P_ACC(a2)
2:  cmpi.b  #5, P_ACC(a2)               | a whole tick in the accumulator?
    bcs.s   9f
    subq.b  #5, P_ACC(a2)
    bsr     tick                        | run one tick of the stream
    tst.l   P_PTR(a2)                   | ended during the tick?
    bne.s   2b
9:  rts

| player a2 from the start of stream a3 (its voices' state cleared)
| Stream header: one byte, 1 = loop, 0 = play once; the commands follow.
| Resets the wait, the PAL accumulator, the noise (P_NATT = 15, state
| only: nothing is written to the PSG), every voice's bend, note (none),
| stereo (both sides) and instrument (none). P_ATT is left as it was: each
| key on sets it. In: a2, a3. Clobbers d0, a0, a3, flags.
start_player:
    move.b  (a3)+, P_LOOPS(a2)          | the header byte
    move.l  a3, P_START(a2)
    move.l  a3, P_PTR(a2)               | non-zero: the player runs
    clr.b   P_WAIT(a2)
    clr.b   P_ACC(a2)
    move.b  #15, P_NATT(a2)
    moveq   #NV-1, d0
1:  clr.b   P_BEND(a2,d0.w)
    st      P_NOTE(a2,d0.w)
    move.b  #0xC0, P_PAN(a2,d0.w)       | both speakers
    dbra    d0, 1b
    lea     P_PATCH(a2), a0
    moveq   #NV-1, d0
2:  clr.l   (a0)+                       | no instrument
    dbra    d0, 2b
    rts

| ------------------------------------------------------------------ C entry points
| void music_update(void): from the frame interrupt
| Runs one frame of the music, then one of the effect (the effect last,
| so its writes win on the shared voices). Called by vdp.c vblank_sound.
| Saves every register it uses beyond d0-d1/a0-a1.
    .global music_update
music_update:
    movem.l d2-d7/a2-a5, -(sp)
    lea     YM, a5
    lea     mus, a2
    bsr     run_player                  | the music's frame
    lea     sfxp, a2
    bsr     run_player                  | the effect's frame
    movem.l (sp)+, d2-d7/a2-a5
    rts

| the music's voices off (interrupts must be off, or this be the interrupt)
| Keys off the music's six voices (voices 4 and 5 are skipped while an
| effect holds them), marks no note sounding, silences the music's noise
| state, and writes the PSG noise volume to silent unless an effect plays.
| In: a5. Out: a2 = mus. Clobbers d0, d1, d7, a2, flags.
silence_all:
    lea     mus, a2
    moveq   #NV-1, d7                   | voices 5 down to 0
1:  bsr     keyoff
    st      P_NOTE(a2,d7.w)
    dbra    d7, 1b
    move.b  #15, P_NATT(a2)
    tst.l   sfxp+P_PTR                  | an effect owns the noise?
    bne.s   2f
    move.b  #0xFF, PSG                  | no: noise volume to silent
2:  rts

| void music_stop(void)
| Stops the music (an effect playing goes on). Masks interrupts so the
| frame interrupt's music_update cannot run halfway through.
| Clobbers d0-d1/a0-a1 only; SR restored.
    .global music_stop
music_stop:
    move.w  sr, -(sp)
    move.w  #0x2700, sr                 | interrupts off
    movem.l d2-d7/a2-a5, -(sp)
    lea     YM, a5
    clr.l   mus+P_PTR                   | the music player stops
    bsr     silence_all                 | and its voices go quiet
    movem.l (sp)+, d2-d7/a2-a5
    move.w  (sp)+, sr                   | interrupts as before
    rts

| void fm_init(void): the chip: no DAC, no LFO, channel 3 in normal mode
| Register $2B = 0: DAC off (channel 6 is FM). $22 = 0: LFO off. $27 = 0:
| channel 3 normal mode, timers stopped. Called by sound.c at start-up and
| by music_play. Clobbers d0, d1; keeps a5.
    .global fm_init
fm_init:
    move.l  a5, -(sp)
    lea     YM, a5
    moveq   #0x2B, d0
    moveq   #0, d1
    bsr     ym0                         | DAC off
    moveq   #0x22, d0
    moveq   #0, d1
    bsr     ym0                         | LFO off
    moveq   #0x27, d0
    moveq   #0, d1
    bsr     ym0                         | channel 3 mode normal, timers off
    movea.l (sp)+, a5
    rts

| void music_play(u8 id)
| Starts song `id` (MUS_* from the generated music.h, an index into
| music_songs) from its beginning, replacing any music playing. The id is
| not range-checked. An effect playing keeps its voices.
| Clobbers d0-d1/a0-a1 only; SR restored.
    .global music_play
music_play:
    move.l  4(sp), d0                   | id (promoted to a long)
    move.w  sr, -(sp)
    move.w  #0x2700, sr                 | interrupts off
    movem.l d2-d7/a2-a5, -(sp)
    lea     YM, a5
    andi.w  #0xFF, d0
    lsl.w   #2, d0                      | one long per song
    lea     music_songs, a0
    movea.l 0(a0,d0.w), a3              | the song
    bsr     fm_init                     | known chip state
    bsr     silence_all                 | the old song's notes off
    lea     mus, a2
    bsr     start_player                | the music player at the start
    movem.l (sp)+, d2-d7/a2-a5
    move.w  (sp)+, sr                   | interrupts as before
    rts

| void sfx_play(const u8 *stream): an effect (it replaces one playing)
| Takes voices 4 and 5 (and the noise) from the music until the stream
| ends. Called by sound.c, which picks the stream (sfx_table) and decides
| whether to interrupt the effect already playing.
| Clobbers d0-d1/a0-a1 only; SR restored.
    .global sfx_play
sfx_play:
    movea.l 4(sp), a1                   | the stream
    move.w  sr, -(sp)
    move.w  #0x2700, sr                 | interrupts off
    movem.l d2-d7/a2-a5, -(sp)
    movea.l a1, a3
    lea     YM, a5
    lea     sfxp, a2
    move.b  #1, P_SFX(a2)               | mark the effects player
    moveq   #V_SFX0, d7                 | the voices: quiet first
1:  bsr     keyoff                      | (key off voices 4 and 5)
    addq.b  #1, d7
    cmpi.b  #6, d7
    bcs.s   1b
    bsr     start_player                | the effect from its start
    movem.l (sp)+, d2-d7/a2-a5
    move.w  (sp)+, sr                   | interrupts as before
    rts

| u8 sfx_busy(void)
| Returns d0 = 0xFF while an effect plays, 0 when none does (C treats any
| non-zero as true). Clobbers d0, flags.
    .global sfx_busy
sfx_busy:
    moveq   #0, d0
    tst.l   sfxp+P_PTR
    sne     d0                          | 0xFF if P_PTR is non-zero
    rts
