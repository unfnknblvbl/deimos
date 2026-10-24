/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the 32X: the music and the sound effects (sound32x.c). Licence: GPL-2.0-or-later. */
/* (sound_init and music_play/stop/volume, which ui.c calls, are declared
 * in ui.c itself, as on the SNES.) */
#ifndef SOUND32X_H
#define SOUND32X_H
void sound_start(void);                 /* (the master) the slave plays from here on */
void sound_tick(void);                  /* (the master) each frame: the effects the game queued */
void slave_main(void);                  /* the slave SH-2's program (from crt0.s; never returns) */
#endif
