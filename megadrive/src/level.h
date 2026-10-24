/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * The level blob: everything about the map that the rules need, read at
 * run time from ROM (tools/gen_cdata.py, big-endian).
 *
 * Each map's level blob sits in the campaign block (the data block at the
 * end of the ROM, tools/md_campaign.py); src/pack.c points level_blob at
 * the blob of the map in play. Its layout: a 24-byte header (counts,
 * spawn point, map id, floor and ceiling colours), then one u16 offset per
 * table (from the blob's start, at byte 24 + 2 * table), then the tables.
 * level_load (src/rules.s) reads the counts and sets the pointers below.
 * Everything here is defined in src/rules.s; C reads it (ui.c: the
 * entity definitions' types and names, the string count).
 */
#ifndef LEVEL_H
#define LEVEL_H

/* (not used now: the blob's address is level_blob, set by src/pack.c) */
#define LEVEL (level_blob)

/* counts: entity definitions ("defs"), monster slots, lines (doors and
   other wall-like things), events (scripts on cells), strings; entities
   placed on the map (n_map_ents) and in all (n_ents: with the slots kept
   for dropped things), event commands */
extern unsigned char n_defs, n_mons, n_lines, n_events, n_strings;
extern unsigned short n_map_ents, n_ents, n_cmds;
/* where the player starts: cell x, z (0..31) and facing (0 N, 1 E, 2 S, 3 W) */
extern unsigned char spawn_x, spawn_z, spawn_f;

/* the tables, all in ROM (one entry per def / entity / line / event /
   command; wall_bits, static_block and line_cells are per cell):
   def_etype   each def's entity type (ET_* in game.h); def_esub its subtype
   ent_def0, ent_x0, ent_z0, ent_hidden0  each entity's def, cell, hidden
               at the start; ent_mon its monster slot ($FF: not a monster)
   line_x, line_z, line_etype, line_locked0  each line's cell, type, locked
   ev_x, ev_z, ev_state0, ev_flags  each event's cell, first state, flags
   ev_cmd0, ev_ncmd  its first command and how many; ev_at  per cell its
               event ($FF none)
   cmd_op, cmd_a1, cmd_a2  each command's opcode and two arguments
   wall_bits   1 bit per cell: a wall; line_cells 1 bit per cell: a line
   drop_def_tab  the defs that dropped things use
   def_parm    each def's parameter (32-bit, signed)
   def_name_off  each def's name: a u16 offset from the table's start */
extern const unsigned char *def_etype, *def_esub, *ent_def0, *ent_x0, *ent_z0, *ent_hidden0, *ent_mon;
extern const unsigned char *line_x, *line_z, *line_etype, *line_locked0;
extern const unsigned char *ev_x, *ev_z, *ev_state0, *ev_flags, *ev_ncmd, *ev_at;
extern const unsigned short *ev_cmd0;
extern const unsigned char *cmd_op, *wall_bits, *static_block, *line_cells, *drop_def_tab;
extern const long *def_parm;
extern const unsigned long *cmd_a1, *cmd_a2;
extern const unsigned short *def_name_off;
/* the upper-case name of def i (a zero-ended string) */
#define def_name(i) ((const char *)def_name_off + def_name_off[i])

/* reads the counts and table pointers out of level_blob (rules.s) */
void level_load(void);

#endif
