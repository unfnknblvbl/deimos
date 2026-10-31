/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * The Mega Drive game - settings and the saved game, in the cartridge's
 * battery-backed RAM (8 KB at $200001, odd bytes).
 *
 * src/save.c. ui.c uses it for the options screen, the title's CONTINUE,
 * the pause menu's save and the save made at the start of every map;
 * it also maps the player's actions to joypad buttons (action_bits).
 *
 * Licence: GPL-2.0-or-later.
 */
#ifndef SAVE_H
#define SAVE_H
#include "md.h"

/* actions the player can put on any of the seven buttons */
enum { ACT_USE, ACT_PASS, ACT_MEDKIT, ACT_PREV, ACT_NEXT, ACT_NOTES, ACT_DETAIL, N_ACTIONS };
#define N_BUTTONS 7                 /* A B C X Y Z Mode */
extern const u16 button_bits[N_BUTTONS];            /* each button's PAD_* bit */
extern const char * const button_names[N_BUTTONS];  /* each button's name, for the options screen */

struct settings {
    u8 smooth;                      /* 0 step, 1 smooth movement */
    u8 outward;                     /* 1: in step movement the view drawn from the middle out */
    u8 key[N_ACTIONS];              /* button index (0..6) of each action */
};
extern struct settings cfg;         /* the settings in force */

void settings_defaults(void);       /* step movement, the default buttons */
void settings_load(void);           /* defaults if nothing valid is saved */
void settings_save(void);           /* write cfg to the battery RAM */
u16 action_bits(u8 action);         /* pad bits of the button for an action */

u8 game_saved(void);                /* 1 if a valid saved game exists */
void game_save(void);               /* write S to the battery RAM */
u8 game_load(void);                 /* 1 = loaded (S filled, game_restore done) */

#endif
