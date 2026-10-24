/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the Game Boy Advance: the music and the sound effects - the
 * 3DS port's (3ds/src/sound3ds.c: the SNES port's sound driver in C, with
 * as much of the SNES's DSP as it uses), playing the SNES game's data from
 * its cartridge image (the driver's tables and samples, the streams).
 *
 * Out through Direct Sound (A left, B right; 8 bits) at 18157 Hz: 304
 * samples a frame, made in the vertical blank's interrupt (sound_mix) for
 * the next frame while the DMA plays this one's. The SNES's DSP runs at
 * 32 kHz: its pitches, envelopes, noise and the players' ticks are
 * stepped 32000 / 18157 (1805 / 1024) as far a sample.
 *
 * Words used below: "aram" is a rebuilt copy of the SNES sound CPU's
 * memory (only its first 32 KB: the engine at $0200, the streams at
 * $2000); a "stream" is a byte-coded list of note commands and waits
 * (run_player), a "player" reads one (0 music, 1 effects); a "voice" is one
 * of the S-DSP's 8 channels: the music uses 0..5 and 6 (the noise voice),
 * the effects 4..6, taking them from the music while they play. "BRR" is
 * the SNES's sample compression; the samples are decoded once at the start.
 *
 * The output: timer 0 overflows every 924 cycles (16.78 MHz / 924 = 18157
 * Hz) and each overflow takes a sample from each FIFO; DMA 1 and 2 refill
 * the FIFOs from buf_l and buf_r, two frames' worth each (double
 * buffering: one half plays while sound_mix fills the other).
 * Callers: ui.c (sound_init, music_play/stop), gfx.c (sound_mix in the
 * V-blank interrupt, sound_tick from wait_vblank); effects come from glue.c's sfx_q.
 * Licence: GPL-2.0-or-later.
 */
#include "plat.h"
#include "sound.h"
#include "libc.h"

/* SPC addresses of the driver's tables (snes/spc/driver.s's layout) */
#define PITCH 0x0E00                    /* pitches: 12 semitones x 32 fine steps, u16 (4.12), octave 8 */
#define VOLTAB 0x1100                   /* attenuation 0..127 -> DSP volume (s8) */
#define INST 0x1180                     /* instruments, 8 bytes: sample, ADSR1, ADSR2, attenuation, release GAIN */
#define NOISE 0x11D0                    /* noise kinds, 4 bytes: noise clock, attenuation, ADSR1, ADSR2 */
#define DIR 0x1200                      /* the sample directory: 4 bytes a sample (start, loop) */
#define STREAMS 0x2000                  /* the streams: music pointers at +0, effects' at +8 */
#define SILENT 10                       /* a silent sample (for the noise voice) */
#define STEP 1805                       /* 32000 / 18157, in 1024ths */
#define TICK 532                        /* the players' tick: 133 x 125 us (32 kHz samples) */
#define MIN_FRAMES 20                   /* an effect may be cut by the next after this many frames */
#define FRAME 304                       /* samples a frame (280896 / 924) */
#define ARAM_MASK 0x7FFF                /* aram is 32 KB: SPC addresses wrap into it */

static u8 aram[ARAM_MASK + 1] EWRAM_BSS;   /* the SPC's memory (its first 32 KB) */
static int ready;                       /* 1 once sound_init has run */

/* ------------------------------------------------------------ the DSP */
#define N_SRC 32                        /* samples decoded (directory entries 0..31) */
static s16 *pcm[N_SRC];                 /* each sample's PCM in pcm_store */
static int pcm_len[N_SRC], pcm_loop[N_SRC];    /* its length and loop point in samples (0 length: none) */
static s16 pcm_store[12000] EWRAM_BSS;  /* all of them (24 KB) */
static int pcm_used;

/* brr_decode: every sample of the directory into pcm_store, as the S-DSP
 * decodes BRR (header: shift bits 4..7, filter 2..3, end bit 0; signed
 * nibbles scaled by the shift, filters 1..3 predicting from the last two
 * samples, clamped to 16 bits and wrapped to 15). A sample outside
 * $1300..$1FFF, or that does not fit, gets length 0. Same as
 * 3ds/src/sound3ds.c. Called once by sound_init. */
static void brr_decode(void)
{
    int s;
    for (s = 0; s < N_SRC; s++) {
        u16 start = aram[DIR + 4 * s] | (aram[DIR + 4 * s + 1] << 8), loop = aram[DIR + 4 * s + 2] | (aram[DIR + 4 * s + 3] << 8);
        int p1 = 0, p2 = 0, n = 0;
        u32 a = start;
        pcm[s] = &pcm_store[pcm_used];
        pcm_loop[s] = 0;
        if (start < 0x1300 || start >= 0x2000) {
            pcm_len[s] = 0;
            continue;
        }
        for (;;) {
            u8 h = aram[a & ARAM_MASK], shift = h >> 4, filter = (h >> 2) & 3;
            int k;
            if (a == loop) pcm_loop[s] = n;
            for (k = 0; k < 16; k++) {
                int nib = (aram[(a + 1 + k / 2) & ARAM_MASK] >> ((k & 1) ? 0 : 4)) & 15, v;
                if (nib >= 8) nib -= 16;        /* signed */
                v = shift <= 12 ? (nib << shift) >> 1 : (nib < 0 ? -2048 : 0);
                if (filter == 1) v += p1 + ((-p1) >> 4);
                else if (filter == 2) v += 2 * p1 + ((-3 * p1) >> 5) - p2 + (p2 >> 4);
                else if (filter == 3) v += 2 * p1 + ((-13 * p1) >> 6) - p2 + ((3 * p2) >> 4);
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                v = (s16)(v << 1) >> 1;
                p2 = p1;
                p1 = v;
                if (pcm_used + n < (int)(sizeof pcm_store / 2)) pcm[s][n] = (s16)v;
                n++;
            }
            if (h & 1) break;           /* the end block */
            a += 9;
            if (n > 8192) break;        /* (a guard against a sample with no end) */
        }
        if (pcm_used + n > (int)(sizeof pcm_store / 2)) n = 0;
        pcm_len[s] = n;
        pcm_used += n;
    }
}

/* the S-DSP's rate table: the period in 32 kHz samples of an envelope (or noise) step, rates 1..31 */
static const u16 RATES[32] = {0, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
                              64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1};
enum { E_ATTACK, E_DECAY, E_SUSTAIN, E_RELEASE };     /* the envelope's phase */

/* one DSP voice */
struct voice {
    u8 on, srcn, adsr1, adsr2, gain, emode, noise;
    /* sounding; its sample; the DSP's envelope registers (ADSR1 bit 7:
       ADSR mode, else GAIN); the phase; plays the noise */
    s8 vl, vr;                          /* left and right volume */
    u16 pitch;                          /* the DSP's pitch (4.12 at 32 kHz) */
    u32 step;                           /* pitch at our rate */
    s32 env;                            /* the envelope, 0..$7FF */
    u32 pos;                            /* 20.12 */
    u32 ecount;                         /* 32 kHz steps since the envelope's last change */
};
static struct voice V[8] IWRAM_DATA;
static u8 noise_clock;                  /* the noise's rate (0..31) */
static u32 noise_count;
static u16 lfsr = 0x4000;               /* the noise generator's 15-bit shift register */
static int master = 0x26;               /* the DSP's main volume (fixed) */

/* dsp_kon: key on voice v: from its sample's start, the envelope from 0 in attack */
static void dsp_kon(int v)
{
    struct voice *x = &V[v];
    x->on = 1;
    x->pos = 0;
    x->env = 0;
    x->emode = E_ATTACK;
    x->ecount = 0;
}

/* dsp_koff: key off voice v: its envelope goes to release */
static void dsp_koff(int v) { V[v].emode = E_RELEASE; }

/* a voice's envelope k 32 kHz steps on (the DSP's: a change each
   RATES[rate] steps; event by event)
   (Release: -8 a step to 0, then the voice stops. ADSR: attack +32 a
   change, +1024 at rate 31, to $7FF; decay exponential to the sustain
   level, ADSR2 bits 5..7; then sustain, exponential. GAIN: bit 7 clear
   the level itself; else bits 5..6: linear down, exponential down,
   linear up, bent up.) */
IWRAM_CODE static void envelope(struct voice *x, int k)
{
    while (k > 0) {
        int rate, need;
        if (x->emode == E_RELEASE) {
            x->env -= 8 * k;
            if (x->env <= 0) {
                x->env = 0;
                x->on = 0;
            }
            return;
        }
        if (x->adsr1 & 0x80) {
            if (x->emode == E_ATTACK) rate = (x->adsr1 & 15) * 2 + 1;
            else if (x->emode == E_DECAY) rate = ((x->adsr1 >> 4) & 7) * 2 + 16;
            else rate = x->adsr2 & 31;
        } else {
            if (!(x->gain & 0x80)) {
                x->env = (x->gain & 0x7F) << 4;
                return;
            }
            rate = x->gain & 31;
        }
        if (!rate) return;
        need = (int)RATES[rate] - (int)x->ecount;       /* the steps to its next change */
        if (k < need) {
            x->ecount += (u32)k;
            return;
        }
        k -= need;
        x->ecount = 0;
        if (x->adsr1 & 0x80) {
            if (x->emode == E_ATTACK) {
                x->env += rate == 31 ? 1024 : 32;
                if (x->env >= 0x7FF) {
                    x->env = 0x7FF;
                    x->emode = E_DECAY;
                }
            } else {
                x->env -= ((x->env - 1) >> 8) + 1;     /* exponential: -1/256, at least 1 */
                if (x->emode == E_DECAY && (x->env >> 8) <= (x->adsr2 >> 5)) x->emode = E_SUSTAIN;
            }
        } else
            switch ((x->gain >> 5) & 3) {
            case 0: x->env -= 32; break;
            case 1: x->env -= ((x->env - 1) >> 8) + 1; break;
            case 2: x->env += 32; break;
            default: x->env += x->env < 0x600 ? 32 : 8; break;
            }
        if (x->env < 0) x->env = 0;
        if (x->env > 0x7FF) x->env = 0x7FF;
    }
}

static u32 step_acc;                    /* the 32 kHz steps owed (1024ths) */
#define BLK 16                          /* samples a block: the envelopes move between blocks */
static s32 mix_l[FRAME] IWRAM_BSS, mix_r[FRAME] IWRAM_BSS, mix_c[FRAME] IWRAM_BSS;
/* the sums: left only, right only, and both (voices with equal sides, added once) */
static s16 noise_buf[FRAME] IWRAM_BSS;  /* the noise's samples for this call */

/* (fast RAM, ARM) n samples (n <= FRAME) into lo, ro (8 bits): voice by
   voice, a block at a time (the envelope stepped as far as the block's
   32 kHz steps first) */
IWRAM_CODE static void dsp_run(s8 *lo, s8 *ro, int n)
{
    u8 steps[FRAME / BLK + 1];          /* each block's 32 kHz steps */
    int i, v, b, nb = (n + BLK - 1) / BLK;
    u32 acc = step_acc;
    for (b = 0; b < nb; b++) {          /* 1805 / 1024 steps an output sample */
        int len = n - b * BLK < BLK ? n - b * BLK : BLK;
        acc += (u32)len * STEP;
        steps[b] = (u8)(acc >> 10);
        acc &= 1023;
    }
    if (noise_clock) {                  /* the noise, a sample at a time */
        u32 a = step_acc;
        for (i = 0; i < n; i++) {
            int k;
            a += STEP;
            for (k = (int)(a >> 10); k > 0; k--)
                if (++noise_count >= RATES[noise_clock]) {
                    noise_count = 0;
                    lfsr = (u16)((lfsr >> 1) | (((lfsr ^ (lfsr >> 1)) & 1) << 14));  /* bit 0 xor bit 1 in at bit 14 */
                }
            a &= 1023;
            noise_buf[i] = (s16)((s16)(lfsr << 1) >> 1);
        }
    }
    step_acc = acc;
    for (i = 0; i < n; i++) mix_l[i] = mix_r[i] = mix_c[i] = 0;
    for (v = 0; v < 8; v++) {
        struct voice *x = &V[v];
        int nn = pcm_len[x->srcn], lp = pcm_loop[x->srcn];
        const s16 *p = pcm[x->srcn];
        if (!x->on) continue;
        if (!x->noise && !nn) continue;
        for (b = 0; b < nb && x->on; b++) {
            int i0 = b * BLK, i1 = i0 + BLK < n ? i0 + BLK : n;
            s32 gl, gr;
            envelope(x, steps[b]);
            gl = (x->env * x->vl * master) >> 10;       /* (o = s env >> 10; l += o vl >> 7; l master >> 15) */
            gr = (x->env * x->vr * master) >> 10;
            if (x->noise) {
                for (i = i0; i < i1; i++) {
                    mix_l[i] += noise_buf[i] * gl;
                    mix_r[i] += noise_buf[i] * gr;
                }
                continue;
            }
            if (((x->pos + x->step * (u32)(i1 - i0)) >> 12) + 1 < (u32)nn) {
                /* (the block stays clear of the sample's end: no checks) */
                u32 pos = x->pos, st = x->step;
                if (gl == gr)
                    for (i = i0; i < i1; i++, pos += st) {
                        const s16 *q = p + (pos >> 12);
                        s32 sa = q[0], sm = sa + (((q[1] - sa) * (s32)(pos & 0xFFF)) >> 12);
                        mix_c[i] += sm * gl;
                    }
                else
                    for (i = i0; i < i1; i++, pos += st) {
                        const s16 *q = p + (pos >> 12);
                        s32 sa = q[0], sm = sa + (((q[1] - sa) * (s32)(pos & 0xFFF)) >> 12);
                        mix_l[i] += sm * gl;
                        mix_r[i] += sm * gr;
                    }
                x->pos = pos;
                continue;
            }
            for (i = i0; i < i1; i++) { /* (near the end: checked sample by sample) */
                u32 j = x->pos >> 12;
                s32 sa, sb, sm;
                if (j >= (u32)nn) {     /* past the end: back by the loop's length, or stop */
                    int ln = nn - lp;
                    if (ln <= 0) {
                        x->on = 0;
                        break;
                    }
                    while (j >= (u32)nn) {
                        x->pos -= (u32)ln << 12;
                        j -= (u32)ln;
                    }
                }
                sa = p[j];
                sb = p[j + 1 < (u32)nn ? j + 1 : (u32)lp];
                sm = sa + (((sb - sa) * (s32)(x->pos & 0xFFF)) >> 12);
                x->pos += x->step;
                mix_l[i] += sm * gl;
                mix_r[i] += sm * gr;
            }
        }
    }
    for (i = 0; i < n; i++) {           /* to 8 bits, clipped */
        s32 l = (mix_l[i] + mix_c[i]) >> 21, r = (mix_r[i] + mix_c[i]) >> 21;
        lo[i] = (s8)(l > 127 ? 127 : l < -128 ? -128 : l);
        ro[i] = (s8)(r > 127 ? 127 : r < -128 ? -128 : r);
    }
}

/* ------------------------------------------------------------ the driver (the SNES's driver.s) */
/* per player (0 music, 1 effects): */
static u16 ptr[2], start[2];            /* the next stream byte; the loop's start */
static u8 wait_[2], loops[2], active[2];        /* ticks to wait; loops at the end; playing */
/* per player and channel (pv = player x 7 + voice): note ($FF none), bend
   (bits 5..7 signed semitones, 0..4 32nds), attenuation, pan (bit 7 left,
   bit 6 right), instrument + 1 (0 none) */
static u8 note[14], bend[14], att[14], pan[14], patch[14];
static u8 pl, pv, vv, mvol;             /* the command's player, channel, voice; the music's extra attenuation (always 0 here) */

/* is_free: may the current player change voice vv? The effects always;
   the music on 0..3, and on 4..6 only while no effect plays */
static int is_free(void) { return pl || vv < 4 || !active[1]; }

/* set_pitch: voice vv's pitch from channel pv's note and bend (the
   octave-8 table entry, halved per octave below), and its step at 18157 Hz */
static void set_pitch(void)
{
    int s, oct, semi, fine;
    u16 p;
    if (!is_free()) return;
    fine = bend[pv] & 31;
    s = note[pv] + ((s8)bend[pv] >> 5);
    s &= 0xFF;
    oct = s / 12;
    semi = s % 12;
    p = (u16)(aram[PITCH + (semi * 32 + fine) * 2] | (aram[PITCH + (semi * 32 + fine) * 2 + 1] << 8));
    if (oct >= 9) p = 0x3FFF;
    else p >>= (8 - oct);
    V[vv].pitch = p;
    V[vv].step = ((u32)p * STEP) >> 10;
}

/* set_volume: voice vv's volumes from the attenuations through VOLTAB, to the sides the pan allows */
static void set_volume(void)
{
    int a, vol;
    if (!is_free() || !patch[pv]) return;
    a = aram[INST + (patch[pv] - 1) * 8 + 3] + att[pv];
    if (!pl) a += mvol;
    if (a > 127) a = 127;
    vol = (s8)aram[VOLTAB + a];
    V[vv].vl = (s8)((pan[pv] & 0x80) ? vol : 0);
    V[vv].vr = (s8)((pan[pv] & 0x40) ? vol : 0);
}

/* keyoff: end channel pv's note: an instrument fades with its release GAIN; else a key-off */
static void keyoff(void)
{
    if (!is_free()) return;
    if (patch[pv]) {
        V[vv].gain = aram[INST + (patch[pv] - 1) * 8 + 4];
        V[vv].adsr1 = 0;
    } else
        dsp_koff(vv);
}

/* noise_hit: a noise note of kind type (0..7) on voice 6, extra attenuation y (effects only) */
static void noise_hit(u8 type, u8 y)
{
    int x = (type & 7) * 4, a;
    if (!is_free()) return;
    noise_clock = aram[NOISE + x] & 31;
    a = (pl ? y : 0) + aram[NOISE + x + 1];
    if (a > 15) a = 15;
    a = a * 8 / 3;                      /* 0..15 -> 0..40 of VOLTAB */
    V[6].vl = V[6].vr = (s8)aram[VOLTAB + a];
    V[6].srcn = SILENT;
    V[6].adsr1 = aram[NOISE + x + 2];
    V[6].adsr2 = aram[NOISE + x + 3];
    V[6].noise = 1;
    dsp_kon(6);
}

/* c_on: note n on at attenuation y on the current channel (voice 6: a noise hit of kind n) */
static void c_on(u8 n, u8 y)
{
    if (vv == 6) {
        noise_hit(n, y);
        return;
    }
    note[pv] = n;
    att[pv] = y;
    if (!is_free() || !patch[pv]) return;
    {
        const u8 *in = &aram[INST + (patch[pv] - 1) * 8];
        V[vv].srcn = in[0];
        V[vv].adsr1 = in[1];
        V[vv].adsr2 = in[2];
        V[vv].noise = 0;
    }
    set_volume();
    set_pitch();
    dsp_kon(vv);
}

/* sfx_off: release the effects' voices 4..6 */
static void sfx_off(void)
{
    dsp_koff(4);
    dsp_koff(5);
    dsp_koff(6);
}

/* start_player: player x plays the stream at s (first byte: loops or not), its channels reset */
static void start_player(int x, u16 s)
{
    int v;
    loops[x] = aram[s & ARAM_MASK];
    ptr[x] = start[x] = (u16)(s + 1);
    wait_[x] = 0;
    active[x] = 1;
    for (v = 0; v < 7; v++) {
        note[x * 7 + v] = 0xFF;
        bend[x * 7 + v] = 0;
        att[x * 7 + v] = 0;
        patch[x * 7 + v] = 0;
        pan[x * 7 + v] = 0xC0;
    }
}

/* stop_music: the music player off, voices 0..5 released */
static void stop_music(void)
{
    active[0] = 0;
    for (vv = 0; vv < 6; vv++) {
        pv = vv;
        pl = 0;
        keyoff();
        note[vv] = 0xFF;
    }
}

/* run_player: one tick of player x: when its wait is over, run commands
 * up to the next wait ($00..$7F wait n ticks; v = the low 3 bits: $80+v
 * note n, attenuation y; $88+v note off; $90+v instrument; $98+v bend;
 * $A0+v attenuation; $A8+v pan; $B0+v change the sounding note; $FF the
 * end: loop or stop). Called by sound_mix every TICK 32 kHz steps. */
static void run_player(int x)
{
    u16 sp;
    pl = (u8)x;
    if (!active[x]) return;
    if (wait_[x] && --wait_[x]) return;
    sp = ptr[x];
    for (;;) {
        u8 c = aram[sp++ & ARAM_MASK];
        if (c < 0x80) {
            wait_[x] = c;
            ptr[x] = sp;
            return;
        }
        if (c == 0xFF) {
            if (loops[x]) {
                sp = start[x];
                continue;
            }
            active[x] = 0;
            if (x == 1) sfx_off();
            return;
        }
        vv = c & 7;
        pv = (u8)(x * 7 + vv);
        switch (c & 0xF8) {
        case 0x80: {
            u8 n = aram[sp++ & ARAM_MASK], y = aram[sp++ & ARAM_MASK];
            c_on(n, y);
            break;
        }
        case 0x88:
            if (vv != 6) {
                note[pv] = 0xFF;
                keyoff();
            }
            break;
        case 0x90: patch[pv] = (u8)(aram[sp++ & ARAM_MASK] + 1); break;
        case 0x98:
            bend[pv] = aram[sp++ & ARAM_MASK];
            if (note[pv] != 0xFF) set_pitch();
            break;
        case 0xA0:
            att[pv] = aram[sp++ & ARAM_MASK];
            set_volume();
            break;
        case 0xA8:
            pan[pv] = aram[sp++ & ARAM_MASK];
            set_volume();
            break;
        case 0xB0: {
            u8 n = aram[sp++ & ARAM_MASK];
            if (note[pv] != 0xFF) {
                note[pv] = n;
                set_pitch();
            }
            break;
        }
        }
    }
}

/* ------------------------------------------------------------ the game's side */
static s8 buf_l[2 * FRAME] __attribute__((aligned(4))) IWRAM_BSS;    /* two frames of left samples */
static s8 buf_r[2 * FRAME] __attribute__((aligned(4))) IWRAM_BSS;    /* and right */
static u8 half;                         /* the half of the buffers the DMA is playing now */
static s32 tick_at;                     /* 32 kHz steps (x 1024) to the players' next tick */
static u8 playing = 255;                /* frames since the last effect began */

/* irq_off / irq_on: the interrupts off and on around the player's state
   (sound_mix, in the interrupt, changes it too) */
static void irq_off(void) { REG_IME = 0; }
static void irq_on(void) { REG_IME = 1; }

/* dma_start: DMA 1 and 2 feed FIFO A and B from the buffers' starts, 4
   bytes (4 samples) each time a FIFO asks */
static void dma_start(void)
{
    REG_DMA1CNT = 0;
    REG_DMA2CNT = 0;
    REG_DMA1SAD = (u32)buf_l;
    REG_DMA1DAD = REG_FIFO_A;
    REG_DMA1CNT = DMA_ON | DMA_SPECIAL | DMA_REPEAT | DMA_32 | DMA_DST_FIXED;
    REG_DMA2SAD = (u32)buf_r;
    REG_DMA2DAD = REG_FIFO_B;
    REG_DMA2CNT = DMA_ON | DMA_SPECIAL | DMA_REPEAT | DMA_32 | DMA_DST_FIXED;
}

/* (ui.c's sound_init: the SNES's engine's tables and samples at $C3A000,
   the streams: the resources' entry 8) */
void sound_init(void)
{
    const u8 *eng = snes_ptr(0xC3A000), *rb = snes_ptr((u32)snes_rom[0xFFB7] << 16 | 0xC00000);
    u32 n, i, at, len;
    memset(aram, 0, sizeof aram);
    n = eng[0] | (eng[1] << 8);
    for (i = 0; i < n && 0x200 + i <= ARAM_MASK; i++) aram[0x200 + i] = eng[2 + i];
    at = rb[12 + 64] | (rb[13 + 64] << 8) | ((u32)rb[14 + 64] << 16);
    len = rb[16 + 64] | (rb[17 + 64] << 8);
    for (i = 0; i < len && STREAMS + i <= ARAM_MASK; i++) aram[STREAMS + i] = rb[at + i];
    brr_decode();
    memset(buf_l, 0, sizeof buf_l);
    memset(buf_r, 0, sizeof buf_r);
    REG_SOUNDCNT_X = 0x0080;            /* the sound hardware on */
    REG_SOUNDCNT_H = 0x0004 | 0x0008 | 0x0200 | 0x0800 | 0x1000 | 0x8000;  /* A, B full; A left, B right; timer 0; reset */
    REG_TM0CNT_H = 0;                   /* timer 0 stopped */
    REG_TM0CNT_L = (u16)(65536 - 924);  /* overflow every 924 cycles: 18157 Hz */
    half = 0;
    dma_start();
    REG_TM0CNT_H = 0x0080;              /* timer 0 on (every cycle) */
    tick_at = 0;
    ready = 1;
}

/* music_play: play music id (its stream at STREAMS + 2 id) */
void music_play(u8 id)
{
    u16 s = (u16)(aram[STREAMS + 2 * id] | (aram[STREAMS + 2 * id + 1] << 8));
    irq_off();
    stop_music();
    start_player(0, s);
    irq_on();
}

/* music_stop: the music off */
void music_stop(void)
{
    irq_off();
    stop_music();
    irq_on();
}

/* play_sfx: effect id (its stream at STREAMS + 8 + 2 id), cutting any playing (from sound_mix) */
static void play_sfx(u8 id)
{
    u16 s = (u16)(aram[STREAMS + 8 + 2 * id] | (aram[STREAMS + 9 + 2 * id] << 8));
    sfx_off();
    start_player(1, s);
}

/* sound_tick: nothing (the effects are taken in sound_mix) */
void sound_tick(void) {}

/* the vertical blank (the interrupt): the next frame's samples, the
   players ticked as they come due */
void sound_mix(void)
{
    s8 *l, *r;
    int done = 0;
    if (!ready) {
        sfx_head = sfx_tail;
        return;
    }
    if (half) {                         /* (the second half is over: from the start again) */
        dma_start();
        half = 0;
    } else
        half = 1;
    /* the next effect (as the SNES's NMI: when the last is over or has had MIN_FRAMES) */
    if (playing < 255) playing++;
    if (sfx_head != sfx_tail && (!active[1] || playing >= MIN_FRAMES)) {
        u8 id = sfx_q[sfx_head];
        sfx_head = (sfx_head + 1) & 7;
        play_sfx(id);
        playing = 0;
    }
    l = buf_l + (half ^ 1) * FRAME;     /* fill the half not playing */
    r = buf_r + (half ^ 1) * FRAME;
    while (done < FRAME) {              /* in pieces, ticking the players between them */
        int n;
        if (tick_at <= 0) {
            tick_at += TICK * 1024;
            run_player(0);
            run_player(1);
        }
        n = (int)((tick_at + STEP - 1) / STEP);         /* samples to the next tick */
        if (n > FRAME - done) n = FRAME - done;
        dsp_run(l + done, r + done, n);
        tick_at -= n * STEP;
        done += n;
    }
}
