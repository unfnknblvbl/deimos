/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the Game Boy Color: the sound driver (bank 0: it runs in
 * the vertical blank interrupt, so the music keeps time while the view is
 * drawn). It plays the Mega Drive game's streams (tools/gb_sound.py) from the sound's
 * ROM bank: a song and an effect at a time, the effect taking its channels
 * from the song while it plays. The effects come from plat.c's queue.
 *
 * The sound blob (resource RES_SOUND, made by tools/gb_res.py; it starts
 * a ROM bank, mapped at $4000 = base while the driver runs):
 *   0   "GBSN"
 *   4   u16 the three songs' offsets (MUS_TITLE, MUS_INTRO, MUS_END)
 *   10  u16 the number of effects, 12 u16 each effect's offset
 *   a song: 7 bytes (each voice's channel), u8 loops, then its stream
 *   an effect: its stream (voices 4, 5, 6 on channels 0, 1, 3)
 * All big-endian. A stream (megadrive/tools/build_music.py format), a
 * byte at a time:
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
 * Channels: 0, 1 the square waves (NR1x, NR2x), 2 the wave channel (NR3x),
 * 3 the noise (NR4x).
 * Licence: GPL-2.0-or-later.
 */
#include <gb/gb.h>
#include <string.h>
#include "plat.h"
#include "snd.h"

#define VOICES 7                /* the Mega Drive game's music voices: 0..4 tones, 5 the kick, 6 the drums */
/* a stream being played: the song (mus) or the effect (efx) */
struct player {
    const u8 *p, *start;        /* the stream (in the sound's bank), its start; 0: off */
    u8 wait, loops, sfx;        /* frames to wait; loops at its end; 1 for the effect */
    u8 map[VOICES];             /* voice -> channel (255: none) */
    u8 inst[VOICES], note[VOICES], att[VOICES];     /* each voice's instrument, note, attenuation */
    s8 bend[VOICES];                                /* its pitch bend (1/32 semitones) */
};
static struct player mus, efx;
static u8 owned;                /* the channels an effect has (bit per channel) */
static u8 snd_bank;             /* the ROM bank the sound blob is in */
static const u8 *base;          /* the blob, mapped */
extern const u8 snd_inst[][3], snd_drum[9][2], snd_vol[128];   /* (snd_tab.c, tools/gb_sound.py) */
extern const u16 snd_per[96];
static u8 pan = 0xFF;           /* NR51's value: bits 4..7 left, 0..3 right, a bit per channel */
static volatile u8 snd_on;      /* the interrupt's part runs only when 1 (off while snd_music changes the song) */

static u16 be16(const u8 *p) { return ((u16)p[0] << 8) | p[1]; }    /* (a big-endian u16) */

/* a channel's 11-bit frequency for a note (MIDI) and bend (1/32 semitones);
   the wave channel sounds an octave lower for the same value. snd_per
   holds the period (131072 / Hz) of notes 36..47 in 1/8 semitones; each
   octave up halves it. The register value is 2048 - period */
static u16 freq(u8 ch, u8 note, s8 bend)
{
    s16 i = ((s16)note - (ch == 2 ? 24 : 36)) * 8 + (bend >> 2);    /* (1/8 semitones from the table's first note) */
    u8 oct = 0;
    u16 per;
    while (i < 0) i += 96;              /* (below the table: an octave up, as near as it goes) */
    while (i >= 96) {
        i -= 96;
        oct++;
    }
    per = snd_per[i] >> oct;
    if (per < 1) per = 1;
    return 2048 - per;
}

/* channel ch silenced: its volume envelope register (NRx2) to 0, or for
   the wave channel NR30 (its DAC) off */
static void ch_off(u8 ch)
{
    if (ch == 0) NR12_REG = 0;
    else if (ch == 1) NR22_REG = 0;
    else if (ch == 2) NR30_REG = 0;
    else NR42_REG = 0;
}

/* voice v of pl: note n on at attenuation a (0.75 dB steps). An effect
   takes the channel (owned); the song's notes on an owned channel are
   dropped. The noise channel plays a drum preset instead of a note */
static void key_on(struct player *pl, u8 v, u8 n, u8 a)
{
    u8 ch = pl->map[v], vol, env;
    const u8 *in;
    u16 f;
    if (ch > 3) return;
    if (!pl->sfx && (owned & (1 << ch))) return;
    if (pl->sfx) owned |= 1 << ch;
    pl->note[v] = n;
    pl->att[v] = a;
    if (ch == 3) {                          /* the noise: a drum (voice 6) or the kick (5) */
        const u8 *d = snd_drum[v == 6 ? (n & 7) : 8];
        NR42_REG = d[0];                /* (NR42: start volume, envelope) */
        NR43_REG = d[1];                /* (NR43: the noise's clock and width) */
        NR44_REG = 0x80;                /* (NR44 bit 7: trigger) */
        return;
    }
    in = snd_inst[pl->inst[v] < 10 ? pl->inst[v] : 0];  /* (duty, envelope pace, base attenuation) */
    {
        u16 at = (u16)in[2] + a;
        vol = snd_vol[at > 127 ? 127 : at];     /* (total attenuation -> volume 0..15) */
    }
    if (!vol) {
        ch_off(ch);
        return;
    }
    env = (u8)(vol << 4) | in[1];           /* (a falling envelope when the instrument decays) */
    f = freq(ch, n, pl->bend[v]);
    if (ch == 0) {
        NR10_REG = 0;                   /* (no frequency sweep) */
        NR11_REG = (u8)(in[0] << 6);    /* (bits 6..7: the duty) */
        NR12_REG = env;                 /* (bits 4..7 volume, 0..2 envelope pace, decreasing) */
        NR13_REG = (u8)f;               /* (the frequency's low 8 bits) */
        NR14_REG = 0x80 | (u8)(f >> 8); /* (trigger, the frequency's high 3 bits) */
    } else if (ch == 1) {
        NR21_REG = (u8)(in[0] << 6);
        NR22_REG = env;
        NR23_REG = (u8)f;
        NR24_REG = 0x80 | (u8)(f >> 8);
    } else {
        NR30_REG = 0x80;                /* (the wave channel's DAC on) */
        NR32_REG = vol >= 11 ? 0x20 : vol >= 6 ? 0x40 : 0x60;  /* (its output level: 100%, 50%, 25%) */
        NR33_REG = (u8)f;
        NR34_REG = 0x80 | (u8)(f >> 8);
    }
}

/* voice v of pl off (the noise channel's drums end by themselves) */
static void key_off(struct player *pl, u8 v)
{
    u8 ch = pl->map[v];
    if (ch > 2) return;
    if (!pl->sfx && (owned & (1 << ch))) return;
    ch_off(ch);
}

/* voice v's pitch bend b: the frequency rewritten without a trigger (bit 7
   clear), so the note goes on */
static void bend(struct player *pl, u8 v, s8 b)
{
    u8 ch = pl->map[v];
    u16 f;
    pl->bend[v] = b;
    if (ch > 2) return;
    if (!pl->sfx && (owned & (1 << ch))) return;
    f = freq(ch, pl->note[v], b);
    if (ch == 0) {
        NR13_REG = (u8)f;
        NR14_REG = (u8)(f >> 8);
    } else if (ch == 1) {
        NR23_REG = (u8)f;
        NR24_REG = (u8)(f >> 8);
    } else {
        NR33_REG = (u8)f;
        NR34_REG = (u8)(f >> 8);
    }
}

/* voice v's channel panned: s bit 7 the left output, bit 6 the right
   (NR51, $FF25) */
static void stereo(struct player *pl, u8 v, u8 s)
{
    u8 ch = pl->map[v], m = (u8)(0x11 << ch);
    if (ch > 3) return;
    pan &= ~m;
    if (s & 0x80) pan |= m & 0xF0;
    if (s & 0x40) pan |= m & 0x0F;
    NR51_REG = pan;
}

static void release(void)                   /* an effect over: its channels back to the song */
{
    u8 ch;
    for (ch = 0; ch < 4; ch++)
        if (owned & (1 << ch)) ch_off(ch);
    owned = 0;
}

/* a frame of pl's stream: when its wait is over, the commands up to the
   next wait (or the end) are carried out */
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

/* pl started on the stream at offset at of the blob: a song has its
   voice map and loop flag first; an effect uses FX_MAP */
static void start(struct player *pl, u16 at, u8 song)
{
    const u8 *s = base + at;
    u8 v;
    pl->p = 0;
    for (v = 0; v < VOICES; v++) {
        pl->inst[v] = 0;
        pl->bend[v] = 0;
    }
    if (song) {
        memcpy(pl->map, s, VOICES);
        pl->loops = s[VOICES];
        s += VOICES + 1;
    } else {
        static const u8 FX_MAP[VOICES] = {255, 255, 255, 255, 0, 1, 3};
        memcpy(pl->map, FX_MAP, VOICES);
        pl->loops = 0;
    }
    pl->start = s;
    pl->wait = 0;
    pl->sfx = !song;
    pl->p = s;
}

/* the vertical blank's part: a waiting effect started (it replaces the
   one playing), then a frame of the song and of the effect. Runs in the
   interrupt, so it maps the sound's ROM bank and puts the interrupted
   code's bank back */
static void snd_isr(void)
{
    u8 b;
    if (!snd_on) return;
    b = _current_bank;
    SWITCH_ROM(snd_bank);
    if (sfx_head != sfx_tail) {             /* the next effect asked for */
        u8 id = sfx_q[sfx_head];
        sfx_head = (sfx_head + 1) & 7;
        if (id && id < be16(base + 10)) {
            release();                  /* (the last effect's channels back to the song) */
            start(&efx, be16(base + 12 + 2 * id), 0);
        }
    }
    run(&mus);                          /* (the song's frame) */
    run(&efx);                          /* (the effect's, after: it wins its channels) */
    SWITCH_ROM(b);
}

/* the sound on: the blob's bank found, the APU set up, the wave channel's
   wave written, snd_isr added to GBDK's vertical blank handlers. Called
   once (sound.c's sound_init) */
void snd_init(void)
{
    u32 at = res_at(RES_SOUND);
    u8 b = _current_bank, i;
    static const u8 WAVE[16] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
                                0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10};
    snd_bank = (u8)(DATA_BANK0 + (u8)(at >> 14));
    base = (const u8 *)0x4000;          /* (gb_res.py puts the blob at a bank's start) */
    NR52_REG = 0x80;                    /* (NR52: the sound on) */
    NR50_REG = 0x77;                    /* (NR50: both outputs at full volume) */
    NR51_REG = 0xFF;                    /* (NR51: every channel to both sides) */
    NR30_REG = 0;                       /* (the wave channel off while its RAM is written) */
    for (i = 0; i < 16; i++) (&AUD3WAVE[0])[i] = WAVE[i];  /* (wave RAM, $FF30: a triangle, 32 4-bit samples) */
    mus.p = efx.p = 0;
    disable_interrupts();
    add_VBL(snd_isr);                   /* (GBDK: called on every vertical blank interrupt) */
    snd_on = 1;
    enable_interrupts();
}

/* song id (0..2) started, any other id: the song stopped. The channels an
   effect holds are left alone. snd_on is cleared meanwhile so the
   interrupt does not run a half-set player */
void snd_music(u8 id)
{
    u8 b = _current_bank, ch;
    snd_on = 0;
    SWITCH_ROM(snd_bank);
    for (ch = 0; ch < 4; ch++)
        if (!(owned & (1 << ch))) ch_off(ch);
    if (id < 3) start(&mus, be16(base + 4 + 2 * id), 1);
    else mus.p = 0;
    SWITCH_ROM(b);
    snd_on = 1;
}
