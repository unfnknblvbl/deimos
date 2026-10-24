/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the Game Boy Color: the sound driver (snd.c, bank 0). Licence: GPL-2.0-or-later.
 * sound.c is its banked front for ui.c; the core asks for effects through
 * plat.c's sfx(), which puts them in sfx_q. */
#ifndef GBC_SND_H
#define GBC_SND_H
#include "../../core/types.h"
extern u8 sfx_q[8], sfx_head, sfx_tail;  /* the effects asked for: a ring of 8 (plat.c fills, snd.c empties) */
void snd_init(void);            /* the sound chip on, the driver on the vertical blank interrupt */
void snd_music(u8 id);          /* 0..2 (MUS_*), else silence */
#endif
