/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the Virtual Boy: the sound driver, the Game Boy Color's
 * (gbc/src/snd.c) on the VSU. It plays the Mega Drive game's streams -
 * the SNES game's spc_streams, read where they are in the SNES image (the
 * resources' entry 8) - a song and an effect at a time, the effect taking
 * its channels from the song while it plays. It runs in the timer's
 * interrupt (plat.c: 59.88 times a second, the Mega Drive game's frame
 * rate), so the music keeps time while the view is drawn. The effects
 * come from glue.c's queue (sfx_q).
 *
 * spc_streams (tools/snes_sound.py, little-endian): u16 the three songs'
 * and, after the number of effects (u16), each effect's ARAM address
 * ($2000 = the entry's start); a song is a loop byte then its stream, an
 * effect its stream. A stream (megadrive/tools/build_music.py's format),
 * a byte at a time:
 *   $01..$7F        wait that many frames
 *   $00             nothing
 *   $80+v n a       voice v: note n (MIDI) on, attenuation a
 *   $88+v           voice v off
 *   $90+v i         voice v's instrument i
 *   $98+v b         voice v's pitch bend (signed, 1/32 semitones)
 *   $A0+v a         voice v's attenuation
 *   $A8+v s         voice v's stereo (bit 7 left, bit 6 right)
 *   other $B0..$FE  one argument byte, ignored
 *   $FF             the end (a looping song starts over)
 * Channels: the VSU's 0..4 play waves (snd_wave: each instrument's FM
 * tone, tools/vb_sound.py), 5 the noise. A song's voices 0..4 go to
 * channels 0..4, its drums (voices 5, 6) to the noise; an effect's voices
 * 4, 5, 6 to channels 3, 4 and the noise.
 * Licence: GPL-2.0-or-later.
 */
#include "plat.h"
#include "sound.h"
#include "libc.h"

#define VOICES 7                /* the Mega Drive game's music voices: 0..4 tones, 5 the kick, 6 the drums */
#define NOISE 5                 /* the noise channel */
/* a stream being played: the song (mus) or the effect (efx) */
struct player {
    const u8 *p, *start;        /* the stream, its start; 0: off */
    u8 wait, loops, sfx;        /* frames to wait; loops at its end; 1 for the effect */
    const u8 *map;              /* voice -> channel (255: none) */
    u8 inst[VOICES], note[VOICES], att[VOICES];     /* each voice's instrument, note, attenuation */
    s8 bend[VOICES];                                /* its pitch bend (1/32 semitones) */
};
static struct player mus, efx;
static u8 owned;                /* the channels an effect has (bit per channel) */
static u8 pan[6] = {0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0};   /* each channel's sides (bit 7 left, 6 right) */
static const u8 *streams;       /* spc_streams in the SNES image */
static volatile u8 snd_on;      /* the interrupt's part runs only when 1 (off while music_play changes the song) */
static const u8 SONG_MAP[VOICES] = {0, 1, 2, 3, 4, NOISE, NOISE};
static const u8 FX_MAP[VOICES] = {255, 255, 255, 255, 3, 4, NOISE};
extern const u8 snd_wave[5][32], snd_n_inst, snd_inst[][4], snd_drum[9][3], snd_vol[128];   /* (snd_tab.c) */
extern const u16 snd_per[96];

static u16 le16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }    /* (a little-endian u16) */

/* freq: a wave channel's 11-bit frequency register for a note (MIDI) and
   bend (1/32 semitones), the wave holding `two` periods or one. snd_per
   holds the period (156250 / Hz) of notes 48..59 in 1/8 semitones; each
   octave down doubles it; below what the register can reach (2047), an
   octave up */
static u16 freq(u8 note, s8 bend, u8 two)
{
    s16 i = ((s16)note - 48 - (two ? 12 : 0)) * 8 + (bend >> 2);   /* (1/8 semitones from the table's first note) */
    u32 per;
    s8 oct = 0;
    while (i < 0) {
        i += 96;
        oct--;
    }
    while (i >= 96) {
        i -= 96;
        oct++;
    }
    per = snd_per[i];
    if (oct > 0) per >>= oct;
    else
        while (oct < 0 && per * 2 < 2048) {     /* (an octave down while it fits) */
            per *= 2;
            oct++;
        }
    if (per < 1) per = 1;
    return (u16)(2048 - per);
}

/* ch_off: channel ch silenced (stopped) */
static void ch_off(u8 ch) { VSU_CH(ch, VSU_INT) = 0; }

/* lrv: channel ch's left / right volumes at volume v (0..15), on its sides */
static u8 lrv(u8 ch, u8 v)
{
    return (u8)(((pan[ch] & 0x80) ? v << 4 : 0) | ((pan[ch] & 0x40) ? v : 0));
}

/* key_on: voice v of pl: note n on at attenuation a (0.75 dB steps). An
   effect takes the channel (owned); the song's notes on an owned channel
   are dropped. The noise channel plays a drum preset instead of a note */
static void key_on(struct player *pl, u8 v, u8 n, u8 a)
{
    u8 ch = pl->map[v], vol;
    const u8 *in;
    u16 f, at;
    if (ch > NOISE) return;
    if (!pl->sfx && (owned & (1 << ch))) return;
    if (pl->sfx) owned |= 1 << ch;
    pl->note[v] = n;
    pl->att[v] = a;
    if (ch == NOISE) {                  /* the noise: a drum (voice 6) or the kick (5) */
        const u8 *d = snd_drum[v == 6 ? (n & 7) : 8];
        f = (u16)(d[1] << 3);
        VSU_CH(ch, VSU_INT) = 0;
        VSU_CH(ch, VSU_LRV) = lrv(ch, snd_vol[a > 127 ? 127 : a]);
        VSU_CH(ch, VSU_FQL) = (u8)f;
        VSU_CH(ch, VSU_FQH) = (u8)(f >> 8);
        VSU_CH(ch, VSU_EV0) = d[0];     /* (start volume, direction, step time) */
        VSU_CH(ch, VSU_EV1) = (u8)((d[2] << 4) | 1);   /* (its tap; the envelope on: every drum fades) */
        VSU_CH(ch, VSU_INT) = 0x80;     /* (play) */
        return;
    }
    in = snd_inst[pl->inst[v] < snd_n_inst ? pl->inst[v] : 0];     /* (wave, two periods, pace, base attenuation) */
    at = (u16)in[3] + a;
    vol = snd_vol[at > 127 ? 127 : at];
    if (!vol) {
        ch_off(ch);
        return;
    }
    f = freq(n, pl->bend[v], in[1]);
    VSU_CH(ch, VSU_INT) = 0;
    VSU_CH(ch, VSU_LRV) = lrv(ch, vol);
    VSU_CH(ch, VSU_FQL) = (u8)f;
    VSU_CH(ch, VSU_FQH) = (u8)(f >> 8);
    VSU_CH(ch, VSU_EV0) = (u8)(0xF0 | (in[2] ? in[2] - 1 : 0));    /* (full, falling at the instrument's pace) */
    VSU_CH(ch, VSU_EV1) = in[2] ? 1 : 0;                            /* (the envelope on if it decays) */
    VSU_CH(ch, VSU_RAM) = in[0];                                    /* (its wave) */
    VSU_CH(ch, VSU_INT) = 0x80;
}

/* key_off: voice v of pl off (the noise's drums end by themselves) */
static void key_off(struct player *pl, u8 v)
{
    u8 ch = pl->map[v];
    if (ch >= NOISE) return;
    if (!pl->sfx && (owned & (1 << ch))) return;
    ch_off(ch);
}

/* bend: voice v's pitch bend b: the frequency rewritten without a restart */
static void bend(struct player *pl, u8 v, s8 b)
{
    u8 ch = pl->map[v];
    u16 f;
    pl->bend[v] = b;
    if (ch >= NOISE) return;
    if (!pl->sfx && (owned & (1 << ch))) return;
    f = freq(pl->note[v], b, snd_inst[pl->inst[v] < snd_n_inst ? pl->inst[v] : 0][1]);
    VSU_CH(ch, VSU_FQL) = (u8)f;
    VSU_CH(ch, VSU_FQH) = (u8)(f >> 8);
}

/* stereo: voice v's channel panned: s bit 7 the left side, bit 6 the right */
static void stereo(struct player *pl, u8 v, u8 s)
{
    u8 ch = pl->map[v];
    if (ch > NOISE) return;
    if (!pl->sfx && (owned & (1 << ch))) return;
    pan[ch] = s & 0xC0;
}

/* release: an effect over: its channels back to the song */
static void release(void)
{
    u8 ch;
    for (ch = 0; ch <= NOISE; ch++)
        if (owned & (1 << ch)) ch_off(ch);
    owned = 0;
}

/* run: a frame of pl's stream: when its wait is over, the commands up to
   the next wait (or the end) are carried out */
static void run(struct player *pl)
{
    const u8 *p = pl->p;
    if (!p) return;
    if (pl->wait && --pl->wait) return; /* (still waiting) */
    for (;;) {
        u8 c = *p++, v;
        if (c < 0x80) {
            if (c) {
                pl->wait = c;
                break;
            }
            continue;
        }
        if (c == 0xFF) {
            if (pl->loops) {
                p = pl->start;
                continue;
            }
            p = 0;
            if (pl->sfx) release();
            break;
        }
        v = c & 7;                      /* (the voice: the low 3 bits) */
        if (v >= VOICES) v = VOICES - 1;
        switch (c & 0xF8) {
        case 0x80: key_on(pl, v, p[0], p[1]); p += 2; break;
        case 0x88: key_off(pl, v); break;
        case 0x90: pl->inst[v] = *p++; break;
        case 0x98: bend(pl, v, (s8)*p++); break;
        case 0xA0: pl->att[v] = *p++; break;
        case 0xA8: stereo(pl, v, *p++); break;
        default: p++; break;
        }
    }
    pl->p = p;
}

/* start: pl on the stream at ARAM address at (a song: its loop byte first) */
static void start(struct player *pl, u16 at, u8 song)
{
    const u8 *s = streams + (u16)(at - 0x2000);
    u8 v;
    pl->p = 0;
    for (v = 0; v < VOICES; v++) {
        pl->inst[v] = 0;
        pl->bend[v] = 0;
    }
    pl->map = song ? SONG_MAP : FX_MAP;
    pl->loops = song ? *s++ : 0;
    pl->start = s;
    pl->wait = 0;
    pl->sfx = !song;
    pl->p = s;
}

/* sound_frame: the timer's interrupt: a waiting effect started (it
   replaces the one playing), then a frame of the song and of the effect */
void sound_frame(void)
{
    if (!snd_on) return;
    if (sfx_head != sfx_tail) {         /* the next effect asked for */
        u8 id = sfx_q[sfx_head];
        sfx_head = (sfx_head + 1) & 7;
        if (id && id < le16(streams + 6)) {
            release();                  /* (the last effect's channels back to the song) */
            start(&efx, le16(streams + 8 + 2 * id), 0);
        }
    }
    run(&mus);                          /* (the song's frame) */
    run(&efx);                          /* (the effect's, after: it wins its channels) */
}

/* sound_init: the streams found (the SNES resources' entry 8), every
   channel stopped, the five waves written (the VSU takes them only while
   every channel is stopped), the driver on */
void sound_init(void)
{
    const u8 *rb = snes_ptr((u32)(snes_rom[0xFFB7] & 0x3F) << 16 | 0xC00000), *e = rb + 12 + 8 * 8;
    u32 w, i;
    streams = rb + (e[0] | (e[1] << 8) | (e[2] << 16) | ((u32)e[3] << 24));
    VSU_SSTOP = 1;                      /* (every channel stopped) */
    for (w = 0; w < 5; w++)
        for (i = 0; i < 32; i++) VSU_WAVE(w, i) = snd_wave[w][i];
    mus.p = efx.p = 0;
    snd_on = 1;
}

/* music_play: song id (0..2) started, any other id: the song stopped. The
   channels an effect holds are left alone. snd_on is cleared meanwhile so
   the interrupt does not run a half-set player */
void music_play(u8 id)
{
    u8 ch;
    snd_on = 0;
    for (ch = 0; ch <= NOISE; ch++)
        if (!(owned & (1 << ch))) ch_off(ch);
    if (id < 3) start(&mus, le16(streams + 2 * id), 1);
    else mus.p = 0;
    snd_on = 1;
}

void music_stop(void) { music_play(0xFF); }     /* (any id past 2: silence) */
void sound_tick(void) {}                        /* (the driver runs in the timer's interrupt) */
