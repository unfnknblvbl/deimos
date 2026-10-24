#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Sound effects for the Mega Drive game: the original game's sound clips, as a Mega
Drive game would play them - on the YM2612 (FM voices 4 and 5) and the
PSG's noise channel - in the music's stream format (tools/build_music.py,
played by src/music.s).

The phone game (J2ME) has fifteen sound clips, tiny General MIDI files for
the phone's synthesiser (gunshot, distortion guitar, synth drum, timpani...).
Its code plays them as follows (read from the game's classes):
  0.mid .. 8.mid   the weapons' attacks: axe, fire extinguisher, pistol,
                   shotgun, chaingun, super shotgun, plasma gun, rocket
                   launcher, BFG (7.mid also for every explosion)
  9, 10, 11.mid    the dog's bite (the Dog Collar)
  n.mid            the player's death
  p.mid            an item used (medkit, Soul Sphere, Berserker, collar), a
                   secret found
  m.mid            the menus
Those are turned into FM here note for note: each General MIDI instrument
gets an FM instrument (the gunshot is the PSG's noise crack over an FM
body falling in pitch, the synth drum and timpani fall in pitch as they
do on a synthesiser). The phone game is silent for doors, pick-ups,
monsters and the like; the Mega Drive game keeps sounds for those, written here in the
same instruments so they sound of a piece.

Writes:
  <gen>/sfx_data.c   one stream per effect, and sfx_table
  <gen>/sfx.h        SFX_* ids for the C code;  <gen>/sfx.inc  for the assembler

Usage: build_sfx.py <jar dir> <gen dir>
Licence: GPL-2.0-or-later.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_music import parse_midi, att_of, FPS  # noqa: E402

V_FM = (4, 5)                       # the effects' FM voices
V_NOISE = 6  # the stream's 'voice' for the PSG noise channel
# instruments (indices into music_patches, tools/build_music.py PATCHES)
P_GUITAR, P_KICK, P_GUN, P_LEAD, P_TIMPANI, P_SYNDRUM = 0, 4, 6, 7, 8, 9
# PSG noise types (src/music.s noise_drums)
N_SNARE, N_HAT, N_OPEN, N_CRASH, N_SHOT, N_BLAST, N_HISS, N_TICK = range(8)

# General MIDI program -> FM instrument for the clips
GM = {30: P_GUITAR, 31: P_GUITAR, 29: P_GUITAR, 87: P_LEAD, 47: P_TIMPANI, 118: P_SYNDRUM}  # (0-based GM programs: 29/30/31 guitars, 87 bass+lead, 47 timpani, 118 synth drum)
FALLING = {P_TIMPANI: (2, 2), P_SYNDRUM: (1, 1)}       # semitones down per step, frames per step
DRUMS = {35: "kick", 36: "kick", 37: N_SNARE, 38: N_SNARE, 69: None, 70: N_TICK, 42: N_HAT}  # GM drum note -> noise type, 'kick' or None (left out)
# (69 cabasa: the extinguisher's rattle - left out, the PSG has one noise and the hiss needs it)


# ------------------------------------------------------------------ notes
class Note:
    """one sound: start/end in frames; kind 'fm' (instrument, MIDI note),
    'kick', 'gun' (the gunshot), or 'noise' (a PSG noise type)"""

    def __init__(self, start, end, kind, inst=None, note=60, vel=127):
        """start, end: frames (end may be None until the note-off is found);
        kind: 'fm', 'kick', 'gun' or 'noise'; inst: the FM instrument (for
        'fm') or the noise type (for 'noise'); note: MIDI note; vel: velocity.
        """
        self.start, self.end, self.kind, self.inst, self.note, self.vel = start, end, kind, inst, note, vel


def from_midi(path):
    """a clip -> [Note]

    Reads the MIDI file at `path`: drum-channel notes become kicks or noise
    hits, program 127 (gunshot) a 'gun' note, program 122 (seashore) a long
    hiss, others FM notes (held until their note-off; falling drums at most
    30 frames). Every note lasts at least 2 frames.
    """
    div, events = parse_midi(path)
    tempo = [(0, 500000)] + [(e[0], int.from_bytes(e[4], "big")) for e in events
                              if e[2] == "meta" and e[3] == 0x51]

    def frame(tick):
        """MIDI tick -> 1/60 s frame (with the clip's tempo changes)."""
        sec, last_t, last_tp = 0.0, 0, tempo[0][1]
        for tt, tp in tempo[1:]:
            if tt >= tick:
                break
            sec += (tt - last_t) * last_tp / div / 1e6
            last_t, last_tp = tt, tp
        return round((sec + (tick - last_t) * last_tp / div / 1e6) * FPS)

    prog = {c: 0 for c in range(16)}
    held, out = {}, []
    for e in events:
        if e[2] == "meta":
            continue
        tick, kind, ch, data = e[0], e[2], e[3], e[4]
        fr = frame(tick)
        if kind == 0xC0:
            prog[ch] = data[0]
        elif kind in (0x80, 0x90):
            on = kind == 0x90 and data[1] > 0
            key = (ch, data[0])
            if on:
                if ch == 9:
                    d = DRUMS.get(data[0], N_SNARE)
                    if d is None:
                        continue
                    out.append(Note(fr, fr + 8, "kick") if d == "kick" else Note(fr, fr + 8, "noise", d))
                elif prog[ch] == 127:
                    out.append(Note(fr, fr + 24, "gun", note=data[0], vel=data[1]))
                elif prog[ch] == 122:                   # seashore: a long hiss
                    out.append(Note(fr, fr + 60, "noise", N_HISS, vel=data[1]))
                else:
                    n = Note(fr, None, "fm", GM.get(prog[ch], P_GUITAR), data[0], data[1])
                    n.cap = 30 if n.inst in FALLING else None    # (a drum's ring, not held)
                    held[key] = n
                    out.append(n)
            elif key in held:
                held.pop(key).end = fr
    end = max([frame(e[0]) for e in events] + [0])
    for n in out:
        if n.end is None:
            n.end = end
        if getattr(n, "cap", None):
            n.end = min(n.end, n.start + n.cap)
        n.end = max(n.end, n.start + 2)
    return out


def fm(start, frames, inst, note, vel=127):
    """An FM note of instrument inst starting at `start`, lasting `frames`."""
    return Note(start, start + frames, "fm", inst, note, vel)


def noise(start, typ, vel=127):
    """A PSG noise hit of type `typ` at `start` (8 frames)."""
    return Note(start, start + 8, "noise", typ, vel=vel)


def seq(start, step, inst, notes, frames=None, vel=127):
    """notes one after another, every `step` frames

    Each lasts `frames` (default `step`). Returns a list of FM Notes.
    """
    return [fm(start + k * step, frames or step, inst, n, vel) for k, n in enumerate(notes)]


# ------------------------------------------------------------------ the stream
def stream(notes):
    """[Note] -> the driver's stream: FM voices 4-5 (the newest note takes
    the voice that is free, or else the older one), noise on voice 6

    Returns the stream bytes (header 0: no loop). Besides build_music's
    commands it uses $B0+v n (a new pitch without a new attack, for the
    slides of the gunshot and the falling drums). The noise volume is the
    attenuation in 2 dB PSG steps (0..15).
    """
    ev = []                        # (frame, order, bytes)
    k = [0]

    def emit(fr, *b):
        """Queues command bytes b at frame fr, keeping the order of commands within a frame."""
        ev.append((fr, k[0], bytes(b)))
        k[0] += 1

    for v in V_FM:
        emit(0, 0xA8 + v, 0xC0)     # both speakers
    # which voice each note gets; a note ends early when the next one on its voice starts
    busy = {v: None for v in V_FM}
    on_voice = {v: [] for v in V_FM}
    for n in sorted(notes, key=lambda n: n.start):
        if n.kind == "noise":
            emit(n.start, 0x80 + V_NOISE, n.inst, min(15, round(att_of(n.vel, 127) / 2.67)))
            continue
        free = [v for v in V_FM if busy[v] is None or busy[v].end <= n.start]
        v = free[0] if free else min(V_FM, key=lambda v: busy[v].start)
        busy[v] = n
        on_voice[v].append(n)
    for v in V_FM:
        patch = None
        lst = on_voice[v]
        for i, n in enumerate(lst):
            stop = min(n.end, lst[i + 1].start) if i + 1 < len(lst) else n.end
            a = att_of(n.vel, 127)
            inst = {"kick": P_KICK, "gun": P_GUN}.get(n.kind, n.inst)
            if patch != inst:
                emit(n.start, 0x90 + v, inst)
                patch = inst
            if n.kind == "gun":
                emit(n.start, 0x80 + V_NOISE, N_SHOT if n.note >= 50 else N_BLAST, 0)  # the crack on the noise channel
                pitch = n.note - 12  # the FM body an octave down, then sliding 2 semitones a frame
                emit(n.start, 0x98 + v, 0)
                emit(n.start, 0x80 + v, pitch, a)
                slide = [(n.start + k, 0xB0 + v, pitch - k * 2) for k in range(1, 10)]
            elif n.kind == "kick":
                emit(n.start, 0x98 + v, 0)
                emit(n.start, 0x80 + v, 40, a)
                slide = [(n.start + k, 0x98 + v, (-k * 20) & 0xFF) for k in range(1, 7)]  # pitch bend down 20/32 semitone per frame
            else:
                emit(n.start, 0x98 + v, 0)
                emit(n.start, 0x80 + v, n.note, a)
                slide = []
                if n.inst in FALLING:
                    semis, every = FALLING[n.inst]
                    slide = [(n.start + k * every, 0xB0 + v, max(24, n.note - k * semis))
                             for k in range(1, min(12 // semis, (n.end - n.start) // every) + 1)]
            for fr, c, x in slide:
                if fr < stop:
                    emit(fr, c, x)
            if i + 1 == len(lst) or stop < lst[i + 1].start:  # key off, unless the next note on this voice starts right away
                emit(stop, 0x88 + v)
    end = max([n.end for n in notes] + [0])
    ev.sort(key=lambda e: (e[0], e[1]))
    body = bytearray([0])           # (does not loop)
    now = 0
    for fr, _k, b in ev:
        while fr > now:
            w = min(127, fr - now)
            body.append(w)
            now += w
        body += b
    if end > now:  # wait until the last note's end before $FF
        body.append(min(127, end - now))
    body.append(0xFF)
    return bytes(body)


# ------------------------------------------------------------------ the effects
def effects(jar):
    """The effects, in SFX_* id order: [(name, [Note])]. Index 0 is "no
    effect". The original's clips come from jar/<n>.mid; the others are
    written here with the same instruments.
    """
    clip = lambda f: from_midi(jar / f)  # noqa: E731
    G, L, T, D = P_GUITAR, P_LEAD, P_TIMPANI, P_SYNDRUM
    return [
        ("NONE", []),
        # ---- the player's weapons: the original clips
        ("AXE", clip("0.mid")),
        ("EXTINGUISHER", clip("1.mid")),
        ("PISTOL", clip("2.mid")),
        ("SHOTGUN", clip("3.mid")),
        ("CHAINGUN", clip("4.mid")),
        ("PLASMA", clip("6.mid")),
        ("ROCKET", clip("7.mid")),
        ("BFG", clip("8.mid")),
        # ---- monsters (the phone game is silent here)
        ("MON_ATTACK", [fm(0, 9, G, 40, 120), fm(0, 9, G, 47, 110)]),
        ("FIREBALL", [noise(0, N_HISS, 110), fm(0, 18, D, 64, 120)]),
        ("RAISE", seq(0, 4, G, [36, 39, 42, 45, 48, 51], vel=110)),
        ("MON_HURT", [fm(0, 6, G, 45, 115), noise(0, N_TICK, 100)]),
        ("MON_DIE", seq(0, 5, G, [42, 41, 40, 39, 38, 37], vel=115)),
        # ---- the player
        ("HURT", [Note(0, 6, "kick", vel=127), fm(0, 10, D, 52, 115)]),
        ("DODGE", [noise(0, N_OPEN, 100)]),
        ("PLAYER_DIE", clip("n.mid")),
        ("STEP", [noise(0, N_TICK, 70)]),
        ("BUMP", [Note(0, 6, "kick", vel=100)]),
        # ---- items
        ("PICKUP", seq(0, 4, L, [76, 83], vel=110)),
        ("CREDITS", seq(0, 3, L, [84, 91], vel=105)),
        ("WEAPON", seq(0, 4, L, [60, 64, 67, 72], frames=6, vel=115)),
        ("KEY", seq(0, 5, L, [67, 72, 76], frames=8, vel=115)),
        ("HEAL", clip("p.mid")),
        # ---- the world
        ("DOOR", [fm(0, 16, T, 40, 120), noise(0, N_HISS, 90)]),
        ("DENIED", seq(0, 6, L, [50, 47], frames=7, vel=115)),
        ("EXPLODE", clip("7.mid")),
        ("BREAK", [noise(0, N_SNARE, 120), fm(0, 10, D, 48, 120)]),
        ("LEVEL_UP", seq(0, 3, L, [60, 64, 67, 72, 76, 79], frames=4, vel=115) + [fm(18, 20, L, 84, 115)]),
        # ---- menus
        ("BLIP", clip("m.mid")),
        ("SELECT", seq(0, 3, L, [72, 79], frames=4, vel=100)),
        ("DIALOG", seq(0, 3, L, [69, 81], frames=4, vel=90)),
        ("SWITCH", [noise(0, N_TICK, 120), fm(0, 5, D, 72, 110)]),
        ("COMPLETE", []),          # (the sector's music plays)
        # ---- added for the original's sounds
        ("DOG", clip("9.mid")),
        ("SECRET", clip("p.mid")),
        ("ITEM", clip("p.mid")),
    ]


def main():
    """build_sfx.py <jar dir> <gen dir>: builds every effect's stream and
    writes <gen>/sfx_data.c (the streams and sfx_table), sfx.h and sfx.inc
    (the SFX_* ids for C and for the assembler).
    """
    jar, gen = Path(sys.argv[1]), Path(sys.argv[2])
    eff = [(name, stream(notes)) for name, notes in effects(jar)]
    c = ["/* generated by tools/build_sfx.py - do not edit */"]
    for name, b in eff:
        c.append("static const unsigned char sfx_%s[] = {%s};" % (name.lower(), ",".join(str(x) for x in b)))
    c.append("const unsigned char * const sfx_table[] = {")
    c.append(",\n".join("    sfx_%s" % name.lower() for name, _ in eff))
    c.append("};")
    (gen / "sfx_data.c").write_text("\n".join(c) + "\n")
    h = ["/* generated by tools/build_sfx.py - do not edit */", "#ifndef SFX_H", "#define SFX_H"]
    h += ["#define SFX_%s %d" % (name, i) for i, (name, _) in enumerate(eff)]
    h += ["#define SFX_COUNT %d" % len(eff), "#endif"]
    (gen / "sfx.h").write_text("\n".join(h) + "\n")
    inc = ["| generated by tools/build_sfx.py - do not edit"]
    inc += ["    .equ    SFX_%s, %d" % (name, i) for i, (name, _) in enumerate(eff)]
    inc += ["    .equ    SFX_COUNT, %d" % len(eff)]
    (gen / "sfx.inc").write_text("\n".join(inc) + "\n")
    print("sound effects: %d, %d bytes" % (len(eff) - 1, sum(len(b) for _, b in eff)))


if __name__ == "__main__":
    main()
