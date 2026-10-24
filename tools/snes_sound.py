#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The SNES's sound: the Mega Drive game's music and sound effects (the same streams,
tools/build_music.py's format) played by a driver on the SPC700
(snes/spc/driver.s), with the Mega Drive game's FM instruments turned into samples:
each instrument's steady tone is worked out here (the YM2612's operators,
algorithm and feedback, one period of the wave) and stored as a looped
BRR sample; the envelope becomes the DSP's ADSR, the release a GAIN fall.
The noise drums (the PSG's noise channel) are the DSP's noise.

  engine (spc_engine.bin): the driver and its tables, made from the Mega Drive game's
      own data (no part of the original game):
        $0200 the driver, $0E00 the pitch table (384 words: the top octave
        in 1/32 semitones), $1100 the volume table (128 bytes, 0.75 dB a
        step), $1180 the instruments (8 bytes each: sample, ADSR1, ADSR2,
        base attenuation, release GAIN, 0, 0, 0), $11D0 the noise drums
        (4 bytes: noise clock, attenuation, ADSR1, ADSR2), $1200 the
        sample directory, $1300 the samples (each one period, or two
        when the instrument has a half-frequency operator: 1000 Hz at
        pitch $1000 either way)
  streams (spc_streams.bin, from the original game's tunes and clips, so
      the resources' part): for ARAM $2000: u16 the three songs, u16 the
      number of effects, u16 each effect (ARAM addresses), then the streams

Usage: snes_sound.py <deimos dir> <out dir>       (writes both)

Background: the Mega Drive game's instruments are YM2612 FM patches (4 operators,
an algorithm wiring them, feedback on operator 1); the SNES's DSP plays
sampled sounds in BRR (9-byte blocks of 16 4-bit samples with a shift and
a prediction filter). gb_sound.py reuses ops_of, CARRIERS, base_att and
patches_of for the Game Boy Color's tables. Environment: WLA_SPC700,
WLALINK (the assembler for driver.s), SPC_TEST (a test tune).
Licence: GPL-2.0-or-later.
"""
import math
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

WLA = os.environ.get("WLA_SPC700", "wla-spc700")
WLALINK = os.environ.get("WLALINK", "wlalink")
DRIVER = Path(__file__).resolve().parent.parent / "snes" / "spc" / "driver.s"

# the ARAM addresses of the engine's parts (see the docstring)
PITCH_AT, VOL_AT, INST_AT, NOISE_AT, DIR_AT, SAMPLES_AT, STREAMS_AT = \
    0x0E00, 0x1100, 0x1180, 0x11D0, 0x1200, 0x1300, 0x2000
TOP_NOTE = 96                       # the pitch table's octave: notes 96..107


# ------------------------------------------------------------------ FM
def ops_of(patch):
    """the Mega Drive game's 30-byte instrument -> operators OP1..OP4 as dicts, alg, fb"""
    regs = [patch[i * 7:(i + 1) * 7] for i in range(4)]          # register order S1, S3, S2, S4
    by_op = {1: regs[0], 3: regs[1], 2: regs[2], 4: regs[3]}
    out = {}
    for k, r in by_op.items():
        out[k] = {"mul": r[0] & 15, "dt": r[0] >> 4, "tl": r[1], "ar": r[2] & 31, "d1r": r[3] & 31,
                  "d2r": r[4] & 31, "sl": r[5] >> 4, "rr": r[5] & 15}
    return out, patch[28] & 7, patch[28] >> 3


# per algorithm: the operators that are carriers (heard), the rest modulate
CARRIERS = {0: (4,), 1: (4,), 2: (4,), 3: (4,), 4: (2, 4), 5: (2, 3, 4), 6: (2, 3, 4), 7: (1, 2, 3, 4)}


def render_period(patch, n=2048):
    """one period of the instrument's steady tone (all envelopes at their
    sustain level), n points"""
    ops, alg, fb = ops_of(patch)
    mul = {k: (0.5 if o["mul"] == 0 else o["mul"]) for k, o in ops.items()}
    halves = any(m == 0.5 for m in mul.values())
    periods = 2 if halves else 1
    N = n * periods

    def amp(o):
        """an operator's steady amplitude (1.0 = full)"""
        # sustain: TL plus the sustain level (3 dB a step) when it decays to it
        att = o["tl"] * 0.75 + (o["sl"] * 3 if o["d1r"] else 0)
        return 10 ** (-att / 20)

    a = {k: amp(o) for k, o in ops.items()}
    t = np.arange(N) / n * 2 * math.pi              # fundamental phase
    # (the modulation formulas below are an approximation of the YM2612's)
    beta = 0.0 if fb == 0 else math.pi / 16 * (2 ** (fb - 1)) * 2   # modulation index of the feedback
    # op1 with feedback: iterate over a few periods to settle
    o1 = np.zeros(N)
    prev = [0.0, 0.0]
    for _settle in range(3):
        for i in range(N):
            fbv = beta * (prev[0] + prev[1]) / 2
            v = a[1] * math.sin(mul[1] * t[i] + fbv)
            prev = [v / max(a[1], 1e-9), prev[0]]
            o1[i] = v
    MI = 4 * math.pi                                # a full-scale modulator's index (~2pi .. 8pi)

    def osc(k, mod):
        """operator k's output, phase-modulated by mod"""
        return a[k] * np.sin(mul[k] * t + MI * mod)

    # the eight algorithms' wirings
    if alg == 0:
        o2 = osc(2, o1); o3 = osc(3, o2); out = osc(4, o3)
    elif alg == 1:
        o2 = osc(2, 0); o3 = osc(3, o1 + o2); out = osc(4, o3)
    elif alg == 2:
        o2 = osc(2, 0); o3 = osc(3, o2); out = osc(4, o1 + o3)
    elif alg == 3:
        o2 = osc(2, o1); o3 = osc(3, 0); out = osc(4, o2 + o3)
    elif alg == 4:
        out = osc(2, o1) + osc(4, osc(3, 0))
    elif alg == 5:
        out = osc(2, o1) + osc(3, o1) + osc(4, o1)
    elif alg == 6:
        out = osc(2, o1) + osc(3, 0) + osc(4, 0)
    else:
        out = o1 + osc(2, 0) + osc(3, 0) + osc(4, 0)
    return out, periods


def band_limit(wave, points, harmonics):
    """resample one (or two) periods to `points`, keeping the low harmonics"""
    f = np.fft.rfft(wave)
    keep = np.zeros(points // 2 + 1, complex)
    k = min(harmonics + 1, len(keep), len(f))
    keep[:k] = f[:k] * (points / len(wave))
    keep[0] = 0
    return np.fft.irfft(keep, points)


# ------------------------------------------------------------------ BRR
def _dec(nib, shift, flt, p1, p2):
    """the DSP's BRR decoding of one nibble (16-bit result, kept in 15 bits)"""
    v = nib - 16 if nib > 7 else nib
    x = (v << shift) >> 1
    if flt == 1:
        x += p1 + ((-p1) >> 4)
    elif flt == 2:
        x += 2 * p1 + ((-3 * p1) >> 5) - p2 + (p2 >> 4)
    elif flt == 3:
        x += 2 * p1 + ((-13 * p1) >> 6) - p2 + ((3 * p2) >> 4)
    x = max(-32768, min(32767, x))
    x = ((x << 1) & 0xFFFF)
    x = x - 0x10000 if x & 0x8000 else x
    return x >> 1                                       # (15 bits, as the DSP keeps it)


def _block(blk, p1, p2, filters=range(4)):
    """the best shift and filter for 16 samples after history p1, p2"""
    best = None
    for flt in filters:
        for shift in range(1, 13):
            q1, q2, err, nibs = p1, p2, 0, []
            for v in blk:
                pred = _dec(0, shift, flt, q1, q2)          # (the prediction alone)
                step = 1 << (shift - 1)
                n = max(-8, min(7, int(round((v - pred) / step))))
                x = _dec(n & 15, shift, flt, q1, q2)
                err += (x - v) ** 2
                nibs.append(n & 15)
                q2, q1 = q1, x
                if best is not None and err >= best[0]:
                    break
            else:
                if best is None or err < best[0]:
                    best = (err, shift, flt, nibs, q1, q2)
    return best


def brr(samples):
    """samples (|v| < 16384, a multiple of 16, one loop) -> looped BRR. The
    loop's first block uses no prediction (filter 0), so it decodes the
    same whatever came before; the others predict from the samples before"""
    s = [int(v) for v in np.clip(np.round(samples), -16000, 16000)]
    nb = len(s) // 16
    out = bytearray()
    q1 = q2 = 0
    for b in range(nb):
        _err, shift, flt, nibs, q1, q2 = _block(s[b * 16:(b + 1) * 16], q1, q2, (0,) if b == 0 else range(4))
        out.append(shift << 4 | flt << 2 | (3 if b == nb - 1 else 0))
        for i in range(0, 16, 2):
            out.append(nibs[i] << 4 | nibs[i + 1])
    return bytes(out)


# ------------------------------------------------------------------ envelopes
def adsr_of(patch):
    """the carriers' envelope -> DSP ADSR1, ADSR2, release GAIN"""
    ops, alg, _fb = ops_of(patch)
    car = [ops[k] for k in CARRIERS[alg] if ops[k]["tl"] < 100]
    o = min(car, key=lambda c: c["tl"])
    ar = 15 if o["ar"] >= 28 else max(8, o["ar"] // 2)
    # D1R (0..31, YM) -> DR (0..7); the YM's decay is ~0.4 s at rate 10, fast above 20
    dr = max(0, min(7, (o["d1r"] - 2) // 3)) if o["d1r"] else 0
    sl = 7 if not o["d1r"] else max(0, min(7, 7 - (o["sl"] * 3) // 4))
    sr = 0 if not o["d2r"] else max(1, min(31, o["d2r"] + 6))
    adsr1 = 0x80 | (dr << 4) | ar
    adsr2 = (sl << 5) | sr
    # release: GAIN exponential decrease, rate from RR (0..15)
    rel = 0xA0 | max(8, min(31, o["rr"] * 2 + 1))
    return adsr1, adsr2, rel


def base_att(patch):
    """the patch's loudness: the smallest carrier total level (0.75 dB steps)"""
    ops, alg, _fb = ops_of(patch)
    return min(ops[k]["tl"] for k in CARRIERS[alg])


# ------------------------------------------------------------------ tables
def pitch_table():
    """the DSP pitch of each 1/32 semitone of the top octave (notes 96..107);
    the driver halves it for each octave down"""
    out = []
    for i in range(384):
        f = 440 * 2 ** ((TOP_NOTE + i / 32 - 69) / 12)
        out.append(min(0x3FFF, round(4096 * f / 1000)))          # 32-sample periods: 1000 Hz at $1000
    return out


def vol_table():
    """attenuation (0.75 dB steps) -> DSP volume 0..127"""
    return [max(0, min(127, round(127 * 10 ** (-0.75 * a / 20)))) for a in range(128)]


# PSG noise types (megadrive/src/music.s noise_drums): noise clock, first
# attenuation (PSG steps, 2 dB), frames per step, steps -> ADSR
NOISE = [(0x1B, 1, 1, 1), (0x1E, 3, 1, 3), (0x1D, 4, 2, 1), (0x1C, 2, 4, 1),
         (0x1A, 0, 1, 1), (0x12, 0, 2, 1), (0x1D, 1, 5, 1), (0x1F, 3, 1, 3)]


def noise_table():
    """per noise drum, 4 bytes: noise clock, attenuation, ADSR1 ($FF: attack
    at once, fastest decay), ADSR2 (sustain level, sustain rate)"""
    out = bytearray()
    for clk, att, rate, inc in NOISE:
        # silent after (15 - att) / inc steps of `rate` frames
        frames = max(1, (15 - att) // inc * rate)
        # SR: exponential decay; ~ frames / 60 s to fall away
        sr = max(10, min(31, round(31 - math.log2(max(frames, 1)) * 3)))
        sl = 0 if frames < 12 else 2
        out += bytes([clk, att, 0xFF, (sl << 5) | sr])
    return bytes(out)


# ------------------------------------------------------------------ the streams
def c_arrays(text):
    """the 'char name[] = {..};' arrays of a C file -> {name: bytes}"""
    out = {}
    for m in re.finditer(r'char (\w+)\[\]\s*=\s*\{([^}]*)\}', text):
        vals = [int(v, 0) for v in m.group(2).replace("\n", "").split(",") if v.strip()]
        out[m.group(1)] = bytes(vals)
    return out


def songs_of(asm):
    """the songs of megadrive/tools/music_data.s: each 'song_NAME:' label's .byte
    lines -> {NAME: bytes}"""
    out, cur = {}, None
    for line in asm.splitlines():
        m = re.match(r"song_(\w+):", line)
        if m:
            cur = m.group(1)
            out[cur] = bytearray()
            continue
        line = line.strip()
        if cur and line.startswith(".byte"):
            out[cur] += bytes(int(v) for v in line[5:].split(","))
        elif cur and line:
            cur = None
    return out


def patches_of(asm):
    """the instruments: the .byte rows after 'music_patches:' (30 bytes each)"""
    rows = []
    take = False
    for line in asm.splitlines():
        if line.startswith("music_patches:"):
            take = True
            continue
        if take:
            if line.strip().startswith(".byte"):
                rows.append(bytes(int(v) for v in line.strip()[5:].split(",")))
            else:
                break
    return rows


def sfx_order(c_text, h_text):
    """the effects' streams in SFX id order (sfx_table in sfx_data.c)"""
    arrays = c_arrays(c_text)
    tab = re.search(r'sfx_table\[\]\s*=\s*\{([^}]*)\}', c_text).group(1)
    names = [n.strip() for n in tab.split(",") if n.strip()]
    return [arrays.get(n, b"\x00\xff") if n not in ("0", "NULL") else b"\x00\xff" for n in names]


def streams(md_gen):
    """spc_streams.bin: the header (ARAM addresses, little-endian) and the
    three songs' and the effects' streams, for ARAM $2000"""
    asm = (md_gen / "music_data.s").read_text()
    songs = songs_of(asm)
    order = ["MUS_TITLE", "MUS_INTRO", "MUS_END"]
    import os
    if os.environ.get("SPC_TEST"):        # (a test tune: one note a second, each instrument)
        t = bytearray([1])
        for k in range(10):
            t += bytes([0x90, k, 0x80, 60, 0, 60, 0x88, 30])
        t += bytes([0xFF])
        songs["MUS_TITLE"] = t
    # (a missing effect becomes the stream 00 FF: nothing, then the end)
    effects = sfx_order((md_gen / "sfx_data.c").read_text(), (md_gen / "sfx.h").read_text())
    head = 6 + 2 + 2 * len(effects)
    body = bytearray()
    addrs = []
    for s in [songs[n] for n in order] + effects:
        addrs.append(STREAMS_AT + head + len(body))
        body += s
    out = struct.pack("<3HH", *addrs[:3], len(effects)) + struct.pack(f"<{len(effects)}H", *addrs[3:]) + body
    if STREAMS_AT + len(out) > 0xFF00:
        raise SystemExit("the streams don't fit the SPC's memory")
    return out


# ------------------------------------------------------------------ the engine
def engine(md_gen):
    """spc_engine.bin: the driver assembled with WLA-DX, then the tables and
    one BRR sample per instrument, from ARAM $0200"""
    patches = patches_of((md_gen / "music_data.s").read_text())
    img = bytearray(STREAMS_AT)
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        subprocess.run([WLA, "-o", str(d / "driver.o"), str(DRIVER)], check=True)
        (d / "link").write_text("[objects]\ndriver.o\n")
        subprocess.run([WLALINK, "-b", str(d / "link"), str(d / "driver.bin")], check=True, cwd=d)
        drv = (d / "driver.bin").read_bytes()
    if len(drv) > PITCH_AT - 0x200:
        raise SystemExit(f"the driver is {len(drv)} bytes")
    img[0x200:0x200 + len(drv)] = drv
    for i, p in enumerate(pitch_table()):
        struct.pack_into("<H", img, PITCH_AT + 2 * i, p)
    img[VOL_AT:VOL_AT + 128] = bytes(vol_table())
    img[NOISE_AT:NOISE_AT + 32] = noise_table()
    at = SAMPLES_AT
    for k, p in enumerate(patches):
        wave, periods = render_period(p)
        pts = 128 * periods                      # four periods (two blocks a period)
        w = band_limit(np.tile(wave, 4), pts, 15 * periods * 4)
        w = w / max(1e-9, np.abs(w).max()) * 14000
        b = brr(w)
        img[at:at + len(b)] = b
        struct.pack_into("<HH", img, DIR_AT + 4 * k, at, at)
        a1, a2, rel = adsr_of(p)
        img[INST_AT + 8 * k:INST_AT + 8 * k + 8] = bytes([k, a1, a2, base_att(p), rel, 0, 0, 0])
        at += len(b)
    # a silent loop (the noise voice's sample)
    k = len(patches)
    struct.pack_into("<HH", img, DIR_AT + 4 * k, at, at)
    img[at:at + 9] = bytes([3] + [0] * 8)
    at += 9
    assert at < STREAMS_AT
    return bytes(img[0x200:at])


def main():
    """Write spc_engine.bin and spc_streams.bin into the out dir."""
    md, out = Path(sys.argv[1]), Path(sys.argv[2])
    gen = md / "build" / "gen"
    e = engine(gen)
    s = streams(gen)
    (out / "spc_engine.bin").write_bytes(e)
    (out / "spc_streams.bin").write_bytes(s)
    print(f"SPC: engine {len(e)} bytes (from $0200), streams {len(s)} bytes (from ${STREAMS_AT:04X})")


if __name__ == "__main__":
    main()
