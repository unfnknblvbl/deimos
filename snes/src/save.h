/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the SNES: settings and the saved game in the cartridge's
 * battery-backed RAM (8 KB at $306000). Licence: GPL-2.0-or-later. */
#ifndef SAVE_H
#define SAVE_H
#include "../../core/types.h"

/* Shared by every port that uses ui.c (the 32X, 3DS, GBC and GBA too; the
 * GBC build turns the prototypes below into banked calls with
 * gbc/tools/bankify.py, so keep each prototype on one line). */

/* actions the player can put on any button (ui.c's BUTTON_BITS) */
enum { ACT_USE, ACT_PASS, ACT_MEDKIT, ACT_PREV, ACT_NEXT, ACT_NOTES, N_ACTIONS };

/* the settings saved in the battery RAM (block 0) */
struct settings {
    u8 key[N_ACTIONS];              /* button index (ui.c) of each action */
    u8 outward;                     /* 1: the view drawn from the middle out, a strip at a time (the
                                       fixed-camera ports that do it: the SNES, the Game Boy Color) */
};
extern struct settings cfg;         /* the settings in use */

/* settings_defaults: the default buttons. settings_save: write cfg.
   game_saved: 1 if a valid saved game is there. game_save: write the game
   state S. */
void settings_defaults(void);
void settings_load(void);           /* defaults if nothing valid is saved */
void settings_save(void);
u8 game_saved(void);
void game_save(void);
u8 game_load(void);                 /* 1 = loaded (S filled, game_restore done) */

/* save.s: a byte of the battery RAM at offset i (0..8191) */
u8 sram_rd(u16 i);
void sram_wr(u16 i, u8 v);
#endif
