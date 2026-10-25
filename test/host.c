/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* The portable core on the build machine, for the lockstep test
 * (test/lockstep_host.py): the platform hooks, the maps read from
 * megadrive/build/gen/maps/<name>/{level,strings}.bin and the game's texts
 * from megadrive/build/gen/campaign.bin.
 * lockstep_host.py loads it with ctypes and calls host_load, then the
 * core's own functions (game_init, act_*, resume, map_change); the host_*
 * functions give it the state and the messages. Sounds are ignored.
 * Licence: GPL-2.0-or-later. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/rules.h"

static unsigned char *levels[32], *strings[32];     /* each map's level blob and strings, in campaign order */
static unsigned char *block;                        /* the campaign block (for its texts table) */
static char msgbuf[65536];                          /* the messages since the last host_messages, a line each */
static size_t msglen;

/* a whole file in a malloc'd buffer (0 if it cannot be opened) */
static unsigned char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    unsigned char *b;
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) n = 0;
    fclose(f);
    return b;
}

/* dir/<names[i]>/level.bin, strings.bin: names is a comma-separated list;
   and dir/../campaign.bin, the campaign block (the game's texts). Returns
   the maps loaded, or -1 if one is missing */
int host_load(const char *dir, const char *names)
{
    char path[1024], name[64];
    int i = 0;
    const char *p = names;
    snprintf(path, sizeof path, "%s/../campaign.bin", dir);
    block = slurp(path);
    if (!block) return -1;
    while (*p) {
        int k = 0;
        while (*p && *p != ',') name[k++] = *p++;
        name[k] = 0;
        if (*p) p++;
        snprintf(path, sizeof path, "%s/%s/level.bin", dir, name);
        levels[i] = slurp(path);
        snprintf(path, sizeof path, "%s/%s/strings.bin", dir, name);
        strings[i] = slurp(path);
        if (!levels[i] || !strings[i]) return -1;
        i++;
    }
    return i;
}

static u8 cur_map;                                  /* the map map_level last gave */
/* the core's hooks: the level blob, the map's string id (big-endian u16
   offsets at the strings' start) */
const u8 *map_level(u8 map) { cur_map = map; return levels[map]; }
const char *level_string(u8 id)
{
    const u8 *s = strings[cur_map];
    return (const char *)s + ((s[2 * id] << 8) | s[2 * id + 1]);
}

/* the core's hook: the game's text id, from the block's texts table (its
   offset is the big-endian u32 at byte 24; a big-endian u16 offset per
   text, then the texts) */
const char *gtext(u8 id)
{
    const u8 *t = block + (((u32)block[24] << 24) | ((u32)block[25] << 16) | (block[26] << 8) | block[27]);
    return (const char *)t + ((t[2 * id] << 8) | t[2 * id + 1]);
}

/* s appended to msgbuf (dropped when it is full) */
static void put(const char *s)
{
    size_t n = strlen(s);
    if (msglen + n + 2 < sizeof msgbuf) {
        memcpy(msgbuf + msglen, s, n);
        msglen += n;
    }
}
/* the core's message and sound hooks */
void say(const char *s) { put(s); put("\n"); }
void say2(const char *a, const char *b) { put(a); put(b); put("\n"); }
void say3(const char *a, const char *b, const char *c) { put(a); put(b); put(c); put("\n"); }
void say_num(const char *a, u16 n, const char *b)
{
    char t[8];
    snprintf(t, sizeof t, "%u", (unsigned)n);
    put(a); put(t); put(b); put("\n");
}
void sfx(u8 id) { (void)id; }

/* for lockstep_host.py: the messages so far (then forgotten), S, its size,
   the dialog and door code the core waits on (255: none) */
const char *host_messages(void) { msgbuf[msglen] = 0; msglen = 0; return msgbuf; }
unsigned char *host_state(void) { return (unsigned char *)&S; }
int host_state_size(void) { return STATE_SIZE; }
int host_dialog(void) { return dialog_str; }
int host_password(void) { return pw_want; }
