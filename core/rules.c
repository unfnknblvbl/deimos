/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG portable core: the game rules. A C translation of the Mega Drive game's
 * src/rules.s (68000), itself written from tools/game_model.py: the same
 * routines, the same random numbers in the same order, the same state
 * bytes. The lockstep tests compare S with the model after every action.
 *
 * Written for cc65 as much as for gcc: C89, no structs passed around, the
 * level's big-endian tables read a byte at a time.
 *
 * Where it fits. Every port links this file unchanged: the SNES (cc65,
 * 65816), the 32X (SH-2 gcc), the 3DS and the GBA (ARM gcc), the Game Boy
 * Color (SDCC, after gbc/tools/split_core.py has split it over three ROM
 * banks) and the build machine's tests (test/host.c, gcc). The port's
 * input code calls the act_* functions (one per player action) and the
 * screens read the globals declared in rules.h. The rules call back into
 * the port only through the hooks at the end of rules.h: say*() for the
 * status line, sfx() for sounds, map_level() for a map's data and
 * level_string() for its texts.
 *
 * Jargon used below:
 *  - "the state" (S, struct state in gen/state.h): everything a saved game
 *    keeps, as plain bytes. Its 16-bit and 32-bit fields are big-endian
 *    byte arrays (the 68000's layout), read and written with G16/P16 and
 *    the SG16/SP16 macros below.
 *  - "the model": megadrive/tools/game_model.py, the Python version of
 *    these rules that is the executable specification.
 *  - "lockstep": the tests run this code and the model side by side,
 *    one action at a time, and compare S byte for byte after each one.
 *    That is why the order of the rnd() calls below matters: one call
 *    more or less and every later number differs.
 *  - "the level blob": a map's read-only data, made by the Mega Drive game's
 *    tools/gen_cdata.py. Header: bytes 0-3 "LVB2", 4 version, 5 defs,
 *    6-7 map entities, 8 monsters, 9 lines, 10 events, 11 strings, 12-13
 *    event commands, 14-16 the player's start x, z and facing, 17 map id,
 *    18-20 floor RGB, 21-23 ceiling RGB, then from byte 24 one big-endian
 *    16-bit offset per table (LT_* in gen/data.h). Its tables are
 *    indexed by def, entity, line, event, command or map cell.
 *  - "def": an entity definition (a monster kind, an item kind, a door
 *    kind...). Each def has an entity type (ET_*), a subtype, a 32-bit
 *    parameter and a name.
 *  - "entity": one thing placed in the map (index 0 .. n_ents - 1). The
 *    map's own come first (n_map_ents of them), then N_DROPS free slots
 *    for the items that dead monsters drop. Monsters also own a "monster
 *    slot" (t_ent_mon) that indexes the m_* stat arrays of S.
 *  - "line": a door or a wall that can open (index 0 .. n_lines - 1).
 *  - "event": a script on a map cell (index 0 .. n_events - 1). It runs a
 *    list of "commands" (op code plus two 32-bit arguments) when the
 *    player arrives on, leaves, or uses its cell.
 *  - "cell": a map square. The map is 32 x 32 cells; cell number
 *    z * 32 + x (0..1023). x grows east, z grows south.
 *  - "facing": 0 north (z - 1), 1 east, 2 south, 3 west (DX/DZ below).
 *  - "active": the list of monsters awake and taking turns (S.active).
 *  - "trace": a straight line walked cell by cell along a row or column
 *    to see what it hits (the original game's Game_trace).
 *  - "text": the messages the rules show are the game's own words, not
 *    written here: gtext(TX_...) (rules.h) gives each one, from the texts
 *    table the build makes from DoomRPG.jar (tools/game_text.py).
 * Licence: GPL-2.0-or-later.
 */
#include "rules.h"

/* the game state: the whole of a saved game (see gen/state.h) */
struct state S;

/* rd32be: reads a big-endian 32-bit number at p (G32 uses it).
 * In: p, 4 bytes. Out: the value. No side effects. */
u32 rd32be(const u8 *p)
{
    u32 r = p[0];
    r = (r << 8) | p[1];
    r = (r << 8) | p[2];
    r = (r << 8) | p[3];
    return r;
}

/* ------------------------------------------------------------ the level */
/* Counts and start values from the level blob's header (level_load):
 * n_defs defs, n_mons monster slots, n_lines lines, n_events events,
 * n_strings texts, map_id the map's number, captured_def the def named by
 * the "you captured ...!" dialog (set by an event command). n_map_ents is
 * the map's own entities, n_ents that plus N_DROPS drop slots, n_cmds the
 * event commands. spawn_x/z/f: where the player starts and the facing.
 * floor_rgb0/ceil_rgb0: the map's floor and ceiling colours (0..255 RGB)
 * before any event changes them (S.floor_rgb, S.ceil_rgb hold the current
 * ones). */
u8 n_defs, n_mons, n_lines, n_events, n_strings, map_id, captured_def;
u16 n_map_ents, n_ents, n_cmds;
u8 spawn_x, spawn_z, spawn_f;
u8 floor_rgb0[3], ceil_rgb0[3];

/* lvl: the level blob of the map in play (from map_level()). The t_*
 * pointers are its tables, found once by level_load:
 *   t_def_etype/esub   per def: entity type (ET_*), subtype (u8)
 *   t_def_parm         per def: a 32-bit big-endian parameter
 *   t_def_name         per def: a 16-bit offset (from the table's start)
 *                      of its upper-case, 0-terminated name
 *   t_ent_def0/x0/z0   per map entity: its def and start cell
 *   t_ent_hidden0      per map entity: 1 if it starts hidden
 *   t_ent_mon          per entity: its monster slot, 255 if not a monster
 *   t_line_x/z/etype   per line: cell and entity type
 *   t_line_locked0     per line: 1 if it starts locked
 *   t_line_kind        per line: bit 0 the exit, bit 1 a secret
 *   t_ev_x/z           per event: its cell
 *   t_ev_state0        per event: its first state byte
 *   t_ev_flags         per event: bits 0-7 of its 32-bit trigger flags
 *   t_ev_cmd0, ncmd    per event: first command (u16), number of commands
 *   t_ev_at            per cell (1024): the event there, 255 none
 *   t_cmd_op, a1, a2   per command: op code, two 32-bit arguments
 *   t_wall_bits        1 bit per cell (bit c & 7 of byte c >> 3): solid
 *   t_static_block     per cell: the type of a fixed blocker there, 0 none
 *   t_thin             per cell: thin-wall bits (tr_thin)
 *   t_line_cells       1 bit per cell: a line stands there
 *   t_drop_def         per drop kind (DD_*): the def it uses, 255 none
 *   t_spawns           u16 count, then per lost-soul spawner three u16s */
static const u8 *lvl;
static const u8 *t_def_etype, *t_def_esub, *t_def_parm, *t_def_name, *t_ent_def0, *t_ent_x0, *t_ent_z0;
static const u8 *t_ent_hidden0, *t_ent_mon, *t_line_x, *t_line_z, *t_line_etype, *t_line_locked0;
static const u8 *t_line_kind, *t_ev_x, *t_ev_z, *t_ev_state0, *t_ev_flags, *t_ev_cmd0, *t_ev_ncmd, *t_ev_at;
static const u8 *t_cmd_op, *t_cmd_a1, *t_cmd_a2, *t_wall_bits, *t_static_block, *t_thin, *t_line_cells;
static const u8 *t_drop_def, *t_spawns;

/* (the Game Boy Color keeps three of the level's tables apart from the rest,
   to fit its RAM: the platform reads them, PLAT_LEVEL_FAR) */
#ifdef PLAT_LEVEL_FAR
u8 plat_ev_at(u16 cell);
u32 plat_cmd_a1(u16 c);
u32 plat_cmd_a2(u16 c);
#define EV_AT(cell) plat_ev_at(cell)
#define CMD_A1(c) plat_cmd_a1(c)
#define CMD_A2(c) plat_cmd_a2(c)
#else
#define EV_AT(cell) t_ev_at[cell]
#define CMD_A1(c) G32(t_cmd_a1 + 4 * (c))
#define CMD_A2(c) G32(t_cmd_a2 + 4 * (c))
#endif
/* EV_AT(cell): the event on a cell (255 none); CMD_A1/CMD_A2(c): command
   c's two 32-bit arguments. */

/* ltab: the address of the level blob's table i (LT_*), from the header's
 * offset list at byte 24. In: i, lvl. Out: a pointer into the blob. */
static const u8 *ltab(u8 i)
{
    return lvl + G16(lvl + 24 + 2 * i);
}

/* level_load: reads the header of the blob at lvl into the n_* counts,
 * spawn_*, map_id, floor_rgb0/ceil_rgb0, and points every t_* table at
 * its place in the blob. Called by map_data. Does not touch S. */
static void level_load(void)
{
    n_defs = lvl[5];
    n_map_ents = G16(lvl + 6);
    n_ents = n_map_ents + N_DROPS;
    n_mons = lvl[8];
    n_lines = lvl[9];
    n_events = lvl[10];
    n_strings = lvl[11];
    n_cmds = G16(lvl + 12);
    spawn_x = lvl[14];
    spawn_z = lvl[15];
    spawn_f = lvl[16];
    map_id = lvl[17];
    /* each table's address, from the offset list */
    t_def_etype = ltab(LT_DEF_ETYPE);
    t_def_esub = ltab(LT_DEF_ESUB);
    t_def_parm = ltab(LT_DEF_PARM);
    t_def_name = ltab(LT_DEF_NAME);
    t_ent_def0 = ltab(LT_ENT_DEF0);
    t_ent_x0 = ltab(LT_ENT_X0);
    t_ent_z0 = ltab(LT_ENT_Z0);
    t_ent_hidden0 = ltab(LT_ENT_HIDDEN0);
    t_ent_mon = ltab(LT_ENT_MON);
    t_line_x = ltab(LT_LINE_X);
    t_line_z = ltab(LT_LINE_Z);
    t_line_etype = ltab(LT_LINE_ETYPE);
    t_line_locked0 = ltab(LT_LINE_LOCKED0);
    t_line_kind = ltab(LT_LINE_KIND);
    t_ev_x = ltab(LT_EV_X);
    t_ev_z = ltab(LT_EV_Z);
    t_ev_state0 = ltab(LT_EV_STATE0);
    t_ev_flags = ltab(LT_EV_FLAGS);
    t_ev_cmd0 = ltab(LT_EV_CMD0);
    t_ev_ncmd = ltab(LT_EV_NCMD);
    t_ev_at = ltab(LT_EV_AT);
    t_cmd_op = ltab(LT_CMD_OP);
    t_cmd_a1 = ltab(LT_CMD_A1);
    t_cmd_a2 = ltab(LT_CMD_A2);
    t_wall_bits = ltab(LT_WALL_BITS);
    t_static_block = ltab(LT_STATIC_BLOCK);
    t_thin = ltab(LT_THIN);
    t_line_cells = ltab(LT_LINE_CELLS);
    t_drop_def = ltab(LT_DROP_DEF);
    t_spawns = ltab(LT_SPAWNS);
    floor_rgb0[0] = lvl[18];
    floor_rgb0[1] = lvl[19];
    floor_rgb0[2] = lvl[20];
    ceil_rgb0[0] = lvl[21];
    ceil_rgb0[1] = lvl[22];
    ceil_rgb0[2] = lvl[23];
}

/* map_data: makes `map` the map in play: asks the platform for its blob
 * (map_level) and reads the header (level_load). Does not touch S. */
static void map_data(u8 map)
{
    lvl = map_level(map);       /* the platform's copy of the blob */
    level_load();               /* counts and table pointers */
}

/* Small readers of the level's tables (no side effects):
 * def_etype/def_esub: a def's entity type and subtype; def_name: its name
 * (a 0-terminated string in the blob); def_parm: its 32-bit parameter
 * (a monster's stat spread, an item's amount...); line_x/z/etype: a
 * line's cell and entity type. */
u8 def_etype(u8 def) { return t_def_etype[def]; }
u8 def_esub(u8 def) { return t_def_esub[def]; }
const char *def_name(u8 def) { return (const char *)t_def_name + G16(t_def_name + 2 * def); }
static s32 def_parm(u8 def) { return (s32)G32(t_def_parm + 4 * def); }
u8 line_x(u8 i) { return t_line_x[i]; }
u8 line_z(u8 i) { return t_line_z[i]; }
u8 line_etype(u8 i) { return t_line_etype[i]; }

/* ------------------------------------------------------------ globals */
/* What the rules leave for the port's screens (see rules.h):
 *  dialog_str      the text id of a dialog to show (DLG_CAPTURED: the
 *                  "you captured" one)
 *  pw_want         the text id of the password an event waits for
 *  pw_prompt       the text id of the password's question
 *  suspend         why the event in progress stopped (SUSP_*)
 *  skip_turn       1: the action ends without the monsters' turn
 *  monsters_pending 1: the monsters still have to move (settle runs them)
 *  pend_valid, pend_ev, pend_k, pend_why, pend_flags: an event stopped
 *                  half-way (a dialog or a password): its index, the next
 *                  command, why it ran, and its trigger flags, so that
 *                  resume() can carry on from there
 *  status_msg      the status line's text (32 characters and the 0)
 *  status_new      1: status_msg changed
 *  player_hurt     1: the player took damage this turn (the face flinches)
 *  player_fired    1: the player attacked this turn (the weapon animates)
 *  hit_n, hit_kind, hit_val: the last trace's hits, in order: kind
 *                  (H_*) and value (line, entity index or blocker type) */
u8 dialog_str, pw_want, pw_prompt, suspend, skip_turn, monsters_pending;
u8 pend_valid, pend_ev, pend_k, pend_why;
u32 pend_flags;
char status_msg[33];
u8 status_new, player_hurt, player_fired;
u8 hit_n, hit_kind[9];
u16 hit_val[9];

/* The cell lists: every entity that is "linked" (F_LINKED, i.e. present
 * in the map) is on the list of its cell, kept sorted by index. Links are
 * stored as index + 1 so that 0 can mean "end of list". */
#ifdef PLAT_CELL_HEAD                   /* (the platform's place for it) */
#define cell_head ((u16 *)PLAT_CELL_HEAD)
#else
static u16 cell_head[1024];            /* per cell: the first linked entity + 1 (sorted) */
#endif
static u16 ent_next[MAX_ENTS];         /* the next one + 1 */
/* The attacks the monsters decided on this turn (monster_turn): attacker,
 * target (an entity, or the player) and kind (T_*); att_n of them. */
static u16 att_ent[MAX_ACTIVE], att_tgt[MAX_ACTIVE];
static u8 att_kind[MAX_ACTIVE], att_n;
/* The movers' lists of monster_turn: those that want to move, those to
 * try again, and a copy taken before a pass. */
static u16 mt_movers[MAX_ACTIVE], mt_again[MAX_ACTIVE], mt_snap[MAX_ACTIVE];
/* Barrel explosions waiting to go off (up to 8 cells) */
static u8 expl_x[8], expl_z[8], n_pend_expl;
/* calc_damage's results: the damage to health and to armour */
static u8 dmg_hp, dmg_arm;

/* attack target kinds (att_kind): the player, an entity, an arch-vile's
   raise of a corpse, a line (door) */
#define T_PLAYER 0
#define T_ENT 1
#define T_RAISE 2
#define T_LINE 3

/* Trace masks: bit t set means things of entity type t (ET_*, also the
   static-blocker types, which share the numbering) are hits; bit 8 means
   the player is a hit; bits 14 and 15 also enable the thin walls of
   type 14 and 15 (tr_thin). Entity types: 0 line, 1 enemy, 2 human,
   3-6 pick-ups, 8 player mark, 9 wall mark, 10 and 11 fires, 12
   destructible (barrel, crate). Flying monsters drop the fire bits
   (monster_mask). The bits set:
   MASK_PLAYER_MOVE  0,1,2,7,9,12,13,14,15 (what blocks the player's step)
   MASK_MONSTER_MOVE 0-2,7-15 (what blocks a monster's step)
   MASK_SHOT         0,1,2,7,9,10,12,14 (what a shot hits or stops at)
   MASK_SHOW_KILL    1,12 (enemies and destructibles)
   MASK_RAISE_FREE   0-2,7-13 (what keeps a corpse from being raised) */
#define MASK_PLAYER_MOVE 0xF287u
#define MASK_MONSTER_MOVE 0xFF87u
#define MASK_SHOT 0x5687u
#define MASK_SHOW_KILL 4098u
#define MASK_RAISE_FREE 16263u

/* why an event runs: bits of its 32-bit trigger flags (ev_flags). The
   bits 0-7 are the arrive/leave directions (ARRIVE_FLAG, LEAVE_FLAG);
   bits 28-31 the facing (FACING_FLAG). */
#define EVF_USE 0x100UL
#define EVF_MODIFY 0x200UL
#define EVF_MOVE 0x400UL

/* calc_hit's results */
#define HIT_MISS 0
#define HIT_HIT 1
#define HIT_CRIT 2

/* the cell of the boss portal, and the cell where it puts what it spawns */
#define PORTAL_X 16
#define PORTAL_Z 14
#define PORTAL_SPAWN_Z 15

/* per facing (0 N, 1 E, 2 S, 3 W): the step in x and z; the event flag
   for facing that way; the angle (0..255, 0 east, 64 north) a facing
   matches; the event flag for leaving a cell that way and for arriving
   in one moving that way */
static const s8 DX[4] = {0, 1, 0, -1};
static const s8 DZ[4] = {-1, 0, 1, 0};
static const u32 FACING_FLAG[4] = {0x10000000UL, 0x20000000UL, 0x80000000UL, 0x40000000UL};
static const u8 ANGLE_FACING[4] = {64, 0, 192, 128};
static const u8 LEAVE_FLAG[4] = {16, 32, 64, 128};
static const u8 ARRIVE_FLAG[4] = {4, 8, 1, 2};

/* the sound each weapon (0..18) makes when fired; 12..18 are the
   monsters' own attacks */
static const u8 wpn_sfx[19] = {
    SFX_AXE, SFX_EXTINGUISHER, SFX_PISTOL, SFX_SHOTGUN, SFX_CHAINGUN, SFX_SHOTGUN,
    SFX_PLASMA, SFX_ROCKET, SFX_BFG, SFX_DOG, SFX_DOG, SFX_DOG,
    SFX_MON_ATTACK, SFX_MON_ATTACK, SFX_MON_ATTACK, SFX_FIREBALL, SFX_FIREBALL,
    SFX_FIREBALL, SFX_RAISE};

/* the state's big-endian words: SG16(f) reads S.f, SP16(f, v) writes v */
#define SG16(f) G16(S.f)
#define SP16(f, v) P16(S.f, (v))

/* n_active: how many monsters are on the active list;
   active_at(k): the entity index of the k-th one */
u16 n_active(void) { return SG16(n_active); }
u16 active_at(u16 k) { return G16(S.active + 2 * k); }

/* ------------------------------------------------------------ small helpers */
/* rnd: the game's random number generator, a 16-bit xorshift (shifts 7,
 * 9, 8) on S.rng. In: S.rng. Out: the new value's low byte; S.rng holds
 * the new value. Every random choice of the rules goes through it, in the
 * model's order. */
static u8 rnd(void)
{
    u16 x = SG16(rng);
    x ^= x << 7;
    x ^= x >> 9;
    x ^= x << 8;
    SP16(rng, x);
    return (u8)x;
}

/* rnd32: four rnd() bytes, the first one lowest. Out: a 32-bit random
 * number. Side effect: S.rng stepped four times. */
static u32 rnd32(void)
{
    u32 r;
    r = rnd();
    r |= (u32)rnd() << 8;
    r |= (u32)rnd() << 16;
    r |= (u32)rnd() << 24;
    return r;
}

/* clamp8: v limited to 0..255. */
static u8 clamp8(s32 v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (u8)v;
}

/* mask_has: 1 if bit t of a trace mask is set (t >= 16: never). */
static u8 mask_has(u16 mask, u8 t)
{
    if (t >= 16) return 0;
    return (u8)((mask >> t) & 1);
}

/* floor(a / b), b > 0 (the 68000's divs then a step down for a negative remainder) */
static s16 fdiv(s16 a, s16 b)
{
    s16 q = a / b, r = a % b;
    if (r < 0) q--;
    return q;
}

/* An entity's entity type, subtype, name and monster slot (from its def;
   ent_etype needs an entity that has a def). */
u8 ent_etype(u16 i) { return t_def_etype[S.e_def[i]]; }
static u8 esub(u16 i) { return t_def_esub[S.e_def[i]]; }
static const char *ent_name(u16 i) { return def_name(S.e_def[i]); }
static u8 mon_slot(u16 i) { return t_ent_mon[i]; }

/* ------------------------------------------------------------ cells */
/* wall_at: 1 if cell (x, z) is a solid wall (outside the 32 x 32 map
 * counts as wall), from the blob's wall bit map. */
u8 wall_at(u8 x, u8 z)
{
    u16 c;
    if (x >= 32 || z >= 32) return 1;
    c = ((u16)z << 5) | x;
    return (u8)((t_wall_bits[c >> 3] >> (c & 7)) & 1);
}

/* cell_of: the cell number (z * 32 + x) entity i stands on. */
static u16 cell_of(u16 i) { return ((u16)S.e_z[i] << 5) | S.e_x[i]; }

/* list_insert: puts entity i on its cell's list, in index order.
 * Writes cell_head / ent_next. Does not change F_LINKED (ent_link does). */
static void list_insert(u16 i)
{
    u16 *p = &cell_head[cell_of(i)];
    while (*p && *p - 1 < i) p = &ent_next[*p - 1];     /* walk past the lower indices */
    ent_next[i] = *p;
    *p = i + 1;
}

/* list_remove: takes entity i off its cell's list (if it is there).
 * Writes cell_head / ent_next. */
static void list_remove(u16 i)
{
    u16 *p = &cell_head[cell_of(i)];
    while (*p) {
        if (*p - 1 == i) {                 /* found: unhook it */
            *p = ent_next[i];
            ent_next[i] = 0;
            return;
        }
        p = &ent_next[*p - 1];
    }
}

/* ent_link: makes entity i present in the map (F_LINKED, on its cell's
 * list); nothing if it already is. */
static void ent_link(u16 i)
{
    if (S.e_flags[i] & F_LINKED) return;
    S.e_flags[i] |= F_LINKED;
    list_insert(i);
}

/* ent_unlink: takes entity i out of the map (clears F_LINKED, off its
 * cell's list); nothing if it is not linked. */
static void ent_unlink(u16 i)
{
    if (!(S.e_flags[i] & F_LINKED)) return;
    S.e_flags[i] &= ~F_LINKED;
    list_remove(i);
}

/* ent_move_to: moves entity i to cell (x, z) (S.e_x, S.e_z), keeping the
 * cell lists right if it is linked. */
static void ent_move_to(u16 i, u8 x, u8 z)
{
    u8 linked = S.e_flags[i] & F_LINKED;
    if (linked) list_remove(i);        /* off the old cell's list */
    S.e_x[i] = x;
    S.e_z[i] = z;
    if (linked) list_insert(i);        /* onto the new one */
}

/* ent_at: the first linked entity on cell (x, z) whose index is >= from,
 * or NONE. Call it again with the result + 1 to walk the cell's list. */
u16 ent_at(u8 x, u8 z, u16 from)
{
    u16 n;
    if (x >= 32 || z >= 32) return NONE;
    n = cell_head[((u16)z << 5) | x];
    while (n) {
        if (n - 1 >= from) return n - 1;
        n = ent_next[n - 1];
    }
    return NONE;
}

/* line_at: the closed (not L_OPEN) line on cell (x, z), or NONE_B. The
 * line_cells bit map rules out most cells before the search. */
u8 line_at(u8 x, u8 z)
{
    u16 c;
    u8 i;
    if (x >= 32 || z >= 32) return NONE_B;
    c = ((u16)z << 5) | x;
    if (!((t_line_cells[c >> 3] >> (c & 7)) & 1)) return NONE_B;
    for (i = 0; i < n_lines; i++)
        if (t_line_x[i] == x && t_line_z[i] == z && !(S.l_flags[i] & L_OPEN)) return i;
    return NONE_B;
}

/* hidden_at: 1 if cell c's static blocker (and its thin walls) have been
 * taken away by an event (S.static_hidden, one bit per cell). */
static u8 hidden_at(u16 c) { return (u8)((S.static_hidden[c >> 3] >> (c & 7)) & 1); }

/* static_at: the type of the fixed blocker on cell (x, z), 0 if none,
 * off the map, or taken away. */
u8 static_at(u8 x, u8 z)
{
    u16 c = ((u16)z << 5) | x;
    if (x >= 32 || z >= 32 || hidden_at(c)) return 0;
    return t_static_block[c];
}

/* ------------------------------------------------------------ the trace */
/* the trace in progress: its start and end cells and its mask */
static u8 tr_sx, tr_sz, tr_dx, tr_dz;
static u16 tr_mask;

/* tr_add: appends a hit (kind H_*, value) to hit_kind / hit_val, hit_n + 1.
 * The callers check there is room. */
static void tr_add(u8 kind, u16 val)
{
    hit_kind[hit_n] = kind;
    hit_val[hit_n] = val;
    hit_n++;
}

/* the thin walls of cell (x, z) a trace stepping (stx, stz) crosses -> 1 if one stops it
 * A thin wall is an invisible blocker on one edge of a cell. t_thin's
 * bits per cell: 1 W, 2 E, 4 N, 8 S edge for type 14; 16, 32, 64, 128
 * the same for type 15. Only the types the mask enables (bits 14, 15)
 * count. An edge counts if the trace crosses it: the near edge of a cell
 * past the start, the far edge of a cell before the end. Side effect: a
 * stopping wall is added as an H_STATIC hit (value 14 or 15) if there is
 * room. */
static u8 tr_thin(u8 x, u8 z, s8 stx, s8 stz)
{
    u8 w = t_thin[((u16)z << 5) | x], types = 0, edges = 0;
    if (!w) return 0;
    if (tr_mask & 0x4000) types = 0x0F;     /* type 14's four edges */
    if (tr_mask & 0x8000) types |= 0xF0;    /* type 15's */
    w &= types;
    if (!w) return 0;
    /* the edges crossed, for each direction (0x11 W, 0x22 E, 0x44 N, 0x88 S) */
    if (stx > 0) {
        if (x > tr_sx) edges |= 0x11;
        if (x < tr_dx) edges |= 0x22;
    } else if (stx < 0) {
        if (x < tr_sx) edges |= 0x22;
        if (x > tr_dx) edges |= 0x11;
    } else if (stz > 0) {
        if (z > tr_sz) edges |= 0x44;
        if (z < tr_dz) edges |= 0x88;
    } else if (stz < 0) {
        if (z < tr_sz) edges |= 0x88;
        if (z > tr_dz) edges |= 0x44;
    } else
        return 0;
    w &= edges;
    if (!w) return 0;
    if (hit_n < 9) tr_add(H_STATIC, (w & 0x0F) ? 14 : 15);
    return 1;
}

/* Game_trace along a row or a column -> hit_n (hits in hit_kind / hit_val, at most 8)
 * In: start cell (sx, sz), end cell (dx, dz) on the same row or column,
 * mask (see MASK_*), self (an entity to ignore, NONE for none). It walks
 * from the start cell to the end cell, both included. In each cell it
 * records, in this order: a wall (and stops), the player if mask bit 8
 * (and stops), a closed line, a static blocker, the entities (in index
 * order), then thin walls (which stop it). It also stops after 8 hits or
 * at the end cell. Out: hit_n (also returned), hit_kind[], hit_val[];
 * tr_* hold the trace's ends and mask. A thin wall found in the same
 * cell as the 8th hit can be a 9th entry, hence the arrays of 9. */
static u8 trace(u8 sx, u8 sz, u8 dx, u8 dz, u16 mask, u16 self)
{
    s8 stx = 0, stz = 0;
    u8 x = sx, z = sz, li, st, hid;
    u16 c, i;
    tr_sx = sx;
    tr_sz = sz;
    tr_dx = dx;
    tr_dz = dz;
    tr_mask = mask;
    hit_n = 0;
    /* the step: along x if the ends differ in x, else along z (else none) */
    if (sx != dx) stx = dx > sx ? 1 : -1;
    else if (sz != dz) stz = dz > sz ? 1 : -1;
    for (;;) {
        if (wall_at(x, z)) {
            tr_add(H_WALL, 0);
            return hit_n;
        }
        if ((mask & 0x100) && x == S.px && z == S.pz) {     /* the player, if asked for */
            tr_add(H_PLAYER, 0);
            return hit_n;
        }
        li = line_at(x, z);                                  /* a closed door here */
        if (li != NONE_B && mask_has(mask, t_line_etype[li]) && hit_n < 8) tr_add(H_LINE, li);
        c = ((u16)z << 5) | x;
        hid = hidden_at(c);                                  /* blocker taken away */
        if (!hid) {
            st = t_static_block[c];
            if (st && mask_has(mask, st) && hit_n < 8) tr_add(H_STATIC, st);
        }
        i = ent_at(x, z, 0);                                 /* each entity on the cell */
        while (i != NONE) {
            if (i != self && mask_has(mask, ent_etype(i)) && hit_n < 8) tr_add(H_ENT, i);
            i = ent_at(x, z, i + 1);
        }
        if (!hid && tr_thin(x, z, stx, stz)) return hit_n;   /* stopped by a thin wall */
        if (hit_n >= 8) return hit_n;
        if (stx) {                                           /* reached the end cell */
            if (x == dx) return hit_n;
        } else if (z == dz)
            return hit_n;
        x += stx;
        z += stz;
    }
}

/* ------------------------------------------------------------ combat arithmetic */
/* A monster's combat numbers, copied out of S by cb_monster: health,
   armour (and their maxima), defence, strength, agility, accuracy, and
   its class (its def's subtype; dmg_mult's row) */
static u8 cb_hp, cb_maxhp, cb_armor, cb_maxarmor, cb_def, cb_str, cb_agi, cb_acc, cb_class;

/* cb_monster: copies entity i's monster stats (from its slot's m_*
 * arrays) and class into the cb_* variables. */
static void cb_monster(u16 i)
{
    u8 m = mon_slot(i);
    cb_hp = S.m_hp[m];
    cb_maxhp = S.m_maxhp[m];
    cb_armor = S.m_armor[m];
    cb_maxarmor = S.m_maxarmor[m];
    cb_def = S.m_def[m];
    cb_str = S.m_str[m];
    cb_agi = S.m_agi[m];
    cb_acc = S.m_acc[m];
    cb_class = esub(i);
}

/* cb_exp: the experience entity i (a monster) is worth:
 * (5 (defence + strength + max health + max armour)
 *  + 3 (agility + accuracy) + 49) / 50, kept to 8 bits. */
static u16 cb_exp(u16 i)
{
    u8 m = mon_slot(i);
    u32 v = ((u32)S.m_def[m] + S.m_str[m] + S.m_maxhp[m] + S.m_maxarmor[m]) * 5 +
            ((u32)S.m_agi[m] + S.m_acc[m]) * 3 + 49;
    return (u16)((v / 50) & 0xFF);
}

/* range_excess: tiles^2 - rmin^2 - 1: how far past a weapon's best range
 * (rmin tiles) a target `tiles` away is; > 0 means too far. */
static s16 range_excess(u8 tiles, u8 rmin)
{
    return (s16)((u16)tiles * tiles - (u16)rmin * rmin - 1);
}

/* accuracy, weapon, the target's agility, tiles (0 = adjacent / the player)
 * Rolls whether an attack hits. The chance, out of 256, is
 * (acc * 256 / agi) / 2 + wpn_rmax * 32 / 25. Past the weapon's range
 * (range_excess > 0): a weapon with rmin 0 (melee) misses; others, except
 * shotguns (ammo type 2, shells), lose 76/256 of the chance per step k = 2, 3...
 * while k^2 stays under the excess. Then one rnd(): under chance / 20 a
 * critical, under the chance a hit, else a miss.
 * Out: HIT_MISS, HIT_HIT or HIT_CRIT. Side effect: one rnd() (none if the
 * melee range check fails). */
static u8 calc_hit(u8 acc, u8 weapon, u8 agi, u8 tiles)
{
    s16 ch, ex = -1, dec, d;
    u16 k;
    u8 r, rmin = wpn_rmin[weapon];
    if (!agi) agi = 1;                                   /* (no division by 0) */
    ch = (s16)((((u16)acc << 8) / agi) >> 1);
    ch += (s16)(((u16)wpn_rmax[weapon] << 5) / 25);
    if (tiles) ex = range_excess(tiles, rmin);
    if (ex > 0) {
        if (!rmin) return HIT_MISS;
        if (wpn_ammo[weapon] != 2) {
            dec = (s16)(((s32)ch * 76) >> 8);           /* about 30% of the chance */
            k = 2;
            d = 1;
            while (d < ex) {
                ch -= dec;
                d = (s16)(k * k);
                k++;
            }
        }
    }
    r = rnd();
    if ((s16)r >= ch) return HIT_MISS;
    if ((s16)r < fdiv(ch, 20)) return HIT_CRIT;
    return HIT_HIT;
}

/* strength, weapon, the target's defence and class (0xFF the player),
   multiplier (256, 512 crit), tiles -> dmg_hp, dmg_arm
 * Rolls an attack's damage. Fixed point: d is damage x 256 (8.8). It is
 * (wpn_min + (wpn_max - wpn_min) * rnd / 256) scaled by str / dfn, by
 * mult / 256 and by the class's dmg_mult / 256 (dmg_mult is per class
 * and weapon; the player has none). Past the weapon's range it loses
 * 76/256 of itself per step, as in calc_hit. It is then held to
 * 1..999 (256..255744 in 8.8) and split: wpn_armor / 256 of it is the
 * part that goes to armour (dmg_arm), the rest to health (dmg_hp), both
 * rounded. Side effect: one rnd(). */
static void calc_damage(u8 str, u8 weapon, u8 dfn, u8 tclass, u16 mult, u8 tiles)
{
    u8 r = rnd();
    u32 ratio, a, du, cm;
    s32 d, dec, arm;
    s16 ex = -1, dist;
    u16 k;
    if (!dfn) dfn = 1;
    ratio = (((u32)str << 8) / dfn) & 0xFFFF;            /* str / dfn, 8.8 */
    a = ((u32)wpn_min[weapon] << 8) + (u32)(u16)(wpn_max[weapon] - wpn_min[weapon]) * r;   /* base, 8.8 */
    du = (a * ratio) >> 8;
    du = (du * mult) >> 8;
    cm = tclass == 0xFF ? 256 : dmg_mult[(u16)tclass * N_WEAPONS + weapon];
    du = (du * cm) >> 8;
    d = (s32)du;
    if (tiles) ex = range_excess(tiles, wpn_rmin[weapon]);
    dec = (s32)((u32)d * 76) >> 8;
    k = 2;
    dist = 1;
    while (dist < ex) {                                  /* the range fall-off */
        d -= dec;
        dist = (s16)(k * k);
        k++;
    }
    if (d > 255744L) d = 255744L;                        /* 999.0 */
    if (d < 256) d = 256;                                /* 1.0 */
    arm = (s32)((u32)d * wpn_armor[weapon]) >> 8;
    dmg_hp = (u8)(((d - arm) + 128) >> 8);
    dmg_arm = (u8)((arm + 128) >> 8);
}

/* setup_enemy's spread: se_parm the def's parameter p, se_i2 = p / 4,
   se_i3 = p / 2 */
static s16 se_i2, se_i3;
static s32 se_parm;

/* se_factor: a random scale factor (8.8, 256 = x1) from p - p/4 up to
 * p + p/4 (se_i3 * rnd / 256 + p - p/4). Side effect: one rnd(). */
static s16 se_factor(void)
{
    s32 f = ((s32)se_i3 * rnd()) >> 8;
    f += se_parm;
    f -= se_i2;
    return (s16)f;
}

/* se_scale: v * f / 256, held to 0..255. */
static u8 se_scale(u8 v, s16 f)
{
    return clamp8(((s32)v * f) >> 8);
}

/* setup_enemy: gives monster slot m its own stats: each of its base
 * stats is scaled by a random factor around parm / 256 (se_factor).
 * Health and max health share one factor; armour, max armour and defence
 * share the next; then strength, agility and accuracy one each (five
 * rnd() calls). In: m, parm (the def's parameter), S.m_* holding the
 * base stats. Out: S.m_* of slot m. */
static void setup_enemy(u8 m, s32 parm)
{
    s16 f;
    se_parm = parm;
    se_i2 = (s16)(parm >> 2);
    se_i3 = (s16)(se_i2 * 2);
    f = se_factor();
    S.m_maxhp[m] = S.m_hp[m] = se_scale(S.m_maxhp[m], f);
    f = se_factor();
    S.m_maxarmor[m] = S.m_armor[m] = se_scale(S.m_maxarmor[m], f);
    S.m_def[m] = se_scale(S.m_def[m], f);
    f = se_factor();
    S.m_str[m] = se_scale(S.m_str[m], f);
    f = se_factor();
    S.m_agi[m] = se_scale(S.m_agi[m], f);
    f = se_factor();
    S.m_acc[m] = se_scale(S.m_acc[m], f);
}

/* ------------------------------------------------------------ the player's numbers */
/* add_health: adds n (may be negative) to S.hp, held to 0..maxhp, and
 * says "GAINED n HEALTH" if it went up. */
static void add_health(s16 n)
{
    s16 before = S.hp, v = (s16)S.hp + n;
    if (v > (s16)S.maxhp) v = S.maxhp;
    if (v < 0) v = 0;
    S.hp = (u8)v;
    if (v - before > 0) say_num(gtext(TX_GAINED), (u16)(v - before), gtext(TX_HEALTH_GAINED));
}

/* add_armor: adds n to S.armor, held to maxarmor (no lower bound: a
 * negative result wraps, as in the model), and says "GAINED n ARMOR" if
 * it went up. */
static void add_armor(s16 n)
{
    s16 before = S.armor, v = (s16)S.armor + n;
    if (v > (s16)S.maxarmor) v = S.maxarmor;
    S.armor = (u8)v;
    v &= 0xFF;                      /* compare what was stored */
    if (v - before > 0) say_num(gtext(TX_GAINED), (u16)(v - before), gtext(TX_ARMOR_GAINED));
}

/* add_ammo: adds n of ammo type t (0..5), held to 99.
 * Out: 0 if it was already full (nothing taken), 1 otherwise. */
static u8 add_ammo(u8 t, u8 n)
{
    u16 v;
    if (S.ammo[t] == 99) return 0;
    v = (u16)S.ammo[t] + n;
    S.ammo[t] = (u8)(v > 99 ? 99 : v);
    return 1;
}

/* stat_up: v + n, held to 99. */
static u8 stat_up(u8 v, u8 n)
{
    if ((u16)v + n > 99) n = 99 - v;
    return (u8)(v + n);
}

/* level_up: the player goes up a level: the next level needs
 * level * 20 + 60 XP; max health and max armour grow by 3..5 (to 99 at
 * most) and health is refilled; defence, strength, agility and accuracy
 * grow by 1 or 2. Six rnd() calls, a sound and a message. */
static void level_up(void)
{
    u8 n;
    S.level++;
    SP16(nextxp, (u16)S.level * 20 + 60);
    n = 3 + rnd() % 3;
    if ((u16)S.maxhp + n > 99) n = 99 - S.maxhp;
    S.maxhp += n;
    S.hp = S.maxhp;
    n = 3 + rnd() % 3;
    if ((u16)S.maxarmor + n > 99) n = 99 - S.maxarmor;
    S.maxarmor += n;
    S.defense = stat_up(S.defense, (u8)((rnd() & 1) + 1));
    S.strength = stat_up(S.strength, (u8)((rnd() & 1) + 1));
    S.agility = stat_up(S.agility, (u8)((rnd() & 1) + 1));
    S.accuracy = stat_up(S.accuracy, (u8)((rnd() & 1) + 1));
    sfx(SFX_LEVEL_UP);
    say_num(gtext(TX_LEVEL_UP), S.level, "");
}

/* add_xp: gives the player n experience points (with a message), and a
 * level for each S.nextxp reached (the rest carries over). */
static void add_xp(u16 n)
{
    say_num(gtext(TX_GAINED), n, gtext(TX_XP_GAINED));
    SP16(xp, SG16(xp) + n);
    while (SG16(xp) >= SG16(nextxp)) {
        SP16(xp, SG16(xp) - SG16(nextxp));
        level_up();
    }
}

/* weapon_ok: 1 if the player has weapon w (bit w of S.weapons) and,
 * if it uses ammo (wpn_use), has some of its ammo type (wpn_ammo). */
static u8 weapon_ok(u8 w)
{
    if (!((SG16(weapons) >> w) & 1)) return 0;
    if (!wpn_use[w]) return 1;
    return S.ammo[wpn_ammo[w]] != 0;
}

/* select_next_weapon: S.weapon becomes the next usable weapon after the
 * current one among the player's 12 (0..11), wrapping round to 0; it
 * stays if there is none. */
static void select_next_weapon(void)
{
    u8 cur = S.weapon, w;
    for (w = cur + 1; w < 12; w++)
        if (weapon_ok(w)) {
            S.weapon = w;
            return;
        }
    if (!cur) return;
    for (w = 0; w < cur; w++)
        if (weapon_ok(w)) {
            S.weapon = w;
            return;
        }
}

/* restore_weapons: gives back the weapons an event took away
 * (S.disabled_weapons). Bits 9-11 (0xE00) are the dog "weapons": if the
 * player has a dog again, the taken-away dog is dropped instead of
 * restored. If the current weapon is not held, picks the next one. */
static void restore_weapons(void)
{
    u16 dw;
    if ((SG16(weapons) & 0xE00) && (SG16(disabled_weapons) & 0xE00))
        SP16(disabled_weapons, SG16(disabled_weapons) & ~0xE00u);
    dw = SG16(disabled_weapons);
    SP16(weapons, SG16(weapons) | dw);
    SP16(disabled_weapons, 0);
    if (!((SG16(weapons) >> S.weapon) & 1)) select_next_weapon();
}

/* player_pain: the player takes dmg8 health damage and arm armour damage.
 * Nothing in god mode. With a dog (weapons 9..11) out and alive (its
 * health is S.ammo[5]), the dog takes it all, and may die (the dog
 * weapons are lost). Otherwise armour absorbs arm (what it cannot goes to
 * health), a warning is said when health falls below about 10% or 30%
 * (26/256, 78/256 of max) or the armour runs out, and the player dies at
 * 0 (S.dead). Side effects: messages, sfx, player_hurt = 1. */
static void player_pain(u8 dmg8, u8 arm)
{
    s16 dmg = dmg8, h1, h2, v;
    u8 a;
    if (S.god) return;
    if (S.weapon >= 9 && S.weapon <= 11 && S.ammo[5]) {     /* a dog fighting for you takes it all */
        v = (s16)S.ammo[5] - (dmg + arm);
        S.ammo[5] = v > 0 ? (u8)v : 0;
        say_num(gtext(TX_DOG_TOOK), (u16)(dmg + arm), gtext(TX_DAMAGE));
        player_hurt = 1;
        sfx(SFX_HURT);
        if (!S.ammo[5]) {
            SP16(weapons, SG16(weapons) & ~0xE00u);         /* the dog weapons gone */
            select_next_weapon();
            say(gtext(TX_DOG_DIED));
        }
        return;
    }
    say_num("", (u16)(dmg + arm), gtext(TX_DAMAGE));
    if (!(dmg + arm)) return;
    player_hurt = 1;
    a = S.armor;
    if (a < arm) {                          /* armour used up: the rest hurts */
        dmg += arm - a;
        S.armor = 0;
    } else
        S.armor = a - arm;
    /* health as a fraction of the maximum (x 256), before and after */
    h1 = (s16)(((u16)S.hp << 8) / S.maxhp);
    h2 = 0;
    if ((s16)S.hp > dmg) h2 = (s16)((((u16)S.hp - (u16)dmg) << 8) / S.maxhp);
    if (h2) {
        if (h1 > 26 && h2 <= 26) say(gtext(TX_NEAR_DEATH));
        else if (h1 > 78 && h2 <= 78) say(gtext(TX_LOW_HEALTH));
        else if (a && !S.armor) say(gtext(TX_ARMOR_GONE));
    }
    v = (s16)S.hp - dmg;
    if (v <= 0) {
        S.hp = 0;
        S.dead = 1;
        sfx(SFX_PLAYER_DIE);
    } else {
        S.hp = (u8)v;
        sfx(SFX_HURT);
    }
}

/* ------------------------------------------------------------ monsters */
/* activate: puts entity i on the active list (F_ACTIVE, appended to
 * S.active) so it takes turns; nothing if already active. If the list is
 * full (MAX_ACTIVE) the flag is set but it is not listed. */
static void activate(u16 i)
{
    u16 n;
    if (S.e_flags[i] & F_ACTIVE) return;
    S.e_flags[i] |= F_ACTIVE;
    n = SG16(n_active);
    if (n >= MAX_ACTIVE) return;
    P16(S.active + 2 * n, i);
    SP16(n_active, n + 1);
}

/* deactivate: takes entity i off the active list (clears F_ACTIVE,
 * closes the gap in S.active, zeroes the freed last entry). */
static void deactivate(u16 i)
{
    u16 n, k;
    if (!(S.e_flags[i] & F_ACTIVE)) return;
    S.e_flags[i] &= ~F_ACTIVE;
    n = SG16(n_active);
    for (k = 0; k < n; k++)
        if (G16(S.active + 2 * k) == i) {
            for (; k + 1 < n; k++) {            /* shift the rest down a word */
                S.active[2 * k] = S.active[2 * k + 2];
                S.active[2 * k + 1] = S.active[2 * k + 3];
            }
            P16(S.active + 2 * (n - 1), 0);
            SP16(n_active, n - 1);
            return;
        }
}

/* can (x, z) be seen: within 5 cells ahead, inside the 90-degree cone, a
   grid line of sight with no wall or closed door (PORT: the original
   activates a monster when its renderer draws it)
 * In: the cell, S.px/pz/facing. Out: 1 if visible. "depth" is the
 * distance ahead along the facing, "lat" the distance to the side; the
 * cone is |lat| <= depth. The line of sight samples the straight line
 * from the player to the cell at each of max(|dx|, |dz|) - 1 steps,
 * rounding to the nearest cell. */
static u8 visible(u8 x, u8 z)
{
    s16 dx = (s16)x - S.px, dz = (s16)z - S.pz, adx, adz, depth, lat, steps, s, cx, cz;
    adx = dx < 0 ? -dx : dx;
    adz = dz < 0 ? -dz : dz;
    if (adx > 5 || adz > 5) return 0;
    switch (S.facing & 3) {             /* turn (dx, dz) into the player's frame */
    case 0: depth = -dz; lat = dx; break;
    case 1: depth = dx; lat = dz; break;
    case 2: depth = dz; lat = -dx; break;
    default: depth = -dx; lat = -dz; break;
    }
    if (lat < 0) lat = -lat;
    if (depth < 1 || depth > 5 || lat > depth) return 0;
    steps = adx > adz ? adx : adz;
    for (s = 1; s < steps; s++) {
        /* round(dx * s / steps), as floor((2 dx s + steps) / (2 steps)) */
        cx = fdiv((s16)(2 * dx * s + steps), (s16)(2 * steps));
        cz = fdiv((s16)(2 * dz * s + steps), (s16)(2 * steps));
        cx = (u8)(cx + S.px);
        cz = (u8)(cz + S.pz);
        if (wall_at((u8)cx, (u8)cz)) return 0;                  /* a wall in the way */
        if (line_at((u8)cx, (u8)cz) != NONE_B) return 0;        /* a closed door */
    }
    return 1;
}

/* activate_visible: wakes (activate) every live, linked, not yet active
 * enemy among the map's entities that the player can see (visible). */
static void activate_visible(void)
{
    u16 i;
    for (i = 0; i < n_map_ents; i++)
        if ((S.e_flags[i] & (F_LINKED | F_ALIVE | F_ACTIVE)) == (F_LINKED | F_ALIVE) && ent_etype(i) == ET_ENEMY &&
            visible(S.e_x[i], S.e_z[i]))
            activate(i);
}

/* spawn_drop: a monster (entity i) that just died may leave an item.
 * One rnd32(), always (unless already dropped or a subtype that never
 * drops: 1 the hell hounds, 12 the Cyberdemon, 13 Kronos). Its low byte < 51 (about 20%):
 * nothing. Bit 8 picks the kind: set, health (n5 < 115), armour
 * (< 230) or credits (5-credit pick-up more likely for the stronger variants,
 * by the def's parameter 294 / 467 / 640); clear, an item that depends
 * on the monster's subtype (ammo, medkits, credits). The item goes into
 * the next of the N_DROPS drop slots (S.drop_index, round robin; an older
 * drop there is taken away) on the monster's cell.
 * Writes S.e_flags[i] (F_DROPPED), S.drop_index and the slot's entity. */
static void spawn_drop(u16 i)
{
    u32 r = rnd32(), n, n2;
    u8 es = esub(i), n5, n6, b3, b4, n7;
    s8 dt = -1;
    s32 parm;
    u16 s;
    if (S.e_flags[i] & F_DROPPED) return;
    if (es == 1 || es == 12 || es == 13) return;
    if ((u8)r < 51) return;
    S.e_flags[i] |= F_DROPPED;
    n = r >> 8;
    n2 = n >> 1;
    n5 = (u8)n2;                        /* random bits 9-16 */
    n6 = (u8)((u16)n2 >> 8);            /* random bits 17-24 */
    if (n & 1) {
        if (n5 < 115) dt = DD_HEALTH;
        else if (n5 < 230) dt = DD_ARMOR;
        else {
            parm = def_parm(S.e_def[i]);
            n7 = parm == 294 ? 3 : parm == 467 ? 8 : parm == 640 ? 13 : 0;
            dt = (u16)n5 < 230 + (u16)n7 ? DD_CREDIT5 : DD_CREDIT1;
        }
    } else {
        b3 = n5 < 76;                   /* about 30% */
        b4 = n6 < 15;                   /* about 6% */
        /* by subtype: DD_AMMO + k / DD_AMMO2 + k are ammo type k's small
           and big pick-ups */
        switch (es) {
        case 0: dt = (s8)((b4 ? DD_AMMO2 : DD_AMMO) + 1 + b3); break;
        case 2: dt = (s8)(((b4 || b3) ? DD_AMMO2 : DD_AMMO) + 1); break;
        case 6: dt = b4 ? DD_LGMED : DD_SMMED; break;
        case 7: dt = DD_CREDIT5; break;
        case 8: case 9: dt = (s8)((b4 ? DD_AMMO2 : DD_AMMO) + es - 5); break;
        case 10: dt = b3 ? (b4 ? DD_LGMED : DD_SMMED) : DD_CREDIT5; break;
        case 11: dt = (s8)(DD_AMMO2 + (b3 ? 4 : 2)); break;
        }
    }
    if (dt < 0) return;
    s = n_map_ents + S.drop_index;          /* the next drop slot */
    S.drop_index = (S.drop_index + 1) & 7;
    ent_unlink(s);                          /* whatever it held goes */
    S.e_def[s] = t_drop_def[(u8)dt];
    S.e_x[s] = S.e_x[i];
    S.e_z[s] = S.e_z[i];
    S.e_flags[s] = 0;
    ent_link(s);                            /* the new item appears */
}

/* (defined with the events, further down) */
static void run_tile(u8 x, u8 z, u32 flags);
static u8 run_tile_r(u8 x, u8 z, u32 flags);

/* cell_taken: 1 if (x, z) is off the map, a wall, or holds anything a
 * monster's step would hit (MASK_MONSTER_MOVE without bit 8, so the
 * player does not count). Overwrites the hit list. */
static u8 cell_taken(s16 x, s16 z)
{
    if ((u16)x >= 32 || (u16)z >= 32) return 1;
    if (wall_at((u8)x, (u8)z)) return 1;
    return trace((u8)x, (u8)z, (u8)x, (u8)z, MASK_MONSTER_MOVE - 0x100, NONE) != 0;
}

/* place_at: puts entity j on cell (x, z) and links it into the map. */
static void place_at(u16 j, u8 x, u8 z)
{
    S.e_x[j] = x;
    S.e_z[j] = z;
    ent_link(j);
}

/* release_souls' order of the neighbouring cells: west, east, north, south */
static const s8 RS_DX[4] = {-1, 1, 0, 0};
static const s8 RS_DZ[4] = {0, 0, -1, 1};

/* release_souls: a monster of subtype 7 (Beholder / Rahovart / Pain
 * Elemental; entity i) died: it is hidden
 * (F_HIDDEN, once only) and the two lost souls it carries (t_spawns: its
 * row is entity, first soul, second soul) appear: the first on its cell
 * if free, the second on the first free neighbour (not the player's). */
static void release_souls(u16 i)
{
    u16 n, k, s1 = NONE, s2 = NONE;
    const u8 *p;
    u8 x, z, d, found = 0;
    s16 nx, nz;
    if (S.e_flags[i] & F_HIDDEN) return;
    S.e_flags[i] |= F_HIDDEN;
    n = G16(t_spawns);
    p = t_spawns + 2;
    for (k = 0; k < n; k++, p += 6)         /* find i's row */
        if (G16(p) == i) {
            s1 = G16(p + 2);
            s2 = G16(p + 4);
            found = 1;
            break;
        }
    if (!found) return;
    x = S.e_x[i];
    z = S.e_z[i];
    if (s1 != NONE && !cell_taken(x, z)) place_at(s1, x, z);
    if (s2 == NONE) return;
    for (d = 0; d < 4; d++) {
        nx = (s16)x + RS_DX[d];
        nz = (s16)z + RS_DZ[d];
        if (cell_taken(nx, nz)) continue;
        if ((u8)nx == S.px && (u8)nz == S.pz) continue;
        place_at(s2, (u8)nx, (u8)nz);
        return;
    }
}

/* monster_died: entity i (a monster) dies. Clears F_ALIVE (nothing if
 * already dead), plays the death sound, gives the player its XP, takes
 * it off the active list and out of the map. It leaves a corpse
 * (F_CORPSE, which an Archvile can raise) unless it was an overkill or is
 * subtype 8. Then a possible drop, and the special deaths: the
 * Cyberdemon (12), Kronos (13) and class 7 (Beholder, Rahovart, the souls' spawner).
 * Subtype 8 (Ghoul / Fiend / Revenant) explodes: no corpse. */
static void monster_died(u16 i, u8 overkill)
{
    u8 es;
    if (!(S.e_flags[i] & F_ALIVE)) return;
    S.e_flags[i] &= ~F_ALIVE;
    sfx(SFX_MON_DIE);
    add_xp(cb_exp(i));                 /* the player's reward */
    deactivate(i);                     /* no more turns */
    ent_unlink(i);                     /* out of the map */
    es = esub(i);
    if (!overkill && es != 8) S.e_flags[i] |= F_CORPSE;
    spawn_drop(i);                     /* maybe an item */
    es = esub(i);
    if (es == 12) {                    /* the Cyberdemon: the Reactor's exit opens */
        run_tile(18, 19, EVF_USE);     /* (the event on cell 18, 19, as if used) */
        if (S.e_flags[i] & F_CORPSE) {     /* its body bursts; from now on nothing hurts */
            S.e_flags[i] = (S.e_flags[i] & ~F_CORPSE) | F_HIDDEN;
            S.god = 1;
        }
    } else if (es == 13)               /* Kronos (no event there in the maps we have) */
        run_tile(31, 0, EVF_USE);
    else if (es == 7)
        release_souls(i);              /* its lost souls come out */
}

/* monster_pain: monster entity j takes dmg8 health and arm armour damage
 * (what its armour cannot take goes to health; health stops at 0). Does
 * not kill it: the caller checks S.m_hp. */
static void monster_pain(u16 j, u8 dmg8, u8 arm)
{
    u8 m = mon_slot(j), a = S.m_armor[m];
    s16 dmg = dmg8, v;
    if (a < arm) {
        dmg += arm - a;
        S.m_armor[m] = 0;
    } else
        S.m_armor[m] = a - arm;
    v = (s16)S.m_hp[m] - dmg;
    S.m_hp[m] = v > 0 ? (u8)v : 0;
}

/* monster_mask: the trace mask for entity i's steps: MASK_MONSTER_MOVE,
 * less the fires for the flying classes (4 and 13 ignore types 10 and 11
 * (3072 = bits 10, 11), 6 and 7 type 11 (2048), 10 type 10 (1024)). */
static u16 monster_mask(u16 i)
{
    u8 es = esub(i);
    u16 m = MASK_MONSTER_MOVE;
    if (es == 4 || es == 13) m &= ~3072u;
    else if (es == 6 || es == 7) m &= ~2048u;
    else if (es == 10) m &= ~1024u;
    return m;
}

/* the monsters' step directions, in the order tried: east, west, south,
   north; CODE: the original's number for each (0 N, 1 S, 2 E, 3 W) */
static const s8 OX[4] = {1, -1, 0, 0};
static const s8 OZ[4] = {0, 0, 1, -1};
static const u8 CODE[4] = {2, 3, 1, 0};
/* can_step's mask and the entity making the step (not a hit for itself) */
static u16 cp_mask, cp_ent;

/* can_step: 1 if nothing in cp_mask blocks the trace from (sx, sz) to
 * (dx, dz) (ignoring cp_ent). Overwrites the hit list. */
static u8 can_step(u8 sx, u8 sz, u8 dx, u8 dz)
{
    return trace(sx, sz, dx, dz, cp_mask, cp_ent) == 0;
}

/* calc_path: how good (sx, sz) is as a step toward (dx, dz), lower is
 * better (the original's Entity_calcPath). It takes up to two greedy
 * steps from there, each to the open neighbour nearest the goal (squared
 * distance; the first of equals in OX/OZ order). Out: 0 if the goal is
 * the cell itself or reached by the first step, 1 if by the second, else
 * the squared distance left + 2. */
static u16 calc_path(u8 sx, u8 sz, u8 dx, u8 dz)
{
    u16 best, nd, k;
    s16 ex, ez;
    s8 stx, stz;
    u8 d;
    if (sx == dx && sz == dz) return 0;
    for (k = 0; k < 2; k++) {
        best = 0xFFFF;
        stx = stz = 0;
        for (d = 0; d < 4; d++) {
            ex = (s16)sx + OX[d] - dx;
            ez = (s16)sz + OZ[d] - dz;
            nd = (u16)(ex * ex + ez * ez);
            if (nd >= best) continue;           /* not nearer than the best so far */
            if (!can_step(sx, sz, (u8)(sx + OX[d]), (u8)(sz + OZ[d]))) continue;
            best = nd;
            stx = OX[d];
            stz = OZ[d];
        }
        sx += stx;                              /* (stays if no step is open) */
        sz += stz;
        if (sx == dx && sz == dz) return k;
    }
    ex = (s16)sx - dx;
    ez = (s16)sz - dz;
    return (u16)(ex * ex + ez * ez + 2);
}

/* entity i steps toward (tx, tz): two-step greedy look-ahead, ties at random -> 1 if it moved
 * Each open neighbour is scored by calc_path; the best ones (equal
 * scores, at most 4) are listed and one is picked with one rnd()
 * ((rnd & 3) % n). No rnd() if no step is open. Moves the entity
 * (ent_move_to); overwrites the hit list. */
static u8 ai_move(u16 i, u8 tx, u8 tz)
{
    u8 sx = S.e_x[i], sz = S.e_z[i], order[4], n = 0, d, nx, nz, pick;
    u16 best = 9999, pd;
    cp_mask = monster_mask(i);
    cp_ent = i;
    for (d = 0; d < 4; d++) {
        nx = (u8)(sx + OX[d]);
        nz = (u8)(sz + OZ[d]);
        if (!can_step(sx, sz, nx, nz)) continue;
        pd = calc_path(nx, nz, tx, tz);
        /* (the east step, tried first, has its own branch; it does the
           same as the general one below, since best is still 9999) */
        if (CODE[d] == 2) {
            if (pd < 9999) {
                best = pd;
                n = 0;
            } else if (pd != 9999)
                continue;
        } else if (pd < best) {
            best = pd;
            n = 0;
        } else if (pd != best)
            continue;
        order[n++] = d;
    }
    if (!n) return 0;
    pick = order[(rnd() & 3) % n];             /* a tie broken at random */
    ent_move_to(i, (u8)(sx + OX[pick]), (u8)(sz + OZ[pick]));
    return 1;
}

/* queue_attack: records that entity i attacks this turn: kind (T_*) and
 * target tgt (an entity or a line; unused for T_PLAYER). An entity is
 * queued once. New attacks go at the front, so the list runs from the
 * last queued to the first (the order they are carried out in). */
static void queue_attack(u16 i, u8 kind, u16 tgt)
{
    u8 k;
    for (k = 0; k < att_n; k++)
        if (att_ent[k] == i) return;
    if (att_n >= MAX_ACTIVE) return;
    for (k = att_n; k > 0; k--) {              /* make room at the front */
        att_ent[k] = att_ent[k - 1];
        att_kind[k] = att_kind[k - 1];
        att_tgt[k] = att_tgt[k - 1];
    }
    att_ent[0] = i;
    att_kind[0] = kind;
    att_tgt[0] = tgt;
    att_n++;
}

/* attack_weapon: the weapon (wpn_* index) monster i attacks with: its
 * class's first or second attack (mon_atk, two per class), by F_ATK2. */
static u8 attack_weapon(u16 i)
{
    return mon_atk[esub(i) * 2 + ((S.e_flags[i] & F_ATK2) ? 1 : 0)];
}

/* dist2: the squared distance between two cells. */
static u16 dist2(u8 ax, u8 az, u8 bx, u8 bz)
{
    s16 dx = (s16)ax - bx, dz = (s16)az - bz;
    return (u16)(dx * dx + dz * dz);
}

/* entity i decides: move, attack or (an Archvile) raise a corpse -> 1 if it moved
 * An Archvile class monster (subtype 10) first, with a chance of 84/256,
 * looks for a corpse 1 or 2 cells away (squared distance <= 4) on a free
 * cell to raise; if it finds none it swaps between its two attacks
 * (F_ATK2). Then: if the player is on the same row or column within the
 * attack's reach and the shot is clear (or the first thing in the way is
 * a destructible, which it hits instead), it attacks (always for a melee
 * attack, else with a chance of 217/256). Otherwise it moves toward the
 * cell in front of the player (sometimes one cell further from the
 * player, to keep a ranged attacker at a distance). Attacks are queued
 * (queue_attack), not carried out here. */
static u8 ai_think(u16 i)
{
    u16 j, d2, reach, half, tgt = 0;
    u8 aligned, hit = 0, kind = T_PLAYER, tx, tz;
    if (esub(i) == 10) {
        if (rnd() < 84)
            for (j = 0; j < n_map_ents; j++) {          /* a corpse near enough to raise */
                if (!(S.e_flags[j] & F_CORPSE)) continue;
                d2 = dist2(S.e_x[j], S.e_z[j], S.e_x[i], S.e_z[i]);
                if (!d2 || d2 > 4) continue;
                if (S.e_flags[j] & F_RAISING) continue;
                if (trace(S.e_x[j], S.e_z[j], S.e_x[j], S.e_z[j], MASK_RAISE_FREE, NONE)) continue;
                S.e_flags[j] |= F_RAISING;              /* (no other Archvile takes it) */
                queue_attack(i, T_RAISE, j);
                return 0;
            }
        S.e_flags[i] ^= F_ATK2;                         /* alternate its attacks */
    }
    reach = (u16)wpn_rmin[attack_weapon(i)] + 1;        /* the attack's range in cells */
    d2 = dist2(S.e_x[i], S.e_z[i], S.px, S.pz);
    aligned = S.e_x[i] == S.px || S.e_z[i] == S.pz;
    if (aligned && d2 <= reach * reach) {
        if (!trace(S.e_x[i], S.e_z[i], S.px, S.pz, MASK_SHOT, i))  /* clear line to the player */
            hit = 1;
        else if (hit_kind[0] == H_ENT && ent_etype(hit_val[0]) == ET_DESTRUCT) {
            kind = T_ENT;            /* a barrel or crate in the way: hit that */
            tgt = hit_val[0];
            hit = 1;
        }
    }
    half = reach >> 1;
    /* a melee attack (reach 1, half 0) always attacks; others 217/256 */
    if (hit && (!half || rnd() < 217)) {
        queue_attack(i, kind, tgt);
        return 0;
    }
    /* toward the cell in front of the player (or beside it) */
    tx = (u8)(S.px + DX[S.facing]);
    tz = (u8)(S.pz + DZ[S.facing]);
    /* close and in line: sometimes (38/256) aim one cell toward the
       monster's own side instead, to keep its distance */
    if (d2 <= half * half && aligned && rnd() < 38) {
        if (S.e_x[i] < S.px) tx--;
        else if (S.e_x[i] > S.px) tx++;
        if (S.e_z[i] < S.pz) tz--;
        else if (S.e_z[i] > S.pz) tz++;
    }
    if (ai_move(i, tx, tz)) {
        S.e_move[i] = 0;                    /* its first step this turn */
        return 1;
    }
    if (hit) queue_attack(i, kind, tgt);    /* could not move: attack after all */
    return 0;
}

/* entity i has finished a step: on (lost souls go three), or lunge -> 1 if it moved again
 * S.e_move[i] counts its steps this turn. Subtypes 4 and 13 may take 3
 * steps, the rest 1. Next to the player (same row or column, 1 cell) it
 * stops stepping; then the charging classes (1 hounds, 4 lost souls,
 * 5 bull demons, 13 Kronos) queue an attack on the player if the line is
 * clear. Otherwise it takes another step toward the player (ai_move). */
static u8 ai_after_step(u16 i)
{
    u8 es = esub(i), steps = (es == 4 || es == 13) ? 3 : 1, adj = 0;
    if ((S.e_x[i] == S.px || S.e_z[i] == S.pz) && dist2(S.e_x[i], S.e_z[i], S.px, S.pz) <= 1) adj = 1;
    S.e_move[i]++;
    if (S.e_move[i] < steps) {
        if (adj)
            S.e_move[i] = steps;            /* arrived: no more steps */
        else if (ai_move(i, S.px, S.pz))
            return 1;
    }
    if (S.e_move[i] != steps || !adj) return 0;
    if (es != 5 && es != 4 && es != 1 && es != 13) return 0;
    if (trace(S.e_x[i], S.e_z[i], S.px, S.pz, MASK_SHOT, i)) return 0;   /* something in between */
    queue_attack(i, T_PLAYER, 0);
    return 0;
}

/* ------------------------------------------------------------ explosions and destruction */
static void hurt_at(s16 x, s16 z, u8 dmg, u8 arm);
static void destroy(u16 j);

/* an explosion's reach: the 4 neighbours (full damage), then the 4
   diagonal cells (half damage) */
static const s8 EX_AX[4] = {-1, 1, 0, 0};
static const s8 EX_AZ[4] = {0, 0, -1, 1};
static const s8 EX_BX[4] = {-1, 1, 1, -1};
static const s8 EX_BZ[4] = {1, 1, -1, -1};

/* run_explosions: sets off the queued explosions, oldest first (a queue
 * in expl_x / expl_z), including any they chain into. Each does
 * 5 + (random byte) / 11 (5..28) to health and to armour of what is on
 * its 4 neighbours, and half that to the 4 diagonal cells (hurt_at),
 * not to its own cell. One rnd32() per explosion. */
static void run_explosions(void)
{
    u8 x, z, k, dmg;
    while (n_pend_expl) {
        x = expl_x[0];                      /* take the oldest off the queue */
        z = expl_z[0];
        for (k = 1; k < n_pend_expl; k++) {
            expl_x[k - 1] = expl_x[k];
            expl_z[k - 1] = expl_z[k];
        }
        n_pend_expl--;
        dmg = (u8)(((u8)rnd32()) / 11 + 5);
        for (k = 0; k < 4; k++) hurt_at((s16)x + EX_AX[k], (s16)z + EX_AZ[k], dmg, dmg);
        if (dmg >> 1)
            for (k = 0; k < 4; k++) hurt_at((s16)x + EX_BX[k], (s16)z + EX_BZ[k], dmg >> 1, dmg >> 1);
    }
}

/* push_explosion: queues an explosion on cell (x, z) (dropped if 8 are
 * already waiting). */
static void push_explosion(u8 x, u8 z)
{
    if (n_pend_expl >= 8) return;
    expl_x[n_pend_expl] = x;
    expl_z[n_pend_expl] = z;
    n_pend_expl++;
}

/* a barrel, crate, fire or coupling j is destroyed
 * Nothing if j is not F_ALIVE. It loses F_ALIVE and leaves the map. By
 * kind: a fire gives 2 XP; a barrel (destructible subtype 1) explodes; a
 * coupling (subtype 4) counts toward closing the Reactor's portal
 * (2 needed); a crate (subtype 2) rolls rnd() for its contents: a trap
 * (2/256, explodes), credits, a small medkit, armour or ammo (rnd again
 * for the type), or nothing (r >= 213); an item reuses the crate's entity.
 * Then runs any queued explosions (which may destroy more). */
static void destroy(u16 j)
{
    u8 et, es, r, dt = 0xFF, d;
    if (!(S.e_flags[j] & F_ALIVE)) return;
    et = ent_etype(j);
    es = esub(j);
    S.e_flags[j] &= ~F_ALIVE;
    ent_unlink(j);                          /* out of the map */
    if (et == ET_FIRE) {
        say(gtext(TX_FIRE_CLEARED));
        add_xp(2);
    } else if (et == ET_DESTRUCT) {
        if (es == 1) {
            say("BARREL EXPLODES!");
            sfx(SFX_EXPLODE);
            push_explosion(S.e_x[j], S.e_z[j]);
        } else if (es == 4) {
            sfx(SFX_EXPLODE);        /* a coupling */
            S.couplings++;
            if (S.couplings == 2) {   /* both: the portal closes (its line's event) */
                S.portal = 0;
                run_tile(PORTAL_X, PORTAL_Z, EVF_USE);
            }
        } else if (es == 2) {
            r = rnd();               /* a crate: what was inside */
            sfx(SFX_BREAK);
            if (r < 2) {
                say(gtext(TX_TRAPPED));
                sfx(SFX_EXPLODE);
                push_explosion(S.e_x[j], S.e_z[j]);
            } else if (r < 4) dt = DD_CREDIT5;
            else if (r < 12) dt = DD_CREDIT1;
            else if (r < 24) dt = DD_SMMED;
            else if (r < 150) dt = DD_ARMOR;
            else if (r < 213) dt = (u8)(DD_AMMO + rnd() % 5);
            if (dt != 0xFF) {
                d = t_drop_def[dt];
                if (d != NONE_B) {          /* the crate's entity becomes the item */
                    S.e_def[j] = d;
                    S.e_flags[j] = 0;
                    ent_link(j);
                }
            }
        }
    }
    run_explosions();
}

/* the cell (x, z) takes dmg health and arm armour damage: the player, or
   the first thing there a shot would hit
 * Nothing if a wall, a closed door or a static blocker that stops shots
 * is there, or the first shootable entity is not alive. A monster is
 * hurt and may die (overkill 0); fires and destructible subtypes 3 and 4
 * (4: the power couplings) are not harmed by explosions; anything else (a barrel, a crate) is
 * destroyed. Called by run_explosions. */
static void hurt_at(s16 x, s16 z, u8 dmg, u8 arm)
{
    u8 li, st, et;
    u16 c, j;
    if (x == S.px && z == S.pz) {
        player_pain(dmg, arm);
        return;
    }
    if (x < 0 || z < 0 || x >= 32 || z >= 32) return;
    if (wall_at((u8)x, (u8)z)) return;
    li = line_at((u8)x, (u8)z);
    if (li != NONE_B && mask_has(MASK_SHOT, t_line_etype[li])) return;
    c = ((u16)z << 5) | (u16)x;
    if (!hidden_at(c)) {
        st = t_static_block[c];
        if (st && mask_has(MASK_SHOT, st)) return;
    }
    /* the first entity on the cell a shot would hit */
    j = ent_at((u8)x, (u8)z, 0);
    while (j != NONE && !mask_has(MASK_SHOT, ent_etype(j))) j = ent_at((u8)x, (u8)z, j + 1);
    if (j == NONE) return;
    et = ent_etype(j);
    if (!(S.e_flags[j] & F_ALIVE)) return;
    if (et == ET_ENEMY) {
        monster_pain(j, dmg, arm);
        if (!S.m_hp[mon_slot(j)]) {
            say2(ent_name(j), gtext(TX_DIED));
            monster_died(j, 0);
        } else
            sfx(SFX_MON_HURT);
        return;
    }
    if (et == ET_FIRE) return;
    if (et == ET_DESTRUCT && (esub(j) == 3 || esub(j) == 4)) return;
    destroy(j);
}

/* entity i attacks (kind, target)
 * Nothing if i has died since it queued the attack. T_RAISE: an
 * Archvile raises corpse tgt if its cell is still free (full health and
 * armour, back in the map and active), else the raise fails. Otherwise
 * the monster's attack weapon fires its class's number of shots
 * (mon_shots): against the player each shot rolls calc_hit with the
 * player's agility and calc_damage with the player's defence (a critical
 * on the first shot doubles that shot and every later one); the damage
 * is summed and dealt at once (player_pain), or TX_DODGED. Against an
 * entity (a barrel or crate in the way) it is simply destroyed. */
static void monster_attack(u16 i, u8 kind, u16 tgt)
{
    u8 w, shots, s, h, crit = 0, m;
    u16 tot = 0, tarm = 0;
    if (!(S.e_flags[i] & F_ALIVE)) return;
    if (kind == T_RAISE) {           /* an Archvile raises a corpse */
        say2(ent_name(i), gtext(TX_CASTS_RAISE));
        sfx(SFX_RAISE);
        if (!trace(S.e_x[tgt], S.e_z[tgt], S.e_x[tgt], S.e_z[tgt], MASK_RAISE_FREE, NONE)) {   /* cell still free */
            S.e_flags[tgt] = (S.e_flags[tgt] & ~(F_CORPSE | F_RAISING)) | F_ALIVE;
            ent_link(tgt);                          /* back in the map */
            m = mon_slot(tgt);
            S.m_hp[m] = S.m_maxhp[m];
            S.m_armor[m] = S.m_maxarmor[m];
            activate(tgt);                          /* and fighting */
            say2(ent_name(tgt), gtext(TX_REVIVED));
        } else {
            S.e_flags[tgt] &= ~F_RAISING;
            say(gtext(TX_RAISE_FAILED));
        }
        return;
    }
    say2(ent_name(i), gtext(TX_ATTACKS));
    w = attack_weapon(i);
    sfx(wpn_sfx[w]);
    shots = mon_shots[esub(i)];
    cb_monster(i);                                  /* its stats into cb_* */
    for (s = 0; s < shots; s++) {
        if (kind != T_PLAYER) {                     /* (no rolls against an object) */
            tot++;
            continue;
        }
        h = calc_hit(cb_acc, w, S.agility, 0);
        if (!h) continue;
        if (h == HIT_CRIT && !s) crit = 1;
        calc_damage(cb_str, w, S.defense, 0xFF, crit ? 512 : 256, 0);
        tot += dmg_hp;
        tarm += dmg_arm;
    }
    if (kind == T_PLAYER) {
        if (!(tot + tarm)) {
            say(gtext(TX_DODGED));
            sfx(SFX_DODGE);
        } else
            player_pain((u8)tot, (u8)tarm);
    } else
        destroy(tgt);
}

/* ------------------------------------------------------------ the portal */
/* The Reactor map's monster portal: it puts monsters on row
   PORTAL_SPAWN_Z, at these x in this order of preference. */
static const u8 portal_xs[3] = {16, 15, 17};

/* up_cand: 1 if entity i is a monster the portal can bring: alive,
 * in the map, not active (not fighting yet) and not hidden. */
static u8 up_cand(u16 i)
{
    return ent_etype(i) == ET_ENEMY &&
           (S.e_flags[i] & (F_ALIVE | F_LINKED | F_ACTIVE | F_HIDDEN)) == (F_ALIVE | F_LINKED);
}

/* Game_updateSpawnPortals: while the Reactor's portal is open, every few
   turns it brings one of the map's living monsters that are not yet
   fighting in front of it, and waits while that one lives
 * State: S.portal (1 open), S.portal_mon (the monster it last brought,
 * u16, NONE none), S.portal_cnt (turns to wait, counted down; signed).
 * When the count runs out: picks one candidate (up_cand) at random
 * (rnd32), moves it to the first free cell of portal_xs on row
 * PORTAL_SPAWN_Z (if none is free it stays where it was), and sets a
 * new wait of 1..4 turns (rnd). With no candidates left the portal
 * closes. Called at the start of each monster turn. */
static void update_portal(void)
{
    u16 i, n = 0, pm;
    s8 cnt;
    u8 k, x;
    s16 pick;
    if (!S.portal) return;
    pm = SG16(portal_mon);
    if (pm != NONE && (S.e_flags[pm] & F_ALIVE)) return;    /* the last one still lives */
    cnt = (s8)S.portal_cnt;
    S.portal_cnt--;
    if (cnt > 0) return;
    for (i = 0; i < n_map_ents; i++)                         /* count the candidates */
        if (up_cand(i)) n++;
    if (!n) {
        S.portal = 0;
        return;
    }
    pick = (s16)((rnd32() & 4095) % n);
    for (i = 0;; i++)                                        /* find the pick-th one */
        if (up_cand(i) && pick-- == 0) break;
    SP16(portal_mon, NONE);
    for (k = 0; k < 3; k++) {
        x = portal_xs[k];
        if (cell_taken(x, PORTAL_SPAWN_Z)) continue;
        if (x == S.px && S.pz == PORTAL_SPAWN_Z) continue;
        ent_move_to(i, x, PORTAL_SPAWN_Z);
        SP16(portal_mon, i);
        say3(gtext(TX_PORTAL_SPAWNS), ent_name(i), gtext(TX_PORTAL_END));
        break;
    }
    S.portal_cnt = (u8)((rnd() & 3) + 1);
}

/* every active monster thinks (in list order); those that moved get
   further steps; then the attacks, newest first
 * In detail: the portal first; then each monster on the active list (a
 * copy taken before, mt_snap, since the list can change) runs ai_think.
 * Those that stepped get ai_after_step, in the active list's order,
 * round after round while any of them steps again (mt_movers /
 * mt_again). Then the queued attacks run (stopping if the player dies),
 * and every monster that can now see the player wakes up. */
static void monster_turn(void)
{
    u16 n, k, nm = 0, na, i, m;
    update_portal();
    att_n = 0;
    n = SG16(n_active);
    for (k = 0; k < n; k++) mt_snap[k] = G16(S.active + 2 * k);
    for (k = 0; k < n; k++)
        if (ai_think(mt_snap[k])) mt_movers[nm++] = mt_snap[k];
    while (nm) {
        na = 0;
        for (k = 0; k < SG16(n_active); k++) {
            i = G16(S.active + 2 * k);
            for (m = 0; m < nm; m++)                /* is it one of the movers? */
                if (mt_movers[m] == i) break;
            if (m == nm) continue;
            if (ai_after_step(i)) mt_again[na++] = i;
        }
        for (k = 0; k < na; k++) mt_movers[k] = mt_again[k];
        nm = na;
    }
    for (k = 0; k < att_n; k++) {
        if (S.dead) break;
        monster_attack(att_ent[k], att_kind[k], att_tgt[k]);
    }
    att_n = 0;
    activate_visible();                             /* wake those now in view */
}

/* ------------------------------------------------------------ scripted events */
/* (the messages for a door that is locked, by key 0..3, are the texts from
   TX_NEED_GREEN on; those for a cost the player cannot pay, by what it
   costs - health, armour, credits, XP - the texts from TX_LOW_HP on) */

/* find_event: the event on cell (x, z), or NONE_B (also off the map). */
static u8 find_event(u8 x, u8 z)
{
    if (x >= 32 || z >= 32) return NONE_B;
    return EV_AT(((u16)z << 5) | x);
}

/* kot_one: kills entity j if it is an enemy or a destructible
 * (MASK_SHOW_KILL): a monster dies, anything else is destroyed. */
static void kot_one(u16 j)
{
    u8 et = ent_etype(j);
    if (!mask_has(MASK_SHOW_KILL, et)) return;
    if (et == ET_ENEMY) monster_died(j, 0);
    else destroy(j);
}

/* the first two things at (x, z) that a kill shows on die
 * (used by the SHOW command: what stands where an entity appears is
 * killed first). Both are looked up before either is killed. */
static void kill_on_tile(u8 x, u8 z)
{
    u16 a = ent_at(x, z, 0), b;
    if (a == NONE) return;
    b = ent_at(x, z, a + 1);
    kot_one(a);
    if (b != NONE) kot_one(b);
}

static u8 run_event(u8 ev, u8 k, u32 flags);

/* entity-side helpers of the commands */
/* hide_cell: takes away what stands on cell (x, z): its static blocker
 * and thin walls (S.static_hidden bit), and every entity there except
 * monsters, which leave the map. For op 18 (HIDE) they are also marked
 * F_HIDDEN; for op 34 (CHANGESPRITE) they are only unlinked. */
static void hide_cell(u8 x, u8 z, u8 op)
{
    u16 c = ((u16)z << 5) | x, j;
    S.static_hidden[c >> 3] |= (u8)(1 << (c & 7));
    j = ent_at(x, z, 0);
    while (j != NONE) {
        if (ent_etype(j) != ET_ENEMY) {
            if (op == 18) S.e_flags[j] |= F_HIDDEN;
            ent_unlink(j);
        }
        j = ent_at(x, z, j + 1);       /* (looks the cell's list up again, from j + 1) */
    }
}

/* run command c -> 1 if it counts as run
 * A command is an op code (t_cmd_op) and two 32-bit arguments: a1 is its
 * operand (lo = bits 0-7, hi = bits 8-15, and bits 16-23 for some ops),
 * a2 its conditions (checked by run_event). The op numbers are the
 * original game's. Commands that stop the event set `suspend`; those
 * that open a dialog or a code entry also set skip_turn (the monsters
 * wait until it closes). A return of 0 means it did nothing (a locked
 * or already open door, a key already held): it then is not marked as
 * run for one-shot purposes. */
static u8 exec_cmd(u16 c)
{
    u8 op = t_cmd_op[c], lo, hi, k, *p;
    u32 a1 = CMD_A1(c);
    u16 w;
    s16 v;
    const char *s;
    lo = (u8)a1;
    hi = (u8)((u16)a1 >> 8);
    switch (op) {
    case 1:                            /* GOTO x, z, angle */
        S.px = lo;
        S.pz = hi;
        for (k = 0; k < 4; k++)        /* the facing for the angle in bits 16-23 (else unchanged) */
            if (ANGLE_FACING[k] == (u8)(a1 >> 16)) {
                S.facing = k;
                break;
            }
        return 1;
    case 2:                            /* CHANGEMAP: once the exit opens */
        /* (a1's low byte: the target map's index in the campaign, 255 the
           end; kept in S.chg until the exit copies it to S.exit) */
        P32(S.chg, a1);
        S.chg_set = 1;
        return 1;
    case 3:                            /* TRIGGER: another cell's event, as if used */
        skip_turn = 0;
        run_tile(lo, hi, EVF_USE);     /* (nested: that event runs to its end) */
        suspend = 0;                   /* its stop does not stop this one */
        return 1;
    case 4:                            /* a message: the level's text lo */
        say(level_string(lo));
        return 1;
    case 5:                            /* PAIN: lo damage to the player */
        say(gtext(TX_PAIN));
        player_pain(lo, 0);
        return 1;
    case 6: case 15: case 16: case 17: /* doors: 6 / 17 toggle, 15 open, 16 close */
        p = &S.l_flags[lo];            /* line lo's flags */
        if (*p & L_LOCKED) return 0;
        if (op == 15 && (*p & L_OPEN)) return 0;
        if (op == 16 && !(*p & L_OPEN)) return 0;
        *p ^= L_OPEN;
        sfx(SFX_DOOR);
        k = t_line_kind[lo];
        if ((k & 2) && !S.secret_turn) {   /* a sliding wall: a secret, +5 XP once an action */
            S.secret_turn = 1;
            say(gtext(TX_SECRET));
            sfx(SFX_SECRET);
            add_xp(5);
        }
        if ((k & 1) && (S.l_flags[lo] & L_OPEN) && S.chg_set) {   /* the exit: the map changes */
            S.exit[0] = S.chg[0];
            S.exit[1] = S.chg[1];
            S.exit[2] = S.chg[2];
            S.exit[3] = S.chg[3];
            S.exit_set = 1;
        }
        return 1;
    case 7:                            /* SHOW: an entity appears */
        w = (u16)a1;                   /* the entity's index */
        if (w == NONE) return 1;
        kill_on_tile(S.e_x[w], S.e_z[w]);   /* what is in its way dies */
        ent_link(w);
        return 1;
    case 8: case 26:                   /* a dialog */
        dialog_str = lo;
        suspend = SUSP_DIALOG;
        skip_turn = 1;
        return 1;
    case 9:                            /* the automap is given */
        S.map_given = 1;
        return 1;
    case 10:                           /* a door code: lo the answer's text, hi the question's */
        pw_want = lo;
        pw_prompt = hi;
        suspend = SUSP_PASSWORD;
        skip_turn = 1;
        return 1;
    case 11: case 19: case 20:         /* set, up, down an event's state */
        /* (the event on cell lo, hi; its state 0..9 picks which of its
           commands run, see run_event) */
        k = find_event(lo, hi);
        if (k == NONE_B) return 1;
        p = &S.ev_state[k];
        if (op == 11) *p = (u8)(a1 >> 16) & 15;
        else if (op == 19) {
            if (*p < 9) *p = (*p + 1) & 15;
        } else if (*p)
            *p = (*p - 1) & 15;
        return 1;
    case 12:                           /* lock line lo */
        S.l_flags[lo] |= L_LOCKED;
        return 1;
    case 13:                           /* unlock it */
        S.l_flags[lo] &= ~L_LOCKED;
        return 1;
    case 14:                           /* toggle its lock */
        S.l_flags[lo] ^= L_LOCKED;
        return 1;
    case 18:                           /* HIDE what is on a cell (not monsters; props too) */
        hide_cell(lo, hi, 18);
        return 1;
    case 34:                           /* CHANGESPRITE: the things there become scenery */
        /* (its cell packed as x in bits 0-4, z in bits 5-9) */
        hide_cell((u8)(a1 & 31), (u8)((a1 >> 5) & 31), 34);
        return 1;
    case 21: case 22:                  /* give / take */
        /* lo what (0 health, 1 armour, 2 credits, 3 XP), hi how much */
        v = op == 22 ? -(s16)hi : (s16)hi;
        if (lo == 0) add_health(v);
        else if (lo == 1) add_armor(v);
        else if (lo == 2) SP16(credits, SG16(credits) + (u16)v);
        else if (lo == 3) add_xp((u16)v);
        return 1;
    case 23:                           /* require a stat */
        /* lo which stat (as for 21), hi the amount needed; not enough:
           the event stops, with the dialog text in bits 16-23 if that
           text is not empty */
        w = lo == 0 ? S.hp : lo == 1 ? S.armor : lo == 2 ? SG16(credits) : SG16(xp);
        if (w >= hi) return 1;
        say(gtext(TX_LOW_HP + (lo & 3)));
        sfx(SFX_DENIED);
        suspend = SUSP_STOP;
        k = (u8)(a1 >> 16);
        s = level_string(k);
        if (*s) {
            dialog_str = k;
            skip_turn = 1;
        }
        return 1;
    case 24:                           /* the status line */
        s = level_string(lo);
        for (k = 0; k < 32 && s[k]; k++) status_msg[k] = s[k];   /* (32 characters at most) */
        status_msg[k] = 0;
        status_new = 1;
        return 1;
    case 30:                           /* the floor colour (rgb888) */
        S.floor_rgb[0] = (u8)(a1 >> 16);
        S.floor_rgb[1] = (u8)(a1 >> 8);
        S.floor_rgb[2] = (u8)a1;
        return 1;
    case 31:                           /* the ceiling colour */
        S.ceil_rgb[0] = (u8)(a1 >> 16);
        S.ceil_rgb[1] = (u8)(a1 >> 8);
        S.ceil_rgb[2] = (u8)a1;
        return 1;
    case 32:                           /* ENABLEWEAPONS */
        /* a1 non-zero: give them back; 0: take them all away (kept in
           S.disabled_weapons), S.weapon reset to 0 */
        if (a1) restore_weapons();
        else {
            S.disabled_weapons[0] = S.weapons[0];
            S.disabled_weapons[1] = S.weapons[1];
            SP16(weapons, 0);
            S.weapon = 0;
        }
        return 1;
    case 33:                           /* OPENSTORE: the vendor's menu (modal) */
        S.store = a1 < 4 ? (u8)a1 : 0; /* vendor 0..3 */
        return 1;
    case 38:                           /* PORTAL: opens; the power couplings become targets */
        S.portal = 1;
        SP16(portal_mon, NONE);
        S.portal_cnt = rnd() % 3;      /* the first monster in 0..2 turns */
        k = 0;
        for (w = 0; w < n_map_ents && k < 2; w++)     /* the first two couplings */
            if (ent_etype(w) == ET_DESTRUCT && esub(w) == 4) {
                S.e_flags[w] |= F_ALIVE;
                k++;
            }
        return 1;
    case 39:                           /* CHECK_COMPLETED_LEVEL */
        /* bits 16-31: a level number (1-based); done if S.completed has
           that bit or any above it. Not done: dialog lo, and the event
           waits */
        w = (u16)(a1 >> 16);
        w = w ? w - 1 : 0;
        if (w < 16 && (SG16(completed) >> w)) return 1;   /* (the 68000: a longer shift gives 0) */
        dialog_str = lo;
        suspend = SUSP_DIALOG;
        skip_turn = 1;
        return 1;
    case 40:                           /* a note for the notebook */
        if (S.n_notes < MAX_NOTES) S.notes[S.n_notes++] = lo;
        say("NOTE ADDED");
        sfx(SFX_SELECT);
        return 1;
    case 41:                           /* require a key */
        /* key lo (bit lo of S.keys: 0 green, 1 yellow, 2 blue, 3 red);
           without it the event stops */
        if ((S.keys >> (lo & 3)) & 1) return 0;
        say(gtext(TX_NEED_GREEN + (lo & 3)));
        sfx(SFX_DENIED);
        suspend = SUSP_STOP;
        return 1;
    }
    return 1;                          /* the rest: nothing to do here */
}

/* event ev from command k on, with flags -> 1 if some command ran
 * flags: why it runs (the trigger bits: arrive / leave directions in
 * bits 0-7, EVF_USE, EVF_MOVE, the facing in bits 28-31). A command runs
 * only if all of these hold for its a2 (conditions):
 *  - it is not dead (S.cmd_dead: one bit per command, set once a command
 *    with EVF_MODIFY, a one-shot, has run);
 *  - the event's state: state s (1..9) needs a2 bit 15 + s; state 0
 *    needs none of the bits 16-24;
 *  - keys: if a2 has any of bits 12-15 they must equal the bit of the
 *    best key held (red 0x8000, blue 0x4000, green 0x2000, yellow
 *    0x1000);
 *  - a2 shares at least one bit with flags.
 * Bit 0 of the event's own flags: movement does not trigger it.
 * If a command suspends the event (a dialog, a code, a stop), where it
 * stopped is saved in pend_* (resume() carries on) and the loop ends. */
static u8 run_event(u8 ev, u8 k, u32 flags)
{
    u32 n7 = 0, n8, a2;
    u16 c;
    u8 st, n, ran = 0;
    if ((t_ev_flags[ev] & 1) && (flags & EVF_MOVE)) return 0;
    st = S.ev_state[ev];
    if (st) n7 = 0x10000UL << (st - 1);     /* the state's condition bit */
    /* the best key held, as a condition bit */
    if (S.keys & 8) flags |= 0x8000;
    else if (S.keys & 4) flags |= 0x4000;
    else if (S.keys & 1) flags |= 0x2000;
    else if (S.keys & 2) flags |= 0x1000;
    n8 = flags & 0xF000;
    n = t_ev_ncmd[ev];
    for (; k < n; k++) {
        c = G16(t_ev_cmd0 + 2 * ev) + k;    /* the command's global index */
        if (S.cmd_dead[c >> 3] & (u8)(1 << (c & 7))) continue;    /* (this form: SDCC 4.5 miscompiles the shift here, gbc/TOOLCHAIN.md) */
        a2 = CMD_A2(c);
        if (n7) {
            if (!(a2 & n7)) continue;
        } else if (a2 & 0x1FF0000UL)
            continue;
        if ((a2 & 0xF000) && (a2 & 0xF000) != n8) continue;
        if (!(a2 & flags)) continue;
        suspend = 0;
        if (exec_cmd(c)) {
            ran = 1;
            if (a2 & EVF_MODIFY) S.cmd_dead[c >> 3] |= (u8)(1 << (c & 7));   /* a one-shot: dead now */
        }
        if (suspend) {                      /* stopped: remember where */
            pend_valid = 1;
            pend_ev = ev;
            pend_k = k;
            pend_flags = flags;
            pend_why = suspend;
            break;
        }
    }
    return ran;
}

/* run_tile_r: runs the event on cell (x, z), if any, from its first
 * command. Out: 1 if some command ran. run_tile: the same, without the
 * result. */
static u8 run_tile_r(u8 x, u8 z, u32 flags)
{
    u8 ev = find_event(x, z);
    if (ev == NONE_B) return 0;
    return run_event(ev, 0, flags);
}

static void run_tile(u8 x, u8 z, u32 flags) { run_tile_r(x, z, flags); }

/* berserk_tick: counts down the Berserker's turns (S.berserk) and says
 * when it runs out. */
static void berserk_tick(void)
{
    if (!S.berserk) return;
    if (!--S.berserk) say(gtext(TX_BERSERK_OFF));
}

/* settle: if the monsters are owed a turn (monsters_pending) and no
 * dialog, code entry or store is open, wakes the monsters in view and,
 * unless the player is dead, runs their turn. The port calls it after a
 * modal screen closes; the actions call it through end_player_turn. */
void settle(void)
{
    if (dialog_str != NONE_B || pw_want != NONE_B || S.store != NONE_B) return;
    if (!monsters_pending) return;
    monsters_pending = 0;
    activate_visible();
    if (S.dead) return;
    monster_turn();
}

/* modal: 1 while a dialog, a code entry or a store waits for the player
 * (the port shows it and calls resume / store_buy / store_close). */
u8 modal(void)
{
    return dialog_str != NONE_B || pw_want != NONE_B || S.store != NONE_B;
}

/* end_player_turn: the player's action is over: the Berserker ticks and
 * the monsters get their turn (now, or after the modal screen). */
static void end_player_turn(void)
{
    berserk_tick();
    monsters_pending = 1;
    settle();
}

/* resume: the port closed a dialog or a code entry. password: what the
 * player typed (0: cancelled). An event stopped by a dialog carries on
 * after the command that stopped it; one stopped by a code carries on
 * only if the code matches the wanted text exactly (TX_CODE_BAD
 * unless nothing was typed). Then the monsters' turn, if owed (settle). */
void resume(const char *password)
{
    u8 valid = pend_valid, ev = pend_ev, k = (u8)(pend_k + 1), why = pend_why;
    u32 flags = pend_flags;
    const char *want;
    S.secret_turn = 0;
    pend_valid = 0;
    dialog_str = NONE_B;
    if (valid) {
        if (why == SUSP_DIALOG)
            run_event(ev, k, flags);        /* on from the next command */
        else if (why == SUSP_PASSWORD) {
            want = level_string(pw_want);
            pw_want = NONE_B;
            if (!password) return;          /* (cancelled: no settle either) */
            {
                const char *a = password, *b = want;    /* compare the strings */
                while (*a && *a == *b) {
                    a++;
                    b++;
                }
                if (*a || *b) {
                    if (*password) {
                        say(gtext(TX_CODE_BAD));
                        sfx(SFX_DENIED);
                    }
                    return;
                }
            }
            say(gtext(TX_CODE_GOOD));
            sfx(SFX_SELECT);
            run_event(ev, k, flags);
        }
    }
    settle();
}

/* ------------------------------------------------------------ the player's actions */
/* select_target's result: the target's kind (T_ENT or T_LINE) and index;
   sel_locked: 1 if a door that is locked was in the way */
static u8 sel_kind, sel_locked;
static u16 sel_idx;

/* what the player would hit straight ahead -> 1 if it can be attacked
 * Traces up to 8 cells ahead (MASK_SHOT) and looks at the hits in order:
 * a wall or a static blocker ends the search (nothing); a breakable line
 * (a line of type ET_DESTRUCT) can be hit with the axe only; any other
 * line stops it, unless a monster stands in that doorway (then the
 * monster); things on the player's own cell are skipped; a human (a
 * friendly character) cannot be attacked; a destructible can if its
 * def's parameter has the bit of the current weapon; a fire only with
 * the extinguisher (weapon 1); a monster always. Writes sel_kind,
 * sel_idx, sel_locked and the hit list. */
static u8 select_target(void)
{
    s16 tx = (s16)S.px + 8 * DX[S.facing], tz = (s16)S.pz + 8 * DZ[S.facing];
    u8 n, k, li, et;
    u16 e, k2;
    sel_locked = 0;
    if (tx < 0) tx = 0;                 /* the end cell, kept on the map */
    if (tx > 31) tx = 31;
    if (tz < 0) tz = 0;
    if (tz > 31) tz = 31;
    n = trace(S.px, S.pz, (u8)tx, (u8)tz, MASK_SHOT, NONE);
    for (k = 0; k < n; k++) {
        e = hit_val[k];
        if (hit_kind[k] == H_WALL || hit_kind[k] == H_PLAYER || hit_kind[k] == H_STATIC) return 0;
        if (hit_kind[k] == H_LINE) {
            li = (u8)e;
            if (t_line_etype[li] == ET_DESTRUCT) {      /* a breakable door: the axe only */
                if (S.weapon) return 0;
                sel_kind = T_LINE;
                sel_idx = li;
                return 1;
            }
            if (t_line_etype[li]) return 0;             /* not a plain door */
            if (S.l_flags[li] & L_LOCKED) sel_locked = 1;
            for (k2 = k + 1; k2 < n; k2++)      /* a monster in the doorway */
                if (hit_kind[k2] == H_ENT && ent_etype(hit_val[k2]) == ET_ENEMY &&
                    S.e_x[hit_val[k2]] == t_line_x[li] && S.e_z[hit_val[k2]] == t_line_z[li]) {
                    sel_kind = T_ENT;
                    sel_idx = hit_val[k2];
                    return 1;
                }
            return 0;
        }
        if (S.e_x[e] == S.px && S.e_z[e] == S.pz) continue;     /* (on the player's own cell) */
        et = ent_etype(e);
        if (et == ET_HUMAN) return 0;
        sel_kind = T_ENT;
        sel_idx = e;
        if (et == ET_DESTRUCT) return (u8)((def_parm(S.e_def[e]) >> S.weapon) & 1);
        if (et == ET_FIRE) return S.weapon == 1;
        return et == ET_ENEMY;
    }
    return 0;
}

/* calc_hit_object: one hit roll of the player's weapon against the
 * selected object (not a monster): calc_hit with the player's accuracy
 * against an "agility" of accuracy * 204/256. A hit counts only with the
 * right tool: a door needs the axe, a fire the extinguisher, anything
 * else any weapon but the extinguisher. Out: 1 hit, 0 not. */
static u8 calc_hit_object(u8 tiles)
{
    u8 agi = (u8)(((u16)S.accuracy * 204) >> 8);
    if (!calc_hit(S.accuracy, S.weapon, agi, tiles)) return 0;
    if (sel_kind == T_LINE) return S.weapon == 0;
    if (ent_etype(sel_idx) == ET_FIRE) return S.weapon == 1;
    return S.weapon != 1;
}

/* line_destroyed: breakable door li is broken: it opens for good (L_OPEN,
 * not L_ALIVE), 1 XP, and its cell's event runs as if used. */
static void line_destroyed(u8 li)
{
    S.l_flags[li] = (S.l_flags[li] & ~L_ALIVE) | L_OPEN;
    say(gtext(TX_DOOR_CLEARED));
    sfx(SFX_BREAK);
    add_xp(1);
    run_tile(t_line_x[li], t_line_z[li], EVF_USE);
}

/* at the selected target -> 1 if it took a turn (0: no ammunition)
 * Also 0, silently, if the current weapon was taken away by an event.
 * Uses the weapon's ammo, then fires wpn_shots rolls. Against a monster:
 * calc_hit with its agility and calc_damage with its defence and class,
 * at 3x damage under the Berserker and 2x for a critical first shot;
 * the total is dealt at once. A kill from the next cell (tiles == 1)
 * that leaves its health + armour at or below -(about 40% of its max
 * health) is an overkill (no corpse). Against an object: the last
 * shot's calc_hit_object decides, and a hit breaks it. */
static u8 player_attack(void)
{
    u8 w = S.weapon, enemy = 0, crit = 0, hit = 0, s, tiles, tx, tz, m;
    u16 j = sel_idx, tot = 0, tarm = 0, mult;
    s16 left, thr;
    if (SG16(disabled_weapons) && !((SG16(weapons) >> w) & 1)) return 0;
    if (wpn_use[w] && S.ammo[wpn_ammo[w]] < wpn_use[w]) {
        say(gtext(TX_NO_AMMO));
        sfx(SFX_DENIED);
        return 0;
    }
    S.ammo[wpn_ammo[w]] -= wpn_use[w];
    sfx(wpn_sfx[w]);
    player_fired = 1;
    if (sel_kind == T_LINE) {
        tx = t_line_x[j];
        tz = t_line_z[j];
    } else {
        tx = S.e_x[j];
        tz = S.e_z[j];
    }
    /* the distance in cells (on a row or a column, so |dx| + |dz|) */
    tiles = (u8)((tx > S.px ? tx - S.px : S.px - tx) + (tz > S.pz ? tz - S.pz : S.pz - tz));
    if (sel_kind == T_ENT && ent_etype(j) == ET_ENEMY) {
        enemy = 1;
        cb_monster(j);                  /* its stats into cb_* */
    }
    for (s = 0; s < wpn_shots[w]; s++) {
        if (enemy) {
            hit = calc_hit(S.accuracy, w, cb_agi, tiles);
            if (!hit) continue;
            if (hit == HIT_CRIT && !s) crit = 1;
            mult = S.berserk ? 768 : 256;
            if (crit) mult += mult;
            calc_damage(S.strength, w, cb_def, cb_class, mult, tiles);
            tot += dmg_hp;
            tarm += dmg_arm;
        } else
            hit = calc_hit_object(tiles);
    }
    if (enemy) {
        if (!(tot + tarm)) {
            say(gtext(w == 1 ? TX_NO_EFFECT : TX_MISSED));
            return 1;
        }
        m = mon_slot(j);
        left = (s16)((u16)S.m_hp[m] + S.m_armor[m] - tot - tarm);   /* what it would have left */
        say_num(crit ? "CRITICAL! " : "", (u16)(tot + tarm), gtext(TX_DAMAGE));
        monster_pain(j, (u8)tot, (u8)tarm);
        if (S.m_hp[m]) {
            sfx(SFX_MON_HURT);
            return 1;
        }
        say2(ent_name(j), gtext(TX_DIED));
        thr = (s16)(((((u32)S.m_maxhp[m] << 16) / 637) & 0xFFFF) >> 8);   /* maxhp * 256 / 637 */
        monster_died(j, (u8)(tiles == 1 && left <= -thr));
        return 1;
    }
    if (hit) {
        if (sel_kind == T_LINE) line_destroyed((u8)j);
        else destroy(j);
    } else
        say(gtext(w == 1 ? TX_NO_EFFECT : TX_MISSED));
    return 1;
}

/* the player touches entity j: picks it up, or gets burnt
 * By entity type and subtype: pick-ups 20 health, 21 armour (not taken
 * when full), 22 and 23 credits, 24 a key (key number in the parameter);
 * inventory items (subtype 25.. -> S.inventory[subtype - 25], 99 at
 * most); ammo (subtype = ammo type, parameter = amount; not taken when
 * full); a weapon (subtype = weapon; taken in hand if new, with its
 * ammo); fires hurt (1 + 2 armour, or 10 + 10 for the second kind) and
 * stay. Anything else is ignored. A taken thing leaves the map. */
static void touch(u16 j)
{
    u8 et = ent_etype(j), es = esub(j), snd = SFX_PICKUP, *a;
    s32 parm = def_parm(S.e_def[j]);
    const char *name = ent_name(j);
    switch (et) {
    case ET_PICKUP:
        if (es == 22 || es == 23) {
            SP16(credits, SG16(credits) + (u16)parm);
            snd = SFX_CREDITS;
        } else if (es == 21) {
            if (S.armor >= S.maxarmor) {
                say(gtext(TX_ARMOR_MAX));
                return;
            }
            add_armor((s16)parm);
        } else if (es == 20) {
            if (S.hp >= S.maxhp) {
                say(gtext(TX_HEALTH_MAX));
                return;
            }
            add_health((s16)parm);
        } else if (es == 24) {
            S.keys |= (u8)(1 << (parm & 7));
            snd = SFX_KEY;
        }
        break;
    case ET_INV:
        a = &S.inventory[es - 25];
        if (*a == 99) {
            say2("CAN'T CARRY MORE ", name);
            return;
        }
        (*a)++;
        break;
    case ET_AMMO: case ET_AMMO2:
        if (!add_ammo(es, (u8)parm)) {
            say(gtext(TX_AMMO_MAX));
            return;
        }
        break;
    case ET_WEAPON:
        if (!(((SG16(weapons) | SG16(disabled_weapons)) >> es) & 1)) S.weapon = es;
        SP16(weapons, SG16(weapons) | (1u << es));
        if (wpn_use[es]) add_ammo(wpn_ammo[es], (u8)parm);
        snd = SFX_WEAPON;
        break;
    case ET_FIRE:
        player_pain(1, 2);
        say(gtext(TX_BURNS));
        return;
    case ET_FIRE2:
        player_pain(10, 10);
        say(gtext(TX_BURNS_MORE));
        return;
    default:
        return;
    }
    sfx(snd);
    say2(gtext(TX_GOT), name);
    ent_unlink(j);                      /* taken: out of the map */
}

/* the things on the player's cell: all of them (walked in), else only fire
 * (at most the first 8, listed before any is touched since touching
 * changes the cell's list) */
static void touch_tile(u8 all)
{
    u16 list[8], j;
    u8 n = 0, k, et;
    j = ent_at(S.px, S.pz, 0);
    while (n < 8 && j != NONE) {
        list[n++] = j;
        j = ent_at(S.px, S.pz, j + 1);
    }
    for (k = 0; k < n; k++) {
        if (!all) {
            et = ent_etype(list[k]);
            if (et != ET_FIRE && et != ET_FIRE2) continue;
        }
        touch(list[k]);
    }
}

/* act_turn: the player turns by delta quarter turns (+1 right, -1 left).
 * Runs the cell's event for the new facing and wakes the monsters now in
 * view. A turn is free: the monsters do not move. */
void act_turn(s8 delta)
{
    S.secret_turn = 0;
    S.facing = (u8)(S.facing + delta) & 3;
    run_tile(S.px, S.pz, FACING_FLAG[S.facing] | EVF_MOVE);
    activate_visible();
}

/* act_move: the player steps one cell forward (back = 0) or backward
 * (back = 1, still facing the same way). Blocked (MASK_PLAYER_MOVE):
 * a bump sound and 0. Otherwise runs the old cell's "leave" event, moves,
 * runs the new cell's "arrive" event, touches what is there, and the
 * monsters take their turn. Out: 1 if it moved. */
u8 act_move(u8 back)
{
    u8 d = S.facing, nx, nz;
    S.secret_turn = 0;
    if (back) d = (d + 2) & 3;          /* the direction of the step */
    nx = (u8)(S.px + DX[d]);
    nz = (u8)(S.pz + DZ[d]);
    if (trace(S.px, S.pz, nx, nz, MASK_PLAYER_MOVE, NONE)) {   /* anything in the way */
        sfx(SFX_BUMP);
        return 0;
    }
    sfx(SFX_STEP);
    run_tile(S.px, S.pz, LEAVE_FLAG[d] | EVF_MOVE);
    S.px = nx;
    S.pz = nz;
    run_tile(nx, nz, ARRIVE_FLAG[d] | EVF_MOVE | FACING_FLAG[S.facing]);
    touch_tile(1);                      /* pick up what is there */
    end_player_turn();
    return 1;
}

/* act_use: the player's "use / attack" button. First the event of the
 * cell ahead runs with EVF_USE | EVF_MOVE (0x500); if a command of it
 * ran, the turn ends (unless the event opened a dialog or code entry,
 * skip_turn: the monsters wait). Otherwise the player attacks what is
 * ahead (select_target, player_attack); with nothing to attack it says
 * why and the turn does not pass (nor if there was no ammo). After a
 * turn: fires on the player's cell burn, then the monsters' turn. */
void act_use(void)
{
    S.secret_turn = 0;
    skip_turn = 0;
    if (run_tile_r((u8)(S.px + DX[S.facing]), (u8)(S.pz + DZ[S.facing]), 0x500)) {
        if (skip_turn) return;
    } else if (!select_target()) {
        if (sel_locked) {
            say("THE DOOR IS LOCKED");
            sfx(SFX_DENIED);
        } else {
            say(gtext(TX_NOTHING));
            sfx(SFX_BLIP);
        }
        return;
    } else if (!player_attack())
        return;
    touch_tile(0);                      /* fires on the player's cell */
    end_player_turn();
}

/* act_pass: the player does nothing for a turn: fires burn, the
 * monsters move. */
void act_pass(void)
{
    S.secret_turn = 0;
    say(gtext(TX_TURN_PASSED));
    touch_tile(0);
    end_player_turn();
}

/* act_cycle_weapon: selects the next (step = 1) or previous (step = -1)
 * usable weapon among the player's 12, wrapping round. Free (no turn).
 * Nothing if there is no other usable weapon. */
void act_cycle_weapon(s8 step)
{
    s16 w;
    u8 k;
    S.secret_turn = 0;
    for (k = 1; k < 12; k++) {
        w = (s16)step * k + S.weapon;
        while (w < 0) w += 12;          /* wrap to 0..11 */
        while (w >= 12) w -= 12;
        if (!((SG16(weapons) >> w) & 1)) continue;
        if (wpn_use[w] && !S.ammo[wpn_ammo[w]]) continue;
        S.weapon = (u8)w;
        sfx(SFX_SWITCH);
        return;
    }
}

/* act_health_item: uses the first health item held: inventory 0 a small
 * medkit, 1 a large one (the health of the matching drop def's
 * parameter), 2 a soulsphere (+200 health and armour, held to the
 * maxima). Takes a turn. Out: 1 used, 0 none held. */
u8 act_health_item(void)
{
    u8 k;
    S.secret_turn = 0;
    for (k = 0; k < 3; k++)
        if (S.inventory[k]) break;
    if (k == 3) {
        say("NO HEALTH ITEMS");
        sfx(SFX_DENIED);
        return 0;
    }
    if (k == 2) {
        add_health(200);
        add_armor(200);
    } else
        add_health((s16)def_parm(t_drop_def[k ? DD_LGMED : DD_SMMED]));
    S.inventory[k]--;
    sfx(SFX_HEAL);
    end_player_turn();
    return 1;
}

/* use_item: uses inventory item k: 3 the Berserker (3x damage for 31
 * more turns; takes a turn), 4 the collar (captures a hell hound
 * class monster, subtype 1, right in front: it leaves the map and
 * becomes weapon 9, 10 or 11 by its def's parameter, with 5x its health
 * as the dog's health in S.ammo[5], held to 99; a "captured" dialog
 * opens and the turn passes when it closes). Out: 1 used, 0 not (none
 * held, another k, or no fitting target, with a message). */
u8 use_item(u8 k)
{
    u8 n, i, w;
    u16 j;
    s32 parm;
    u16 v;
    S.secret_turn = 0;
    if (!S.inventory[k]) {
        say("YOU HAVE NONE");
        return 0;
    }
    if (k == 3) {                      /* the Berserker: x3 damage for 31 turns */
        say(gtext(TX_BERSERK_ON));
        S.berserk += 31;
        S.inventory[3]--;
        end_player_turn();
        return 1;
    }
    if (k != 4) return 0;
    /* the collar: the first thing ahead */
    n = trace(S.px, S.pz, (u8)(S.px + DX[S.facing]), (u8)(S.pz + DZ[S.facing]), MASK_SHOT, NONE);   /* this cell and the next */
    for (i = 0; i < n; i++)
        if (hit_kind[i] == H_ENT || hit_kind[i] == H_LINE) break;
    if (i == n) {
        say(gtext(TX_NO_DOG));
        return 0;
    }
    if (hit_kind[i] == H_LINE || ent_etype(hit_val[i]) != ET_ENEMY) {
        say(gtext(TX_BAD_TARGET));
        return 0;
    }
    j = hit_val[i];
    if (esub(j) != 1) {
        say2(gtext(TX_WONT_FIT), ent_name(j));
        return 0;
    }
    S.inventory[4]--;                  /* the dog leaves the map and fights for you */
    deactivate(j);
    ent_unlink(j);
    captured_def = S.e_def[j];         /* (named by the dialog) */
    parm = def_parm(S.e_def[j]);
    w = parm == 294 ? 9 : parm == 467 ? 10 : 11;   /* the three dogs */
    S.weapon = w;
    SP16(weapons, SG16(weapons) | (1u << w));
    v = (u16)((S.m_hp[mon_slot(j)] * 5) & 0xFF);   /* (kept to 8 bits before the 99 limit) */
    S.ammo[5] = (u8)(v > 99 ? 99 : v);
    dialog_str = DLG_CAPTURED;
    pend_valid = 0;
    skip_turn = 1;
    monsters_pending = 1;              /* the turn passes when it closes */
    berserk_tick();
    return 1;
}

/* ------------------------------------------------------------ the vendor */
/* (the store's screens are the port's; these are its prices and what
   buying does) */
/* per vendor: 16 x (name tag, price), tag 0 ends (Menu.c vendingMenuTable) */
const u8 STORE_LINES[128] = {
    2, 8, 3, 35, 7, 5, 8, 5, 9, 10, 11, 10, 10, 15, 13, 15, 14, 15, 15, 20, 16, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    2, 6, 8, 3, 7, 4, 9, 6, 13, 15, 14, 15, 15, 20, 16, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    2, 6, 3, 25, 4, 50, 7, 3, 8, 4, 11, 6, 9, 8, 10, 12, 13, 15, 14, 15, 15, 20, 16, 20, 0, 0, 0, 0, 0, 0, 0, 0,
    2, 4, 3, 15, 4, 30, 5, 40, 7, 2, 8, 2, 9, 4, 11, 5, 10, 8, 13, 8, 14, 8, 15, 10, 16, 10, 0, 0, 0, 0, 0, 0};
/* per name tag: kind (1 item, 2 ammo, 3 stat), what
 * (item: its inventory subtype 25..28; ammo: its type 0..5; stat: 1
 * defence, 2 agility, 3 strength, 4 accuracy); tags 0 and 6 unused */
const u8 STORE_ITEMS[34] = {0, 0, 0, 0, 1, 25, 1, 26, 1, 27, 1, 28, 0, 0, 2, 0, 2, 1, 2, 2, 2, 4, 2, 3, 0, 0,
                            3, 4, 3, 2, 3, 3, 3, 1};

/* store_buy: buys line `line` (0..15) of the open vendor (S.store):
 * pays the price in credits and adds one item, one stat point, or ammo
 * (3 rockets, else 10). Out: 0 bought, 1 too few credits, 2 already
 * at 99. The port's store screen calls it; it takes no turn. */
u8 store_buy(u8 line)
{
    const u8 *l = STORE_LINES + S.store * 32 + line * 2;
    u8 tag = l[0], price = l[1], kind = STORE_ITEMS[tag * 2], what = STORE_ITEMS[tag * 2 + 1], *have;
    if (kind == 1) have = &S.inventory[what - 25];     /* what the 99 limit applies to */
    else if (kind == 2) have = &S.ammo[what];
    else have = what == 1 ? &S.defense : what == 2 ? &S.agility : what == 3 ? &S.strength : &S.accuracy;
    if (price > SG16(credits)) return 1;
    if (*have == 99) return 2;
    SP16(credits, SG16(credits) - price);
    if (kind == 2) add_ammo(what, what == 3 ? 3 : 10);
    else (*have)++;
    return 0;
}

/* store_close: the port closed the vendor's menu: the store is shut
 * (S.store = NONE_B) and the monsters get their turn if owed. */
void store_close(void)
{
    S.secret_turn = 0;
    S.store = NONE_B;
    settle();
}

/* ------------------------------------------------------------ starting and restoring */
/* clear_lists: empties every cell's entity list. */
static void clear_lists(void)
{
    u16 i;
    for (i = 0; i < 1024; i++) cell_head[i] = 0;
    for (i = 0; i < MAX_ENTS; i++) ent_next[i] = 0;
}

/* turn_reset: clears what a turn in progress leaves behind (no dialog, no
 * code entry, no stopped event, no monster turn owed, no queued attacks
 * or explosions). */
static void turn_reset(void)
{
    dialog_str = NONE_B;
    pw_want = NONE_B;
    pend_valid = 0;
    monsters_pending = 0;
    att_n = 0;
    n_pend_expl = 0;
    suspend = 0;
}

/* map `map` with the player in S; spawn: 0 the map's own, else x | z << 5 | angle << 10
 * Keeps the player's own part of S (stats, weapons, ammo, keys...,
 * everything before n_active) and clears the map's part (n_active to the
 * end), then sets it up from the level blob: entities at their start
 * cells (linked unless they start hidden), lines (locked, breakable),
 * event states, each monster's stats (its class's base stats scaled by
 * setup_enemy, and a random first attack), fires and destructibles
 * alive (power couplings only once the portal opens). Then runs the
 * start cell's event (facing flag, EVF_MOVE and all four arrive bits,
 * 0x40F) and wakes the monsters in view. Uses rnd() (in the model's
 * order). */
static void map_start(u8 map, u32 spawn)
{
    u16 i, off;
    u8 *p, k, m;
    const u8 *b;
    S.map = map;
    map_data(map);
    off = (u16)(S.n_active - (u8 *)&S);         /* where the map's part of S starts */
    p = S.n_active;
    for (i = off; i < STATE_SIZE; i++) *p++ = 0;
    S.dead = 0;
    S.map_given = 0;
    S.drop_index = 0;
    S.berserk = 0;
    S.store = NONE_B;
    S.secret_turn = 0;
    S.chg_set = S.exit_set = 0;
    P32(S.chg, 0);
    P32(S.exit, 0);
    SP16(portal_mon, NONE);
    S.portal = S.portal_cnt = S.couplings = 0;
    for (k = 0; k < 3; k++) {
        S.floor_rgb[k] = floor_rgb0[k];
        S.ceil_rgb[k] = ceil_rgb0[k];
    }
    S.px = spawn_x;
    S.pz = spawn_z;
    S.facing = spawn_f;
    if (spawn) {                                /* the exit's own start cell */
        S.px = (u8)(spawn & 31);
        S.pz = (u8)((spawn >> 5) & 31);
        S.facing = 0;
        for (k = 0; k < 4; k++)
            if (ANGLE_FACING[k] == (u8)(spawn >> 10)) {
                S.facing = k;
                break;
            }
    }
    clear_lists();
    turn_reset();
    for (i = 0; i < n_ents; i++) {
        if (i >= n_map_ents) {                  /* a drop slot: empty */
            S.e_def[i] = NONE_B;
            continue;
        }
        S.e_def[i] = t_ent_def0[i];
        S.e_x[i] = t_ent_x0[i];
        S.e_z[i] = t_ent_z0[i];
        if (!t_ent_hidden0[i]) ent_link(i);
    }
    for (k = 0; k < n_lines; k++)
        S.l_flags[k] = (t_line_locked0[k] ? L_LOCKED : 0) | (t_line_etype[k] == ET_DESTRUCT ? L_ALIVE : 0);
    for (k = 0; k < n_events; k++) S.ev_state[k] = t_ev_state0[k];
    for (i = 0; i < n_map_ents; i++) {
        k = ent_etype(i);
        if (k == ET_ENEMY) {
            m = mon_slot(i);
            b = mon_base + esub(i) * 6;         /* the class's hp, armour, def, str, agi, acc */
            S.m_maxhp[m] = S.m_hp[m] = b[0];
            S.m_maxarmor[m] = S.m_armor[m] = b[1];
            S.m_def[m] = b[2];
            S.m_str[m] = b[3];
            S.m_agi[m] = b[4];
            S.m_acc[m] = b[5];
            setup_enemy(m, def_parm(S.e_def[i]));
            if (rnd() & 1) S.e_flags[i] |= F_ATK2;
            S.e_flags[i] |= F_ALIVE;
        } else if (k == ET_FIRE || (k == ET_DESTRUCT && esub(i) != 4))
            S.e_flags[i] |= F_ALIVE;   /* (power couplings: once the portal opens) */
    }
    if (SG16(disabled_weapons)) restore_weapons();   /* Player_setup: weapons put away come back */
    run_tile(S.px, S.pz, FACING_FLAG[S.facing] | 0x40F);   /* the start cell's event */
    activate_visible();
}

/* game_init: a fresh game: S cleared, the random seed 0x1234, the player's
 * starting stats (30 health, 0 of 20 armour, level 1, 80 XP to the
 * next), the pistol (weapon 2, bit 2 of S.weapons) with 8 bullets, and
 * map 0. */
void game_init(void)
{
    u16 i;
    u8 *p = (u8 *)&S;
    for (i = 0; i < STATE_SIZE; i++) p[i] = 0;
    SP16(rng, 0x1234);
    S.hp = S.maxhp = 30;
    S.maxarmor = 20;
    S.defense = 16;
    S.strength = 12;
    S.agility = 14;
    S.accuracy = 16;
    S.level = 1;
    SP16(nextxp, 80);
    SP16(weapons, 4);
    S.weapon = 2;
    S.ammo[1] = 8;
    map_start(0, 0);
}

/* through the exit (S.exit) to map `map`: Game_changeMap (the sector
   completed, Player_addLevelStats) then the map's start
 * S.exit holds the CHANGEMAP command's argument: bit 31 set marks the
 * map in play as completed (bit map_id - 1 of S.completed; not for map
 * ids 0 and 2); bits 8-30 (sign-extended from bit 30) are the spawn
 * place for map_start (0: the map's own). The port reads the target map
 * from S.exit's low byte and calls this. */
void map_change(u8 map)
{
    u32 e = G32(S.exit);
    s32 sp;
    if ((e & 0x80000000UL) && map_id != 2 && map_id)
        SP16(completed, SG16(completed) | (1u << (map_id - 1)));
    sp = (s32)(e << 1) >> 9;               /* spawnParam = (param << 1) >> 9 (arithmetic) */
    map_start(map, (u32)sp);
}

/* game_restore: S has just been loaded from a saved game: makes the
 * saved map the map in play again and rebuilds what is not saved (the
 * cell lists, from each entity's F_LINKED flag; the turn's variables). */
void game_restore(void)
{
    u16 i;
    map_data(S.map);
    clear_lists();
    for (i = 0; i < n_ents; i++)
        if (S.e_flags[i] & F_LINKED) {
            S.e_flags[i] &= ~F_LINKED;      /* (so that ent_link puts it back) */
            ent_link(i);
        }
    turn_reset();
}
