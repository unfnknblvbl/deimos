/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the Game Boy Color: the sound, as ui.c asks for it (the
 * driver: snd.c, in bank 0). Licence: GPL-2.0-or-later.
 *
 * The same four functions the SNES port's ui.c calls, so ui.c compiles
 * unchanged; each is a banked wrapper around snd.c. */
#pragma bank 255
#include "plat.h"
#include "sound.h"
#include "snd.h"
void sound_init(void) BANKED { snd_init(); }            /* start the driver */
void music_play(u8 id) BANKED { snd_music(id); }        /* song id (MUS_*: 0..2) */
void music_stop(void) BANKED { snd_music(0xFF); }       /* (any id past 2: silence) */
void sound_tick(void) BANKED {}                         /* nothing: snd.c runs in the interrupt */
