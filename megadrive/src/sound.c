/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * The Mega Drive game - sound effects: the original game's sound clips on the
 * YM2612 and the PSG's noise channel (tools/build_sfx.py makes them, the
 * driver in src/music.s plays them on FM voices 4 and 5 (counting from 0:
 * the YM2612's channels 5 and 6) and the noise, and gives those back to
 * the music afterwards).
 *
 * The rules ask for several effects in one turn (a shot, the monster's
 * pain, its answer): they queue, and the next one starts when the one
 * playing has ended or has had a third of a second, as the phone game cut
 * one clip short for the next.
 *
 * The queue: q[8], a ring of effect ids; qt (tail) is where the next id
 * goes, qh (head) the next to play; empty when qh == qt, so it holds 7 at
 * most. sfx() adds to it from the main program; sound_update(), run by the
 * frame interrupt (vdp.c vblank_sound), takes from it.
 * Licence: GPL-2.0-or-later.
 */
#include "game.h"

extern const unsigned char * const sfx_table[];    /* per SFX_* id its stream (build/gen/sfx_data.c) */
void sfx_play(const unsigned char *stream);         /* music.s: start an effect's stream */
u8 sfx_busy(void);                                  /* music.s: 1 while an effect plays */
void fm_init(void);                                 /* music.s: set up the YM2612 */
void music_stop(void);

#define MIN_FRAMES 20                 /* an effect plays this long before the next may cut in */

static volatile u8 q[8], qh, qt;    /* the queue (see above) */
static u8 playing_for;              /* frames since the last effect started (stops at 255) */
volatile u8 sfx_test;                 /* (the tests poke an effect's id here) */
volatile u8 grin_frames;              /* the status bar's grin after a pick-up (src/ui.c) */

/* queue effect `id` (SFX_*), from the rules or the menus. A pick-up also
   starts the status bar face's grin. Dropped if the queue is full. */
void sfx(u8 id)
{
    u8 t = qt, n = (t + 1) & 7;         /* the tail after this one (ring of 8) */
    if (id == SFX_PICKUP || id == SFX_CREDITS || id == SFX_WEAPON || id == SFX_KEY)
        grin_frames = 30;             /* half a second, as in the original */
    if (n == qh) return;              /* queue full: drop it */
    q[t] = id;
    qt = n;
}

/* a new input drops the effects still waiting from the last turn (main's
   loop, before each input is handled). The queue is emptied with the
   interrupts masked (SR $2700), then all interrupts are let in again (SR
   $2000), whatever the mask was before. */
void sfx_flush(void)
{
    __asm__ volatile("move #0x2700, %sr");
    qt = qh;
    __asm__ volatile("move #0x2000, %sr");
}

/* at start-up (main): every PSG voice silent, the YM2612 set up */
void sound_init(void)
{
    /* PSG latch bytes 1 cc 1 vvvv: voice cc's attenuation = 15 (off) */
    PSG_PORT = 0x9F;                /* tone 0 */
    PSG_PORT = 0xBF;                /* tone 1 */
    PSG_PORT = 0xDF;                /* tone 2 */
    PSG_PORT = 0xFF;                /* noise */
    fm_init();
}

/* from the frame interrupt, before music_update: counts the grin and the
   playing time down / up, and starts the next queued effect when the one
   playing has ended or has had MIN_FRAMES */
void sound_update(void)
{
    u8 id;
    if (sfx_test) {
        if (sfx_test == 0xFF) music_stop();   /* (tests: the effects alone) */
        else sfx(sfx_test);
        sfx_test = 0;
    }
    if (grin_frames) grin_frames--;
    if (playing_for < 255) playing_for++;
    if (qh == qt) return;                               /* nothing queued */
    if (sfx_busy() && playing_for < MIN_FRAMES) return; /* let this one play on */
    id = q[qh];
    qh = (qh + 1) & 7;
    if (!id || id >= SFX_COUNT) return;                 /* (0 and unknown ids: none) */
    sfx_play(sfx_table[id]);                            /* cuts the one playing short */
    playing_for = 0;
}
