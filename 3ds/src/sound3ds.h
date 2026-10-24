/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the 3DS: the music and the sound effects (sound3ds.c). Licence: GPL-2.0-or-later. */
#ifndef SOUND3DS_H
#define SOUND3DS_H
/* (sound_init and music_play/stop/volume, which ui.c calls, are declared in ui.c) */
void sound_init3ds(void);               /* (empty; called once by plat.c's main) */
void sound_tick(void);                  /* each frame: the effects the game queued (sfx_q) */
#endif
