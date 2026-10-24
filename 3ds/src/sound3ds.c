/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the 3DS: the music and the sound effects - the SNES port's
 * sound driver (snes/spc/driver.s) in C, with as much of the SNES's DSP
 * as it uses (sample voices: pitch, ADSR and GAIN envelopes, the noise,
 * volumes), playing the same data from the cartridge image: the driver's
 * tables and samples (the engine at $C3A000) and the streams (the
 * resources' spc_streams), in a copy of the SPC's 64 KB. 32 kHz stereo
 * through ndsp; the players tick at the SNES driver's 60.15 Hz.
 *
 * Words used below: "the SPC's memory" (aram) is the SNES sound CPU's 64 KB
 * address space, rebuilt here: the engine image at $0200, the streams at
 * $2000. A "stream" is a byte-coded list of note commands and waits
 * (run_player); a "player" reads one (0 the music, 1 the effects). A
 * "voice" is one of the S-DSP's 8 channels (V[]): the music uses 0..5 and
 * 6 (the noise voice), the effects 4..6, taking them from the music while
 * they play. "BRR" is the SNES's sample compression (9-byte blocks of 16
 * samples).
 *
 * Unlike the 32X's version, the samples are all decoded once at the start
 * (brr_decode), and the mixing runs on the main thread: sound_tick, once a
 * frame (plat.c's wait_vblank), refills whichever of the 4 ndsp buffers
 * (1024 samples each, 32 ms) have finished playing. ui.c calls sound_init
 * and music_play/stop/volume; the game queues effects in glue.c's sfx_q.
 * Licence: GPL-2.0-or-later.
 */
#include <3ds.h>
#include <string.h>
#include "../../core/types.h"
#include "../../snes/src/glue.h"
#include "sound3ds.h"

extern u8 rom_u8(u32 a);
extern u16 rom_u16le(u32 a);

/* SPC addresses of the driver's tables (snes/spc/driver.s's layout) */
#define PITCH 0x0E00                    /* pitches: 12 semitones x 32 fine steps, u16 (4.12), octave 8 */
#define VOLTAB 0x1100                   /* attenuation 0..127 -> DSP volume (s8) */
#define INST 0x1180                     /* instruments, 8 bytes: sample, ADSR1, ADSR2, attenuation, release GAIN */
#define NOISE 0x11D0                    /* noise kinds, 4 bytes: noise clock, attenuation, ADSR1, ADSR2 */
#define DIR 0x1200                      /* the sample directory: 4 bytes a sample (start, loop) */
#define STREAMS 0x2000                  /* the streams: music pointers at +0, effects' at +8 */
#define SILENT 10                       /* a silent sample (for the noise voice) */
#define RATE 32000                      /* the output rate: the S-DSP's own */
#define TICK_SAMPLES 532                /* 133 x 125 us */
#define MIN_FRAMES 20                   /* an effect may be cut by the next after this many frames */

static u8 aram[65536];                  /* the SPC's memory */
static int ready;                       /* 1 once sound_init has set up ndsp */

/* ------------------------------------------------------------ the DSP */
#define N_SRC 32                        /* samples decoded (directory entries 0..31) */
static s16 *pcm[N_SRC];                 /* each sample's decoded PCM, in pcm_store */
static int pcm_len[N_SRC], pcm_loop[N_SRC];    /* its length and loop point, in samples (length 0: none) */
static s16 pcm_store[60000];            /* all of them */
static int pcm_used;                    /* samples of pcm_store used */

/* brr_decode: decode every sample of the directory into pcm_store, as the
 * S-DSP decodes BRR: each block's header byte gives the shift (bits 4..7),
 * the filter (2..3) and the end (bit 0); each signed nibble is scaled by
 * the shift, filter 1..3 adds a prediction from the last two samples, the
 * result is clamped to 16 bits and wrapped to 15. The loop point is where
 * the block at the directory's loop address starts. A sample outside
 * $1300..$1FFF, or one that does not fit in what is left of pcm_store, gets
 * length 0 (silence). Called once by sound_init. */
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
            u8 h = aram[a & 0xFFFF], shift = h >> 4, filter = (h >> 2) & 3;
            int k;
            if (a == loop) pcm_loop[s] = n;
            for (k = 0; k < 16; k++) {
                int nib = (aram[(a + 1 + k / 2) & 0xFFFF] >> ((k & 1) ? 0 : 4)) & 15, v;
                if (nib >= 8) nib -= 16;        /* signed */
                v = shift <= 12 ? (nib << shift) >> 1 : (nib < 0 ? -2048 : 0);    /* (shifts 13..15 as the DSP does them) */
                if (filter == 1) v += p1 + ((-p1) >> 4);
                else if (filter == 2) v += 2 * p1 + ((-3 * p1) >> 5) - p2 + (p2 >> 4);
                else if (filter == 3) v += 2 * p1 + ((-13 * p1) >> 6) - p2 + ((3 * p2) >> 4);
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                v = (s16)(v << 1) >> 1;         /* (the DSP keeps 15 bits) */
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

/* the S-DSP's rate table: the period in samples of an envelope (or noise) step, for rates 1..31 */
static const u16 RATES[32] = {0, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
                              64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1};
enum { E_ATTACK, E_DECAY, E_SUSTAIN, E_RELEASE };     /* the envelope's phase */

/* one DSP voice */
struct voice {
    u8 on, srcn, adsr1, adsr2, gain, emode, noise;
    /* sounding; its sample; the DSP's envelope registers (ADSR1 bit 7: ADSR
       mode, else GAIN); the phase; plays the noise instead of a sample */
    s8 vl, vr;                          /* left and right volume */
    u16 pitch;                          /* the DSP's pitch, 4.12 ($1000: one sample a sample) */
    s32 env;                            /* the envelope, 0..$7FF */
    u32 pos;                            /* 20.12 */
    u32 ecount;                         /* samples since the envelope's last step */
};
static struct voice V[8];
static u8 noise_clock;                  /* the noise's rate (0..31) */
static u32 noise_count;                 /* its counter */
static u16 lfsr = 0x4000;               /* the noise generator's 15-bit shift register */
static int master = 0x26;               /* the DSP's main volume (fixed) */

/* dsp_kon: key on voice v: from the sample's start, the envelope from 0 in attack */
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

/* envelope: one sample of voice x's envelope. Release: -8 a sample until 0
 * (the voice then stops). ADSR: attack +32 a step (+1024 at rate 31) to
 * $7FF, decay (exponential) to the sustain level (ADSR2 bits 5..7), then
 * sustain (exponential, at ADSR2's rate). GAIN: bit 7 clear sets the level
 * directly; else bits 5..6 pick linear down, exponential down, linear up or
 * bent up (fast below $600), at rate bits 0..4. */
static void envelope(struct voice *x)
{
    int rate;
    if (x->emode == E_RELEASE) {
        x->env -= 8;
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
    if (!rate || ++x->ecount < RATES[rate]) return;    /* not time for a step */
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

/* dsp_sample: one stereo output sample (out[0] left, out[1] right) of all
 * 8 voices: each voice's sample (linearly interpolated between decoded
 * samples, looping or stopping at the end) or the noise, times its
 * envelope and volumes, summed, times the main volume, clipped to 16 bits.
 * Called by fill. */
static void dsp_sample(s16 *out)
{
    int v, l = 0, r = 0;
    if (noise_clock && ++noise_count >= RATES[noise_clock]) {
        noise_count = 0;
        lfsr = (u16)((lfsr >> 1) | (((lfsr ^ (lfsr >> 1)) & 1) << 14));   /* bit 0 xor bit 1 in at bit 14 */
    }
    for (v = 0; v < 8; v++) {
        struct voice *x = &V[v];
        int s, o;
        if (!x->on) continue;
        envelope(x);
        if (x->noise) s = (s16)(lfsr << 1) >> 1;
        else {
            int n = pcm_len[x->srcn], i;
            if (!n) continue;
            i = (int)(x->pos >> 12);    /* the sample's index */
            if (i >= n) {               /* past the end: back by the loop's length, or stop */
                int ln = n - pcm_loop[x->srcn];
                if (ln <= 0) {
                    x->on = 0;
                    continue;
                }
                x->pos -= (u32)ln << 12;
                i -= ln;
            }
            {
                int a = pcm[x->srcn][i], b = pcm[x->srcn][i + 1 < n ? i + 1 : pcm_loop[x->srcn]];
                s = a + (((b - a) * (int)(x->pos & 0xFFF)) >> 12);
            }
            x->pos += x->pitch;         /* (32 kHz out, so the DSP's pitch steps as it is) */
        }
        o = (s * x->env) >> 10;       /* (the DSP: the 15-bit sample doubled) */
        l += (o * x->vl) >> 7;
        r += (o * x->vr) >> 7;
    }
    l = (l * master) >> 7;
    r = (r * master) >> 7;
    out[0] = (s16)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
    out[1] = (s16)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
}

/* ------------------------------------------------------------ the driver (driver.s) */
/* per player (0 music, 1 effects): */
static u16 ptr[2], start[2];            /* the next stream byte; the loop's start */
static u8 wait_[2], loops[2], active[2];        /* ticks to wait; loops at the end; playing */
/* per player and channel (pv = player x 7 + voice): note ($FF none), bend
   (bits 5..7 signed semitones, 0..4 32nds), attenuation, pan (bit 7 left,
   bit 6 right), instrument + 1 (0 none) */
static u8 note[14], bend[14], att[14], pan[14], patch[14];
static u8 pl, pv, vv, mvol;             /* the command's player, channel, voice; the music's extra attenuation */

/* is_free: may the current player change voice vv? The effects always;
   the music on voices 0..3, and on 4..6 only while no effect plays */
static int is_free(void) { return pl || vv < 4 || !active[1]; }

/* set_pitch: voice vv's pitch from channel pv's note and bend: the
   octave-8 table entry for the semitone and fine step, halved for each
   octave below (octave 9 and up: the highest pitch) */
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
}

/* set_volume: voice vv's volumes: the instrument's and note's attenuation
   (plus mvol for the music) through VOLTAB, to the sides the pan allows */
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
    loops[x] = aram[s];
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
 * end: loop or stop). Called by fill, every TICK_SAMPLES samples. */
static void run_player(int x)
{
    u16 sp;
    pl = (u8)x;
    if (!active[x]) return;
    if (wait_[x] && --wait_[x]) return;
    sp = ptr[x];
    for (;;) {
        u8 c = aram[sp++];
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
            u8 n = aram[sp++], y = aram[sp++];
            c_on(n, y);
            break;
        }
        case 0x88:
            if (vv != 6) {
                note[pv] = 0xFF;
                keyoff();
            }
            break;
        case 0x90: patch[pv] = (u8)(aram[sp++] + 1); break;
        case 0x98:
            bend[pv] = aram[sp++];
            if (note[pv] != 0xFF) set_pitch();
            break;
        case 0xA0:
            att[pv] = aram[sp++];
            set_volume();
            break;
        case 0xA8:
            pan[pv] = aram[sp++];
            set_volume();
            break;
        case 0xB0: {
            u8 n = aram[sp++];
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
static ndspWaveBuf wb[4];               /* 4 buffers queued on ndsp channel 0, refilled in turn */
static s16 *wbuf[4];                    /* their samples (linear memory: the DSP reads them) */
#define WB_SAMPLES 1024                 /* stereo samples a buffer */
static int tick_at, playing = 255;      /* samples to the next player tick; frames since the last effect */

/* sound_init3ds: nothing (called by plat.c's main; the work is in sound_init) */
void sound_init3ds(void) {}

/* (ui.c's sound_init: after the cartridge is in)
 * Builds aram (the engine image, length word first, at $C3A000 in the
 * cartridge; the spc_streams resource, entry 8 of the resource table,
 * whose bank is header byte 7), decodes the samples, starts ndsp (stereo,
 * 16-bit, 32 kHz, no interpolation) and queues 4 silent buffers. Without
 * ndsp (no DSP firmware) the game stays silent. */
void sound_init(void)
{
    u32 eng = 0xC30000UL + 0xA000, res;
    u16 n, i;
    int k;
    memset(aram, 0, sizeof aram);
    n = rom_u16le(eng);
    for (i = 0; i < n; i++) aram[0x200 + i] = rom_u8(eng + 2 + i);
    /* the streams: the resources' entry 8 */
    res = (u32)rom_u8(0xC0FFB0UL + 7) << 16;
    {
        u32 at = res + rom_u16le(res + 12 + 8 * 8) + ((u32)rom_u16le(res + 14 + 8 * 8) << 16);
        u32 len = rom_u16le(res + 16 + 8 * 8);
        for (i = 0; i < len && STREAMS + i < 65536; i++) aram[STREAMS + i] = rom_u8(at + i);
    }
    brr_decode();
    if (R_FAILED(ndspInit())) return;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnSetInterp(0, NDSP_INTERP_NONE);
    ndspChnSetRate(0, RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    for (k = 0; k < 4; k++) {
        wbuf[k] = (s16 *)linearAlloc(WB_SAMPLES * 4);
        memset(wbuf[k], 0, WB_SAMPLES * 4);
        memset(&wb[k], 0, sizeof wb[k]);
        wb[k].data_vaddr = wbuf[k];
        wb[k].nsamples = WB_SAMPLES;
        DSP_FlushDataCache(wbuf[k], WB_SAMPLES * 4);   /* (out of the CPU cache for the DSP) */
        ndspChnWaveBufAdd(0, &wb[k]);
    }
    ready = 1;
}

/* music_play: play music id (its stream pointer at STREAMS + 2 id) */
void music_play(u8 id)
{
    u16 s = (u16)(aram[STREAMS + 2 * id] | (aram[STREAMS + 2 * id + 1] << 8));
    stop_music();
    start_player(0, s);
}

/* music_stop / music_volume: stop the music; its extra attenuation */
void music_stop(void) { stop_music(); }
void music_volume(u8 v) { mvol = v; }

/* play_sfx: start effect id (its stream at STREAMS + 8 + 2 id), cutting any playing */
static void play_sfx(u8 id)
{
    u16 s = (u16)(aram[STREAMS + 8 + 2 * id] | (aram[STREAMS + 9 + 2 * id] << 8));
    sfx_off();
    start_player(1, s);
}

/* fill: n stereo samples into d, ticking the players every TICK_SAMPLES */
static void fill(s16 *d, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (--tick_at <= 0) {
            tick_at = TICK_SAMPLES;
            run_player(0);
            run_player(1);
        }
        dsp_sample(d + 2 * i);
    }
}

/* sound_tick: once a frame (plat.c's wait_vblank): start the next queued
 * effect if the last is over or has had MIN_FRAMES frames, then refill
 * every ndsp buffer that has finished playing and queue it again. */
void sound_tick(void)
{
    int k;
    if (!ready) {
        sfx_head = sfx_tail;            /* no sound: forget the effects */
        return;
    }
    /* the next effect (as the SNES's NMI: when the last is over or has had MIN_FRAMES) */
    if (playing < 255) playing++;
    if (sfx_head != sfx_tail && (!active[1] || playing >= MIN_FRAMES)) {
        u8 id = sfx_q[sfx_head];
        sfx_head = (sfx_head + 1) & 7;
        play_sfx(id);
        playing = 0;
    }
    for (k = 0; k < 4; k++)
        if (wb[k].status == NDSP_WBUF_DONE) {
            fill(wbuf[k], WB_SAMPLES);
            DSP_FlushDataCache(wbuf[k], WB_SAMPLES * 4);
            ndspChnWaveBufAdd(0, &wb[k]);
        }
}
