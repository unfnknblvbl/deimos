/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the 32X: the music and the sound effects - the SNES port's
 * sound driver (snes/spc/driver.s) in C, with as much of the SNES's DSP as
 * it uses (sample voices: pitch, ADSR and GAIN envelopes, the noise,
 * volumes), as the 3DS port has it (3ds/src/sound3ds.c), playing the same
 * data from the cartridge: the driver's tables and samples (the engine at
 * $C1A000 in the 32X's image: tools/mars_image.py) and the streams (the resources' spc_streams), read where they
 * are as the SPC's memory would hold them. The samples (BRR) are decoded as
 * they play.
 *
 * The slave SH-2 does it all: the players (ticking at the SNES driver's
 * 60.15 Hz), the voices, and the 32X's PWM (stereo, 21.3 kHz: 1.5 DSP
 * samples a sample), filled as fast as it empties. The master only passes
 * on what the game asks (the music, the effects) through shared memory.
 *
 * Words used below:
 *  - "the SPC's memory": the 64 KB address space of the SNES sound CPU.
 *    Nothing is copied: ar() maps an SPC address to the cartridge bytes
 *    that would be there ($0200.. the engine image, $2000.. the streams).
 *  - "stream": a byte-coded list of note commands and waits (see
 *    run_player); "player": one reader of a stream (0 music, 1 effects).
 *  - "voice": one of the 8 S-DSP sample channels (V[]). Music uses voices
 *    0..5 (and 6 for its noise), the effects 4..6, voice 6 being the noise
 *    voice; an effect takes voices 4..6 from the music while it plays.
 *  - "BRR": the SNES's sample compression: 9-byte blocks of 16 samples.
 *
 * Callers: ui.c calls sound_init and music_play/stop/volume (on the
 * master); plat.c calls sound_start and sound_tick (master); crt0.s
 * starts slave_main. The game queues effects in glue.c's sfx_q ring.
 *
 * Licence: GPL-2.0-or-later.
 */
#include "mars.h"
#include "libc.h"
#include "sound32x.h"
#include "r32x.h"
#include "../../snes/src/glue.h"

extern u8 rom_u8(u32 a);
extern u16 rom_u16le(u32 a);
extern const u8 *rom_image;

/* SPC addresses of the driver's tables (as snes/spc/driver.s lays them out) */
#define PITCH 0x0E00                    /* pitches: 12 semitones x 32 fine steps, u16 (4.12), octave 8 */
#define VOLTAB 0x1100                   /* attenuation 0..127 -> DSP volume (s8) */
#define INST 0x1180                     /* instruments, 8 bytes: sample, ADSR1, ADSR2, attenuation, release GAIN */
#define NOISE 0x11D0                    /* noise kinds, 4 bytes: noise clock, attenuation, ADSR1, ADSR2 */
#define DIR 0x1200                      /* the sample directory: 4 bytes a sample (start, loop) */
#define STREAMS 0x2000                  /* the streams: music pointers at +0, effects' at +8 */
#define SILENT 10                       /* a silent sample (for the noise voice) */
#define TICK_HALF 1064                  /* 133 x 125 us = 532 DSP samples, in half samples */
/* (the players tick every 532 DSP samples at 32 kHz: 60.15 times a second) */
#define MIN_FRAMES 20                   /* an effect may be cut by the next after this many frames */
#define PWM_CYCLE 1078                  /* 23.01 MHz / 1078 = 21.35 kHz */

/* ---- what the master and the slave share (used through UNCACHED) */
/* Both SH-2s have their own cache, so this struct is only ever accessed
   through its cache-through alias (SH), so each sees the other's writes. */
struct shared {
    u32 ready;                          /* 1 once sound_init has found the data */
    u32 music;                          /* 0x100 | id: play; 0x200: stop */
    u32 volume;                         /* the music's extra attenuation (0 loudest) */
    u8 sfx_q[8];                        /* effects to start: a ring, the master writes at tail, */
    u32 sfx_head, sfx_tail;             /* the slave reads at head (indexes 0..7) */
    u32 sfx_active;                     /* (the slave's) the effects' player is playing */
    u32 samples;                        /* (the slave's) made so far */
};
static struct shared shared_mem;
#define SH ((volatile struct shared *)UNCACHED(&shared_mem))

/* ---- the SPC's memory: the engine at $0200, the streams at $2000 */
static const u8 *eng_p, *str_p;         /* the engine image (SPC $0200..) and the streams ($2000..) */
static u32 eng_n, str_n;                /* their sizes in bytes */

/* ar: the byte at SPC address a (16 bits); 0 where nothing is loaded */
static u8 ar(u32 a)
{
    a &= 0xFFFF;
    if (a >= 0x200 && a - 0x200 < eng_n) return eng_p[a - 0x200];
    if (a >= STREAMS && a - STREAMS < str_n) return str_p[a - STREAMS];
    return 0;
}

/* rom_at: an SNES ROM address ($C00000..) -> its place in the cartridge (unchecked) */
static const u8 *rom_at(u32 a) { return rom_image + ((a & 0xFFFFFF) - 0xC00000); }

/* ------------------------------------------------------------ the DSP */
/* the S-DSP's rate table: for rate 1..31, the period in DSP samples of an
   envelope (or noise) step; rate 0 never steps */
static const u16 RATES[32] = {0, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
                              64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1};
enum { E_ATTACK, E_DECAY, E_SUSTAIN, E_RELEASE };     /* the envelope's phase */

/* one DSP voice */
struct voice {
    u8 on, srcn, adsr1, adsr2, gain, emode, noise, end;
    /* on: sounding; srcn: its sample; adsr1/adsr2/gain: the DSP's envelope
       registers (ADSR1 bit 7 set: ADSR, else GAIN); emode: the phase;
       noise: plays the noise instead of a sample; end: the block's end
       code (brr_block) */
    s8 vl, vr;                          /* left and right volume (DSP, signed) */
    u16 pitch;                          /* the DSP's pitch (4.12: $1000 plays at 32 kHz) */
    u32 step;                           /* the pitch at this rate: x 1.5 */
    s32 env;                            /* the envelope, 0..$7FF */
    u32 pos;                            /* in the block: 16.16 */
    u32 ecount;                         /* half DSP samples */
    u16 blk, loop;                      /* the block playing (SPC address), the loop's */
    s16 p1, p2, last;                   /* the last two decoded samples (BRR filters); the last block's last */
    s16 buf[16];                        /* the block's decoded samples */
};
static struct voice V[8];
static u8 noise_clock;                  /* the noise's rate (0..31) */
static u32 noise_count;                 /* its counter, in half DSP samples */
static u16 lfsr = 0x4000;               /* the noise generator's 15-bit shift register */
static int master_vol = 0x26;           /* the DSP's main volume (fixed) */

/* (the SH-2 shifts right one bit at a time, and arithmetically only one:
   a signed shift by a constant, for |x| < 2^30, through logical ones) */
#define ASR(x, n) ((s32)(((u32)(x) + 0x40000000u) >> (n)) - (0x40000000 >> (n)))
static s16 nib_tab[16][16];             /* [shift][nibble]: (nibble << shift) >> 1 */

/* brr_block: decode the 9-byte BRR block at x->blk into x->buf (16
 * samples), as the S-DSP does: each 4-bit nibble (signed) is scaled by the
 * header's shift, then filter 1..3 adds a prediction from the previous
 * two samples (p1, p2); the result is clamped to 16 bits and wrapped to
 * 15. Header byte: bits 4..7 shift, 2..3 filter, bit 1 loop, bit 0 end.
 * Sets x->end: 0 go on to the next block, 1 jump to the loop, 2 stop. */
static void brr_block(struct voice *x)
{
    u8 h = ar(x->blk), shift = h >> 4, filter = (h >> 2) & 3;
    int k, p1 = x->p1, p2 = x->p2;
    const s16 *nt = nib_tab[shift];
    for (k = 0; k < 16; k++) {
        int nib = (ar(x->blk + 1 + k / 2) >> ((k & 1) ? 0 : 4)) & 15, v;     /* the high nibble first */
        v = nt[nib];
        if (filter == 1) v += p1 + ASR(-p1, 4);
        else if (filter == 2) v += 2 * p1 + ASR(-3 * p1, 5) - p2 + ASR(p2, 4);
        else if (filter == 3) v += 2 * p1 + ASR(-13 * p1, 6) - p2 + ASR(3 * p2, 4);
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        v = (s16)(v << 1);              /* (the DSP keeps 15 bits: drop the top one, */
        v = ASR(v, 1);                  /* sign-extending from bit 14) */
        p2 = p1;
        p1 = v;
        x->buf[k] = (s16)v;
    }
    x->p1 = (s16)p1;
    x->p2 = (s16)p2;
    x->end = h & 1 ? (h & 2 ? 1 : 2) : 0;           /* 1 loop, 2 stop, 0 go on */
}

/* dsp_kon: key on voice v: restart its envelope in the attack phase and,
 * for a sample voice, look its sample up in the directory and decode the
 * first block. A sample outside $1300..$1FFF (the samples' area) leaves
 * the voice off. */
static void dsp_kon(int v)
{
    struct voice *x = &V[v];
    x->pos = 0;
    x->env = 0;
    x->emode = E_ATTACK;
    x->ecount = 0;
    x->p1 = x->p2 = x->last = 0;
    if (!x->noise) {
        u16 s = (u16)(ar(DIR + 4 * x->srcn) | (ar(DIR + 4 * x->srcn + 1) << 8));
        x->loop = (u16)(ar(DIR + 4 * x->srcn + 2) | (ar(DIR + 4 * x->srcn + 3) << 8));
        if (s < 0x1300 || s >= 0x2000) {
            x->on = 0;
            return;
        }
        x->blk = s;
        brr_block(x);
    }
    x->on = 1;
}

/* dsp_koff: key off voice v: its envelope goes to the release phase */
static void dsp_koff(int v) { V[v].emode = E_RELEASE; }

/* the envelope, n output samples on (1.5 DSP samples each)
 * Release: -8 a DSP sample until 0 (then the voice stops). ADSR: attack
 * +32 a step (+1024 at rate 31) up to $7FF, then decay (exponential) down
 * to the sustain level (ADSR2 bits 5..7), then sustain (exponential, at
 * ADSR2's rate). GAIN: bit 7 clear sets the level directly; otherwise
 * bits 5..6 pick linear down, exponential down, linear up, or "bent" up
 * (fast below $600, slow above), at rate bits 0..4. */
static void envelope(struct voice *x, int n)
{
    int rate;
    if (x->emode == E_RELEASE) {
        x->env -= 12 * n;               /* (8 a DSP sample) */
        if (x->env <= 0) {
            x->env = 0;
            x->on = 0;
        }
        return;
    }
    if (x->adsr1 & 0x80) {              /* ADSR mode */
        if (x->emode == E_ATTACK) rate = (x->adsr1 & 15) * 2 + 1;
        else if (x->emode == E_DECAY) rate = ((x->adsr1 >> 4) & 7) * 2 + 16;
        else rate = x->adsr2 & 31;
    } else {                            /* GAIN mode */
        if (!(x->gain & 0x80)) {        /* direct: the level itself */
            x->env = (x->gain & 0x7F) << 4;
            return;
        }
        rate = x->gain & 31;
    }
    if (!rate) return;
    x->ecount += 3 * n;                 /* 3 half DSP samples an output sample */
    while (x->ecount >= (u32)RATES[rate] * 2) {     /* a step each rate period */
        x->ecount -= (u32)RATES[rate] * 2;
        if (x->adsr1 & 0x80) {
            if (x->emode == E_ATTACK) {
                x->env += rate == 31 ? 1024 : 32;
                if (x->env >= 0x7FF) {
                    x->env = 0x7FF;
                    x->emode = E_DECAY;
                }
            } else {
                x->env -= ((x->env - 1) >> 8) + 1;     /* exponential: -1/256 of it, at least 1 */
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

/* BLOCK output samples of every voice, added into L and R (the envelopes
   move every 8 samples) */
#define BLOCK 32
/* (L and R come out at about 16 bits a voice, before clipping; slave_main
   scales them to the PWM.) A sample voice reads its decoded block at
   x->pos, interpolating linearly between neighbouring samples, and
   decodes the next block (or the loop's) as it runs off the end. */
static void dsp_block(int *L, int *R)
{
    int v, i;
    for (i = 0; i < BLOCK; i++) L[i] = R[i] = 0;
    for (v = 0; v < 8; v++) {
        struct voice *x = &V[v];
        int k;
        if (!x->on) continue;
        for (k = 0; k < BLOCK; k += 8) {
            int gl, gr, j;
            envelope(x, 8);
            if (!x->on) break;
            gl = x->env * x->vl * master_vol / 256;         /* (o = s env >> 10, x vl >> 7, x master >> 7: >> 24 in all) */
            gr = x->env * x->vr * master_vol / 256;
            if (x->noise) {             /* the noise: the LFSR's bits as the sample */
                for (j = 0; j < 8; j++) {
                    int sm;
                    if (noise_clock) {
                        noise_count += 3;
                        while (noise_count >= (u32)RATES[noise_clock] * 2) {
                            noise_count -= (u32)RATES[noise_clock] * 2;
                            lfsr = (u16)((lfsr >> 1) | (((lfsr ^ (lfsr >> 1)) & 1) << 14));  /* (the DSP's: bit 0 xor bit 1 in at bit 14) */
                        }
                    }
                    sm = (s16)(lfsr << 1);
                    sm = ASR(sm, 1);
                    L[k + j] += (sm * gl) >> 16;
                    R[k + j] += (sm * gr) >> 16;
                }
                continue;
            }
            for (j = 0; j < 8; j++) {
                int p = (int)(x->pos >> 16), a, b, sm;
                if (p >= 16) {          /* the next block */
                    x->last = x->buf[15];
                    if (x->end == 2) {
                        x->on = 0;
                        break;
                    }
                    x->blk = x->end == 1 ? x->loop : (u16)(x->blk + 9);
                    brr_block(x);
                    x->pos -= 16 << 16;
                    p -= 16;
                }
                a = p ? x->buf[p - 1] : x->last;
                b = x->buf[p];
                sm = a + (((b - a) * (int)(x->pos & 0xFFFF)) >> 16);  /* (linear interpolation by pos's fraction) */
                x->pos += x->step;
                L[k + j] += (sm * gl) >> 16;
                R[k + j] += (sm * gr) >> 16;
            }
            if (!x->on) break;
        }
    }
}

/* ------------------------------------------------------------ the driver (driver.s) */
/* per player (0 music, 1 effects): */
static u16 ptr[2], start[2];            /* the next stream byte; the loop's start (SPC addresses) */
static u8 wait_[2], loops[2], active[2];        /* ticks to wait; loops at the end; playing */
/* per player and channel (index pv = player x 7 + voice): */
static u8 note[14], bend[14], att[14], pan[14], patch[14];
/* note (semitone, $FF none), pitch bend (bits 5..7 signed semitones, 0..4
   32nds), attenuation, pan (bit 7 left, bit 6 right), instrument + 1 (0 none) */
static u8 pl, pv, vv, mvol;
/* the command being run: player, channel index, voice; and the music's
   extra attenuation (from SH->volume) */

/* is_free: may the current player change voice vv now? The effects always
   may; the music may on voices 0..3, and on 4..6 only while no effect plays */
static int is_free(void) { return pl || vv < 4 || !active[1]; }

/* set_pitch: voice vv's pitch from channel pv's note and bend: the
   table's octave-8 pitch for the semitone and fine step, halved for each
   octave below 8 (octave 9 and up: the highest pitch) */
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
    p = (u16)(ar(PITCH + (semi * 32 + fine) * 2) | (ar(PITCH + (semi * 32 + fine) * 2 + 1) << 8));
    if (oct >= 9) p = 0x3FFF;
    else p >>= (8 - oct);
    V[vv].pitch = p;
    V[vv].step = (u32)p * 24;            /* (x 1.5, 4.12 -> 16.16) */
}

/* set_volume: voice vv's left and right volume: the instrument's and the
   note's attenuation (plus mvol for the music) through VOLTAB, to the
   sides the pan allows */
static void set_volume(void)
{
    int a, vol;
    if (!is_free() || !patch[pv]) return;
    a = ar(INST + (patch[pv] - 1) * 8 + 3) + att[pv];
    if (!pl) a += mvol;
    if (a > 127) a = 127;
    vol = (s8)ar(VOLTAB + a);
    V[vv].vl = (s8)((pan[pv] & 0x80) ? vol : 0);
    V[vv].vr = (s8)((pan[pv] & 0x40) ? vol : 0);
}

/* keyoff: end channel pv's note: an instrument fades out with its release
   GAIN (ADSR off); otherwise a plain key-off */
static void keyoff(void)
{
    if (!is_free()) return;
    if (patch[pv]) {
        V[vv].gain = ar(INST + (patch[pv] - 1) * 8 + 4);
        V[vv].adsr1 = 0;
    } else
        dsp_koff(vv);
}

/* noise_hit: a noise note of kind type (0..7) on voice 6, at extra
   attenuation y (effects only), from the NOISE table */
static void noise_hit(u8 type, u8 y)
{
    int x = (type & 7) * 4, a;
    if (!is_free()) return;
    noise_clock = ar(NOISE + x) & 31;
    a = (pl ? y : 0) + ar(NOISE + x + 1);
    if (a > 15) a = 15;
    a = a * 8 / 3;                      /* 0..15 -> 0..40 of VOLTAB */
    V[6].vl = V[6].vr = (s8)ar(VOLTAB + a);
    V[6].srcn = SILENT;
    V[6].adsr1 = ar(NOISE + x + 2);
    V[6].adsr2 = ar(NOISE + x + 3);
    V[6].noise = 1;
    dsp_kon(6);
}

/* c_on: note n on, attenuation y, on the current channel: on voice 6 a
   noise hit (n the kind); otherwise the instrument's sample and envelope,
   volume, pitch, and key on */
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
        u32 in = INST + (patch[pv] - 1) * 8;
        V[vv].srcn = ar(in);
        V[vv].adsr1 = ar(in + 1);
        V[vv].adsr2 = ar(in + 2);
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

/* start_player: player x plays the stream at SPC address s (its first
   byte: loops or not), its 7 channels reset (no note, no instrument, both
   sides) */
static void start_player(int x, u16 s)
{
    int v;
    loops[x] = ar(s);
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

/* stop_music: the music player off, its voices 0..5 released */
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

/* run_player: one tick of player x: when its wait is over, run its
 * stream's commands up to the next wait. Commands (v = the low 3 bits,
 * the voice):
 *   $00..$7F       wait this many ticks
 *   $80+v n y      note n on, attenuation y
 *   $88+v          note off
 *   $90+v i        instrument i
 *   $98+v b        pitch bend b
 *   $A0+v a        attenuation a
 *   $A8+v p        pan p
 *   $B0+v n        change the sounding note to n (no new key-on)
 *   $FF            the end: loop to the start, or stop
 * Called by slave_main at each tick (60.15 Hz). */
static void run_player(int x)
{
    u16 sp;
    pl = (u8)x;
    if (!active[x]) return;
    if (wait_[x] && --wait_[x]) return;     /* still waiting */
    sp = ptr[x];
    for (;;) {
        u8 c = ar(sp++);
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
            u8 n = ar(sp++), y = ar(sp++);
            c_on(n, y);
            break;
        }
        case 0x88:
            if (vv != 6) {
                note[pv] = 0xFF;
                keyoff();
            }
            break;
        case 0x90: patch[pv] = (u8)(ar(sp++) + 1); break;
        case 0x98:
            bend[pv] = ar(sp++);
            if (note[pv] != 0xFF) set_pitch();
            break;
        case 0xA0:
            att[pv] = ar(sp++);
            set_volume();
            break;
        case 0xA8:
            pan[pv] = ar(sp++);
            set_volume();
            break;
        case 0xB0: {
            u8 n = ar(sp++);
            if (note[pv] != 0xFF) {
                note[pv] = n;
                set_pitch();
            }
            break;
        }
        }
    }
}

/* ------------------------------------------------------------ the master's side */
static int playing = 255;               /* frames since the last effect was passed on (to 255) */
static u8 sfx_pending_head;             /* (unused) */

/* sound_init: find the driver's data in the cartridge (the engine image
 * and the spc_streams resource) and tell the slave it may start (ready).
 * Called once by ui.c on the master. */
void sound_init(void)
{
    u32 eng = 0xC1A000UL, res;           /* (tools/mars_image.py puts the SNES image's $C3A000 there) */
    eng_n = rom_u16le(eng);             /* (a length word, then the bytes) */
    eng_p = rom_at(eng + 2);
    /* the resources' bank (header byte 7); resource 8 (spc_streams):
       offset low word at +12 + 8 i, high word at +14, size at +16 */
    res = (u32)rom_u8(0xC0FFB0UL + 7) << 16;
    str_p = rom_at(res + rom_u16le(res + 12 + 8 * 8) + ((u32)rom_u16le(res + 14 + 8 * 8) << 16));
    str_n = rom_u16le(res + 16 + 8 * 8);
    SH->music = 0;
    SH->sfx_head = SH->sfx_tail = 0;
    SH->volume = 0;
    SH->ready = 1;
}

/* sound_start: nothing to do (the slave starts on its own once ready) */
void sound_start(void) {}

/* music_play / music_stop / music_volume: the game's music requests,
   passed to the slave (it takes them at its next block) */
void music_play(u8 id) { SH->music = 0x100 | id; }
void music_stop(void) { SH->music = 0x200; }
void music_volume(u8 v) { SH->volume = v; }

/* sound_tick: once a frame on the master (plat.c's wait_vblank): pass the
 * next effect from the game's queue (glue.c's sfx_q) to the slave's, if
 * the effect before has finished or has had MIN_FRAMES frames. If the
 * slave's queue is full the effect is dropped. */
void sound_tick(void)
{
    if (!SH->ready) {
        sfx_head = sfx_tail;            /* not started: forget the effects */
        return;
    }
    /* the next effect (as the SNES's NMI: when the last is over or has had MIN_FRAMES) */
    if (playing < 255) playing++;
    if (sfx_head != sfx_tail && (!SH->sfx_active || playing >= MIN_FRAMES)) {
        u32 t = SH->sfx_tail;
        if (((t + 1) & 7) != SH->sfx_head) {
            SH->sfx_q[t] = sfx_q[sfx_head];
            SH->sfx_tail = (t + 1) & 7;
            SH->sfx_active = 1;
        }
        sfx_head = (sfx_head + 1) & 7;
        playing = 0;
    }
    (void)sfx_pending_head;
}

/* ------------------------------------------------------------ the slave */
/* the samples made, waiting for the PWM: its interrupt (every 3 samples it
   plays) moves them into its FIFO (3 deep) */
#define RING 2048                       /* (a power of two: indexes wrap by masking) */
static u32 ring[RING];                  /* left << 16 | right, the PWM's values */
static volatile u32 ring_in, ring_out;  /* written by slave_main; by pwm_isr */

/* pwm_isr: the PWM's interrupt (level 6, on the slave): acknowledge it and
   move samples from the ring into the PWM's FIFOs until the ring is empty
   or the FIFO is full. (interrupt_handler: GCC saves every register it
   uses and returns with rte.) */
void pwm_isr(void) __attribute__((interrupt_handler));
void pwm_isr(void)
{
    u32 o = ring_out;
    MREG16(0x1C) = 0;                   /* (the interrupt: taken) */
    /* (reading the left channel's register: bit 15 set while its FIFO is full) */
    while (o != ring_in && !(MARS_PWM_LEFT & 0x8000)) {
        u32 v = ring[o];
        MARS_PWM_LEFT = (u16)(v >> 16);
        MARS_PWM_RIGHT = (u16)v;
        o = (o + 1) & (RING - 1);
    }
    ring_out = o;
}

extern u32 slave_vbr[];                 /* the vector table (crt0.s's _slave_vbr) */

/*
 * slave_main: the slave SH-2's program, started by crt0.s; never returns.
 * Waits for sound_init, sets up the BRR table, its PWM interrupt and the
 * PWM, then loops: draw the right half of a picture when the master asks
 * and enough sound is buffered; take the music and effect requests; tick
 * the players at 60.15 Hz; mix BLOCK samples and put them in the ring as
 * PWM values (the interrupt plays them).
 */
void slave_main(void)
{
    u32 tick = 0;                       /* half DSP samples since the last tick */
    int i;
    static int L[BLOCK], R[BLOCK];
    while (!SH->ready) {}               /* until the master has found the data */
    {                                   /* nib_tab: each shift and signed nibble, as the DSP scales it
                                           (shifts 13..15: -2048 for negative nibbles, else 0) */
        int sh, n;
        for (sh = 0; sh < 16; sh++)
            for (n = 0; n < 16; n++) {
                int v = n >= 8 ? n - 16 : n;
                nib_tab[sh][n] = (s16)(sh <= 12 ? (v * (1 << sh)) / 2 - ((v * (1 << sh)) & 1 && v < 0 ? 1 : 0) : (v < 0 ? -2048 : 0));
            }
    }
    /* the PWM's interrupt (level 6: auto-vector 67) */
    ((volatile u32 *)UNCACHED(slave_vbr))[67] = (u32)pwm_isr;
    CCR = 0x10;                         /* (the cache: purged, then on) */
    CCR = 0x01;
    for (i = 0; i < 64; i++) ring[i] = ((u32)(PWM_CYCLE / 2) << 16) | (PWM_CYCLE / 2);   /* 64 samples of silence (mid-level) */
    ring_in = 64;
    MARS_PWM_CYCLE = PWM_CYCLE + 1;     /* (the cycle register takes the period + 1) */
    MARS_PWM_CTRL = 0x0305;             /* left to the left, right to the right; an interrupt each 3 samples */
    MARS_SYS = (u16)(MARS_SYS | 1);     /* (this SH-2's interrupt mask: the PWM's) */
    __asm__ volatile("mov #0x50, r0\n\tldc r0, sr" : : : "r0");   /* (interrupts of level 6 and above) */
    for (;;) {
        u32 m;
        if (((ring_in - ring_out) & (RING - 1)) > RING / 4) r32x_slave_poll();     /* (enough sound made: a picture's half) */
        if (((ring_in - ring_out) & (RING - 1)) > RING - BLOCK - 1) {               /* (full) */
            volatile int w;
            for (w = 0; w < 40; w++) {}     /* (a while between looks: emulators stop an SH-2 that polls) */
            continue;
        }
        m = SH->music;                  /* a music request? */
        if (m) {
            SH->music = 0;
            if (m & 0x100) {
                u16 s = (u16)(ar(STREAMS + 2 * (m & 0xFF)) | (ar(STREAMS + 2 * (m & 0xFF) + 1) << 8));  /* the music's stream */
                stop_music();
                start_player(0, s);
            } else
                stop_music();
        }
        mvol = (u8)SH->volume;
        if (SH->sfx_head != SH->sfx_tail) {     /* an effect to start (it cuts the one playing) */
            u32 h = SH->sfx_head;
            u8 id = SH->sfx_q[h];
            u16 s = (u16)(ar(STREAMS + 8 + 2 * id) | (ar(STREAMS + 9 + 2 * id) << 8));     /* its stream */
            SH->sfx_head = (h + 1) & 7;
            sfx_off();
            start_player(1, s);
        }
        /* a block of samples */
        tick += 3 * BLOCK;
        if (tick >= TICK_HALF) {
            tick -= TICK_HALF;
            run_player(0);
            run_player(1);
            SH->sfx_active = active[1];
        }
        dsp_block(L, R);
        SH->samples += BLOCK;
        {
            u32 in = ring_in;
            for (i = 0; i < BLOCK; i++) {       /* -> PWM values: +-32768 to about +-539 around the middle */
                int l = (L[i] * (PWM_CYCLE / 2 * 2)) >> 16, r = (R[i] * (PWM_CYCLE / 2 * 2)) >> 16;
                l += PWM_CYCLE / 2;
                r += PWM_CYCLE / 2;
                if (l < 1) l = 1;       /* clipped to 1..cycle-1 */
                if (l > PWM_CYCLE - 1) l = PWM_CYCLE - 1;
                if (r < 1) r = 1;
                if (r > PWM_CYCLE - 1) r = PWM_CYCLE - 1;
                ring[in] = ((u32)l << 16) | (u32)r;
                in = (in + 1) & (RING - 1);
            }
            ring_in = in;               /* (published last: the interrupt sees whole samples) */
        }
    }
}
