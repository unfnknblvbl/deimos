/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the Virtual Boy: the music and the sound effects (sound.c). Licence: GPL-2.0-or-later. */
#ifndef VB_SOUND_H
#define VB_SOUND_H
#include "../../core/types.h"
void sound_init(void);                  /* once (ui.c): the VSU's waves, the driver on */
void music_play(u8 id);                 /* start song id (0 title, 1 intro, 2 end) */
void music_stop(void);
void sound_tick(void);                  /* (each game frame: nothing; the driver runs in the timer's interrupt) */
void sound_frame(void);                 /* the timer's interrupt (plat.c): a frame of the song and the effect */
#endif
