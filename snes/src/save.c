/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the SNES: settings and the saved game in the cartridge's
 * battery-backed RAM: two blocks, each 'S' 'N' tag version, a length, the
 * data, a checksum (as the Mega Drive game's):
 *   0     settings (the buttons)
 *   64    the saved game: the whole game state S
 * Licence: GPL-2.0-or-later.
 *
 * A block in the battery RAM:
 *   +0  'S' 'N'         magic
 *   +2  tag             'S' settings, 'G' the game
 *   +3  1               the format version
 *   +4  u16 length n    big-endian
 *   +6  n data bytes
 *   +6+n u16 checksum   big-endian: start at $5A5A; per data byte rotate
 *                       left by one bit (16 bits) and XOR the byte in
 * A block is valid only if all of that matches, so a blank or corrupted
 * battery RAM reads as "nothing saved".
 *
 * Called by ui.c (the title screen, the pause menu, the options screen).
 * The byte reads and writes are save.s on the SNES and the machine layer
 * on the other ports (this file is also compiled for the 32X, 3DS, GBC and
 * GBA; the GBC build preprocesses it and passes it through
 * gbc/tools/bankify.py).
 */
#include "../../core/rules.h"
#include "save.h"

#define SET_OFF 0                       /* the settings block's offset in the battery RAM */
#define GAME_OFF 64                     /* the saved game's block */
#define N_BUTTONS 7                     /* buttons an action can go on (ui.c: A B X Y L R Select) */

/* A use, B pass, Y medkit, L / R weapons, X notebook */
static const u8 default_keys[N_ACTIONS] = {0, 1, 3, 4, 5, 2};

struct settings cfg;

/* write a block at off: its header, the n bytes at data, the checksum */
static void block_write(u16 off, u8 tag, const void *data, u16 n)
{
    const u8 *p = data;
    u16 s = 0x5A5A, i;
    sram_wr(off + 0, 'S');
    sram_wr(off + 1, 'N');
    sram_wr(off + 2, tag);
    sram_wr(off + 3, 1);
    sram_wr(off + 4, n >> 8);
    sram_wr(off + 5, n & 0xFF);
    for (i = 0; i < n; i++) {
        sram_wr(off + 6 + i, p[i]);
        s = (u16)((s << 1) | (s >> 15)) ^ p[i];     /* rotate left 1, XOR the byte */
    }
    sram_wr(off + 6 + n, s >> 8);
    sram_wr(off + 7 + n, s & 0xFF);
}

/* check the block at off: tag, version 1, length n and checksum. With copy
   set, its data are also copied to data as they are read (even if the
   checksum then fails), so callers check first with copy = 0.
   -> 1 valid, 0 not */
static u8 block_read(u16 off, u8 tag, void *data, u16 n, u8 copy)
{
    u8 *p = data;
    u16 i, c, s = 0x5A5A;
    if (sram_rd(off) != 'S' || sram_rd(off + 1) != 'N' || sram_rd(off + 2) != tag || sram_rd(off + 3) != 1 ||
        ((sram_rd(off + 4) << 8) | sram_rd(off + 5)) != n)
        return 0;                       /* not a block of this kind and size */
    for (i = 0; i < n; i++) {
        u8 v = sram_rd(off + 6 + i);
        s = (u16)((s << 1) | (s >> 15)) ^ v;
        if (copy) p[i] = v;
    }
    c = (sram_rd(off + 6 + n) << 8) | sram_rd(off + 7 + n);
    return c == s;
}

/* cfg = the default buttons */
void settings_defaults(void)
{
    u8 i;
    for (i = 0; i < N_ACTIONS; i++) cfg.key[i] = default_keys[i];
}

/* cfg = the saved settings if they are valid and every action has its own
   button; else the defaults */
void settings_load(void)
{
    struct settings t;
    u8 i, used = 0, bad = 0;            /* used: a bit per button already taken */
    if (block_read(SET_OFF, 'S', &t, sizeof t, 1)) {
        for (i = 0; i < N_ACTIONS; i++) {        /* each button once */
            if (t.key[i] >= N_BUTTONS || (used & (1 << t.key[i]))) bad = 1;
            else used |= 1 << t.key[i];
        }
        if (!bad) {
            cfg = t;
            return;
        }
    }
    settings_defaults();
}

/* write cfg to the battery RAM */
void settings_save(void)
{
    block_write(SET_OFF, 'S', &cfg, sizeof cfg);
}

/* -> 1 if there is a valid saved game (checks it without copying) */
u8 game_saved(void)
{
    return block_read(GAME_OFF, 'G', 0, sizeof(struct state), 0);
}

/* save the whole game state S */
void game_save(void)
{
    block_write(GAME_OFF, 'G', &S, sizeof(struct state));
}

/* load the saved game into S if it is valid -> 1; else 0 and S untouched */
u8 game_load(void)
{
    if (!block_read(GAME_OFF, 'G', 0, sizeof(struct state), 0)) return 0;  /* check first */
    block_read(GAME_OFF, 'G', &S, sizeof(struct state), 1);                 /* then copy into S */
    game_restore();                     /* the rules rebuild what is not saved (core/rules.c) */
    return 1;
}
