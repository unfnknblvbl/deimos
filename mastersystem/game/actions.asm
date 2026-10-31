; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

;=======================================================================
; The player's actions (move, turn, use/attack, pass, weapons, medkits)
; and the start of a game.
;
; Bank 1. The act_* routines are called by play_input (ui.asm), one per
; button press; game_init by new_game (ui.asm). An action changes the
; game state, runs the scripted events it sets off (run_tile,
; events.asm) and, if it used the player's turn, calls end_player_turn,
; which lets the monsters move (settle). The UI redraws afterwards.
; Uses combat.asm for attacks and stats, core.asm for the trace and the
; entity lists.
;
; Facings: 0 north (z - 1), 1 east (x + 1), 2 south (z + 1), 3 west.
; Each move and turn sends event flags (re_flags) to the cells involved:
; EVF_MOVE, the facing bit (bits 28-31) and the arrive / leave bits.
;=======================================================================

.ENUM $D340
pa_w            db                  ; player_attack: the weapon
pa_j            db                  ; the target (entity or line)
pa_tiles        db                  ; distance to it (|dx| + |dz|)
pa_shots        db                  ; shots this weapon fires
pa_s            db                  ; the shot being fired
pa_hit          db                  ; the last shot's calc_hit result
pa_crit         db                  ; 1: the first shot was a critical
pa_enemy        db                  ; 1: the target is a monster
pa_tot          dw                  ; total health damage
pa_tarm         dw                  ; total armour damage
pa_thr          dw                  ; overkill threshold (maxhp * 256 / 637)
pa_left         dw                  ; hp + armour - total damage (signed)
st_n            db                  ; select_target: hits in the trace
st_k            db                  ; the one being looked at
st_e            db                  ; its value
tt_list         dsb 8               ; touch_tile: entities on the cell
tt_n            db                  ; how many
tt_k            db                  ; the one being touched
tt_touched      db                  ; 1: stepped on (touch all), 0: only fires
to_j            db                  ; touch: the entity
to_et           db                  ; its type
to_es           db                  ; its sub-type
mv_d            db                  ; act_move: the direction of the step
mv_nx           db                  ; the cell stepped to
mv_nz           db
gi_i            db                  ; game_init: entity counter
actions_ram_end db
.ENDE

; the four facings: north, east, south, west
dir_dx:
    .db 0, 1, 0, -1
dir_dz:
    .db -1, 0, 1, 0
; event flag bits: facing (high byte of the 32-bit flags), leaving and
; arriving (low byte; by the direction of the step)
; (facing: bits 28, 29, 31, 30 for N, E, S, W; leaving a cell going
; N, E, S, W: bits 4..7; arriving going N, E, S, W: bits 2, 3, 0, 1)
facing_flag:
    .db $10, $20, $80, $40
leave_flag:
    .db 16, 32, 64, 128
arrive_flag:
    .db 4, 8, 1, 2

; a = facing -> re_flags = its facing bit | EVF_MOVE | the low byte in c
; Clobbers a, hl.
facing_move_flags:
    ld hl, facing_flag
    call add_hl_a_read
    ld (re_flags+3), a
    xor a
    ld (re_flags+2), a
    ld a, >EVF_MOVE
    ld (re_flags+1), a
    ld a, c
    ld (re_flags), a
    ret

;-----------------------------------------------------------------------
; what Use / attack aims at: the first thing straight ahead (up to 8 cells)
; -> a = 1 (NZ) with sel_kind / sel_idx; sel_locked = 1 if a door that is locked
; was in the way
; A MASK_SHOT trace from the player 8 cells ahead (clamped to the map).
; The first hit decides, skipping entities on the player's own cell:
;   a monster: T_ENT
;   a destructible: T_ENT if bit S_WEAPON of its parm is set (the
;     weapons that can break it), else nothing
;   a fire: T_ENT with the extinguisher (weapon 1) only
;   a breakable door (kind 12): T_LINE with the axe (weapon 0) only
;   a door (kind 0): a monster standing on the door's cell further along
;     the trace, as T_ENT; else nothing (sel_locked set if locked)
;   anything else (a person, a wall, a prop, other entities): nothing
; Clobbers every register except IX, IY.
;-----------------------------------------------------------------------
select_target:
    xor a
    ld (sel_locked), a
    ld a, (S_FACING)
    and 3
    ld c, a
    ld hl, dir_dx
    call add_hl_a_read
    add a, a
    add a, a
    add a, a                    ; x step * 8
    ld hl, S_PX
    add a, (hl)
    call clamp31
    ld d, a                     ; d = end x
    ld a, c
    ld hl, dir_dz
    call add_hl_a_read
    add a, a
    add a, a
    add a, a                    ; z step * 8
    ld hl, S_PZ
    add a, (hl)
    call clamp31
    ld e, a                     ; e = end z
    ld a, (S_PX)
    ld b, a
    ld a, (S_PZ)
    ld c, a
    ld hl, MASK_SHOT
    ld a, $FF
    call trace                  ; what a shot would meet
    ld (st_n), a
    xor a
    ld (st_k), a
_st_k:
    ld a, (st_k)
    ld hl, st_n
    cp (hl)
    jp nc, _st_no
    ld hl, hit_kind
    call add_hl_a_read
    cp H_LINE
    jp z, _st_line
    cp H_ENT
    jp nz, _st_no               ; a wall, the player or a prop
    ld a, (st_k)
    ld hl, hit_val
    call add_hl_a_read
    ld (st_e), a
    ; not something on the player's own cell
    ld c, a
    ld b, 0
    ld hl, S_E_X
    add hl, bc
    ld a, (S_PX)
    cp (hl)
    jr nz, +
    ld hl, S_E_Z
    add hl, bc
    ld a, (S_PZ)
    cp (hl)
    jr nz, +
    ld hl, st_k
    inc (hl)
    jr _st_k
+:  ld a, c
    call etype                  ; what is it?
    cp ET_HUMAN
    jp z, _st_no                ; people are not attacked
    ld b, a
    ld a, T_ENT
    ld (sel_kind), a
    ld a, c
    ld (sel_idx), a
    ld a, b
    cp ET_DESTRUCT
    jr nz, +
    ; a destructible: only some weapons (bit w of its parm)
    ld hl, S_E_DEF
    ld a, c
    call add_hl_a_read
    call def_parm               ; dehl
    ld (m32a), hl               ; (m32a as a scratch copy of the 32-bit mask)
    ld (m32a+2), de
    ld a, (S_WEAPON)
    ld c, a
    rrca
    rrca
    rrca
    and 3                       ; the byte: weapon / 8
    ld hl, m32a
    call add_hl_a
    ld a, c
    and 7
    push hl
    ld hl, bit_mask
    call add_hl_a_read          ; the bit: 1 << (weapon & 7)
    pop hl
    and (hl)
    jp z, _st_no                ; this weapon can't break it
    jp _st_yes
+:  cp ET_FIRE
    jr nz, +
    ld a, (S_WEAPON)            ; a fire: the extinguisher
    cp 1
    jp z, _st_yes
    jp _st_no
+:  cp ET_ENEMY
    jp z, _st_yes
    jp _st_no
_st_line:
    ld a, (st_k)
    ld hl, hit_val
    call add_hl_a_read
    ld (st_e), a
    LTAB LT_LINE_ETYPE
    call add_hl_a_read          ; the door's kind
    cp ET_DESTRUCT
    jr nz, +
    ld a, (S_WEAPON)            ; a breakable door: the axe
    or a
    jp nz, _st_no
    ld a, T_LINE
    ld (sel_kind), a
    ld a, (st_e)
    ld (sel_idx), a
    jp _st_yes
+:  or a                        ; ET_LINE: a door
    jp nz, _st_no
    ld a, (st_e)
    ld hl, S_L_FLAGS
    call add_hl_a_read
    and L_LOCKED
    jr z, +
    ld a, 1
    ld (sel_locked), a
+:  ; a monster standing in the doorway, further along the trace
    ld a, (st_k)
    ld c, a
-:  inc c
    ld a, (st_n)
    cp c
    jr z, _st_no
    jr c, _st_no
    ld a, c
    ld hl, hit_kind
    call add_hl_a_read
    cp H_ENT
    jr nz, -
    ld a, c
    ld hl, hit_val
    call add_hl_a_read
    ld b, a
    push bc
    call etype
    pop bc
    cp ET_ENEMY
    jr nz, -
    ld hl, S_E_X                ; a monster: is it on the door's cell?
    ld a, b
    call add_hl_a_read
    ld d, a
    LTAB LT_LINE_X
    ld a, (st_e)
    call add_hl_a_read
    cp d
    jr nz, -
    ld hl, S_E_Z
    ld a, b
    call add_hl_a_read
    ld d, a
    LTAB LT_LINE_Z
    ld a, (st_e)
    call add_hl_a_read
    cp d
    jr nz, -
    ld a, T_ENT
    ld (sel_kind), a
    ld a, b
    ld (sel_idx), a
_st_yes:
    ld a, 1
    or a
    ret
_st_no:
    xor a
    ret

; a (signed) -> a clamped to 0 .. 31; changes only a and the flags
clamp31:
    bit 7, a
    jr z, +
    xor a
    ret
+:  cp 32
    ret c
    ld a, 31
    ret

; a = tiles -> a = 1 (NZ) if a shot at a non-monster target works
; One calc_hit roll (the target's "agility" being 80% of the player's
; accuracy); a hit then works only with the right tool: a door needs the
; axe, a fire the extinguisher, anything else any weapon but the
; extinguisher. Clobbers every register except IX, IY.
calc_hit_object:
    ld e, a
    ld a, (S_ACCURACY)          ; the target's "agility": accuracy * 204 >> 8
    ld h, a
    push de
    ld e, 204
    call mul8
    pop de
    ld d, h
    ld a, (S_ACCURACY)
    ld b, a
    ld a, (S_WEAPON)
    ld c, a
    call calc_hit               ; one random number
    or a
    ret z                       ; missed
    ld a, (sel_kind)
    cp T_LINE
    jr nz, +
    ld a, (S_WEAPON)            ; a door: only the axe
    or a
    jr ++
+:  ld a, (sel_idx)
    call etype                  ; the target's type
    cp ET_FIRE
    ld a, (S_WEAPON)
    jr nz, +
    cp 1                        ; a fire: only the extinguisher
++: ld a, 0
    ret nz
    inc a
    ret
+:  cp 1                        ; others: anything but the extinguisher
    ld a, 0
    ret z
    inc a
    ret

; a = line: a breakable door is broken open
; (L_ALIVE cleared, L_OPEN set), TX_DOOR_CLEARED, 1 XP, then the door
; cell's event runs as if used (EVF_USE). Clobbers every register except
; IX, IY.
line_destroyed:
    ld c, a
    ld hl, S_L_FLAGS
    call add_hl_a
    ld a, (hl)
    and $FF - L_ALIVE
    or L_OPEN
    ld (hl), a
    push bc
    ld hl, (TX_DOOR_CLEARED)
    call say
    ld a, SFX_BREAK
    call sfx
    ld hl, 1
    call add_xp
    pop bc
    LTAB LT_LINE_X
    ld a, c
    call add_hl_a_read
    ld b, a                     ; the door's cell
    LTAB LT_LINE_Z
    ld a, c
    call add_hl_a_read
    ld c, a
    ld hl, EVF_USE
    call flags_lo               ; re_flags = EVF_USE
    jp run_tile                 ; its event

;-----------------------------------------------------------------------
; the player attacks the selected target -> a = 1 (NZ) if the turn was
; used (0: not enough ammo)
; In: sel_kind / sel_idx (select_target). Takes the ammo (wpn_use of
; type wpn_ammo), plays the weapon's sound, then fires wpn_shots shots.
; At a monster: calc_hit (range = |dx| + |dz|) and calc_damage per hit,
; a crit only on the first shot; n TX_DAMAGE (or TX_MISSED /
; TX_NO_EFFECT); monster_pain, then its death or hurt sound. A blow from the
; next cell that leaves hp + armour at or below -maxhp * 0.4 blows it
; apart (no corpse). At anything else: calc_hit_object per shot, and if
; the last shot worked the door breaks or the thing is destroyed.
; Clobbers every register except IX, IY.
;-----------------------------------------------------------------------
player_attack:
    ld a, (S_WEAPON)
    ld (pa_w), a
    ld a, (sel_idx)
    ld (pa_j), a
    ; ammo
    ld hl, wpn_use
    ld a, (pa_w)
    call add_hl_a_read
    ld b, a                     ; b = ammo per attack
    ld a, (cheats)
    and CHEAT_AMMO
    jr z, +
    ld b, 0                     ; (the infinite ammunition cheat: none)
+:
    ld hl, wpn_ammo
    ld a, (pa_w)
    call add_hl_a_read
    ld hl, S_AMMO
    call add_hl_a               ; hl = &ammo[its type]
    ld a, b
    or a
    jr z, +                     ; uses none
    ld a, (hl)
    cp b
    jr nc, +
    ld hl, (TX_NO_AMMO)
    call say
    ld a, SFX_DENIED
    call sfx
    xor a
    ret
+:  ld a, (hl)
    sub b
    ld (hl), a
    ld hl, wpn_sfx
    ld a, (pa_w)
    call add_hl_a_read
    call sfx                    ; the weapon's sound
    ; tiles away (row + column)
    ld a, (sel_kind)
    cp T_LINE
    jr nz, +
    LTAB LT_LINE_X
    ld a, (pa_j)
    call add_hl_a_read
    ld d, a
    LTAB LT_LINE_Z
    ld a, (pa_j)
    call add_hl_a_read
    ld e, a
    jr ++
+:  ld hl, S_E_X
    ld a, (pa_j)
    call add_hl_a_read
    ld d, a
    ld hl, S_E_Z
    ld a, (pa_j)
    call add_hl_a_read
    ld e, a
++: ld a, (S_PX)
    sub d
    call abs8
    ld d, a
    ld a, (S_PZ)
    sub e
    call abs8
    add a, d
    ld (pa_tiles), a
    ld hl, wpn_shots
    ld a, (pa_w)
    call add_hl_a_read
    ld (pa_shots), a
    ; a monster?
    xor a
    ld (pa_enemy), a
    ld (pa_hit), a
    ld (pa_crit), a
    ld (pa_s), a
    ld hl, 0
    ld (pa_tot), hl
    ld (pa_tarm), hl
    ld a, (sel_kind)
    cp T_ENT
    jr nz, +
    ld a, (pa_j)
    call etype
    cp ET_ENEMY
    jr nz, +
    ld a, 1
    ld (pa_enemy), a
    ld a, (pa_j)
    call cb_monster             ; cb_* = the target's stats
+:
_pa_shot:                       ; for each shot pa_s = 0 .. pa_shots-1
    ld a, (pa_shots)
    ld hl, pa_s
    cp (hl)
    jp z, _pa_done
    jp c, _pa_done
    ld a, (pa_enemy)
    or a
    jr nz, +
    ld a, (pa_tiles)
    call calc_hit_object        ; not a monster: does this shot work?
    ld (pa_hit), a
    jr _pa_next
+:  ld a, (S_ACCURACY)
    ld b, a
    ld a, (pa_w)
    ld c, a
    ld a, (cb_agi)
    ld d, a
    ld a, (pa_tiles)
    ld e, a
    call calc_hit               ; at a monster
    ld (pa_hit), a
    or a
    jr z, _pa_next              ; missed
    cp HIT_CRIT
    jr nz, +
    ld a, (pa_s)
    or a
    jr nz, +
    ld a, 1
    ld (pa_crit), a
+:  ld a, (S_STRENGTH)
    ld (cd_str), a
    ld a, (pa_w)
    ld (cd_w), a
    ld a, (cb_def)
    ld (cd_def), a
    ld a, (cb_mclass)
    ld (cd_class), a
    ld a, (pa_crit)
    ld (cd_crit), a
    ld a, (pa_tiles)
    ld (cd_tiles), a
    call calc_damage            ; dmg_hp, dmg_arm for this shot
    ld hl, (pa_tot)
    ld de, (dmg_hp)
    add hl, de
    ld (pa_tot), hl
    ld hl, (pa_tarm)
    ld de, (dmg_arm)
    add hl, de
    ld (pa_tarm), hl
_pa_next:
    ld hl, pa_s
    inc (hl)
    jp _pa_shot
_pa_done:
    ld a, (pa_enemy)
    or a
    jp z, _pa_object
    ld hl, (pa_tot)
    ld de, (pa_tarm)
    add hl, de
    ld a, h
    or l
    jr nz, +
    ld hl, (TX_MISSED)           ; (the extinguisher: no effect)
    ld a, (pa_w)
    cp 1
    jr nz, ++
    ld hl, (TX_NO_EFFECT)
++: call say
    jp _pa_true
+:  ; left = hp + armour - damage; thr = maxhp * 256 / 637 (a blow past
    ; -thr from next to it leaves no corpse)
    push hl
    ld a, (pa_j)
    call ent_mon
    ld c, a
    ld b, 0
    ld hl, S_M_HP
    add hl, bc
    ld e, (hl)
    ld d, 0
    ld hl, S_M_ARMOR
    add hl, bc
    ld a, (hl)
    ex de, hl
    call add_hl_a
    pop de
    push de
    or a
    sbc hl, de
    ld (pa_left), hl
    ld hl, S_M_MAXHP
    add hl, bc
    ld h, (hl)
    ld l, 0
    ld bc, 637
    call div16                  ; thr = maxhp * 256 / 637
    ld (pa_thr), hl
    pop de                      ; tot + tarm
    ld hl, str_critical
    ld a, (pa_crit)
    or a
    jr nz, +
    ld hl, str_empty
+:  ld bc, (TX_DAMAGE)
    call say_num                ; "[CRITICAL! ]n DAMAGE!"
    ld hl, (pa_tot)
    ld de, (pa_tarm)
    ld a, (pa_j)
    call monster_pain
    ; overkill: tiles == 1 and left <= -thr
    ld e, 0
    ld a, (pa_tiles)
    cp 1
    jr nz, +
    ld hl, (pa_left)
    ld de, (pa_thr)
    add hl, de                  ; left + thr <= 0
    ld e, 0
    bit 7, h
    jr nz, ++
    ld a, h
    or l
    jr nz, +
++: ld e, 1
+:  ld a, (pa_j)
    call monster_hurt_or_died_e ; e = 1: no corpse
    jp _pa_true
_pa_object:                     ; (only the last shot's result counts)
    ld a, (pa_hit)
    or a
    jr z, +
    ld a, (sel_kind)
    cp T_LINE
    ld a, (pa_j)
    jr nz, ++
    call line_destroyed         ; the door breaks open
    jp _pa_true
++: call destroy                ; put out, blown up, broken open
    jp _pa_true
+:  ld hl, (TX_MISSED)
    ld a, (pa_w)
    cp 1
    jr nz, +
    ld hl, (TX_NO_EFFECT)
+:  call say
_pa_true:
    ld a, 1
    or a
    ret

;-----------------------------------------------------------------------
; picking things up, fires underfoot
;-----------------------------------------------------------------------
; a = entity on the player's cell
; By type (the parm is the def's amount):
;   ET_PICKUP sub-type 20 health, 21 armour (refused at the maximum),
;     22 / 23 credits, 24 a key (key number = parm & 7); others are
;     simply taken. "GOT <name>" and the item leaves the map.
;   ET_INV: one more of carried item (sub-type - 25: small medkit, large
;     medkit, soulsphere), at most 99
;   ET_AMMO / ET_AMMO2: parm rounds of ammo type (sub-type), refused if
;     full
;   ET_WEAPON: the weapon (sub-type) is added; a new one is selected;
;     its ammo gets parm rounds
;   ET_FIRE: 1 damage and 2 armour damage; ET_FIRE2: 10 and 10 (the fire
;     stays)
; Clobbers every register except IX, IY.
touch:
    ld (to_j), a
    call etype
    ld (to_et), a
    ld a, (to_j)
    call esub
    ld (to_es), a
    ld a, (to_et)
    cp ET_PICKUP
    jp nz, _to_inv
    ld a, (to_es)
    cp 22
    jr z, +
    cp 23
    jr nz, ++
+:  call to_parm                ; credits
    ex de, hl
    ld hl, (S_CREDITS)
    add hl, de
    ld (S_CREDITS), hl
    jr _to_got
++: cp 21
    jr nz, +
    ld a, (S_MAXARMOR)          ; armour
    ld b, a
    ld a, (S_ARMOR)
    cp b
    ld hl, (TX_ARMOR_MAX)
    jp nc, say                  ; full: left where it is
    call to_parm
    call add_armor              ; + parm, "GAINED n ARMOR"
    jr _to_got
+:  cp 20
    jr nz, +
    ld a, (S_MAXHP)             ; health
    ld b, a
    ld a, (S_HP)
    cp b
    ld hl, (TX_HEALTH_MAX)
    jp nc, say                  ; full: left where it is
    call to_parm
    call add_health             ; + parm, "GAINED n HEALTH"
    jr _to_got
+:  cp 24
    jr nz, _to_got
    call to_parm                ; a key
    ld a, l
    and 7
    ld hl, bit_mask
    call add_hl_a_read          ; a = 1 << key
    ld hl, S_KEYS
    or (hl)
    ld (hl), a
_to_got:                        ; the sound for what was taken
    ld a, (to_es)
    ld c, SFX_KEY
    cp 24
    jr z, +
    ld c, SFX_CREDITS
    cp 22
    jr z, +
    cp 23
    jr z, +
    ld c, SFX_PICKUP
+:  ld a, c
    call sfx
_to_say_got:
    ld a, (to_j)
    call ent_name
    ex de, hl
    ld hl, (TX_GOT)
    call say2                   ; "GOT <name>"
    ld a, (to_j)
    jp ent_unlink               ; taken: off the map
_to_inv:
    cp ET_INV
    jr nz, _to_ammo
    ld a, (to_es)
    sub 25                      ; inventory slot 0..2
    ld hl, S_INVENTORY
    call add_hl_a
    ld a, (hl)
    cp 99
    jr nz, +
    ld a, (to_j)
    call ent_name
    ex de, hl
    ld hl, str_cant_carry
    jp say2                     ; "CAN'T CARRY MORE <name>"
+:  inc (hl)
    ld a, SFX_PICKUP
    call sfx
    jr _to_say_got
_to_ammo:
    cp ET_AMMO
    jr z, +
    cp ET_AMMO2
    jr nz, _to_weapon
+:  call to_parm
    ld a, (to_es)
    ld c, a                     ; c = the ammo type
    ld a, l
    call add_ammo               ; + parm rounds
    jr nz, +
    ld hl, (TX_AMMO_MAX)
    jp say                      ; full: left where it is
+:  ld a, SFX_PICKUP
    call sfx
    jr _to_say_got
_to_weapon:
    cp ET_WEAPON
    jr nz, _to_fire
    ld a, (to_es)               ; new to us: switch to it
    call weapon_bit             ; de = its bit
    ld a, (S_WEAPONS)
    and e
    ld e, a
    ld a, (S_WEAPONS+1)
    and d
    or e
    jr nz, +                    ; already carried
    ld a, (to_es)
    ld (S_WEAPON), a
+:  ld a, (to_es)
    call weapon_bit
    ld hl, (S_WEAPONS)          ; S_WEAPONS |= its bit
    ld a, l
    or e
    ld l, a
    ld a, h
    or d
    ld h, a
    ld (S_WEAPONS), hl
    ld hl, wpn_use
    ld a, (to_es)
    call add_hl_a_read
    or a
    jr z, +
    ld hl, wpn_ammo
    ld a, (to_es)
    call add_hl_a_read
    ld c, a
    push bc
    call to_parm
    pop bc
    ld a, l
    call add_ammo               ; + parm rounds of its ammo
+:  ld a, SFX_WEAPON
    call sfx
    jp _to_say_got
_to_fire:
    cp ET_FIRE
    jr nz, +
    ld hl, 1
    ld de, 2
    call player_pain            ; 1 damage, 2 armour damage
    ld hl, (TX_BURNS)
    jp say
+:  cp ET_FIRE2
    ret nz                      ; anything else: nothing happens
    ld hl, 10
    ld de, 10
    call player_pain            ; 10 and 10
    ld hl, (TX_BURNS_MORE)
    jp say

; -> hl = the low word of the touched entity's parm (de = the high word);
; keeps bc
to_parm:
    ld a, (to_j)
    ld hl, S_E_DEF
    call add_hl_a_read
    jp def_parm

; a = weapon (0..15) -> de = 1 << a; clobbers b
weapon_bit:
    ld de, 1
    or a
    ret z
    ld b, a
-:  sla e
    rl d
    djnz -
    ret

; a = 1 if the player stepped onto the cell (everything there is
; touched), 0 if they stayed (only fires)
; The list of linked entities on the player's cell (up to 8) is taken
; first, because touching changes the cell's list. Clobbers every
; register except IX, IY.
touch_tile:
    ld (tt_touched), a
    xor a
    ld (tt_n), a
    ld a, (S_PX)
    ld b, a
    ld a, (S_PZ)
    ld c, a
    ld e, 0
-:  call ent_at                 ; the next linked entity >= e
    cp $FF
    jr z, +
    ld d, a
    ld hl, tt_n
    ld a, (hl)
    inc (hl)
    ld hl, tt_list
    call add_hl_a
    ld (hl), d
    ld a, d
    inc a
    ld e, a
    ld a, (tt_n)
    cp 8
    jr c, -
+:  xor a
    ld (tt_k), a
-:  ld a, (tt_k)
    ld hl, tt_n
    cp (hl)
    ret nc
    ld hl, tt_list
    call add_hl_a_read
    ld c, a
    push bc
    call etype
    pop bc
    ld b, a
    ld a, (tt_touched)
    or a
    jr nz, +                    ; stepped on: everything
    ld a, b
    cp ET_FIRE
    jr z, +
    cp ET_FIRE2
    jr nz, ++                   ; stayed: only fires
+:  ld a, c
    call touch
++: ld hl, tt_k
    inc (hl)
    jr -

;-----------------------------------------------------------------------
; the actions (game/ui.asm calls these)
;-----------------------------------------------------------------------
; a = -1 or 1: a quarter turn (free: the monsters don't move)
; The cell's event runs with EVF_MOVE and the new facing; monsters that
; now see the player wake. Clobbers every register except IX, IY.
act_turn:
    ld hl, S_FACING
    add a, (hl)
    and 3
    ld (hl), a
    ld c, 0                     ; no arrive / leave bits
    call facing_move_flags
    call player_cell
    call run_tile               ; the cell's event
    jp activate_visible

; -> b = x, c = z of the player; clobbers a
player_cell:
    ld a, (S_PX)
    ld b, a
    ld a, (S_PZ)
    ld c, a
    ret

; a = 1 to step back -> a = 1 (NZ) if the player moved
; (a = 0: forward.) A MASK_PLAYER_MOVE trace to the next cell; blocked:
; a bump sound and nothing else (no turn used). Otherwise: the old cell's
; event (leaving), the move, the new cell's event (arriving, facing),
; pick up everything there, and the monsters' turn. Clobbers every
; register except IX, IY.
act_move:
    ld b, a
    ld a, (S_FACING)
    and 3
    dec b
    jr nz, +
    add a, 2                    ; back: the opposite direction
    and 3
+:  ld (mv_d), a
    ld hl, dir_dx
    call add_hl_a_read
    ld hl, S_PX
    add a, (hl)
    ld (mv_nx), a
    ld d, a
    ld a, (mv_d)
    ld hl, dir_dz
    call add_hl_a_read
    ld hl, S_PZ
    add a, (hl)
    ld (mv_nz), a
    ld e, a
    call player_cell
    ld hl, MASK_PLAYER_MOVE
    ld a, $FF
    call trace                  ; anything in the way?
    jr z, +
    ld a, SFX_BUMP
    call sfx
    xor a
    ret
+:  ld a, SFX_STEP
    call sfx
    ld a, (mv_d)                ; leaving
    ld hl, leave_flag
    call add_hl_a_read
    ld l, a
    ld h, >EVF_MOVE
    call flags_lo               ; re_flags = EVF_MOVE | the leave bit
    call player_cell
    call run_tile               ; the old cell's event
    ld a, (mv_nx)
    ld (S_PX), a
    ld a, (mv_nz)
    ld (S_PZ), a
    ld a, (mv_d)                ; arriving (and facing)
    ld hl, arrive_flag
    call add_hl_a_read
    ld c, a
    ld a, (S_FACING)
    and 3
    call facing_move_flags      ; re_flags = EVF_MOVE | facing | the arrive bit
    call player_cell
    call run_tile               ; the new cell's event
    ld a, 1
    call touch_tile             ; pick up what is there
    call end_player_turn        ; the monsters' turn
    ld a, 1
    or a
    ret

; Use: the cell ahead's event, or an attack on what is ahead
; If the cell ahead has an event that runs a command (flags EVF_USE |
; EVF_MOVE): unless it opened a dialog or door code (skip_turn), fires
; underfoot burn and the monsters take their turn. Otherwise the target
; from select_target is attacked (a turn, unless out of ammo); with no
; target: TX_NOTHING or "THE DOOR IS LOCKED". Clobbers every
; register except IX, IY.
act_use:
    xor a
    ld (skip_turn), a
    ld a, (S_FACING)
    and 3
    ld c, a
    ld hl, dir_dx
    call add_hl_a_read
    ld hl, S_PX
    add a, (hl)
    ld b, a
    ld a, c
    ld hl, dir_dz
    call add_hl_a_read
    ld hl, S_PZ
    add a, (hl)
    ld c, a
    ld hl, $0500                ; EVF_USE | EVF_MOVE... as the original: $500
    call flags_lo
    call run_tile               ; the cell ahead's event
    jr z, +                     ; nothing ran: attack instead
    ld a, (skip_turn)
    or a
    ret nz                      ; a box opened: no monster turn now
    xor a
    call touch_tile             ; fires underfoot
    jp end_player_turn
+:  call select_target
    jr nz, +
    ld a, (sel_locked)
    or a
    ld hl, (TX_NOTHING)
    ld c, SFX_BLIP
    jr z, ++
    ld hl, str_door_locked
    ld c, SFX_DENIED
++: push bc
    call say
    pop bc
    ld a, c
    jp sfx
+:  call player_attack
    ret z                       ; out of ammo: no turn used
    xor a
    call touch_tile             ; fires underfoot
    jp end_player_turn

; Button 2 + Down: TX_TURN_PASSED, fires underfoot, the monsters' turn.
; Clobbers every register except IX, IY.
act_pass:
    ld hl, (TX_TURN_PASSED)
    call say
    xor a
    call touch_tile
    jp end_player_turn

; a = -1 or 1: the next weapon that way that the player has (with ammo)
; Looks at the player's weapons 0..11 (wrapping round) from the current
; one in that direction; the first carried one that uses no ammo or has
; some left is selected (free: no turn). Nothing if none. Clobbers every
; register except IX, IY.
act_cycle_weapon:
    ld c, a                     ; step
    ld b, 1                     ; k
-:  ld a, (S_WEAPON)            ; w = weapon + step * k, wrapped into 0..11
    ld e, b
    bit 7, c
    jr z, +
    add a, 12
    sub e
    jr ++
+:  add a, e
++: cp 12
    jr c, +
    sub 12
    jr ++
+:  bit 7, a
    jr z, ++
    add a, 12
++: ld e, a                     ; w
    push bc
    push de
    call weapon_bit             ; de = 1 << w
    ld a, (S_WEAPONS)
    and e
    ld e, a
    ld a, (S_WEAPONS+1)
    and d
    or e
    pop de
    pop bc
    jr z, +++                   ; not carried
    ld hl, wpn_use
    ld a, e
    call add_hl_a_read
    or a
    jr z, +                     ; uses no ammo
    ld hl, wpn_ammo
    ld a, e
    call add_hl_a_read
    ld hl, S_AMMO
    call add_hl_a_read
    or a
    jr z, +++                   ; out of ammo
    ; (only checks for any ammo, not for a whole shot's worth)
+:  ld a, e
    ld (S_WEAPON), a
    ld a, SFX_SWITCH
    jp sfx
+++:
    inc b
    ld a, b
    cp 12
    jr c, -
    ret

; Button 2 + Up: a medkit (small, then large, then the soulsphere) ->
; a = 1 (NZ) if one was used
act_health_item:
    ld c, 0
-:  ld hl, S_INVENTORY
    ld a, c
    call add_hl_a_read
    or a
    jr nz, +
    inc c
    ld a, c
    cp 3
    jr c, -
    ld hl, str_no_health
    call say
    ld a, SFX_DENIED
    call sfx
    xor a
    ret
+:  push bc
    ld a, c
    cp 2
    jr nz, +
    ld hl, 200                  ; the soulsphere
    call add_health             ; (+200: up to maxhp)
    ld hl, 200
    call add_armor              ; (+200: up to maxarmour)
    jr ++
+:  or a
    ld a, DD_SMMED
    jr z, +
    ld a, DD_LGMED
+:  LTAB LT_DROP_DEF
    call add_hl_a_read          ; the level's def for that medkit
    call def_parm               ; hl = how much it heals
    call add_health
++: pop bc
    ld hl, S_INVENTORY
    ld a, c
    call add_hl_a
    dec (hl)
    ld a, SFX_HEAL
    call sfx
    call end_player_turn        ; using one takes a turn
    ld a, 1
    or a
    ret

;-----------------------------------------------------------------------
; a fresh game: the level's entities, doors, events and monsters, the
; player at the start
; Order matters for the RNG (the model does the same): the state is
; cleared, the RNG seeded with $1234, then each monster draws 6 random
; numbers (5 in setup_enemy, 1 for F_ATK2) in entity order, then the
; start cell's event runs. Clobbers every register except IX, IY.
; Called by new_game (ui.asm).
;-----------------------------------------------------------------------
game_init:
    call level_load             ; counts and table addresses
    ld hl, STATE_ADDR
    ld de, STATE_ADDR+1
    ld bc, STATE_SIZE-1
    ld (hl), 0
    ldir                        ; the whole game state = 0
    ld hl, cell_head
    ld de, cell_head+1
    ld bc, 1024+MAX_ENTS-1      ; (ent_next follows cell_head)
    ld (hl), 0
    ldir                        ; every cell's list empty
    ld hl, $1234
    ld (S_RNG), hl              ; the RNG's seed
    ; the player's starting stats
    ld a, 30
    ld (S_HP), a
    ld (S_MAXHP), a
    ld a, 20
    ld (S_MAXARMOR), a          ; (armour itself starts at 0)
    ld a, 16
    ld (S_DEFENSE), a
    ld (S_ACCURACY), a
    ld a, 12
    ld (S_STRENGTH), a
    ld a, 14
    ld (S_AGILITY), a
    ld a, 1
    ld (S_LEVEL), a
    ld hl, 80
    ld (S_NEXTXP), hl
    ld hl, 7
    ld (S_WEAPONS), hl          ; weapons 0, 1, 2: axe, fire extinguisher, pistol
    ld a, 2
    ld (S_WEAPON), a            ; holding the pistol
    ld a, 6
    ld (S_AMMO), a              ; ammo type 0 (the extinguisher's): 6
    ld a, 8
    ld (S_AMMO+1), a            ; type 1 (bullets): 8
    LTAB LT_START               ; the UAC Base: the starting kit, the pistol and 8 bullets
    ld a, (hl)                  ; (a nonzero byte in the level's "start" table)
    or a
    jr z, +
    ld hl, 1 << 2
    ld (S_WEAPONS), hl          ; only the pistol
    xor a
    ld (S_AMMO), a              ; no extinguisher charges
+:  ld a, (spawn_x)
    ld (S_PX), a
    ld a, (spawn_z)
    ld (S_PZ), a
    ld a, (spawn_f)
    ld (S_FACING), a
    ld a, $FF
    ld (dialog_str), a          ; no box waiting
    ld (pw_want), a
    xor a
    ld (pend_valid), a
    ld (monsters_pending), a
    ld (att_n), a
    ld (n_pend_expl), a
    ; entities: the map's (linked unless hidden), then empty drop slots
    xor a
    ld (gi_i), a
-:  ld a, (gi_i)
    ld hl, n_ents
    cp (hl)
    jr nc, ++
    ld c, a
    ld hl, n_map_ents
    cp (hl)
    jr c, +
    ld hl, S_E_DEF
    call add_hl_a
    ld (hl), NO_DEF             ; a drop slot: empty
    jr +++
+:  LTAB LT_ENT_DEF0
    call add_hl_a_read          ; the map's def, x, z for entity c
    ld hl, S_E_DEF
    ld b, 0
    add hl, bc
    ld (hl), a
    LTAB LT_ENT_X0
    ld a, c
    call add_hl_a_read
    ld hl, S_E_X
    add hl, bc
    ld (hl), a
    LTAB LT_ENT_Z0
    ld a, c
    call add_hl_a_read
    ld hl, S_E_Z
    add hl, bc
    ld (hl), a
    LTAB LT_ENT_HIDDEN0
    ld a, c
    call add_hl_a_read
    or a
    ld a, c
    call z, ent_link            ; not hidden: on the map
+++:
    ld hl, gi_i
    inc (hl)
    jr -
++: ; doors: locked as the map says; breakable ones alive
    ld c, 0
-:  ld a, c
    ld hl, n_lines
    cp (hl)
    jr nc, ++
    LTAB LT_LINE_LOCKED0
    call add_hl_a_read          ; locked at the start?
    ld b, 0
    or a
    jr z, +
    ld b, L_LOCKED
+:  LTAB LT_LINE_ETYPE
    ld a, c
    call add_hl_a_read          ; the door's kind
    cp ET_DESTRUCT
    jr nz, +
    ld a, b
    or L_ALIVE
    ld b, a
+:  ld hl, S_L_FLAGS
    ld a, c
    call add_hl_a
    ld (hl), b
    inc c
    jr -
++: ; events: their starting states
    ld a, (n_events)
    or a
    jr z, +
    ld c, a
    ld b, 0
    LTAB LT_EV_STATE0
    ld de, S_EV_STATE
    ldir
+:  ; monsters: their class's stats, scaled (setup_enemy); fires and
    ; destructibles alive
    xor a
    ld (gi_i), a
_gi_mon:
    ld a, (gi_i)
    ld hl, n_map_ents
    cp (hl)
    jp nc, _gi_done
    call etype
    cp ET_ENEMY
    jr nz, ++
    ld a, (gi_i)
    call esub                   ; its class
    ld e, a
    add a, a
    add a, e
    add a, a                    ; * 6
    ld hl, mon_base
    call add_hl_a
    push hl
    ld a, (gi_i)
    call ent_mon                ; its stat slot
    ld c, a
    ld b, 0
    pop de                      ; de = this class's six stats
    ld hl, S_M_MAXHP
    call gi_two                 ; maxhp and hp
    ld hl, S_M_MAXARMOR
    call gi_two                 ; maxarmour and armour
    ld hl, S_M_DEF
    call gi_one
    ld hl, S_M_STR
    call gi_one
    ld hl, S_M_AGI
    call gi_one
    ld hl, S_M_ACC
    call gi_one
    push bc
    ld a, (gi_i)
    ld hl, S_E_DEF
    call add_hl_a_read
    call def_parm               ; hl = its def's stat scale
    pop bc
    call setup_enemy            ; c = slot, hl = parm
    call rnd                    ; which attack it starts with
    ld c, a
    ld a, (gi_i)
    ld hl, S_E_FLAGS
    call add_hl_a
    bit 0, c                    ; (rnd & 1)
    jr z, +
    set 4, (hl)                 ; F_ATK2
+:  set 0, (hl)                 ; F_ALIVE
    jr +++
++: cp ET_FIRE
    jr z, +
    cp ET_DESTRUCT
    jr nz, +++
+:  ld a, (gi_i)
    ld hl, S_E_FLAGS
    call add_hl_a
    set 0, (hl)
+++:
    ld hl, gi_i
    inc (hl)
    jp _gi_mon
_gi_done:
    ; the start cell's event: arrival from every side, facing
    ld a, (S_FACING)
    and 3
    ld c, $0F
    call facing_move_flags      ; (EVF_MOVE | 15 | facing)
    call player_cell
    call run_tile               ; the start cell's event
    jp activate_visible         ; wake monsters already in view

; hl = an S_M_* array, bc = slot, de = &stat: array[slot] and the next
; array (hp after maxhp, armour after maxarmor) = stat; de++
gi_two:
    ld a, (de)
    push hl
    add hl, bc
    ld (hl), a
    pop hl
    push de
    ld de, -MAX_MONS            ; (S_M_HP comes before S_M_MAXHP, S_M_ARMOR before S_M_MAXARMOR)
    add hl, de
    pop de
    add hl, bc
    ld (hl), a
    inc de
    ret

; hl = an S_M_* array, bc = slot, de = &stat: array[slot] = stat; de++
; Clobbers a, hl.
gi_one:
    ld a, (de)
    add hl, bc
    ld (hl), a
    inc de
    ret

str_critical:       .db "CRITICAL! ", 0
str_cant_carry:     .db "CAN'T CARRY MORE ", 0
str_door_locked:    .db "THE DOOR IS LOCKED", 0
str_no_health:      .db "NO HEALTH ITEMS", 0

; the scratch blocks must not overlap (game.asm, combat, events, actions, ui, core, math)
; (Assembly-time checks. The trace's block at $D3A0 and math32's at
; $D3B0 are not checked against each other or against the renderer's
; RAM at $D400; they are small and fit.)
.IF combat_ram_end > $D310
.FAIL "combat scratch runs into the events' at $D310"
.ENDIF
.IF events_ram_end > $D340
.FAIL "events scratch runs into the actions' at $D340"
.ENDIF
.IF actions_ram_end > $D370
.FAIL "actions scratch runs into the UI's at $D370"
.ENDIF
.IF ui_ram_end > $D3A0
.FAIL "UI scratch runs into the trace variables at $D3A0"
.ENDIF
.IF shared_end > $D290
.FAIL "the shared block runs into combat scratch at $D290"
.ENDIF



