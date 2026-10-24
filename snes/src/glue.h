/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the SNES: the platform side of the game core. Licence: GPL-2.0-or-later. */
/* The "game core" is the portable rules (core/rules.c); glue.c provides
 * what they ask the platform for. This header shares glue.c's two queues
 * with ui.c (which shows the messages) and sound.s (which plays the
 * effects). Also used by the 32X, 3DS, GBC and GBA ports. */
#ifndef GLUE_H
#define GLUE_H
#include "../../core/types.h"
#define MSGQ 8                          /* message queue length (a power of two) */
extern char msgq[MSGQ][32];             /* ring of messages, each up to 31 chars and a NUL */
extern u8 msg_head, msg_count;          /* the oldest message's slot; how many are waiting */
extern u16 msg_total;                   /* messages ever queued (wraps); counted, not read by ui.c */
extern u8 sfx_q[8], sfx_head, sfx_tail; /* ring of sound effect ids: read at head, written at tail */
char *utoa10(u16 n);                    /* n in decimal, in a static buffer */
#endif
