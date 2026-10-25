#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""Sound effects for the SN76489 PSG ("beeps and bloops").

Writes:
  <gfx>/sfx.inc      the effect data, included into bank 0 by rom/main.asm
  <gen>/sfx.inc      SFX_* ids for the game code
  <gfx>/sfx_preview.wav  (optional, --wav) every effect, rendered by a small
                     PSG model, so they can be listened to without the game

An effect is a list of segments. Each segment lasts some frames (1/60 s) and
drives tone channel 0 (a square wave whose period can sweep) and the noise
channel. In noise mode 3/7 the noise is clocked by tone channel 2, which the
driver keeps silent but sweeps together with channel 0 - that gives the
pitched "whoosh" and explosion sounds.

Segment, 9 bytes (driver: sfx_update in rom/main.asm):
  u8  frames          0 = end of the effect
  u16 period * 16     tone period, 12.4 fixed point (period = 111861 / Hz)
  s16 delta * 16      added every frame
  u8  tone volume     attenuation 0 (loud) .. 15 (off)
  u8  noise control   0-7 (PSG noise register), 255 = no noise
  u8  noise volume    attenuation 0..15
  u8  decay           high nibble: tone gets 2 dB quieter every N frames
                      low nibble: same for the noise (0 = steady)

sfx.inc (bank 0) holds sfx_table, one .dw pointer per effect in id order,
then each effect's segments as .db lines, ended by a 0 byte.

Usage: build_sfx.py <gfx_dir> <gen_dir> [--wav]   (build.sh step 3)
Licence: GPL-2.0-or-later.
"""
import struct
import sys
import wave
from pathlib import Path

CLOCK = 3579545                 # the PSG's input clock in Hz (NTSC Master System)
OFF = 15                        # attenuation 15: the channel is silent
NONE = 255                      # noise control value meaning "no noise in this segment"
# noise control: white (4) / periodic (0) | rate: 0 high, 1 mid, 2 low, 3 = tone 2
WHITE_HI, WHITE_MID, WHITE_LO, WHITE_T2 = 4, 5, 6, 7


def period(hz):
    """The PSG tone period for a frequency in Hz (Hz = CLOCK / 32 / period), clamped to 1..1023."""
    return max(1, min(1023, round(CLOCK / 32 / hz)))


def seg(frames, hz0=None, hz1=None, vol=OFF, noise=NONE, nvol=OFF, tdec=0, ndec=0, p0=None, p1=None):
    """hz0 -> hz1 linear in period over the segment (or raw periods p0/p1).

    Returns the 7-tuple packed by blobs(): (frames, start period * 16,
    per-frame delta * 16, tone volume, noise control, noise volume, decay
    byte). vol/nvol are attenuations (0 loud, 15 off); tdec/ndec the decay
    nibbles. No hz0: the period is 1 (render() treats p <= 1 as a silent tone).
    Asserts the period stays in the PSG's 10-bit range for every frame.
    """
    if p0 is None:
        p0 = period(hz0) if hz0 else 1
    if p1 is None:
        p1 = period(hz1) if hz1 else p0
    start = p0 * 16
    delta = round((p1 - p0) * 16 / frames) if frames > 1 else 0
    end = start + delta * (frames - 1)
    assert 16 <= end <= 1023 * 16 and 16 <= start, (p0, p1, frames)
    return (frames, start, delta, vol, noise, nvol, (tdec << 4) | ndec)


def rest(frames):
    """A silent segment of the given number of frames."""
    return seg(frames)


def note(hz, frames, vol=3, tdec=0):
    """A steady tone at hz for frames, attenuation vol, optional tone decay."""
    return seg(frames, hz, hz, vol, tdec=tdec)


# note frequencies in Hz
C5, D5, E5, F5, G5, A5, B5 = 523, 587, 659, 698, 784, 880, 988
C6, D6, E6, G6, C7 = 1047, 1175, 1319, 1568, 2093
A4, G4 = 440, 392

# id name -> segments.  Order = id (SFX_NONE is 0).
_EFFECTS = [
    ("SFX_NONE", []),
    # ---- player weapons ----
    ("SFX_AXE", [seg(8, noise=WHITE_T2, nvol=4, ndec=2, p0=30, p1=180)]),
    ("SFX_EXTINGUISHER", [seg(16, noise=WHITE_HI, nvol=5, ndec=4)]),
    ("SFX_PISTOL", [seg(7, 260, 70, vol=4, tdec=1, noise=WHITE_MID, nvol=1, ndec=1)]),
    ("SFX_SHOTGUN", [seg(14, 160, 45, vol=3, tdec=2, noise=WHITE_LO, nvol=0, ndec=1)]),
    ("SFX_CHAINGUN", [seg(3, 220, 120, vol=5, noise=WHITE_MID, nvol=1, ndec=2), rest(1)] * 3),
    ("SFX_PLASMA", [seg(9, 1400, 300, vol=2, tdec=2), seg(3, 500, 250, vol=6, tdec=1)]),
    ("SFX_ROCKET", [seg(14, noise=WHITE_T2, nvol=2, ndec=3, p0=60, p1=400, vol=OFF)]),
    ("SFX_BFG", [seg(24, 90, 1400, vol=2, noise=WHITE_LO, nvol=8),
             seg(12, noise=WHITE_T2, nvol=0, ndec=2, p0=200, p1=900)]),
    # ---- monsters ----
    ("SFX_MON_ATTACK", [note(95, 4, 2), note(80, 4, 2), note(110, 5, 3, tdec=2)]),
    ("SFX_FIREBALL", [seg(12, noise=WHITE_T2, nvol=3, ndec=4, p0=40, p1=220)]),
    ("SFX_RAISE", [seg(30, 150, 900, vol=4, tdec=10)]),
    ("SFX_MON_HURT", [seg(7, 240, 110, vol=3, tdec=2, noise=WHITE_MID, nvol=6, ndec=2)]),
    ("SFX_MON_DIE", [seg(20, 320, 55, vol=2, tdec=4, noise=WHITE_LO, nvol=4, ndec=4)]),
    # ---- the player ----
    ("SFX_HURT", [seg(9, 190, 85, vol=1, tdec=2, noise=WHITE_LO, nvol=5, ndec=2)]),
    ("SFX_DODGE", [seg(6, noise=WHITE_T2, nvol=6, ndec=2, p0=20, p1=90)]),
    ("SFX_PLAYER_DIE", [seg(20, 300, 120, vol=1), seg(50, 120, 35, vol=1, tdec=5,
                                                   noise=WHITE_LO, nvol=3, ndec=7)]),
    ("SFX_STEP", [seg(3, noise=WHITE_LO, nvol=9, ndec=1)]),
    ("SFX_BUMP", [seg(6, 90, 55, vol=3, tdec=2, noise=WHITE_LO, nvol=7, ndec=2)]),
    # ---- items ----
    ("SFX_PICKUP", [note(E5, 4, 3), note(A5, 7, 3, tdec=2)]),
    ("SFX_CREDITS", [note(B5, 3, 3), note(E6, 10, 3, tdec=2)]),
    ("SFX_WEAPON", [note(C5, 4), note(E5, 4), note(G5, 4), note(C6, 10, tdec=2)]),
    ("SFX_KEY", [note(G5, 5, 2), note(C6, 5, 2), note(E6, 14, 2, tdec=3)]),
    ("SFX_HEAL", [seg(12, 300, 1000, vol=3, tdec=4)]),
    # ---- the world ----
    ("SFX_DOOR", [seg(16, 55, 50, vol=4, tdec=4, noise=WHITE_LO, nvol=4, ndec=4)]),
    ("SFX_DENIED", [note(110, 6, 2), rest(3), note(98, 9, 2, tdec=3)]),
    ("SFX_EXPLODE", [seg(4, noise=WHITE_HI, nvol=0),
                 seg(34, noise=WHITE_T2, nvol=0, ndec=5, p0=60, p1=900)]),
    ("SFX_BREAK", [seg(6, 130, 70, vol=4, tdec=2, noise=WHITE_MID, nvol=2, ndec=2)]),
    ("SFX_LEVEL_UP", [note(C5, 3), note(E5, 3), note(G5, 3), note(C6, 3), note(E6, 3),
                  note(G6, 3), note(C7, 16, 3, tdec=3)]),
    # ---- menus ----
    ("SFX_BLIP", [note(1000, 2, 5)]),
    ("SFX_SELECT", [note(660, 3, 4), note(990, 5, 4, tdec=2)]),
    ("SFX_DIALOG", [note(A4, 3, 4), note(A5, 6, 4, tdec=2)]),
    ("SFX_SWITCH", [seg(2, 1500, 1500, vol=6, noise=WHITE_HI, nvol=6), seg(2, 700, 700, vol=7)]),
    ("SFX_COMPLETE", [note(C5, 6, 2), note(E5, 6, 2), note(G5, 6, 2), note(C6, 8, 2), rest(2),
                  note(G5, 5, 2), note(C6, 30, 2, tdec=5)]),
]
# (the table above names each effect by its id, SFX_<name>; here as <name>)
EFFECTS = [(name[4:], segs) for name, segs in _EFFECTS]


def blobs():
    """[(name, bytes)]: each effect's segments packed 9 bytes each, plus the 0 end byte."""
    out = []
    for name, segs in EFFECTS:
        b = bytearray()
        for s in segs:
            b += struct.pack("<BHhBBBB", *s)
        b.append(0)
        out.append((name, bytes(b)))
    return out


def write_inc(path):
    """Write the driver's data file: sfx_table (pointers) and every effect's bytes."""
    lines = ["; generated by tools/build_sfx.py - do not edit", "sfx_table:"]
    for name, _ in EFFECTS:
        lines.append("    .dw sfx_%s" % name.lower())
    lines.append("sfx_table_end:")
    for name, b in blobs():
        lines.append("sfx_%s:" % name.lower())
        for k in range(0, len(b), 16):
            lines.append("    .db " + ", ".join("$%02X" % x for x in b[k:k + 16]))
    Path(path).write_text("\n".join(lines) + "\n")


def write_ids(path):
    """Write the effect ids for the game code (game/*.asm): SFX_<NAME> and SFX_COUNT."""
    lines = ["; generated by tools/build_sfx.py - do not edit"]
    for i, (name, _) in enumerate(EFFECTS):
        lines.append(".define SFX_%-14s %d" % (name, i))
    lines.append(".define SFX_COUNT %d" % len(EFFECTS))
    Path(path).write_text("\n".join(lines) + "\n")


# ---- a small model of the driver + PSG, for the preview .wav ----
RATE = 44100                    # preview sample rate (Hz)


def render(segs):
    """Mirrors sfx_update frame by frame.

    segs is a list of seg() tuples. Returns float samples (-1..1 range,
    mixed at 1/4 volume) at RATE: a square wave for tone channel 0 plus the
    noise channel's 15-bit LFSR output, then one silent frame.
    """
    samples = []
    vol_table = [10 ** (-2 * a / 20) if a < 15 else 0.0 for a in range(16)]   # 2 dB per step
    tone_phase = 0.0
    lfsr = 0x4000                      # the noise shift register's reset value
    noise_acc = 0.0
    noise_out = 1
    for (frames, per, dlt, tatt, nctl, natt, dec) in segs:
        tdr, ndr = dec >> 4, dec & 15
        tdc, ndc = tdr, ndr
        if nctl != NONE:
            lfsr = 0x4000              # writing the noise register resets the LFSR
        for _ in range(frames):
            p = per >> 4
            tone_hz = CLOCK / 32 / max(p, 1)
            if nctl == NONE:
                n_hz = 0
            elif nctl & 3 == 3:        # rate 3: clocked by tone 2, which follows channel 0
                n_hz = CLOCK / 32 / max(p, 1)
            else:                      # fixed rates: clock / 512, / 1024, / 2048
                n_hz = CLOCK / 32 / (16 << (nctl & 3))
            tv = vol_table[tatt] if p > 1 else 0
            nv = vol_table[natt] if nctl != NONE else 0
            for _ in range(RATE // 60):    # the samples of one 1/60 s frame
                tone_phase = (tone_phase + tone_hz / RATE) % 1.0
                t = 1 if tone_phase < 0.5 else -1
                if n_hz:
                    noise_acc += n_hz / RATE
                    while noise_acc >= 1:
                        noise_acc -= 1
                        # white noise (bit 2 set): feedback from bits 0 xor 1;
                        # periodic noise: bit 0 alone
                        bit = (lfsr ^ (lfsr >> 1)) & 1 if nctl & 4 else lfsr & 1
                        lfsr = (lfsr >> 1) | (bit << 14)
                        noise_out = 1 if lfsr & 1 else -1
                samples.append(0.25 * (t * tv + noise_out * nv))
            per += dlt                 # the period sweep
            # decay: every tdr (ndr) frames the attenuation goes up by one
            if tdr:
                tdc -= 1
                if tdc == 0:
                    tdc = tdr
                    tatt = min(15, tatt + 1)
            if ndr:
                ndc -= 1
                if ndc == 0:
                    ndc = ndr
                    natt = min(15, natt + 1)
    samples += [0.0] * (RATE // 60)       # the driver's silent frame between effects
    return samples


def write_wav(path):
    """Write every effect (but NONE) to a mono 16-bit WAV, 1/3 s of silence apart."""
    data = []
    for name, segs in EFFECTS[1:]:
        data += render(segs) + [0.0] * (RATE // 3)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1, min(1, s)) * 30000)) for s in data))


def main():
    """Write both include files (and the preview with --wav) and print the size."""
    gfx, gen = Path(sys.argv[1]), Path(sys.argv[2])
    write_inc(gfx / "sfx.inc")
    write_ids(gen / "sfx.inc")
    size = sum(len(b) for _, b in blobs()) + 2 * len(EFFECTS)   # data + the pointer table
    print("sound effects: %d, %d bytes" % (len(EFFECTS) - 1, size))
    if "--wav" in sys.argv:
        write_wav(gfx / "sfx_preview.wav")
        print("preview:", gfx / "sfx_preview.wav")


if __name__ == "__main__":
    main()
