/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the SNES: what the game core asks of the platform - the
 * maps' data (copied out of the ROM into work RAM), messages and sounds.
 * Licence: GPL-2.0-or-later. */
/* The rules (core/rules.c, the "game core" shared by every port) call:
 *   map_level, level_string   the current map's level data and strings,
 *                             copied out of the campaign block (campaign.c)
 *   say, say2, say3, say_num  a message for the status line
 *   sfx                       a sound effect
 * The messages wait in msgq for ui.c to show them one after another; the
 * effects wait in sfx_q for sound.s to send to the sound chip. This file
 * is also compiled for the 32X, 3DS, GBC and GBA ports. */
#include "../../core/rules.h"
#include "campaign.h"
#include "glue.h"

#define LEVEL_BUF 20480                  /* bytes: the largest level data it holds (longer is cut) */
static u8 level_buf[LEVEL_BUF];         /* the current map's level data (the rules' format) */
static u8 loaded_map = 0xFF;            /* the map in level_buf; 0xFF: none yet */
static u32 strings_off;                 /* the map's strings blob in the block */

/* make map the loaded one: copy its level data (pack entry P_LEVEL) into
   level_buf and note where its strings are; nothing if already loaded */
static void load(u8 map)
{
    u32 n;
    u32 off;
    if (map == loaded_map) return;
    off = pack_entry(map, P_LEVEL, &n);         /* where the level data is, and its size */
    if (n > LEVEL_BUF) n = LEVEL_BUF;
    blk_copy(off, level_buf, (u16)n);           /* into work RAM */
    strings_off = pack_entry(map, P_STRINGS, 0);
    loaded_map = map;
}

/* the map's level data, for the rules (the pointer stays valid until
   another map is asked for) */
const u8 *map_level(u8 map)
{
    load(map);
    return level_buf;
}

/* the map's string id, copied out of the ROM into one of four buffers
   (the game's longest is 421 characters) */
#define STR_BUFS 4                      /* buffers used in turn (a power of two) */
#define STR_MAX 767                     /* the most characters a string may have */
static char str_buf[STR_BUFS][STR_MAX + 1];
static u8 str_next;                     /* the buffer the next call fills */

/* string id of the loaded map. The strings blob starts with a big-endian
   word per string: its offset from the blob's start. The result stays
   valid for the next three calls (four buffers in turn). */
const char *level_string(u8 id)
{
    char *b = str_buf[str_next];
    u32 at = strings_off + blk_u16(strings_off + 2 * (u16)id);  /* the string's place */
    str_next = (str_next + 1) & (STR_BUFS - 1);
    blk_strcpy(at, b, STR_MAX);                                 /* copy it out */
    return b;
}

/* ---- messages: a queue the status line shows one after another */
char msgq[MSGQ][32];
u8 msg_head, msg_count;
u16 msg_total;

/* queue the message a + b + c (any may be 0), cut at 31 characters. When
   the queue is full the oldest message is dropped. */
static void push3(const char *a, const char *b, const char *c)
{
    u8 slot, n = 0;
    char *d;
    if (msg_count == MSGQ) {            /* full: drop the oldest */
        msg_head = (msg_head + 1) & (MSGQ - 1);
        msg_count--;
    }
    slot = (msg_head + msg_count) & (MSGQ - 1);     /* the first free slot */
    d = msgq[slot];
    while (a && *a && n < 31) d[n++] = *a++;
    while (b && *b && n < 31) d[n++] = *b++;
    while (c && *c && n < 31) d[n++] = *c++;
    d[n] = 0;
    msg_count++;
    msg_total++;
}

/* the rules' message calls (core/rules.h) */
void say(const char *s) { push3(s, 0, 0); }
void say2(const char *a, const char *b) { push3(a, b, 0); }
void say3(const char *a, const char *b, const char *c) { push3(a, b, c); }

static char numbuf[6];                  /* utoa10's buffer: up to 5 digits and a NUL */

/* n in decimal (no leading zeros), built from the end of numbuf; the next
   call overwrites it */
char *utoa10(u16 n)
{
    u8 i = 5;
    numbuf[5] = 0;
    do {
        numbuf[--i] = '0' + n % 10;
        n /= 10;
    } while (n);
    return numbuf + i;
}

/* a + the number n + b */
void say_num(const char *a, u16 n, const char *b) { push3(a, utoa10(n), b); }

/* ---- sounds: queued for the sound driver */
/* An 8-entry ring: sfx() writes at sfx_tail, sound.s reads at sfx_head
   (in the vertical blank) and sends each id to the SPC700. head == tail
   means empty, so it holds at most 7. */
u8 sfx_q[8], sfx_head, sfx_tail;
extern u8 grin_frames;                  /* the status bar's grin after a pick-up (ui.c) */

/* queue sound effect id (SFX_*, core/rules.h); a pick-up also makes the
   status bar's face grin for 30 frames (half a second) */
void sfx(u8 id)
{
    if (id == SFX_PICKUP || id == SFX_CREDITS || id == SFX_WEAPON || id == SFX_KEY) grin_frames = 30;
    sfx_q[sfx_tail] = id;
    sfx_tail = (sfx_tail + 1) & 7;
    if (sfx_tail == sfx_head) sfx_head = (sfx_head + 1) & 7;    /* full: drop the oldest */
}
