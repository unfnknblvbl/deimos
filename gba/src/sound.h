/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the Game Boy Advance: the music and the sound effects (sound.c). Licence: GPL-2.0-or-later. */
#ifndef GBA_SOUND_H
#define GBA_SOUND_H
#include "../../core/types.h"
void sound_init(void);                  /* once (ui.c): the data, the timer, the DMA */
void music_play(u8 id);                 /* start music id */
void music_stop(void);
void sound_tick(void);                  /* each frame: the queued effects */
void sound_mix(void);                   /* (the vertical blank's interrupt: the next frame's samples) */
#endif
