; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

;=======================================================================
; Scripted events: each cell may have an event, a list of commands that
; run when the player arrives, leaves, faces a way or uses the cell.
; Commands can stop (a dialog, a door code, a locked door) and carry on
; later (resume). Then the monsters take their turn (settle).
;
; Bank 0. Called by actions.asm (act_move, act_turn, act_use,
; line_destroyed, game_init: run_tile, end_player_turn) and ui.asm
; (resume, settle, modal). Calls into combat.asm (player_pain, add_xp,
; destroy, monster_died, monster_turn, activate_visible) and ui.asm
; (say, sfx, level_string).
;
; The data, all in the level blob (docs/rom_format.md):
;   ev_at[cell]   the event on each cell, 255 = none
;   ev_flags[ev]  bit 0: the event ignores moves (EVF_MOVE)
;   ev_cmd0[ev]   its first command (u16), ev_ncmd[ev] how many
;   ev_state0[ev] its starting state (the live one is S_EV_STATE)
;   cmd_op[c]     the opcode (the handlers in ec_table)
;   cmd_a1[c]     a 32-bit argument: "lo" = byte 0, "hi" = byte 1, ...
;   cmd_a2[c]     32-bit "when" flags, the command's conditions:
;                   bits 0-3   arriving from a direction, 4-7 leaving
;                   bit 8      EVF_USE, bit 10 EVF_MOVE (what happened)
;                   bit 9      EVF_MODIFY: runs only once (S_CMD_DEAD)
;                   bits 12-15 the key that must be held (0 = any)
;                   bits 16-24 the event states it runs in (none = state 0)
;                   bits 28-31 the facing
; A command runs when it has not been killed, its state and key
; conditions hold, and it shares at least one bit with the flags of what
; happened (re_flags).
; S_CMD_DEAD is a bit per command (MAX_CMDS bits): set when a command
; with EVF_MODIFY has run.
;=======================================================================

.ENUM $D310
re_ev           db                  ; run_event: the event
re_k            db                  ; the command being looked at (0.. within the event)
re_n            db                  ; the event's command count
re_ran          db                  ; 1 once a command has run
re_c            dw                  ; (commands are numbered in words: up to MAX_CMDS)
re_flags        dsb 4               ; the event flags it runs with (32-bit)
re_n7           dsb 4               ; the event's state as a flag bit (0 = none)
re_a2           dsb 4               ; this command's conditions
ec_c            dw                  ; exec_cmd: the command
ec_op           db                  ; its opcode
ec_a1           dsb 4               ; its argument
kt_list         dsb 2               ; kill_on_tile: up to two entities found
kt_n            db                  ; how many
rs_ev           db                  ; resume: a copy of pend_ev, pend_k + 1, pend_why, pend_flags
rs_k            db
rs_why          db
rs_flags        dsb 4
rs_pw           dw                  ; the code entered (0 = cancelled)
events_ram_end  db
.ENDE

; b = x, c = z -> a = the event on that cell, or $FF (also off the map);
; keeps bc; clobbers de, hl
find_event:
    ld a, b
    or c
    and $E0
    ld a, $FF
    ret nz                      ; off the map
    call cell_index
    ex de, hl
    LTAB LT_EV_AT
    add hl, de
    ld a, (hl)                  ; ev_at[cell]
    ret

; hl = command -> hl = its byte in S_CMD_DEAD, a = its bit mask;
; clobbers de. (Byte c / 8, bit c & 7.)
cmd_bit:
    ld a, l
    and 7                       ; the bit number
    srl h
    rr l
    srl h
    rr l
    srl h
    rr l                        ; hl = command / 8
    ld de, S_CMD_DEAD
    add hl, de
    push hl
    ld hl, bit_mask
    call add_hl_a_read          ; a = 1 << bit
    pop hl
    ret

; hl = command -> NZ if it has run for good (EVF_MODIFY); clobbers a, de, hl
cmd_is_dead:
    call cmd_bit
    and (hl)
    ret

; hl = command: marked as run for good; clobbers a, de, hl
cmd_kill:
    call cmd_bit
    or (hl)
    ld (hl), a
    ret

; b = x, c = z: kills what a SHOW command would replace (at most two)
; Collects the first two linked entities on the cell, then hands each to
; kt_one. Clobbers every register.
kill_on_tile:
    xor a
    ld (kt_n), a
    ld e, 0
-:  call ent_at                 ; the next linked entity >= e
    cp $FF
    jr z, +
    ld hl, kt_n
    ld d, (hl)
    inc (hl)
    ld hl, kt_list
    push af
    ld a, d
    call add_hl_a               ; hl = &kt_list[n]
    pop af
    ld (hl), a
    inc a
    ld e, a
    ld a, (kt_n)
    cp 2
    jr c, -                     ; room for another
+:  ld a, (kt_n)
    or a
    ret z
    ld a, (kt_list)
    call kt_one                 ; the first
    ld a, (kt_n)
    cp 2
    ret c
    ld a, (kt_list+1)           ; then the second (falls into kt_one)
; a = entity: killed or destroyed, if a SHOW kills its kind
; (MASK_SHOW_KILL: monsters die as if killed, with XP and a corpse;
; destructibles are destroyed, which may explode). Clobbers everything.
kt_one:
    push af
    call etype
    ld hl, MASK_SHOW_KILL
    call mask_has
    pop bc
    ret z                       ; another kind: left alone
    ld a, b
    push af
    call etype
    cp ET_ENEMY
    pop bc
    ld a, b
    jp nz, destroy              ; not a monster: destroyed
    ld e, 0                     ; a monster: dies, leaving a corpse
    jp monster_died

; hl = command -> a = 1 if it counts as run (NZ), 0 if it didn't apply
; Reads the command's opcode and argument (into ec_op, ec_a1), finds the
; handler in ec_table and jumps to it with a = lo (byte 0 of a1). An
; unknown opcode does nothing and counts as run. Handlers end at
; _ec_true or _ec_false; they may set suspend (SUSP_*) to stop the
; event. Clobbers every register.
exec_cmd:
    ld (ec_c), hl
    ex de, hl
    LTAB LT_CMD_OP
    add hl, de
    ld a, (hl)                  ; cmd_op[c]
    ld (ec_op), a
    ld de, (ec_c)               ; a1 (32-bit)
    LTAB LT_CMD_A1
    add hl, de
    add hl, de
    add hl, de
    add hl, de                  ; hl = &cmd_a1[c] (4 bytes each)
    ld de, ec_a1
    ld bc, 4
    ldir
    ld a, (ec_op)
    ld hl, ec_table
-:  ld b, (hl)                  ; (op, address) entries, $FF ends
    inc hl
    ld e, (hl)
    inc hl
    ld d, (hl)
    inc hl
    inc b
    jr z, _ec_true              ; unknown: nothing to do
    dec b
    cp b
    jr nz, -
    ex de, hl
    ld a, (ec_a1)               ; a = lo, (ec_a1+1) = hi for the handlers
    jp (hl)                     ; to the handler
; the handlers' two ways out
_ec_true:                       ; -> a = 1, NZ: the command ran
    ld a, 1
    or a
    ret
_ec_false:                      ; -> a = 0, Z: it did not apply
    xor a
    ret

; the opcodes: .db op, .dw handler; an op of $FF ends the table
ec_table:
    .db 2
    .dw ec_exit
    .db 4
    .dw ec_message
    .db 5
    .dw ec_pain
    .db 7
    .dw ec_show
    .db 8
    .dw ec_dialog
    .db 26
    .dw ec_dialog
    .db 9
    .dw ec_map
    .db 10
    .dw ec_password
    .db 11
    .dw ec_state
    .db 19
    .dw ec_state
    .db 20
    .dw ec_state
    .db 12
    .dw ec_lock
    .db 13
    .dw ec_unlock
    .db 14
    .dw ec_toggle_lock
    .db 6
    .dw ec_door
    .db 15
    .dw ec_door
    .db 16
    .dw ec_door
    .db 17
    .dw ec_door
    .db 18
    .dw ec_hide
    .db 21
    .dw ec_stat
    .db 22
    .dw ec_stat
    .db 23
    .dw ec_require
    .db 24
    .dw ec_status
    .db 40
    .dw ec_note
    .db 41
    .dw ec_key
    .db $FF
    .dw 0

; 2: the exit (top bit of a1), or a one-way door
; With bit 31 of a1 set the sector is complete (S_LEVEL_DONE; ui.asm
; shows the end screen). Otherwise "THE WAY BACK IS SEALED" and the event
; stops (README.md: the door back to Junction, a level not in this port).
ec_exit:
    ld a, (ec_a1+3)
    bit 7, a                    ; bit 31 of a1
    jr z, +
    ld a, 1
    ld (S_LEVEL_DONE), a
    jr _ec_true
+:  ld hl, str_sealed
    call say                    ; queue the message
    ld a, SFX_DENIED
    call sfx
    ld a, SUSP_STOP
    ld (suspend), a
    jr _ec_true

; 4: a message (a level string)
ec_message:
    call level_string           ; hl = string lo (copied to strbuf)
    call say                    ; queue it for the top line
    jr _ec_true

; 5: lo damage (to health only; no armour damage)
ec_pain:
    push af
    ld hl, str_pain
    call say                    ; "PAIN!"
    pop af
    ld l, a
    ld h, 0
    ld de, 0
    call player_pain            ; hl = damage, de = armour damage
    jp _ec_true

; 7: shows hidden entity lo (killing what stands there)
; lo = $FF: nothing. The entity is linked onto its cell (so it appears);
; first, monsters and destructibles on that cell are killed / destroyed.
ec_show:
    cp $FF
    jp z, _ec_true
    ld c, a
    push bc
    ld hl, S_E_X
    call add_hl_a_read
    ld b, a                     ; b = its x
    ld a, c
    ld hl, S_E_Z
    call add_hl_a_read
    ld c, a                     ; c = its z
    call kill_on_tile           ; clear the cell
    pop bc
    ld a, c
    call ent_link               ; then it appears
    jp _ec_true

; 8, 26: a dialog (string lo). The event stops here and goes on after the
; dialog is closed (SUSP_DIALOG, resume).
ec_dialog:
    ld (dialog_str), a          ; after_action (ui.asm) opens it
    ld a, SUSP_DIALOG
; a = SUSP_*: the event stops; the Use that started it does not end the
; player's turn (skip_turn)
_ec_suspend_skip:
    ld (suspend), a
    ld a, 1
    ld (skip_turn), a
    jp _ec_true

; 9: the map (only recorded in S_MAP_GIVEN; nothing here reads it)
ec_map:
    ld a, 1
    ld (S_MAP_GIVEN), a
    jp _ec_true

; 10: ask for door code (string lo), prompt hi
; String lo holds the right code; the event stops (SUSP_PASSWORD) and
; goes on only if the code entered matches (resume).
ec_password:
    ld (pw_want), a
    ld a, (ec_a1+1)
    ld (pw_prompt), a
    ld a, SUSP_PASSWORD
    jr _ec_suspend_skip

; 11 (set), 19 (+1, up to 9), 20 (-1, down to 0): the state of the event at (lo, hi)
; Set takes the new state from byte 2 of a1. The result is kept to 4
; bits. Nothing happens if (lo, hi) has no event.
ec_state:
    ld b, a
    ld a, (ec_a1+1)
    ld c, a
    call find_event             ; a = the event at (lo, hi)
    cp $FF
    jp z, _ec_true
    ld hl, S_EV_STATE
    call add_hl_a
    ld b, (hl)                  ; b = its state
    ld a, (ec_op)
    cp 11
    jr nz, +
    ld a, (ec_a1+2)             ; set
    ld b, a
    jr ++
+:  cp 19
    jr nz, +
    ld a, b                     ; + 1, up to 9
    cp 9
    jr nc, ++
    inc b
    jr ++
+:  ld a, b                     ; 20
    or a
    jr z, ++
    dec b
++: ld a, b
    and 15
    ld (hl), a
    jp _ec_true

; 12, 13, 14: lock, unlock, toggle door lo's lock
ec_lock:
    call ec_line
    set 1, (hl)                 ; L_LOCKED
    jp _ec_true
ec_unlock:
    call ec_line
    res 1, (hl)                 ; L_LOCKED
    jp _ec_true
ec_toggle_lock:
    call ec_line
    ld a, (hl)
    xor L_LOCKED
    ld (hl), a
    jp _ec_true

; a = line -> hl = &l_flags[line]; keeps bc, de
ec_line:
    ld hl, S_L_FLAGS
    jp add_hl_a

; 6 (toggle), 15 (open), 16 (close), 17 (toggle): door lo, unless locked
; A locked door, opening an open one or closing a closed one: the
; command does not apply (not counted as run, not killed).
ec_door:
    call ec_line                ; hl = &l_flags[lo]
    bit 1, (hl)                 ; L_LOCKED
    jp nz, _ec_false
    ld a, (ec_op)
    cp 15
    jr nz, +
    bit 0, (hl)                 ; open: already open?
    jp nz, _ec_false
+:  cp 16
    jr nz, +
    bit 0, (hl)                 ; close: already closed?
    jp z, _ec_false
+:  ld a, (hl)
    xor L_OPEN                  ; open <-> closed
    ld (hl), a
    ld a, SFX_DOOR
    call sfx
    jp _ec_true

; 18: hides everything but monsters at (lo, hi)
; Each non-monster linked entity on the cell gets F_HIDDEN and is
; unlinked.
ec_hide:
    ld b, a
    ld a, (ec_a1+1)
    ld c, a
    ld e, 0
-:  call ent_at                 ; the next linked entity >= e
    cp $FF
    jp z, _ec_true
    push bc
    push af
    call etype
    cp ET_ENEMY
    jr z, +                     ; monsters stay
    pop af
    push af
    ld hl, S_E_FLAGS
    call add_hl_a
    set 1, (hl)                 ; F_HIDDEN
    pop af
    push af
    call ent_unlink             ; off the map
+:  pop af
    pop bc
    inc a
    ld e, a                     ; go on from the next index
    jr -

; 21 (+), 22 (-): stat lo (0 health, 1 armour, 2 credits, 3 XP) by hi
; (Another lo does nothing.) Health, armour and XP go through add_health,
; add_armor and add_xp, which say "GAINED ..." and handle level-ups.
ec_stat:
    ld a, (ec_a1+1)
    ld l, a
    ld h, 0                     ; hl = hi
    ld a, (ec_op)
    cp 22
    call z, negw                ; 22: hl = -hi
    ld a, (ec_a1)
    or a
    jp z, ec_stat_health
    dec a
    jr z, +
    dec a
    jr z, ++
    dec a
    jp nz, _ec_true
    call add_xp                 ; 3: XP (and level-ups)
    jp _ec_true
+:  call add_armor              ; 1: armour
    jp _ec_true
++: ex de, hl                   ; 2: credits (16-bit, not bounded)
    ld hl, (S_CREDITS)
    add hl, de
    ld (S_CREDITS), hl
    jp _ec_true
ec_stat_health:
    call add_health             ; 0: health
    jp _ec_true

; 23: requires stat lo >= hi, else stops (and maybe opens dialog a1 >> 16)
; Stat lo as in ec_stat (3 and up: XP). Compared unsigned, 16-bit. When
; it fails: "INSUFFICIENT ...", the event stops for good (SUSP_STOP), and
; if string (byte 2 of a1) is not empty it opens as a dialog. The
; command counts as run either way.
ec_require:
    and 3
    ld c, a                     ; c = which message
    ld a, (ec_a1)
    or a
    jr nz, +
    ld a, (S_HP)                ; 0: health
    ld l, a
    ld h, 0
    jr ++
+:  dec a
    jr nz, +
    ld a, (S_ARMOR)             ; 1: armour
    ld l, a
    ld h, 0
    jr ++
+:  dec a
    jr nz, +
    ld hl, (S_CREDITS)          ; 2: credits
    jr ++
+:  ld hl, (S_XP)               ; 3: XP
++: ld a, (ec_a1+1)
    ld e, a
    ld d, 0
    or a
    sbc hl, de
    jp nc, _ec_true             ; have >= hi
    ld a, c
    add a, a
    ld hl, str_insuff
    call add_hl_a
    ld a, (hl)
    inc hl
    ld h, (hl)
    ld l, a                     ; hl = str_insuff[c]
    call say
    ld a, SFX_DENIED
    call sfx
    ld a, SUSP_STOP
    ld (suspend), a
    ld a, (ec_a1+2)
    call level_string           ; is the dialog string empty?
    ld a, (hl)
    or a
    jp z, _ec_true
    ld a, (ec_a1+2)
    ld (dialog_str), a          ; open it (the event does not resume after it)
    ld a, 1
    ld (skip_turn), a
    jp _ec_true

; 24: a status message (shown at the start of the next frame)
; Copied into status_msg (at most 32 characters); msg_tick (ui.asm)
; queues it.
ec_status:
    call level_string           ; hl = string lo
    ld de, status_msg
    ld b, 32
-:  ld a, (hl)
    or a
    jr z, +
    ld (de), a
    inc hl
    inc de
    djnz -
+:  xor a
    ld (de), a                  ; the terminating 0
    inc a
    ld (status_new), a
    jp _ec_true

; 40: a note for the notebook
; String lo is added to S_NOTES (if there is room for it: MAX_NOTES);
; "NOTE ADDED" is said either way.
ec_note:
    ld c, a
    ld a, (S_N_NOTES)
    cp MAX_NOTES
    jr nc, +                    ; full
    ld hl, S_N_NOTES
    inc (hl)
    ld hl, S_NOTES
    call add_hl_a               ; hl = &notes[old count]
    ld (hl), c
+:  ld hl, str_note_added
    call say
    ld a, SFX_SELECT
    call sfx
    jp _ec_true

; 41: needs key lo & 3 (stops without it)
; Keys: bit 0 green, 1 yellow, 2 blue, 3 red (S_KEYS). Holding the key:
; the command does not apply (_ec_false). Without it: "NEED ... KEY" and
; the event stops for good.
ec_key:
    and 3
    ld c, a
    ld hl, bit_mask
    call add_hl_a_read          ; a = 1 << key
    ld hl, S_KEYS
    and (hl)
    jp nz, _ec_false            ; held
    ld a, c
    add a, a
    ld hl, str_need_key
    call add_hl_a
    ld a, (hl)
    inc hl
    ld h, (hl)
    ld l, a                     ; hl = str_need_key[key]
    call say
    ld a, SFX_DENIED
    call sfx
    ld a, SUSP_STOP
    ld (suspend), a
    jp _ec_true

;-----------------------------------------------------------------------
; a = event, b = first command, re_flags = what happened (EVF_* and the
; facing/arrival bits) -> a = 1 (NZ) if a command ran
;
; Goes through the event's commands from number b (0 = the first) to the
; last, running each whose conditions hold (see the top of this file).
; If a command sets suspend (a dialog, a door code, a stop), the event's
; place is saved in pend_* (resume carries on after that command) and
; the rest is not run. re_flags gets the key bits ORed in. Clobbers
; every register except IX, IY. Called by run_tile and rs_run.
;-----------------------------------------------------------------------
run_event:
    ld (re_ev), a
    ld a, b
    ld (re_k), a
    xor a
    ld (re_ran), a
    ; events that ignore moves
    ld a, (re_ev)
    LTAB LT_EV_FLAGS
    call add_hl_a_read
    rra                         ; carry = ev_flags bit 0
    jr nc, +
    ld a, (re_flags+1)
    and >EVF_MOVE
    jr z, +
    xor a                       ; a move or turn: this event does nothing
    ret
+:  ; n7: its state as a bit (state s: bit 15 + s), 0 for state 0
    ld hl, 0
    ld (re_n7), hl
    ld (re_n7+2), hl
    ld a, (re_ev)
    ld hl, S_EV_STATE
    call add_hl_a_read
    or a
    jr z, +
    add a, 15                   ; the bit number
    ld c, a
    rrca
    rrca
    rrca
    and 3                       ; its byte (bit / 8)
    ld hl, re_n7
    call add_hl_a
    ld a, c
    and 7
    push hl
    ld hl, bit_mask
    call add_hl_a_read          ; a = 1 << (bit & 7)
    pop hl
    ld (hl), a
+:  ; the keys held: one bit of $F000 (red, blue, green, yellow first)
    ; (S_KEYS: bit 0 green, 1 yellow, 2 blue, 3 red; only the first held
    ; in that order is reported: red $8000, blue $4000, green $2000,
    ; yellow $1000)
    ld a, (S_KEYS)
    ld c, $80
    bit 3, a
    jr nz, +
    ld c, $40
    bit 2, a
    jr nz, +
    ld c, $20
    bit 0, a
    jr nz, +
    ld c, $10
    bit 1, a
    jr nz, +
    ld c, 0
+:  ld a, (re_flags+1)
    or c
    ld (re_flags+1), a
    ld a, (re_ev)
    LTAB LT_EV_NCMD
    call add_hl_a_read
    ld (re_n), a
_re_cmd:                        ; for each command k from re_k to re_n - 1
    ld a, (re_k)
    ld hl, re_n
    cp (hl)
    jp nc, _re_done
    ld c, a                     ; c = ev_cmd0[ev] (a word) + k
    ld a, (re_ev)
    ld e, a
    ld d, 0
    LTAB LT_EV_CMD0
    add hl, de
    add hl, de                  ; hl = &ev_cmd0[ev] (u16 each)
    ld a, (hl)
    inc hl
    ld h, (hl)
    add a, c
    ld l, a
    jr nc, +
    inc h
+:  ld (re_c), hl               ; the command's number
    call cmd_is_dead
    jp nz, _re_next             ; run for good already
    ; a2 = cmd_a2[c]
    ld de, (re_c)
    LTAB LT_CMD_A2
    add hl, de
    add hl, de
    add hl, de
    add hl, de                  ; hl = &cmd_a2[c] (4 bytes each)
    ld de, re_a2
    ld bc, 4
    ldir
    ; its state condition: the event's state bit, or (state 0) none of bits 16..24
    ld hl, re_n7
    ld a, (hl)
    inc hl
    or (hl)
    inc hl
    or (hl)
    inc hl
    or (hl)
    jr z, +                     ; state 0
    ld hl, re_n7
    ld de, re_a2
    call and32                  ; a2 has the state's bit?
    jr z, _re_next
    jr ++
+:  ld a, (re_a2+2)             ; state 0: bits 16..23 must be clear
    or a
    jr nz, _re_next
    ld a, (re_a2+3)             ; and bit 24
    rra
    jr c, _re_next
++: ; key condition: a2 & $F000 is 0 or the key bits
    ld a, (re_a2+1)
    and $F0
    jr z, +
    ld c, a
    ld a, (re_flags+1)
    and $F0
    cp c
    jr nz, _re_next
+:  ; and something it reacts to happened
    ld hl, re_flags
    ld de, re_a2
    call and32                  ; any bit in common?
    jr z, _re_next
    xor a
    ld (suspend), a
    ld hl, (re_c)
    call exec_cmd               ; run it
    jr z, +                     ; it did not apply
    ld a, 1
    ld (re_ran), a
    ld a, (re_a2+1)
    and >EVF_MODIFY
    jr z, +
    ld hl, (re_c)
    call cmd_kill               ; a one-shot command: never again
+:  ld a, (suspend)
    or a
    jr z, _re_next
    ; it stopped the event: remember where, for resume
    ld (pend_why), a
    ld a, 1
    ld (pend_valid), a
    ld a, (re_ev)
    ld (pend_ev), a
    ld a, (re_k)
    ld (pend_k), a
    ld hl, re_flags
    ld de, pend_flags
    ld bc, 4
    ldir
    jr _re_done
_re_next:
    ld hl, re_k
    inc (hl)
    jp _re_cmd
_re_done:
    ld a, (re_ran)
    or a
    ret

; hl, de = two 32-bit values -> NZ if they share a bit; a = the OR of the
; four ANDed bytes; clobbers bc, de, hl
and32:
    ld b, 4
    ld c, 0
-:  ld a, (de)
    and (hl)
    or c
    ld c, a
    inc hl
    inc de
    djnz -
    ld a, c
    or a
    ret

; b = x, c = z, re_flags = what happened -> a = 1 (NZ) if its event ran a command
; (No event on the cell: a = 0.) Clobbers every register except IX, IY.
run_tile:
    call find_event
    cp $FF
    jr z, +
    ld b, 0                     ; from its first command
    jp run_event
+:  xor a
    ret

; hl = the low word of the flags (the high word is 0): re_flags = hl
; Clobbers hl.
flags_lo:
    ld (re_flags), hl
    ld hl, 0
    ld (re_flags+2), hl
    ret

; -> NZ while a dialog or door code is waiting (dialog_str or pw_want is
; not $FF); clobbers a
modal:
    ld a, (dialog_str)
    inc a                       ; $FF + 1 = 0: none
    ret nz
    ld a, (pw_want)
    inc a
    ret

; the monsters' turn, once nothing holds it up
; Runs only if monsters_pending is set and neither hold_monsters (ui.asm
; holds it while the player's own move is drawn) nor a waiting dialog or
; door code holds it up. First wakes monsters that can now see the
; player, then (unless the player is dead) runs monster_turn. Clobbers
; every register except IX, IY.
settle:
    ld a, (hold_monsters)
    or a
    ret nz
    call modal
    ret nz
    ld a, (monsters_pending)
    or a
    ret z
    xor a
    ld (monsters_pending), a
    call activate_visible       ; wake those that see the player
    ld a, (S_DEAD)
    or a
    ret nz
    jp monster_turn             ; they move and attack

; the player's action used their turn: the monsters' turn is due (now,
; or later through settle when something holds it up)
end_player_turn:
    ld a, 1
    ld (monsters_pending), a
    jr settle

; hl = the code entered (0 = cancelled): carries on with the event that
; stopped for a dialog or a door code
; Called by ui.asm when a dialog is closed (hl = 0) or a door code is
; entered or cancelled. Clears dialog_str (and pw_want for a code). A
; dialog: the event goes on after the command that opened it. A code:
; compared with the wanted string; right: "CORRECT CODE!" and the event
; goes on; wrong: "INVALID CODE!" (unless nothing was entered); cancelled:
; nothing. Then settle, except after a cancelled or wrong code, which
; return straight away. Clobbers every register except IX, IY.
resume:
    ld (rs_pw), hl
    ld a, (pend_ev)             ; copy the pending event's place (run_event may set it again)
    ld (rs_ev), a
    ld a, (pend_k)
    inc a                       ; from the command after the one that stopped it
    ld (rs_k), a
    ld a, (pend_why)
    ld (rs_why), a
    ld hl, pend_flags
    ld de, rs_flags
    ld bc, 4
    ldir
    ld a, $FF
    ld (dialog_str), a          ; the dialog is closed
    ld a, (pend_valid)
    ld b, a
    xor a
    ld (pend_valid), a
    or b
    jr z, settle                ; nothing pending
    ld a, (rs_why)
    cp SUSP_DIALOG
    jr nz, +
    call rs_run                 ; after a dialog: the rest of the event
    jr settle
+:  cp SUSP_PASSWORD
    jr nz, settle               ; SUSP_STOP: stopped for good
    ld a, (pw_want)
    ld c, a                     ; c = the string with the right code
    ld a, $FF
    ld (pw_want), a
    ld hl, (rs_pw)
    ld a, h
    or l
    ret z                       ; cancelled
    push hl
    ld a, c
    call level_string           ; the code it wants
    pop de
    call str_eq
    jr z, +
    ld hl, (rs_pw)              ; wrong (an empty code says nothing)
    ld a, (hl)
    or a
    ret z
    ld hl, str_invalid_code
    call say
    ld a, SFX_DENIED
    jp sfx
+:  ld hl, str_correct_code
    call say
    ld a, SFX_SELECT
    call sfx
    call rs_run                 ; the rest of the event
    jp settle

; the rest of the stopped event: run_event(rs_ev, rs_k) with rs_flags
rs_run:
    ld hl, rs_flags
    ld de, re_flags
    ld bc, 4
    ldir
    ld a, (rs_k)
    ld b, a
    ld a, (rs_ev)
    jp run_event

; hl, de = two strings -> Z if equal; hl, de left where they differ (or at the 0)
str_eq:
-:  ld a, (de)
    cp (hl)
    ret nz
    or a
    ret z                       ; both ended together
    inc hl
    inc de
    jr -

; messages; str_need_key and str_insuff are indexed by key (green,
; yellow, blue, red) and by stat (health, armour, credits, XP)
str_sealed:       .db "THE WAY BACK IS SEALED", 0
str_pain:         .db "PAIN!", 0
str_note_added:   .db "NOTE ADDED", 0
str_invalid_code: .db "INVALID CODE!", 0
str_correct_code: .db "CORRECT CODE!", 0
str_need_key:
    .dw str_key_g, str_key_y, str_key_b, str_key_r
str_key_g:        .db "NEED GREEN KEY", 0
str_key_y:        .db "NEED YELLOW KEY", 0
str_key_b:        .db "NEED BLUE KEY", 0
str_key_r:        .db "NEED RED KEY", 0
str_insuff:
    .dw str_insuff_hp, str_insuff_ar, str_insuff_cr, str_insuff_xp
str_insuff_hp:    .db "INSUFFICIENT HEALTH!", 0
str_insuff_ar:    .db "INSUFFICIENT ARMOR!", 0
str_insuff_cr:    .db "INSUFFICIENT FUNDS!", 0
str_insuff_xp:    .db "INSUFFICIENT XP!", 0
