; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

;=======================================================================
; Doom RPG for the Sega Master System - the game logic, in Z80 assembler
;
; The rules (combat, monsters, items, doors, scripted events) and the
; player-facing side (input, messages, status bar, dialog boxes, door
; codes, notebook, pause menu). The display engine (rom/main.asm, render)
; draws; this code decides what happens.
;
; The rules are a line-by-line counterpart of tools/game_model.py, the
; executable spec: routine names match the model's, and random numbers
; are drawn in exactly the same order. tools/test_lockstep.py plays the
; ROM and the model side by side and compares the game state (the S_*
; block at STATE_ADDR) byte for byte after every action.
;
; Included by rom/main.asm. Memory:
;   $1000-$3FFF  bank 0: this code (fixed)   $4000-$7FBF  bank 1: this code
;   $8000-$BFFF  slot 2: bank 2 (the level blob at $A000) while this code
;                runs; level_string pages bank 4 (strings) in briefly
;   $C000-$C3FF  engine variables, status text, dialog text, status staging
;   $C400-       the game state (S_*)       $CAE0-$D3FF  game variables
;   $D100-       a shared scratch block (strbuf, or the monsters' turn)
;   $D3A0-       the trace's variables (core.asm)
;   $DCE8-       the message queue (msgq), after the renderer's variables
;
; How the files fit together (rom/main.asm includes them in this order):
;   game.asm    this file: constants, the engine variables, the game RAM.
;               It emits no code.
;   ui.asm      (bank 0) game_main, the main loop, input, messages, the
;               status bar, dialog boxes, door codes, notebook, pause menu
;   core.asm    (bank 0) the level blob, the RNG, small maths, cells,
;               per-cell entity lists, the line trace
;   math32.asm  (bank 0) 32-bit arithmetic on the m32a/m32b/m32c workspace
;   events.asm  (bank 0) the tile scripts (byte code attached to cells)
;   combat.asm  (bank 1) hit and damage rolls, stats, monsters and their AI
;   actions.asm (bank 1) the player's actions: move, turn, use, pick up
; Banks 0 and 1 are both always paged in (slots 0 and 1), so any routine
; can call any other. The engine's services (svc_*, rom/main.asm) are
; called directly; they keep IX and IY and page bank 2 back into slot 2.
;
; Jargon used throughout:
;   level blob   the rules' copy of the level, in ROM bank 2 at $A000
;                (docs/rom_format.md): counts, then tables (LT_*).
;   entity       anything on the map that is not a wall: monsters, people,
;                items, barrels, fires... Indexed 0..n_ents-1; the first
;                n_map_ents are placed by the level, the last N_DROPS are
;                spare slots for drops.
;   def          an entity type in the level blob (its "definition"):
;                def_etype (ET_*), def_esub (subtype), def_parm, def_name.
;   monster slot an entity's index into the S_M_* stat arrays (ent_mon).
;   line         a door (or a wall line that can be opened); each has a
;                cell and flags in S_L_FLAGS.
;   event        a scripted cell: a list of commands run when the player
;                arrives, leaves, uses or acts on the cell (events.asm).
;   the model    tools/game_model.py, the Python spec of these rules.
;   lockstep     the test that runs the ROM and the model side by side.
;
; Conventions: arguments and results in registers as each routine says;
; a routine may change any register (and the flags) except IX and IY,
; unless its comment says otherwise. Byte results are in A, words in HL.
; "Bool" results are 0/1 in A with the Z flag set for 0.
;
; Licence: GPL-2.0-or-later.
;=======================================================================

.include "game/gen/defs.inc"
.include "game/gen/sfx.inc"
.include "game/gen/texts.inc"   ; TX_*: the game's texts (bank 2, $8000; tools/gen_cdata.py)

; ---- engine variables shared with the game (rom/main.asm .ENUM $C000)
; The game writes most of these and the engine (interrupts and services)
; reads them; the comments in rom/main.asm say who writes which.
.define ENG_FRAME       $C000       ; counts frames (the frame interrupt bumps it)
.define ENG_ENT_DEF     $C001       ; sprite to show: entity def, $FF none
.define ENG_ENT_DIST    $C002       ; 1..4 cells ahead (0 with no sprite)
.define ENG_HUD_DIRTY   $C003       ; bit 0 = row 0, bits 1-5 = rows 19-23: rows the interrupt must redraw
.define ENG_FLASH       $C004       ; frames of red damage flash
.define ENG_SOLID       $C005       ; 1 = solid-colour walls
.define ENG_TURN_SEQ    $C006       ; bumped after every handled input
.define ENG_IDLE        $C007       ; 1 while waiting for input
.define ENG_MODE        $C008       ; UI_* (read by the tests)
.define ENG_VIEW_X      $C009       ; the view svc_render draws: cell x
.define ENG_VIEW_Z      $C00A       ; cell z
.define ENG_VIEW_F      $C00B       ; facing (0 north, 1 east, 2 south, 3 west)
.define ENG_VSIZE       $C00C       ; 0 large view, 1 small
.define ENG_VMODE       $C00D       ; drawing the view: 0 outward from the centre (in place), 1 at once
.define ENG_PAUSE       $C00E       ; set by the pause button (NMI; Start on the Game Gear), cleared by the game
.define ENG_DETAIL      $C00F       ; 1 = low detail
.define SFX_QUEUE       $C0C0       ; ring of 8 effect ids (SFX_* from gen/sfx.inc)
.define SFX_HEAD        $C0C8       ; engine: next to play (0..7)
.define SFX_TAIL        $C0C9       ; game: next free slot (0..7); head = tail: empty
.define HUD_TEXT        $C100       ; 6 rows x 32 characters: row 0 (messages), then screen rows 19..23
.define DLG_TEXT        $C200       ; DLG_H rows x DLG_W characters, the box over the view
.define HUD_STAGE       $C380       ; status rows 1..5 (columns 7..31, 25 bytes a row) built here
; Dialog box layout (characters). The Game Gear's screen is 20 columns wide.
.IFDEF GAMEGEAR
.define DLG_W           20          ; (the Game Gear: the box fills its screen's width)
.define BOX_COL         1           ; a dialog's text column
.define BOX_HELP_COL    0           ; column of help lines (door codes, end screens)
.define PAUSE_LABEL_COL 2           ; pause menu: column of the labels
.define PAUSE_VALUE_COL 12          ; pause menu: column of the values
.ELSE
.define DLG_W           24          ; box width in characters
.define BOX_COL         3
.define BOX_HELP_COL    2
.define PAUSE_LABEL_COL 4
.define PAUSE_VALUE_COL 15
.ENDIF
.define DLG_H           16          ; box height in rows

; Banks: the Sega mapper register at $FFFF selects the 16 KB bank in
; slot 2 ($8000-$BFFF). The game keeps bank 2 there.
.define BANK_HOME       2           ; slot 2 while the game runs (level blob at $A000)
; BANK_MISC (4, the level's strings at $8000) comes from rom/main.asm
.define STRINGS_ADDR    $8000       ; string table in BANK_MISC: u16 offsets, then the text

; pad bits (1 = pressed), as svc_read_pad returns them in l
.define PAD_UP          1
.define PAD_DOWN        2
.define PAD_LEFT        4
.define PAD_RIGHT       8
.define PAD_B1          16          ; button 1
.define PAD_B2          32          ; button 2

; entity types (def_etype). Doors' kinds (line_etype) use the same
; numbers: 0 a door, 12 a door that can be broken (with the axe).
.define ET_LINE         0           ; a door
.define ET_ENEMY        1           ; a monster
.define ET_HUMAN        2           ; a person (talked to through events)
.define ET_PICKUP       3           ; health, armour, credits, keys
.define ET_INV          4           ; carried items: medkits, soulsphere
.define ET_WEAPON       5
.define ET_AMMO         6
.define ET_FIRE         10          ; a fire (1 damage, 2 armour when stood in)
.define ET_FIRE2        11          ; a worse fire (10 and 10)
.define ET_DESTRUCT     12          ; barrels (sub-type 1), crates (2), ...
.define ET_AMMO2        16          ; a bigger box of ammo

; entity flag bits (S_E_FLAGS)
.define F_ALIVE         $01         ; a monster alive, a fire burning, a barrel whole
.define F_HIDDEN        $02         ; hidden by a script (no sprite)
.define F_LINKED        $04         ; in its cell's list (core.asm): on the map
.define F_ACTIVE        $08         ; on the active list (S_ACTIVE): takes turns
.define F_ATK2          $10         ; a monster uses its second attack (mon_atk)
.define F_DROPPED       $20         ; it has already dropped something
.define F_CORPSE        $40         ; a dead monster that left a corpse
.define F_RAISING       $80         ; an arch-vile is about to raise this corpse

; door / line flag bits (S_L_FLAGS)
.define L_OPEN          1
.define L_LOCKED        2
.define L_ALIVE         4           ; a breakable door not yet broken

.define NO_DEF          $FF         ; e_def of an empty drop slot

; trace masks (core.asm trace): bit n set = an entity, door kind or
; prop of type n counts (blocks a move, stops a shot...); bit 8 = the
; player. (Bits 10 and 11 are the two fires; bit 12 destructibles.)
.define MASK_PLAYER_MOVE  $F287     ; types 0, 1, 2, 7, 9, 12-15
.define MASK_MONSTER_MOVE $FF87     ; types 0, 1, 2, 7, 9-15 and the player
.define MASK_SHOT         $5687     ; types 0, 1, 2, 7, 9, 10, 12, 14
.define MASK_SHOW_KILL    4098      ; $1002: monsters and destructibles
.define MASK_RAISE_FREE   16263     ; $3F87: types 0, 1, 2, 7, 9-13 and the player

; event flags (32-bit; these are in the low word). The rest of the
; word: bits 0-3 arriving (by the step's direction), bits 4-7 leaving,
; bits 12-15 the key held; the high word: bits 16-24 the event's state
; condition, bits 28-31 the facing. See events.asm and actions.asm.
.define EVF_USE         $0100       ; the player used the cell (button 1)
.define EVF_MODIFY      $0200       ; (in a command's conditions) runs once only
.define EVF_MOVE        $0400       ; a move or a turn

; trace hit kinds (hit_kind[])
.define H_WALL          0
.define H_PLAYER        1
.define H_LINE          2           ; hit_val = the line
.define H_STATIC        3           ; hit_val = the prop's type
.define H_ENT           4           ; hit_val = the entity

; why an event stopped (suspend, pend_why)
.define SUSP_NONE       0
.define SUSP_DIALOG     1           ; a dialog; resumes after it
.define SUSP_PASSWORD   2           ; a door code; resumes if it is right
.define SUSP_STOP       3           ; stopped for good (locked, not enough...)

; attack targets (queue_attack, select_target's sel_kind)
.define T_PLAYER        0
.define T_ENT           1           ; an entity (a monster, a barrel...)
.define T_RAISE         2           ; an arch-vile raising a corpse
.define T_LINE          3           ; a breakable door

; calc_hit results
.define HIT_MISS        0
.define HIT_HIT         1
.define HIT_CRIT        2

; what the screen is showing (ui, copied to ENG_MODE)
.define UI_PLAY         0
.define UI_DIALOG       1
.define UI_PASSWORD     2           ; door-code entry
.define UI_DEAD         3
.define UI_DONE         4           ; the sector is complete
.define UI_NOTES        5           ; the notebook
.define UI_PAUSE        6
.define UI_MAP          7           ; the automap (from the pause menu)
.define UI_CHEATS       8           ; the cheats (from the pause menu)

; the pause menu's cheats (bits of cheats): nothing hurts the player
; (combat.asm player_pain), attacks use no ammunition (actions.asm
; player_attack). Not part of the game state; off at power-on.
.define CHEAT_GOD       1
.define CHEAT_AMMO      2

.define MSGQ            8           ; status messages waiting to be shown (a power of 2: indexes wrap with MSGQ-1)
.define BOX_LINES       13          ; text lines in a dialog box

;-----------------------------------------------------------------------
; game variables (not part of the compared game state; game_init and
; new_game set them up)
;-----------------------------------------------------------------------
.ENUM $CAE0
; the level blob: counts, spawn, and where each table is (level_load)
n_defs          db                  ; entity types (defs) in the level
n_map_ents      db                  ; things placed on the map
n_ents          db                  ; n_map_ents + N_DROPS: all entity slots
n_mons          db                  ; monster stat slots used
n_lines         db                  ; doors / lines
n_events        db                  ; scripted cells
n_cmds          db                  ; script commands
n_strings       db                  ; level strings
spawn_x         db                  ; the player's start cell and facing
spawn_z         db
spawn_f         db
lv_tab          dsb 2*LEVEL_N_TABLES    ; addresses of the tables, LT_* order
; linked entities per cell, sorted by index: cell_head = first index + 1,
; ent_next = next index + 1, 0 = end of the list
cell_head       dsb 1024            ; one per cell (z * 32 + x)
ent_next        dsb MAX_ENTS        ; (game_init clears both together: it relies on this order)
; the last trace (core.asm trace)
hit_n           db                  ; hits listed (0..8)
hit_kind        dsb 8               ; H_* for each
hit_val         dsb 8               ; the line, prop type or entity for each
; events
dialog_str      db                  ; a dialog to open ($FF none)
pw_want         db                  ; a door code to ask for ($FF none): the string holding the right code
pw_prompt       db                  ; the string shown as the door code's prompt
suspend         db                  ; SUSP_*: why the running event stopped
skip_turn       db                  ; 1: a script opened a dialog or door code; act_use then ends without a monster turn
monsters_pending db                 ; the monsters still have their turn to take
hold_monsters   db                  ; the UI draws the view before settle() runs them
pend_valid      db                  ; a suspended event to resume
pend_ev         db                  ; its event
pend_k          db                  ; the command it stopped at (resume carries on after it)
pend_why        db                  ; SUSP_* it stopped for
pend_flags      dsb 4               ; the event flags it was running with
status_msg      dsb 33              ; a status message from a script (up to 32 characters + 0)
status_new      db                  ; 1: status_msg is new (msg_tick shows it)
player_hurt     db                  ; 1: the player was hurt this action (after_action flashes the view)
att_n           db                  ; monster attacks waiting (in the shared block below)
n_pend_expl     db                  ; explosions waiting
expl_x          dsb 8               ; their cells (expl_z must follow expl_x: push_explosion uses +8)
expl_z          dsb 8
; combat scratch
cb_hp           db                  ; a combatant (cb_player / cb_monster)
cb_maxhp        db                  ; (these eight are in the order of S_HP..S_ACCURACY)
cb_armor        db
cb_maxarmor     db
cb_def          db
cb_str          db
cb_agi          db
cb_acc          db
cb_mclass       db                  ; the monster's class (sub-type), $FF for the player
dmg_hp          dw                  ; calc_damage results (the model's are not bounded to a byte)
dmg_arm         dw
sel_kind        db                  ; select_target: T_ENT or T_LINE
sel_idx         db                  ; the entity or the line
sel_locked      db                  ; 1: a door that is locked was in the way
m32a            dsb 4               ; 32-bit maths workspace (math32.asm), little-endian
m32b            dsb 4
m32c            dsb 4
; messages and text
msg_head        db                  ; the oldest message in msgq (0..MSGQ-1)
msg_count       db                  ; messages waiting
msg_timer       db                  ; frames before the next one may be shown
numbuf          dsb 6               ; utoa10's digits (up to 5 and a 0)
hud_snap        dsb 17              ; the values the status bar last showed (hud_same)
; the picture on screen
cur_x           db                  ; the view last drawn ($FF: draw again)
cur_z           db
cur_f           db
cur_doors       dsb MAX_LINES       ; each door's L_OPEN bit when it was drawn
cur_def         db                  ; the sprite last shown
cur_dist        db
; boxes
ui              db                  ; UI_*
box_str         dsb MAX_NOTES       ; the pager's strings
box_n           db                  ; how many
box_i           db                  ; the one shown
box_line        db                  ; its first line shown
code            dsb 6               ; a door code being entered (ASCII digits, then 0)
code_len        db                  ; digits entered (0..3)
code_digit      db                  ; the digit being chosen (0..9)
line1           dsb 25              ; text built for a box (and hud_same's scratch)
line2           dsb 25
pause_cur       db                  ; pause menu row (0..7)
pad_prev        db                  ; the buttons held last frame
cheats          db                  ; the cheats on (CHEAT_* bits)
cheat_cur       db                  ; the cheats box's line (0..3)
game_ram_end    db
.ENDE

; One block, two uses that never overlap in time: level_string's copy of a
; string (read straight away by whoever asked) and the monsters' turn.
.ENUM $D100
strbuf          dsb 400             ; (end_screens also builds its title here)
.ENDE
.ENUM $D100
att_ent         dsb MAX_ACTIVE      ; monster attacks waiting, newest first
att_kind        dsb MAX_ACTIVE      ; their T_* kind
att_tgt         dsb MAX_ACTIVE      ; their target entity
mt_movers       dsb MAX_ACTIVE      ; monster_turn: those that moved
mt_again        dsb MAX_ACTIVE      ; those that moved again (the next round's movers)
mt_snap         dsb MAX_ACTIVE      ; the active list at the start of the turn
shared_end      db
.ENDE

; the messages: in the free RAM after the renderer's variables ($DC00-$DCE7)
.ENUM $DCE8
msgq            dsb MSGQ*32         ; a ring of MSGQ messages, 32 bytes each (31 characters + 0)
msg_ram_end     db
.ENDE

; Each source file also has its own scratch variables, in .ENUMs after
; the shared block: combat $D290, events $D310, actions $D340, ui $D370,
; core (the trace) $D3A0, math32 $D3B0. actions.asm checks most of them
; for overlaps.

; Assembly-time checks that the RAM blocks above do not overlap.
.IF S_END_COMPARED > $CAE0
.FAIL "the game state runs into the game variables at $CAE0"
.ENDIF
.IF msg_ram_end > $DDF0
.FAIL "the messages run into the renderer's counters at $DDF0"
.ENDIF
.IF game_ram_end > $D100
.FAIL "game variables run into the shared block at $D100"
.ENDIF
