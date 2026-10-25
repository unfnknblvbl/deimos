#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""FM music for the Mega Drive game: the original game's MIDI tunes (title, intro,
sector complete) turned into a stream the 68000 driver (src/music.s) plays
on the YM2612, with the drums on FM (kick, toms) and the PSG's noise channel
(snare, hi-hats, cymbals).

Writes <gen>/music_data.s:
  music_songs     .long song addresses (MUS_TITLE, MUS_INTRO, MUS_END)
  music_patches   FM instruments, 30 bytes each (below)
  music_fnum      384 F-numbers: one octave in 1/32 semitones, block 4
and <gen>/music.h with the MUS_* ids for the C code.

Song stream, one byte command at a time; time is in 1/60 s ticks:
  $01-$7F         wait that many ticks
  $80+v n a       key on, voice v (0-5 FM, 6 = PSG noise drum): FM: note n
                  (MIDI number), carrier attenuation a (0-127, added to the
                  patch's TL); noise: drum type n, a unused
  $88+v           key off
  $90+v p         instrument p (FM voices)
  $98+v b         pitch bend, b = signed 1/32 semitones
  $A0+v a         carrier attenuation (a volume change while a note sounds)
  $A8+v s         stereo: $C0 both, $80 left, $40 right
  $B0+v n         a new pitch (MIDI note n) for the sounding note, without
                  a new attack (used only by the sound effects' slides,
                  tools/build_sfx.py)
  $FF             end: back to the start if the song loops, else stop
The song header: u8 loops (1/0), then the stream.

Instrument, 30 bytes: for the operators in register order (S1, S3, S2, S4):
  DT/MUL, TL, RS/AR, AM/D1R, D2R, D1L/RR, SSG-EG  (7 x 4 = 28)
then FB/ALG, and the carrier mask (bit k = register-order operator k is a
carrier: its TL gets the note's attenuation).

Licence: GPL-2.0-or-later.
"""
import math
import struct
import sys
from pathlib import Path

CLOCK = 7670453                     # NTSC YM2612 clock
FPS = 60  # the stream's tick rate (the driver runs from the 60 Hz frame interrupt)

# ------------------------------------------------------------------ MIDI
def vlq(d, i):
    """A MIDI variable-length number at d[i]: 7 bits per byte, top bit set on all but the last. Returns (value, next index)."""
    v = 0
    while True:
        b = d[i]
        i += 1
        v = (v << 7) | (b & 0x7F)
        if not b & 0x80:
            return v, i


def parse_midi(path):
    """-> ticks per beat, [(tick, kind, channel, data bytes or meta)] for all tracks

    Each event is really (tick, order, kind, channel or meta type, data):
    kind is the status nibble (0x80 note off, 0x90 note on, 0xB0
    controller, 0xC0 program, 0xE0 pitch bend...) or "meta". Running status
    is handled; sysex events are skipped. Sorted by tick, then file order.
    """
    d = Path(path).read_bytes()
    assert d[:4] == b"MThd"
    _fmt, ntr, div = struct.unpack(">HHH", d[8:14])
    i = 14
    events = []
    for _ in range(ntr):
        assert d[i:i + 4] == b"MTrk"
        n = struct.unpack(">I", d[i + 4:i + 8])[0]
        t = d[i + 8:i + 8 + n]
        i += 8 + n
        j = tick = run = 0
        order = 0
        while j < len(t):
            dt, j = vlq(t, j)
            tick += dt
            b = t[j]
            if b == 0xFF:
                typ = t[j + 1]
                ln, j = vlq(t, j + 2)
                events.append((tick, order, "meta", typ, t[j:j + ln]))
                j += ln
            elif b in (0xF0, 0xF7):
                ln, j = vlq(t, j + 1)
                j += ln
            else:
                if b & 0x80:
                    run = b
                    j += 1
                hi, ch = run & 0xF0, run & 15
                nb = 1 if hi in (0xC0, 0xD0) else 2
                events.append((tick, order, hi, ch, bytes(t[j:j + nb])))
                j += nb
            order += 1
    events.sort(key=lambda e: (e[0], e[1]))
    return div, events


# ------------------------------------------------------------------ instruments
def op(mul=1, dt=0, tl=127, ar=31, d1r=0, d2r=0, sl=0, rr=15, rs=0, am=0):
    """One FM operator's 7 register bytes, from readable parameters:
    DT/MUL, TL (0 loudest .. 127 silent), RS/AR, AM/D1R, D2R, D1L/RR, SSG-EG
    (always 0).
    """
    return (dt << 4 | mul, tl, rs << 6 | ar, am << 7 | d1r, d2r, sl << 4 | rr, 0)


# per YM2612 algorithm (0..7): which operators (OP1..OP4) are carriers, the ones heard
ALG_CARRIERS = {0: (4,), 1: (4,), 2: (4,), 3: (4,), 4: (2, 4), 5: (2, 3, 4), 6: (2, 3, 4), 7: (1, 2, 3, 4)}


def patch(alg, fb, op1, op2, op3, op4):
    """operators given as OP1..OP4 (the algorithm diagrams' numbering)

    Returns the 30-byte instrument (layout in the module docstring) for
    algorithm `alg` with feedback `fb`.
    """
    regs = [op1, op3, op2, op4]                 # register order S1, S3, S2, S4
    slot_of = {1: 0, 3: 1, 2: 2, 4: 3}
    mask = 0
    for c in ALG_CARRIERS[alg]:
        mask |= 1 << slot_of[c]
    out = []
    for r in regs:
        out += r
    return bytes(out + [fb << 3 | alg, mask])


# General MIDI program -> instrument index
PATCHES = [
    # 0 distortion guitar: two detuned 2-operator stacks, heavy feedback
    patch(4, 7, op(mul=1, tl=22, d1r=2, sl=1), op(mul=1, dt=3, tl=6, d1r=4, d2r=2, sl=3, rr=9),
          op(mul=2, dt=7, tl=30, d1r=2, sl=1), op(mul=1, dt=7, tl=8, d1r=4, d2r=2, sl=3, rr=9)),
    # 1 overdriven guitar: softer
    patch(4, 6, op(mul=1, tl=27, d1r=3, sl=2), op(mul=1, dt=3, tl=6, d1r=5, d2r=3, sl=3, rr=9),
          op(mul=1, dt=7, tl=34, d1r=3, sl=2), op(mul=2, dt=7, tl=12, d1r=5, d2r=3, sl=3, rr=9)),
    # 2 electric bass: a plucked 4-operator chain with a sub octave
    patch(0, 5, op(mul=1, tl=34, d1r=10, d2r=4, sl=4), op(mul=0, tl=30, d1r=6, sl=3),
          op(mul=1, tl=28, d1r=8, d2r=3, sl=4), op(mul=0, tl=2, d1r=3, d2r=2, sl=2, rr=10)),
    # 3 rock organ: additive drawbars (16', 8', 4', 2 2/3')
    patch(7, 4, op(mul=0, tl=16, sl=0, rr=8), op(mul=1, tl=10, sl=0, rr=8),
          op(mul=2, tl=16, sl=0, rr=8), op(mul=3, tl=22, sl=0, rr=8)),
    # 4 kick: a short low sine with a click (the stream sweeps its pitch down)
    patch(4, 0, op(mul=3, tl=24, d1r=31, d2r=31, sl=15), op(mul=1, tl=0, d1r=14, d2r=12, sl=4, rr=15),
          op(mul=1, tl=127), op(mul=1, tl=127)),
    # 5 tom
    patch(4, 2, op(mul=2, tl=30, d1r=20, d2r=20, sl=15), op(mul=1, tl=2, d1r=10, d2r=8, sl=3, rr=15),
          op(mul=1, tl=127), op(mul=1, tl=127)),
    # ---- sound effects (tools/build_sfx.py) ----
    # 6 gunshot body (GM 127): a feedback-noise chain with a fast fall, under the PSG's noise crack
    patch(0, 7, op(mul=1, tl=4, d1r=14, d2r=10, sl=8), op(mul=7, dt=3, tl=12, d1r=12, d2r=8, sl=6),
          op(mul=3, tl=16, d1r=12, d2r=10, sl=6), op(mul=1, tl=0, d1r=11, d2r=13, sl=4, rr=12)),
    # 7 bass + lead (GM 87): a bright lead over a sub octave
    patch(4, 6, op(mul=1, tl=26, d1r=3, sl=2), op(mul=1, tl=6, d1r=4, d2r=2, sl=3, rr=8),
          op(mul=0, dt=3, tl=30, d1r=3, sl=2), op(mul=0, tl=12, d1r=4, d2r=2, sl=3, rr=8)),
    # 8 timpani (GM 47): a deep membrane, slow fall
    patch(4, 3, op(mul=1, tl=34, d1r=8, d2r=6, sl=8), op(mul=1, tl=2, d1r=6, d2r=4, sl=4, rr=9),
          op(mul=3, dt=2, tl=44, d1r=12, d2r=8, sl=10), op(mul=1, tl=14, d1r=7, d2r=5, sl=5, rr=9)),
    # 9 synth drum (GM 118): an electronic tom, swept down by the stream
    patch(4, 4, op(mul=1, tl=28, d1r=12, d2r=8, sl=6), op(mul=1, tl=0, d1r=9, d2r=15, sl=4, rr=12),
          op(mul=1, tl=127), op(mul=1, tl=127)),
]
# General MIDI program (0-based) -> instrument index of PATCHES; any other program uses 0
GM_PATCH = {29: 1, 30: 0, 31: 0, 32: 2, 33: 2, 34: 2, 35: 2, 17: 3, 18: 3, 16: 3, 47: 5, 118: 5}
P_KICK, P_TOM = 4, 5

# PSG noise drums: types the driver knows (src/music.s, noise_drums)
NOISE_SNARE, NOISE_HAT, NOISE_OPEN, NOISE_CRASH = 0, 1, 2, 3
DRUM_MAP = {38: NOISE_SNARE, 40: NOISE_SNARE, 37: NOISE_HAT, 39: NOISE_SNARE, 42: NOISE_HAT, 44: NOISE_HAT,
            46: NOISE_OPEN, 51: NOISE_OPEN, 53: NOISE_OPEN, 59: NOISE_OPEN, 49: NOISE_CRASH, 52: NOISE_CRASH,
            55: NOISE_CRASH, 57: NOISE_CRASH}
TOMS = {41: 43, 43: 46, 45: 50, 47: 53, 48: 57, 50: 60}       # drum note -> pitch
KICKS = {35, 36}  # GM drum notes played as the FM kick

N_MELODIC = 5                      # FM voices 0-4; voice 5 = kick and toms
V_DRUM_FM, V_NOISE = 5, 6


def att_of(vel, vol):
    """velocity x channel volume -> attenuation in 0.75 dB steps

    (0.75 dB is one step of the YM2612's TL.) 127 = silent.
    """
    g = (vel / 127) * (vol / 127)
    if g <= 0:
        return 127
    return max(0, min(127, round(-20 * math.log10(g) / 0.75 * 0.6)))   # (0.6: gentler than linear)


def pan_bits(v):
    """A MIDI pan value (0..127) -> the YM2612 stereo bits: $80 left, $40 right, $C0 both (the middle)."""
    return 0x80 if v < 0x30 else 0x40 if v > 0x50 else 0xC0


def convert(path, loops):
    """One MIDI file -> (song bytes, length in seconds).

    Melodic channels are given to FM voices 0..4 (a free voice, preferably
    the one this channel used last; if none is free the oldest note is cut);
    channel 10 (index 9) drums go to FM voice 5 (kick, toms: a note plus a
    falling pitch bend over 6 frames) or to the noise channel. Volume,
    pan, bend and program changes become stream commands; time is turned
    into 1/60 s waits with the file's tempo map. `loops`: the song repeats.
    """
    div, events = parse_midi(path)
    tempo = 500000
    # (time in ticks, priority, bytes)
    out = []
    t_of = None
    tempo_changes = [(0, 500000)]
    for e in events:
        if e[2] == "meta" and e[3] == 0x51:
            tempo_changes.append((e[0], int.from_bytes(e[4], "big")))

    def frames(tick):
        """MIDI tick -> 1/60 s ticks (tempo map)"""
        sec = 0.0
        last_t, last_tempo = 0, tempo_changes[0][1]
        for tt, tp in tempo_changes[1:]:
            if tt >= tick:
                break
            sec += (tt - last_t) * last_tempo / div / 1e6
            last_t, last_tempo = tt, tp
        sec += (tick - last_t) * last_tempo / div / 1e6
        return round(sec * FPS)

    chan_prog = {c: 0 for c in range(16)}
    chan_vol = {c: 100 for c in range(16)}
    chan_pan = {c: 0x40 for c in range(16)}
    chan_bend = {c: 0 for c in range(16)}
    # voices: (midi channel, note, start frame) or None
    voice = [None] * N_MELODIC  # per FM voice: (channel, note, start frame, velocity) while it sounds
    voice_patch = [None] * 6
    voice_pan = [None] * 6
    voice_bend = [0] * 6
    voice_last_chan = [None] * N_MELODIC
    stream = []                     # (frame, seq, bytes)
    seq = [0]

    def emit(fr, *b):
        """Queues command bytes b at frame fr (seq keeps commands of the same frame in order)."""
        stream.append((fr, seq[0], bytes(b)))
        seq[0] += 1

    def set_voice(v, fr, prog_patch, pan, bend):
        """Emits the instrument, pan and bend commands voice v needs, only those that change."""
        if voice_patch[v] != prog_patch:
            emit(fr, 0x90 + v, prog_patch)
            voice_patch[v] = prog_patch
        if voice_pan[v] != pan:
            emit(fr, 0xA8 + v, pan)
            voice_pan[v] = pan
        if voice_bend[v] != bend:
            emit(fr, 0x98 + v, bend & 0xFF)
            voice_bend[v] = bend

    end_frame = 0
    for e in events:
        tick, _o, kind = e[0], e[1], e[2]
        fr = frames(tick)
        end_frame = max(end_frame, fr)
        if kind == "meta":
            continue
        ch, data = e[3], e[4]
        if kind == 0xC0:
            chan_prog[ch] = data[0]
        elif kind == 0xB0:
            if data[0] == 7:  # controller 7: channel volume
                chan_vol[ch] = data[1]
                for v in range(N_MELODIC):
                    if voice[v] and voice[v][0] == ch:
                        emit(fr, 0xA0 + v, att_of(voice[v][3], data[1]))
            elif data[0] == 10:  # controller 10: pan
                chan_pan[ch] = data[1]
        elif kind == 0xE0:
            b = ((data[1] << 7) | data[0]) - 8192
            chan_bend[ch] = round(b / 8192 * 2 * 32)  # a range of +-2 semitones, in 1/32 semitones
            for v in range(N_MELODIC):
                if voice[v] and voice[v][0] == ch and voice_bend[v] != chan_bend[ch]:
                    emit(fr, 0x98 + v, chan_bend[ch] & 0xFF)
                    voice_bend[v] = chan_bend[ch]
        elif kind in (0x80, 0x90):
            note, vel = data[0], data[1]
            on = kind == 0x90 and vel > 0
            if ch == 9:  # the GM drum channel
                if not on:
                    continue
                a = att_of(vel, chan_vol[9])
                if note in KICKS or note in TOMS:
                    is_kick = note in KICKS
                    set_voice(V_DRUM_FM, fr, P_KICK if is_kick else P_TOM, 0xC0, 0)
                    pitch = 40 if is_kick else TOMS[note]
                    emit(fr, 0x88 + V_DRUM_FM)
                    emit(fr, 0x80 + V_DRUM_FM, pitch, a)
                    # the pitch falls over the first frames
                    for k in range(1, 7):
                        emit(fr + k, 0x98 + V_DRUM_FM, (-k * (20 if is_kick else 8)) & 0xFF)
                    voice_bend[V_DRUM_FM] = None
                elif note in DRUM_MAP:
                    emit(fr, 0x80 + V_NOISE, DRUM_MAP[note], a)
                continue
            if on:
                prog = GM_PATCH.get(chan_prog[ch], 0)
                free = [v for v in range(N_MELODIC) if voice[v] is None]
                same = [v for v in free if voice_last_chan[v] == ch]
                if same:
                    v = same[0]
                elif free:
                    v = free[0]
                else:                                   # steal the oldest note
                    v = min(range(N_MELODIC), key=lambda k: voice[k][2])
                    emit(fr, 0x88 + v)
                set_voice(v, fr, prog, pan_bits(chan_pan[ch]), chan_bend[ch])
                if voice[v] is None:
                    emit(fr, 0x88 + v)                  # (retrigger cleanly)
                voice[v] = (ch, note, fr, vel)
                voice_last_chan[v] = ch
                emit(fr, 0x80 + v, note, att_of(vel, chan_vol[ch]))
            else:
                for v in range(N_MELODIC):
                    if voice[v] and voice[v][0] == ch and voice[v][1] == note:
                        voice[v] = None
                        emit(fr, 0x88 + v)
                        break
    # silence at the end
    for v in range(6):
        emit(end_frame + 1, 0x88 + v)
    stream.sort(key=lambda s: (s[0], s[1]))
    # the song: the loop flag, then the commands with waits (at most 127 ticks each) between them
    body = bytearray([1 if loops else 0])
    now = 0
    for fr, _s, b in stream:
        while fr > now:
            w = min(127, fr - now)
            body.append(w)
            now += w
        body += b
    body.append(0xFF)
    return bytes(body), end_frame / FPS


def fnum_table():
    """one octave from C in 1/32 semitones, as F-numbers at block 4 (C4 = MIDI 60)

    F-number = frequency x 144 x 2^20 / clock / 2^(block - 1). The driver
    picks the entry for the note's position in its octave and changes the
    block for the octave.
    """
    out = []
    for p in range(384):
        f = 440.0 * 2 ** ((60 + p / 32 - 69) / 12)
        out.append(round(f * 144 * 2 ** 20 / CLOCK / 2 ** 3))
    return out


SONGS = [("MUS_TITLE", "t.mid", True), ("MUS_INTRO", "i.mid", False), ("MUS_END", "e.mid", False)]  # (id name, MIDI file in the jar, loops?)


def main():
    """build_music.py <jar dir> <gen dir>: converts the songs and writes
    <gen>/music_data.s (songs, instruments, F-number table) and
    <gen>/music.h (the MUS_* ids and the driver's C entry points).
    """
    assets, gen = Path(sys.argv[1]), Path(sys.argv[2])
    bodies = []
    for name, f, loops in SONGS:
        body, secs = convert(assets / f, loops)
        bodies.append(body)
        print(f"{f}: {len(body)} bytes, {secs:.1f} s")
    write(gen, bodies)


def write(gen, bodies):
    """music_data.s and music.h in gen; bodies: each song's stream, in SONGS
    order (the no-jar build, tools/nojar.py, gives zeros of their sizes)."""
    lines = ["| Generated by tools/build_music.py - do not edit", "    .section .rodata", "    .even",
             "    .global music_songs, music_patches, music_fnum"]
    lines.append("music_songs:")
    for name, _f, _l in SONGS:
        lines.append(f"    .long   song_{name}")
    lines.append("music_patches:")
    for p in PATCHES:
        assert len(p) == 30
        lines.append("    .byte   " + ", ".join(str(b) for b in p))
    lines.append("music_fnum:")
    fn = fnum_table()
    for i in range(0, 384, 16):
        lines.append("    .word   " + ", ".join(str(v) for v in fn[i:i + 16]))
    for (name, _f, _l), body in zip(SONGS, bodies):
        lines.append(f"song_{name}:")
        for i in range(0, len(body), 24):
            lines.append("    .byte   " + ", ".join(str(b) for b in body[i:i + 24]))
    lines.append("    .even")
    (gen / "music_data.s").write_text("\n".join(lines) + "\n")
    hdr = ["/* Generated by tools/build_music.py - do not edit */", "#ifndef MUSIC_H", "#define MUSIC_H"]
    for k, (name, _f, _l) in enumerate(SONGS):
        hdr.append(f"#define {name} {k}")
    hdr += ["void music_play(unsigned char id);", "void music_stop(void);", "#endif"]
    (gen / "music.h").write_text("\n".join(hdr) + "\n")
    print(f"music: {sum(len(b) for b in bodies)} bytes of song data")


if __name__ == "__main__":
    main()
