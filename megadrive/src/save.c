/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * The Mega Drive game - settings and the saved game in battery-backed RAM.
 *
 * The cartridge RAM sits at $200001, one byte every other address. Two
 * blocks, each with a magic word, a length and a checksum:
 *   0     settings (movement, button mapping)
 *   64    the saved game: the whole game state struct S
 * $A130F1 = 1 switches the RAM in (needed on boards where it shares the
 * address space with ROM above 2 MB; harmless otherwise).
 *
 * Offsets here count bytes of save RAM (one per odd address): byte i is at
 * $200001 + 2i. A block is: 'M' 'D', a tag ('S' settings, 'G' game), a
 * version (1), the data length (u16, big-endian), the data, a 16-bit
 * checksum (sum16). A block that does not match in every part is treated
 * as absent. The game block is sizeof(struct state) + 8 bytes; the
 * settings block (8 data bytes) fits in the 64 bytes before it.
 *
 * Callers: ui.c (the title, options and pause menus, the start of each
 * map, try again after death). The saved game is S as it is in RAM, so a
 * build that changes the size of struct state cannot read older saves
 * (the length check fails and CONTINUE is not offered).
 *
 * Licence: GPL-2.0-or-later.
 */
#include "game.h"
#include "save.h"

#define SRAM ((volatile u8 *)0x200001)     /* save RAM byte 0; byte i at SRAM[2i] */
#define SRAM_CTRL (*(volatile u8 *)0xA130F1)   /* bit 0: 1 save RAM mapped in, 0 ROM */
#define SET_OFF 0                   /* the settings block's first byte */
#define GAME_OFF 64                 /* the saved game block's first byte */

const u16 button_bits[N_BUTTONS] = {PAD_A, PAD_B, PAD_C, PAD_X, PAD_Y, PAD_Z, PAD_MODE};
const char * const button_names[N_BUTTONS] = {"A", "B", "C", "X", "Y", "Z", "MODE"};
/* the agreed default layout: A use, B pass, C medkit, X/Z weapons, Y notebook, Mode detail */
/* (button index per action, in ACT_* order: A, B, C, X, Z, Y, Mode) */
static const u8 default_keys[N_ACTIONS] = {0, 1, 2, 3, 5, 4, 6};

struct settings cfg;

/* save RAM byte i (the RAM must be switched in) */
static u8 rd(u16 i)
{
    return SRAM[i * 2];
}

/* write save RAM byte i (the RAM must be switched in) */
static void wr(u16 i, u8 v)
{
    SRAM[i * 2] = v;
}

/* the checksum of n bytes at p: start at $5A5A; for each byte rotate the
   sum left one bit and XOR the byte in */
static u16 sum16(const u8 *p, u16 n)
{
    u16 s = 0x5A5A;
    while (n--) s = (u16)((s << 1) | (s >> 15)) ^ *p++;
    return s;
}

/* block: 'M' 'D' tag version, u16 length, data, u16 checksum.
   Writes n bytes of data as block `tag` at save RAM byte off, switching
   the RAM in for the time it takes. */
static void block_write(u16 off, u8 tag, const void *data, u16 n)
{
    const u8 *p = data;
    u16 c = sum16(p, n), i;
    SRAM_CTRL = 1;
    wr(off + 0, 'M');
    wr(off + 1, 'D');
    wr(off + 2, tag);
    wr(off + 3, 1);
    wr(off + 4, n >> 8);
    wr(off + 5, n & 0xFF);
    for (i = 0; i < n; i++) wr(off + 6 + i, p[i]);
    wr(off + 6 + n, c >> 8);
    wr(off + 7 + n, c & 0xFF);
    SRAM_CTRL = 0;
}

/* checks the block `tag` of n data bytes at save RAM byte off: its header
   and its checksum. copy = 1 also copies the data to `data` as it reads
   (even when the checksum then fails, so a caller that must not lose its
   data checks first with copy = 0). Returns 1 if the block is valid. */
static u8 block_read(u16 off, u8 tag, void *data, u16 n, u8 copy)
{
    u8 *p = data, ok = 0;
    u16 i, c;
    SRAM_CTRL = 1;
    if (rd(off) == 'M' && rd(off + 1) == 'D' && rd(off + 2) == tag && rd(off + 3) == 1 &&
        ((rd(off + 4) << 8) | rd(off + 5)) == n) {
        u16 s = 0x5A5A;                     /* (sum16, done while reading) */
        for (i = 0; i < n; i++) {
            u8 v = rd(off + 6 + i);
            s = (u16)((s << 1) | (s >> 15)) ^ v;
            if (copy) p[i] = v;
        }
        c = (rd(off + 6 + n) << 8) | rd(off + 7 + n);
        ok = c == s;
    }
    SRAM_CTRL = 0;
    return ok;
}

/* the defaults: step movement, the default buttons (default_keys) */
void settings_defaults(void)
{
    u8 i;
    cfg.smooth = 0;
    cfg.outward = 1;
    for (i = 0; i < N_ACTIONS; i++) cfg.key[i] = default_keys[i];
}

/* cfg from save RAM, if the block is valid, every action has a button
   0..6 and no button is used twice; else the defaults. Called once at
   start-up (main). */
void settings_load(void)
{
    struct settings t;
    u8 i, used = 0, bad = 0;
    if (block_read(SET_OFF, 'S', &t, sizeof t, 1)) {
        for (i = 0; i < N_ACTIONS; i++) {        /* each button once */
            if (t.key[i] >= N_BUTTONS || (used & (1 << t.key[i]))) bad = 1;
            else used |= 1 << t.key[i];
        }
        if (!bad && t.smooth <= 1 && t.outward <= 1) {
            cfg = t;
            return;
        }
    }
    settings_defaults();
}

/* cfg to save RAM (after the options screen) */
void settings_save(void)
{
    block_write(SET_OFF, 'S', &cfg, sizeof cfg);
}

/* a 3-button pad has fixed buttons: A use, B pass, C (held) full detail;
   the rest are C + a direction (ui.c, handle_c_combo) */
static const u16 bits3[N_ACTIONS] = {PAD_A, PAD_B, 0, 0, 0, 0, PAD_C};

/* the PAD_* bit(s) that trigger action `action` (ACT_*): the chosen button
   on a 6-button pad; the fixed one on a 3-button pad (0 for the actions
   reached through C + a direction, which ui.c tests itself) */
u16 action_bits(u8 action)
{
    return pad_six ? button_bits[cfg.key[action]] : bits3[action];
}

/* 1 if a valid saved game is in save RAM (the title's CONTINUE) */
u8 game_saved(void)
{
    return block_read(GAME_OFF, 'G', 0, sizeof(struct state), 0);
}

/* S to save RAM (the save item, and the start of every map) */
void game_save(void)
{
    block_write(GAME_OFF, 'G', &S, sizeof(struct state));
}

/* the saved game into S, then game_restore (rules.s) rebuilds the map's
   tables from it. Checks first, so S is untouched if the save is bad.
   Returns 1 if loaded. */
u8 game_load(void)
{
    if (!block_read(GAME_OFF, 'G', 0, sizeof(struct state), 0)) return 0;   /* check only */
    block_read(GAME_OFF, 'G', &S, sizeof(struct state), 1);                 /* copy */
    game_restore();
    return 1;
}
