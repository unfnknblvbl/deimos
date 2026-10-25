/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG portable core: the game rules (core/rules.c), the same rules as
 * megadrive/src/rules.s and tools/game_model.py (the executable spec),
 * in C for the SNES (cc65) and the 3DS (gcc). The state S has the byte
 * layout of the Mega Drive's, so the lockstep tests compare it with the
 * model byte for byte.
 *
 * The platform provides the hooks at the end (messages, sounds, the maps'
 * data). Entity indices are u16 (up to MAX_ENTS); NONE = 0xFFFF.
 *
 * Who uses it: every port's game code (the SNES port's snes/src/glue.c,
 * ui.c, campaign.c, save.c, which the 32X, 3DS, GBA and Game Boy Color
 * ports also build; the GBC's gbc/src/ui.c) and the build machine's tests
 * (test/host.c). The port calls the actions below when the player presses
 * a button, then reads S and the globals to draw the screens.
 *
 * Jargon: "the state" S is the whole saved game, plain bytes in the Mega
 * Drive's big-endian layout (gen/state.h, made by tools/gen_core.py);
 * "lockstep" tests run these rules next to the Mega Drive game's Python model and
 * compare S after every action; an "entity" is a thing in the map (a
 * monster, an item, a prop), a "def" its kind, a "line" a door, an
 * "event" a script on a map cell; a "modal" is a dialog, a code entry or
 * a store that must close before the monsters move; a "level blob" is a
 * map's read-only data (see map_level below).
 *
 * (The Game Boy Color's gbc/tools/split_core.py copies this file into
 * core_api.h, adding __banked to each prototype it finds on one line: keep
 * comments on their own lines above the prototypes.)
 * Licence: GPL-2.0-or-later.
 */
#ifndef CORE_RULES_H
#define CORE_RULES_H

#include "types.h"
#include "gen/state.h"
#include "gen/sfx.h"
#include "gen/texts.h"

/* the game state (core/rules.c) */
extern struct state S;

/* "no entity" (u16 index), "no line / event / text" (u8 index) */
#define NONE 0xFFFFu
#define NONE_B 0xFF

/* entity types (def_etype): 0 doors and walls that open, 1 monsters,
   2 friendly people, 3 pick-ups (health, armour, credits, keys), 4
   inventory items, 5 weapons, 6 and 16 ammo, 10 and 11 fires (the
   second hurts more), 12 destructibles (barrels, crates, power
   couplings). The numbers are the original game's. */
#define ET_LINE 0
#define ET_ENEMY 1
#define ET_HUMAN 2
#define ET_PICKUP 3
#define ET_INV 4
#define ET_WEAPON 5
#define ET_AMMO 6
#define ET_FIRE 10
#define ET_FIRE2 11
#define ET_DESTRUCT 12
#define ET_AMMO2 16

/* entity flags (S.e_flags, one byte per entity):
   F_ALIVE    can be hit (a living monster, an unbroken object)
   F_HIDDEN   not drawn (taken away by an event, or burst)
   F_LINKED   present in the map (on its cell's list)
   F_ACTIVE   a monster on the active list, taking turns
   F_ATK2     a monster using its second attack
   F_DROPPED  a dead monster has already dropped its item
   F_CORPSE   a dead monster's body lies there (can be raised)
   F_RAISING  an Archvile is about to raise this corpse */
#define F_ALIVE 0x01
#define F_HIDDEN 0x02
#define F_LINKED 0x04
#define F_ACTIVE 0x08
#define F_ATK2 0x10
#define F_DROPPED 0x20
#define F_CORPSE 0x40
#define F_RAISING 0x80

/* line flags (S.l_flags): open, locked, breakable and not yet broken */
#define L_OPEN 1
#define L_LOCKED 2
#define L_ALIVE 4

/* trace hit kinds (hit_kind[]): a wall, the player, a line (hit_val the
   line), a static blocker (hit_val its type), an entity (hit_val its
   index) */
#define H_WALL 0
#define H_PLAYER 1
#define H_LINE 2
#define H_STATIC 3
#define H_ENT 4

/* why an event stopped (suspend): not stopped, a dialog to read, a code
   to enter, stopped for good (a requirement not met) */
#define SUSP_NONE 0
#define SUSP_DIALOG 1
#define SUSP_PASSWORD 2
#define SUSP_STOP 3

#define DLG_CAPTURED 0xFE          /* dialog_str: TX_CAPTURED <captured_def> TX_CAPTURED_END */

/* the level in play (core/rules.c: level_load): the numbers of defs,
   monster slots, lines, events, texts; the map's id; the def a "you
   captured" dialog names; the map's entities, all entities (with the 8
   drop slots), event commands; the player's start; the map's own floor
   and ceiling colours (RGB 0..255) */
extern u8 n_defs, n_mons, n_lines, n_events, n_strings, map_id, captured_def;
extern u16 n_map_ents, n_ents, n_cmds;
extern u8 spawn_x, spawn_z, spawn_f;
extern u8 floor_rgb0[3], ceil_rgb0[3];

/* what the rules leave for the screens (core/rules.c explains each):
   dialog_str a dialog's text id (NONE_B none); pw_want / pw_prompt a
   code entry's answer and question text ids (NONE_B none); suspend,
   skip_turn, monsters_pending: the turn's progress; pend_*: a stopped
   event; status_msg: the status line (status_new 1 when it changed);
   player_hurt / player_fired: for the face and the weapon's animation;
   hit_*: the last trace's hits */
extern u8 dialog_str, pw_want, pw_prompt, suspend, skip_turn, monsters_pending;
extern u8 pend_valid, pend_ev, pend_k, pend_why;
extern u32 pend_flags;
extern char status_msg[33];
extern u8 status_new, player_hurt, player_fired;
extern u8 hit_n, hit_kind[9];
extern u16 hit_val[9];
extern const u8 STORE_LINES[];     /* per vendor 16 x (name tag, price), tag 0 ends */
extern const u8 STORE_ITEMS[];     /* per name tag: kind (1 item, 2 ammo, 3 stat), what */

/* the actions (the platform's input code calls these)
   game_init       a fresh game: the first map
   game_restore    S was loaded from a saved game
   act_turn        turn by delta quarter turns (+1 right, -1 left); free
   act_move        a step forward (back 0) or back (1); 1 if it moved
   act_use         use what is ahead, or attack it; may take a turn
   act_pass        let a turn pass
   act_cycle_weapon the next (+1) or previous (-1) usable weapon; free
   act_health_item use a medkit or soulsphere; 1 if one was used
   use_item        3 the Berserker, 4 the collar; 1 if used
   resume          a dialog closed, or a code was entered (0: cancelled)
   settle          let the monsters move if they are owed a turn
   modal           1 while a dialog, a code entry or a store is open
   store_buy       buy a line of the open store: 0 bought, 1 too few
                   credits, 2 too many
   store_close     the store's menu closed
   map_change      through the exit (S.exit) to map `map` */
void game_init(void);              /* a fresh game: the first map */
void game_restore(void);           /* S was loaded from a saved game */
void act_turn(s8 delta);
u8 act_move(u8 back);              /* -> 1 if it moved */
void act_use(void);
void act_pass(void);
void act_cycle_weapon(s8 step);
u8 act_health_item(void);
u8 use_item(u8 k);                 /* 3 the Berserker, 4 the collar -> 1 used */
void resume(const char *password); /* a dialog closed / a code was entered (0: cancelled) */
void settle(void);
u8 modal(void);
u8 store_buy(u8 line);             /* -> 0 bought, 1 too few credits, 2 too many */
void store_close(void);
void map_change(u8 map);           /* through the exit (S.exit) to map `map` */

/* the level's texts and things (no side effects):
   def_name / def_etype / def_esub   a def's name, type, subtype
   ent_etype     an entity's type (the entity must have a def)
   wall_at       1 if the cell is solid (off the map counts)
   ent_at        the first entity on the cell with index >= from, NONE
                 none (call again with the result + 1 for the next)
   line_at       the closed line there, or NONE_B
   line_x / line_z / line_etype   a line's cell and type
   static_at     a static blocker's type there (0 none)
   n_active / active_at   the active monsters' list */
const char *def_name(u8 def);
u8 def_etype(u8 def);
u8 def_esub(u8 def);
u8 ent_etype(u16 i);               /* (the entity must have a def) */
u8 wall_at(u8 x, u8 z);
u16 ent_at(u8 x, u8 z, u16 from);
u8 line_at(u8 x, u8 z);            /* the closed line there, or NONE_B */
u8 line_x(u8 i);
u8 line_z(u8 i);
u8 line_etype(u8 i);
u8 static_at(u8 x, u8 z);          /* a static blocker's type (unless taken away), 0 none */
u16 n_active(void);
u16 active_at(u16 k);

/* ---- the platform provides these ----
   say, say2, say3   a status message (the strings joined)
   say_num           a, the number n in decimal, b
   sfx               play sound effect id (SFX_* in gen/sfx.h) */
void say(const char *s);
void say2(const char *a, const char *b);
void say3(const char *a, const char *b, const char *c);
void say_num(const char *a, u16 n, const char *b);
void sfx(u8 id);
/* map `map`'s level blob (megadrive/tools/gen_cdata.py format),
   readable until the next call; the map in play's string `id` (its strings
   blob), readable until a few more strings have been asked for */
const u8 *map_level(u8 map);
const char *level_string(u8 id);
/* the game's text `id` (TX_* in gen/texts.h: the texts table that the
   build reads from DoomRPG.jar, tools/game_text.py), 0-terminated;
   readable until three more texts have been asked for */
const char *gtext(u8 id);

#endif
