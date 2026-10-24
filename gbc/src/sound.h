/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the Game Boy Color: the music and the sound effects (sound.c). Licence: GPL-2.0-or-later.
 * ui.c's sound calls (the SNES port's names); the driver is snd.c. */
#ifndef GBC_SOUND_H
#define GBC_SOUND_H
#include "../../core/types.h"
#include <gb/gb.h>
void sound_init(void) BANKED;           /* start the sound driver */
void music_play(u8 id) BANKED;          /* play song id (0 title, 1 intro, 2 end) */
void music_stop(void) BANKED;           /* stop the song */
void sound_tick(void) BANKED;           /* (nothing to do on the Game Boy) */
#endif
