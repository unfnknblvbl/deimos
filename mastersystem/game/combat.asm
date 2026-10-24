; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

;=======================================================================
; Combat: the combatants, hit and damage rolls, the player's stats,
; monsters (activation, drops, death, pain), their AI and their turn,
; explosions. tools/game_model.py has the same routines (same names).
;
; Bank 1 ($4000-). Called by actions.asm (the player's attacks, pickups,
; game_init), events.asm (pain, stats, kills, settle) and ui.asm. Uses
; core.asm (cells, lists, trace, rnd), math32.asm and ui.asm (say, sfx).
;
; Data:
;   the S_* game state (gen/defs.inc): the player's stats S_HP..S_ACCURACY,
;     each entity's S_E_* bytes, each monster slot's S_M_* stats, the
;     active list S_ACTIVE (monsters that take turns, in waking order)
;   gen/tables.inc (from tools/game_rules.py), indexed by weapon w
;     (0..N_WEAPONS-1; 0..8 the player's, 9.. the monsters' attacks):
;     wpn_min / wpn_max   damage range
;     wpn_rmin / wpn_rmax minimum range for range fall-off / accuracy bonus
;     wpn_ammo / wpn_use  ammo type and amount per shot
;     wpn_armor           the share of damage armour takes (x/256)
;     wpn_shots           shots per attack
;   and by monster class c (the monster's sub-type, 0..13):
;     mon_base[c*6]       base maxhp, maxarmour, def, str, agi, acc
;     mon_atk[c*2]        its two attack weapons
;     mon_shots[c]        shots per attack
;     dmg_mult[c][w]      damage multiplier (8.8: 256 = x1)
;   cb_* (game.asm): one combatant's stats, loaded by cb_player or
;   cb_monster for the formulas.
; Fixed point: "8.8" means a 16-bit value with 256 = 1.0.
; Every rnd call must match the model's order: the comments say how many
; random numbers each routine draws.
;=======================================================================

.ENUM $D290                         ; scratch (after the shared block; actions.asm checks the overlaps)
ch_w            db                  ; calc_hit
ch_tiles        db
ch_ch           dw
ch_dec          dw
ch_rmin         db
ch_ex           dw
cd_str          db                  ; calc_damage: its inputs
cd_w            db
cd_def          db
cd_class        db                  ; $FF = the player
cd_crit         db
cd_tiles        db
cd_r            db
cd_d            dsb 4
cd_ex           dw
cd_k            db
cd_dist         dw
se_f            dw                  ; setup_enemy: the current factor
se_parm         dw
se_i2           dw
se_i3           dw
se_m            db
pp_dmg          dw                  ; player_pain
pp_arm          dw
pp_a            db
pp_h1           dw
pp_h2           dw
mp_m            db                  ; monster_pain
vis_dx          db                  ; visible
vis_dz          db
vis_steps       db
vis_s           db
sd_i            db                  ; spawn_drop
sd_es           db
sd_n5           db
sd_n6           db
sd_dt           db
cp_i            db                  ; calc_path
cp_sx           db
cp_sz           db
cp_dx           db
cp_dz           db
cp_mask         dw
cp_k            db
cp_d            db
cp_best         dw
cp_stx          db
cp_stz          db
am_i            db                  ; ai_move
am_dx           db
am_dz           db
am_sx           db
am_sz           db
am_d            db
am_n            db
am_best         dw
am_pd           dw
am_mask         dw
am_order        dsb 4
at_i            db                  ; ai_think
at_ex           db
at_ez           db
at_w            db
at_reach        db
at_half         db
at_aligned      db
at_canhit       db
at_kind         db
at_tgt          db
at_tx           db
at_tz           db
at_d2           dw
at_j            db
as_i            db                  ; ai_after_step
as_steps        db
as_lunge        db
as_adj          db
ha_x            db                  ; hurt_at
ha_z            db
ha_dmg          db
ha_arm          db
ha_j            db
ds_j            db                  ; destroy
ds_et           db
ds_es           db
ds_dt           db
ma_i            db                  ; monster_attack
ma_kind         db
ma_tgt          db
ma_w            db
ma_shots        db
ma_s            db
ma_crit         db
ma_tot          dw
ma_tarm         dw
mt_nm           db                  ; monster_turn
mt_na           db
mt_k            db
mt_n            db
combat_ram_end  db
.ENDE

;-----------------------------------------------------------------------
; combatants: cb_* = the player or a monster
;-----------------------------------------------------------------------
; cb_* = the player's stats (S_HP .. S_ACCURACY), cb_mclass = $FF;
; clobbers a, bc, de, hl. (Nothing calls it at present: the player's
; side of a fight reads S_* directly.)
cb_player:
    ld hl, S_HP                 ; hp, maxhp, armor, maxarmor, defense, strength, agility, accuracy
    ld de, cb_hp
    ld bc, 8
    ldir
    ld a, $FF
    ld (cb_mclass), a
    ret

; a = monster entity: cb_* = its slot's stats, cb_mclass = its class
; (sub-type); clobbers every register except IX, IY
cb_monster:
    push af
    call ent_mon                ; a = its monster slot
    ld hl, S_M_HP               ; m_hp, m_maxhp, ... m_acc: arrays of MAX_MONS, same order
    call add_hl_a
    ld de, cb_hp
    ld b, 8
    ld c, MAX_MONS
-:  ld a, (hl)                  ; one byte from each of the 8 arrays
    ld (de), a
    inc de
    ld a, l
    add a, c                    ; the next array: MAX_MONS bytes on
    ld l, a
    jr nc, +
    inc h
+:  djnz -
    pop af
    call esub
    ld (cb_mclass), a
    ret

; a = monster entity -> a = the XP it is worth:
; ((def + str + maxhp + maxarmor) * 5 + (agi + acc) * 3 + 49) / 50
; (Rounded up.) Loads cb_* (cb_monster); clobbers every register except
; IX, IY.
cb_exp:
    call cb_monster
    ld hl, 0
    ld a, (cb_def)
    call add_hl_a
    ld a, (cb_str)
    call add_hl_a
    ld a, (cb_maxhp)
    call add_hl_a
    ld a, (cb_maxarmor)
    call add_hl_a               ; hl = def + str + maxhp + maxarmor
    ld d, h
    ld e, l
    add hl, hl
    add hl, hl
    add hl, de                  ; * 5
    push hl
    ld hl, 0
    ld a, (cb_agi)
    call add_hl_a
    ld a, (cb_acc)
    call add_hl_a               ; hl = agi + acc
    ld d, h
    ld e, l
    add hl, hl
    add hl, de                  ; * 3
    pop de
    add hl, de
    ld de, 49
    add hl, de
    ld bc, 50
    call div16                  ; / 50
    ld a, l
    ret

; a = tiles, e = minimum range -> hl = tiles^2 - rmin^2 - 1 (signed)
; How far past its minimum range a target is ("range excess"); > 0 means
; the fall-off applies. Clobbers a, de.
range_excess:
    push de
    ld h, a
    ld e, a
    call mul8                   ; tiles^2
    pop de
    push hl
    ld h, e
    call mul8                   ; rmin^2
    ex de, hl
    pop hl
    or a
    sbc hl, de
    dec hl
    ret

;-----------------------------------------------------------------------
; calc_hit: b = attacker's accuracy, c = weapon, d = target's agility,
; e = tiles away (0 = not a ranged check) -> a = HIT_MISS/HIT/CRIT.
; Draws one random number, unless a melee weapon can't reach.
;
; The chance ch is out of 256: ((acc * 256 / agi) / 2) + rmax * 32 / 25.
; Past the weapon's minimum range (rmin, "range excess" ex > 0) a melee
; weapon (rmin = 0) misses outright; other weapons, except shell weapons,
; lose ch * 76 / 256 (about 30%) for each square k^2 (1, 4, 9, ...) below
; ex. Then r = rnd: a hit if r < ch, a critical if also r < ch / 20.
; Clobbers every register except IX, IY.
;-----------------------------------------------------------------------
calc_hit:
    ld a, c
    ld (ch_w), a
    ld a, e
    ld (ch_tiles), a
    ; ch = ((acc << 8) / agi) >> 1 + rmax * 32 / 25
    ld a, d
    or a
    jr nz, +
    inc a                       ; (agility 0 counts as 1)
+:  ld c, a
    ld h, b
    ld l, 0                     ; hl = acc << 8
    ld b, 0
    call div16                  ; / agi
    srl h
    rr l                        ; >> 1
    push hl
    ld hl, wpn_rmax
    ld a, (ch_w)
    call add_hl_a_read          ; the weapon's accuracy bonus
    ld l, a
    ld h, 0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl                  ; * 32
    ld bc, 25
    call div16                  ; / 25
    pop de
    add hl, de
    ld (ch_ch), hl
    ; ex = range excess (-1 without tiles)
    ld hl, wpn_rmin
    ld a, (ch_w)
    call add_hl_a_read
    ld (ch_rmin), a
    ld e, a
    ld hl, -1
    ld a, (ch_tiles)
    or a
    call nz, range_excess       ; hl = tiles^2 - rmin^2 - 1
    ld (ch_ex), hl
    ; ex > 0?
    bit 7, h
    jr nz, _chit_roll           ; negative: in range
    ld a, h
    or l
    jr z, _chit_roll            ; 0: in range
    ld a, (ch_rmin)             ; a melee weapon that can't reach: no roll
    or a
    jr nz, +
    xor a                       ; HIT_MISS
    ret
+:  ld hl, wpn_ammo             ; shells (ammo 2) don't fall off with range
    ld a, (ch_w)
    call add_hl_a_read
    cp 2
    jr z, _chit_roll
    ; dec = ch * 76 >> 8 (ch > 0 here); ch -= dec until k^2 reaches ex
    ld hl, (ch_ch)
    ld a, 76
    call mul_hl_a               ; a:hl = ch * 76
    ld l, h
    ld h, a                     ; >> 8
    ld (ch_dec), hl
    ld c, 2                     ; k
    ld de, 1                    ; d
-:  ld hl, (ch_ex)              ; while d < ex (signed)
    or a
    sbc hl, de                  ; ex - d
    jr z, _chit_roll
    jp m, _chit_roll            ; (|values| < $4000: no overflow)
    ld hl, (ch_ch)
    ld de, (ch_dec)
    or a
    sbc hl, de
    ld (ch_ch), hl              ; ch -= dec (it can go negative)
    push bc
    ld h, c
    ld e, c
    call mul8                   ; d = k * k
    pop bc
    ex de, hl
    inc c
    jr -
_chit_roll:
    call rnd
    ld e, a                     ; r
    ld hl, (ch_ch)
    bit 7, h
    jr nz, ++                   ; ch < 0: a miss
    ld a, h
    or a
    jr nz, +                    ; ch >= 256 > r
    ld a, e
    cp l
    jr nc, ++                   ; r >= ch
+:  push de                     ; r < ch: a crit if r < ch / 20
    ld bc, 20
    call div16                  ; hl = ch / 20
    pop de
    ld a, h
    or a
    jr nz, +                    ; ch / 20 >= 256 > r: a crit
    ld a, e
    cp l
    jr c, +
    ld a, HIT_HIT
    ret
+:  ld a, HIT_CRIT
    ret
++: xor a                       ; HIT_MISS
    ret

;-----------------------------------------------------------------------
; calc_damage: cd_str, cd_w, cd_def, cd_class ($FF = the player is hit),
; cd_crit (0/1: x2), cd_tiles -> dmg_hp, dmg_arm. Draws one random number.
; In 8.8 fixed point, as the original:
;   d = (min << 8 + r * (max - min)) * ((str << 8) / def) >> 8
;   d = d * mult >> 8 (crit), d = d * class multiplier >> 8
;   d -= d * 76 >> 8 for each step of range past the weapon's
;   d = clamp(d, 1.0, 999.0); armour takes d * weapon's armour >> 8
; Outputs, rounded to whole points: dmg_arm = armour's share, dmg_hp =
; the rest. cd_d holds the clamped d (8.8, 32-bit) on the way. Clobbers
; every register except IX, IY, and m32a/b/c, m40.
;-----------------------------------------------------------------------
calc_damage:
    call rnd
    ld (cd_r), a                ; r: where in the weapon's range (0..255)
    ; ratio = (str << 8) / def
    ld a, (cd_def)
    or a
    jr nz, +
    inc a                       ; (defence 0 counts as 1)
+:  ld c, a
    ld b, 0
    ld a, (cd_str)
    ld h, a
    ld l, 0
    call div16                  ; hl = ratio (8.8)
    push hl
    ; a = (min << 8) + r * (max - min)
    ld hl, wpn_min
    ld a, (cd_w)
    call add_hl_a_read
    ld b, a                     ; b = min
    ld hl, wpn_max
    ld a, (cd_w)
    call add_hl_a_read
    sub b
    ld e, a                     ; e = max - min
    ld a, (cd_r)
    ld h, a
    push bc
    call mul8                   ; hl = r * (max - min)
    pop bc
    ld a, h
    add a, b
    ld h, a                     ; + min << 8: the base damage, 8.8
    pop de
    call mulw                  ; a * ratio
    call set32                  ; m32a = the 16.16 product
    call shr8_32                ; back to 8.8
    ld a, (cd_crit)             ; mult 512: x2
    or a
    jr z, +
    ld hl, m32a                 ; m32a <<= 1
    sla (hl)
    inc hl
    rl (hl)
    inc hl
    rl (hl)
    inc hl
    rl (hl)
+:  ld a, (cd_class)            ; the monster class's multiplier for this weapon
    cp $FF
    jr z, +                     ; the player is hit: none
    ld l, a
    ld h, 0
    ld e, N_WEAPONS
    ld h, l
    call mul8                   ; class * N_WEAPONS
    ld a, (cd_w)
    call add_hl_a
    add hl, hl
    ld de, dmg_mult
    add hl, de                  ; hl = &dmg_mult[class][weapon] (words)
    ld e, (hl)
    inc hl
    ld d, (hl)
    call mul32x16_shr8          ; d = d * mult >> 8
+:  ; ex
    ld hl, wpn_rmin
    ld a, (cd_w)
    call add_hl_a_read
    ld e, a
    ld hl, -1
    ld a, (cd_tiles)
    or a
    call nz, range_excess       ; hl = tiles^2 - rmin^2 - 1, or -1
    ld (cd_ex), hl
    ; dec = d * 76 >> 8 (into m32b), d back in m32a
    ld hl, m32a
    ld de, cd_d
    ld bc, 4
    ldir                        ; save d
    ld de, 76
    call mul32x16_shr8          ; m32a = d * 76 >> 8
    ld hl, m32a
    ld de, m32c                 ; (m32c = dec)
    ld bc, 4
    ldir
    ld hl, cd_d
    ld de, m32a
    ld bc, 4
    ldir                        ; d back in m32a
    ld a, 2
    ld (cd_k), a
    ld hl, 1
    ld (cd_dist), hl
-:  ld hl, (cd_ex)              ; while dist < ex (signed)
    ld de, (cd_dist)
    or a
    sbc hl, de
    jr z, +
    jp m, +
    ld hl, m32c
    ld de, m32b
    ld bc, 4
    ldir                        ; m32b = dec (sub32 subtracts m32b)
    call sub32                  ; d -= dec
    ld a, (cd_k)
    ld h, a
    ld e, a
    inc a
    ld (cd_k), a
    call mul8                   ; dist = k * k
    ld (cd_dist), hl
    jr -
+:  ; clamp to 256 .. 255744 ($03E700)
    ld hl, m32a+3
    bit 7, (hl)
    jr nz, _cd_min              ; negative
    ld a, (hl)
    or a
    jr nz, _cd_max
    dec hl
    ld a, (hl)                  ; byte 2
    cp 3
    jr c, ++
    jr nz, _cd_max
    dec hl
    ld a, (hl)                  ; byte 1
    cp $E7
    jr c, +++
    jr nz, _cd_max
    dec hl
    ld a, (hl)                  ; byte 0
    or a
    jr z, +++
    jr _cd_max
++: or a                        ; byte 2 = 0: at least 256 unless byte 1 = 0 too
    jr nz, +++
    ld a, (m32a+1)
    or a
    jr nz, +++
_cd_min:
    ld hl, 256
    jr +
_cd_max:
    ld hl, $E700
    ld a, 3
    jr ++
+:  xor a
++: ld (m32a), hl
    ld (m32a+2), a
    xor a
    ld (m32a+3), a
+++:
    ; arm = d * the weapon's armour share >> 8 (bytes 1..3 of the product)
    ld hl, m32a
    ld de, cd_d
    ld bc, 4
    ldir                        ; cd_d = the clamped d
    ld hl, wpn_armor
    ld a, (cd_w)
    call add_hl_a_read          ; a = armour's share (x/256)
    call mul32_8                ; m40 = d * share
    ; dmg_arm = (arm + 128) >> 8
    ld hl, (m40+1)
    ld a, (m40+3)
    ld de, 128
    add hl, de
    adc a, 0
    ld l, h
    ld h, a
    ld (dmg_arm), hl
    ; dmg_hp = ((d - arm) + 128) >> 8
    ld hl, (cd_d)
    ld de, (m40+1)
    or a
    sbc hl, de
    ld a, (cd_d+2)
    ld b, a
    ld a, (m40+3)
    ld c, a
    ld a, b
    sbc a, c
    ld de, 128
    add hl, de
    adc a, 0
    ld l, h
    ld h, a
    ld (dmg_hp), hl
    ret

;-----------------------------------------------------------------------
; setup_enemy: c = monster slot, hl = the def's parm (low 16 bits;
; monster parms are small): every stat scaled by parm/256 times a random
; factor in [0.75, 1.25); armour and defence share one. Five random numbers.
; In: the slot's S_M_* already hold the class's base stats (game_init).
; Each factor f = parm * (0.75 .. 1.25), 8.8 fixed point; each stat =
; min(255, stat * f >> 8); hp = maxhp, armour = maxarmour. Clobbers every
; register except IX, IY.
;-----------------------------------------------------------------------
setup_enemy:
    ld a, c
    ld (se_m), a
    ld (se_parm), hl
    srl h
    rr l
    srl h
    rr l
    ld (se_i2), hl              ; i2 = parm >> 2
    add hl, hl
    ld (se_i3), hl              ; i3 = i2 * 2
    call se_factor              ; random number 1: hp
    ld hl, S_M_MAXHP
    call se_scale
    ld (hl), a
    ld hl, S_M_HP
    call se_put                 ; hp = maxhp
    call se_factor              ; 2: armour and defence
    ld hl, S_M_MAXARMOR
    call se_scale
    ld (hl), a
    ld hl, S_M_ARMOR
    call se_put                 ; armour = maxarmour
    ld hl, S_M_DEF
    call se_scale
    ld (hl), a
    call se_factor              ; 3: strength
    ld hl, S_M_STR
    call se_scale
    ld (hl), a
    call se_factor              ; 4: agility
    ld hl, S_M_AGI
    call se_scale
    ld (hl), a
    call se_factor              ; 5: accuracy
    ld hl, S_M_ACC
    call se_scale
    ld (hl), a
    ret

; se_f = parm + (i3 * rnd >> 8) - i2
; (i2 = parm / 4, i3 = parm / 2: f runs from 0.75 parm to just under
; 1.25 parm.) Draws one random number; clobbers a, bc, de, hl.
se_factor:
    call rnd
    ld hl, (se_i3)
    call mul_hl_a               ; a:hl = i3 * rnd
    ld l, h
    ld h, a                     ; >> 8
    ld de, (se_parm)
    add hl, de
    ld de, (se_i2)
    or a
    sbc hl, de
    ld (se_f), hl
    ret

; hl = an S_M_* array -> hl = &array[m], a = clamp8(array[m] * f >> 8)
; (m = se_m.) Clobbers bc, de.
se_scale:
    ld a, (se_m)
    call add_hl_a
    push hl
    ld a, (hl)
    ld hl, (se_f)
    call mul_hl_a               ; a:hl = stat * f (f > 0)
    pop de
    or a
    ld a, h                     ; (stat * f) >> 8, if it fits in a byte
    jr z, +
    ld a, 255                   ; the top byte is set: at most 255
+:  ex de, hl
    ret

; hl = an S_M_* array: array[m] = a (m = se_m); clobbers hl
se_put:
    push af
    ld a, (se_m)
    call add_hl_a
    pop af
    ld (hl), a
    ret

;-----------------------------------------------------------------------
; the player's stats
;-----------------------------------------------------------------------
; hl = n (signed): health, kept within 0 .. maxhp
; Says "GAINED n HEALTH" if it went up. Clobbers every register except
; IX, IY.
add_health:
    ld a, (S_HP)
    ld (pp_a), a                ; (before)
    call add_hl_a               ; v = hp + n
    bit 7, h
    jr nz, ++                   ; v < 0: 0
    ld a, h
    or a
    jr nz, +                    ; v >= 256: maxhp
    ld a, (S_MAXHP)
    cp l
    ld a, l
    jr nc, +++                  ; v <= maxhp: v
+:  ld a, (S_MAXHP)
    jr +++
++: xor a
+++:
    ld (S_HP), a
    ld hl, str_health
    jr gained                   ; "GAINED n HEALTH" if it went up

; hl = n (signed): armour, at most maxarmor
; A result below 0 is stored as its low byte (it wraps round to 255 and
; down), as the comment below says the C did. Says "GAINED n ARMOR" if
; it went up. Clobbers every register except IX, IY.
add_armor:
    ld a, (S_ARMOR)
    ld (pp_a), a                ; (before)
    call add_hl_a               ; v = armour + n
    bit 7, h
    jr nz, ++                   ; (negative: kept as it comes, like the C)
    ld a, h
    or a
    jr nz, +                    ; v >= 256: maxarmour
    ld a, (S_MAXARMOR)
    cp l
    jr nc, ++                   ; v <= maxarmour: v
+:  ld a, (S_MAXARMOR)
    ld l, a
++: ld a, l
    ld (S_ARMOR), a
    ld hl, str_armor
; a = the new value, (pp_a) = the old, hl = what: "GAINED <diff><what>" if more
; Clobbers every register except IX, IY.
gained:
    ld b, a
    ld a, (pp_a)
    cp b
    ret nc                      ; not more: nothing said
    ld c, a
    ld a, b
    sub c
    ld e, a
    ld d, 0                     ; de = the difference
    ld b, h
    ld c, l                     ; bc = what
    ld hl, str_gained
    jp say_num                  ; "GAINED " n what

; c = ammo type, a = n -> a = 0 (Z) if that ammo was already full
; Otherwise S_AMMO[c] += n, at most 99, and a = 1 (NZ). Clobbers b, e, hl.
add_ammo:
    ld hl, S_AMMO
    ld b, 0
    add hl, bc                  ; hl = &ammo[c]
    ld e, a
    ld a, (hl)
    cp 99
    jr z, ++                    ; full
    add a, e
    jr c, +                     ; past 255: 99
    cp 100
    jr c, +++
+:  ld a, 99
+++:
    ld (hl), a
    ld a, 1
    or a
    ret
++: xor a
    ret

; a = a stat, e = n -> a = min(stat + n, 99) (as the original: never past 99)
; (Assumes stat + n < 256.)
stat_up:
    add a, e
    cp 100
    ret c
    ld a, 99
    ret

; e = the modulus (e > 0) -> a = rnd % e; draws one random number;
; keeps c, e; clobbers a, b, d, hl.
; Used as 3 + rnd % 3 and 1 + rnd % 2 by level_up, rnd % 5 by destroy.
rnd_mod:
    push de
    call rnd
    pop de
    call div8                   ; b = rnd % e
    ld a, b
    ret

; one level up: nextxp = level * 20 + 60; maxhp and maxarmour + 3..5
; (hp refilled); defence, strength, agility, accuracy + 1..2 each; all
; at most 99. Six random numbers. Then "LEVEL UP! LEVEL n". Clobbers
; every register except IX, IY.
level_up:
    ld hl, S_LEVEL
    inc (hl)
    ld h, (hl)                  ; nextxp = level * 20 + 60
    ld e, 20
    call mul8
    ld de, 60
    add hl, de
    ld (S_NEXTXP), hl
    ld e, 3                     ; maxhp += 3 + rnd % 3, at most 99; hp = maxhp
    call rnd_mod
    add a, 3
    ld e, a
    ld a, (S_MAXHP)
    call stat_up
    ld (S_MAXHP), a
    ld (S_HP), a
    ld e, 3                     ; maxarmor likewise
    call rnd_mod
    add a, 3
    ld e, a
    ld a, (S_MAXARMOR)
    call stat_up
    ld (S_MAXARMOR), a
    ld hl, S_DEFENSE            ; defense, strength, agility, accuracy: + 1 + rnd % 2
    ld b, 4                     ; (four bytes in a row in the state)
-:  push bc
    push hl
    ld e, 2
    call rnd_mod
    inc a
    ld e, a
    pop hl
    ld a, (hl)
    call stat_up
    ld (hl), a
    inc hl
    pop bc
    djnz -
    ld a, SFX_LEVEL_UP
    call sfx
    ld a, (S_LEVEL)
    ld e, a
    ld d, 0
    ld hl, str_level_up
    ld bc, str_empty
    jp say_num                  ; "LEVEL UP! LEVEL n"

; hl = n: "GAINED n XP!", then any levels gained
; Each level up subtracts nextxp from the XP before nextxp grows.
; (n may be negative: ec_stat's "-" on XP. XP below 0 would wrap to a
; large unsigned value, which this loop would take for many level-ups.)
; Clobbers every register except IX, IY.
add_xp:
    push hl
    ex de, hl
    ld hl, str_gained
    ld bc, str_xp
    call say_num                ; "GAINED n XP!"
    pop de
    ld hl, (S_XP)
    add hl, de
-:  ld (S_XP), hl
    ld de, (S_NEXTXP)
    or a
    sbc hl, de                  ; while xp >= nextxp
    ret c
    ld (S_XP), hl               ; xp -= nextxp
    call level_up
    ld hl, (S_XP)
    jr -

; hl = damage, de = armour damage (both 16-bit)
; Says "n DAMAGE!" (n = both together). If both are 0 nothing else
; happens. Otherwise: player_hurt = 1 (a red flash); armour absorbs the
; armour damage, and any it cannot absorb is added to the damage
; (armour 0); warnings "NEAR DEATH!" / "LOW HEALTH!" when health crosses
; 26/256 or 78/256 of maxhp, or "ARMOR GONE!"; then health drops, and at
; 0 the player is dead (S_DEAD). Clobbers every register except IX, IY.
; Called by monster_attack, hurt_at (explosions), touch (fires) and
; ec_pain.
player_pain:
    ld (pp_dmg), hl
    ld (pp_arm), de
    add hl, de
    ex de, hl
    ld hl, str_empty
    ld bc, str_damage
    call say_num                ; "n DAMAGE!"
    ld hl, (pp_dmg)
    ld de, (pp_arm)
    ld a, h
    or l
    or d
    or e
    ret z                       ; no damage at all
    ld a, 1
    ld (player_hurt), a         ; after_action flashes the view
    ld a, (S_ARMOR)
    ld (pp_a), a                ; armour before (for "ARMOR GONE!")
    ld l, a
    ld h, 0
    or a
    sbc hl, de                  ; armour - arm
    jr nc, +
    ex de, hl                   ; (hl = -(arm - a)): dmg += arm - a, armour = 0
    ld hl, (pp_dmg)
    or a
    sbc hl, de
    ld (pp_dmg), hl
    ld l, 0
+:  ld a, l
    ld (S_ARMOR), a
    ; h1 = (hp << 8) / maxhp, h2 = ((hp - dmg) << 8) / maxhp or 0
    ; (health before and after, as fractions of maxhp in 8.8)
    ld a, (S_MAXHP)
    ld c, a
    ld b, 0
    ld a, (S_HP)
    ld h, a
    ld l, 0
    push bc
    call div16                  ; h1
    ld (pp_h1), hl
    pop bc
    ld a, (S_HP)
    ld l, a
    ld h, 0
    ld de, (pp_dmg)
    or a
    sbc hl, de
    jr z, +                     ; dies: no warnings
    jr c, +
    ld h, l                     ; (hp - dmg < 256)
    ld l, 0
    call div16                  ; h2
    ld a, h                     ; h2 > 0: the warnings
    or l
    jr z, +
    ld (pp_h2), hl
    ld de, 26
    call pp_crossed             ; crossed about 10% of maxhp?
    ld hl, str_near_death
    jr c, ++
    ld de, 78
    call pp_crossed             ; crossed about 30%?
    ld hl, str_low_health
    jr c, ++
    ld a, (pp_a)
    or a
    jr z, +                     ; had no armour
    ld a, (S_ARMOR)
    or a
    jr nz, +                    ; still has some
    ld hl, str_armor_gone
++: call say                    ; the warning
+:  ; hp - dmg <= 0: dead
    ld a, (S_HP)
    ld l, a
    ld h, 0
    ld de, (pp_dmg)
    or a
    sbc hl, de
    jr z, +
    jr c, +
    ld a, l
    ld (S_HP), a                ; still alive
    ld a, SFX_HURT
    jp sfx
+:  xor a
    ld (S_HP), a
    inc a
    ld (S_DEAD), a              ; dead (after_action shows the end screen)
    ld a, SFX_PLAYER_DIE
    jp sfx

; de = a threshold -> carry if h1 > de >= h2
; (health fell from above the threshold to at or below it); clobbers
; de, hl
pp_crossed:
    ld hl, (pp_h1)
    or a
    sbc hl, de                  ; h1 - threshold
    jr z, +
    jr c, +
    ld hl, (pp_h2)
    ex de, hl
    or a
    sbc hl, de                  ; threshold - h2: borrow if threshold < h2
    ccf
    ret
+:  or a
    ret

; a = monster entity, hl = damage, de = armour damage
; Like player_pain without the messages: the slot's armour absorbs the
; armour damage (any excess goes to health), then health drops, to 0 at
; the least. Death is handled by the caller (monster_hurt_or_died).
; Clobbers every register except IX, IY.
monster_pain:
    push hl
    call ent_mon
    ld (mp_m), a                ; its slot
    ld hl, S_M_ARMOR
    call add_hl_a
    ld a, (hl)
    ld c, l
    ld b, h                     ; bc = &m_armor[slot]
    ld l, a
    ld h, 0
    or a
    sbc hl, de                  ; m_armor - arm
    pop de                      ; de = damage
    jr nc, +
    ex de, hl                   ; dmg += arm - m_armor, m_armor = 0
    or a
    sbc hl, de
    ex de, hl
    ld l, 0
+:  ld a, l
    ld (bc), a
    ld hl, S_M_HP
    ld a, (mp_m)
    call add_hl_a
    ld a, (hl)
    push hl
    ld l, a
    ld h, 0
    or a
    sbc hl, de                  ; hp - dmg
    ld a, l
    pop hl
    jr z, +
    jr c, +
    ld (hl), a
    ret
+:  ld (hl), 0                  ; dead
    ret

;-----------------------------------------------------------------------
; monsters: active list, line of sight
; S_ACTIVE[0 .. S_N_ACTIVE-1] lists the monsters that take turns, in the
; order they woke; F_ACTIVE marks them.
;-----------------------------------------------------------------------
; a = entity: on the active list (at the end), if it isn't
; -> c = entity; clobbers a, b, hl
activate:
    ld c, a
    ld hl, S_E_FLAGS
    ld b, 0
    add hl, bc
    bit 3, (hl)                 ; F_ACTIVE
    ret nz
    set 3, (hl)
    ld hl, S_N_ACTIVE
    ld a, (hl)
    inc (hl)
    ld hl, S_ACTIVE
    call add_hl_a
    ld (hl), c
    ret

; a = entity: off the active list (the rest move up), if it is on it
; Clobbers a, bc, de, hl.
deactivate:
    ld c, a
    ld hl, S_E_FLAGS
    ld b, 0
    add hl, bc
    bit 3, (hl)                 ; F_ACTIVE
    ret z
    res 3, (hl)
    ld a, (S_N_ACTIVE)
    or a
    ret z
    ld b, a
    ld hl, S_ACTIVE
-:  ld a, (hl)                  ; find it
    cp c
    jr z, +
    inc hl
    djnz -
    ret
+:  dec b                       ; b = entries after it
    jr z, +
    ld d, h
    ld e, l
    inc hl
    push bc
    ld c, b
    ld b, 0
    ldir                        ; the ones after it move up one place
    pop bc
    ex de, hl
+:  ld (hl), 0                  ; the last one cleared
    ld hl, S_N_ACTIVE
    dec (hl)
    ret

; PORT: the original activates a monster the first time it is drawn; here:
; within 5 cells ahead, inside the 90-degree cone, grid line of sight.
; b = x, c = z -> a = 1 (NZ) if seen
; Seen: |dx| and |dz| at most 5; depth along the facing 1..5 and the
; sideways offset at most the depth (the 90-degree cone); and every cell
; strictly between the player and (x, z) on the rounded straight line
; free of walls and closed doors. Clobbers every register except IX, IY.
visible:
    ld a, (S_PX)
    ld d, a
    ld a, b
    sub d
    ld (vis_dx), a
    call abs8
    cp 6
    jp nc, _vis_no
    ld b, a                     ; |dx|
    ld a, (S_PZ)
    ld d, a
    ld a, c
    sub d
    ld (vis_dz), a
    call abs8
    cp 6
    jp nc, _vis_no
    ld c, a                     ; |dz|
    cp b
    jr nc, +
    ld a, b
+:  ld (vis_steps), a           ; max(|dx|, |dz|)
    ; depth along the facing, lat across it
    ld a, (vis_dx)
    ld d, a
    ld a, (vis_dz)
    ld e, a
    ld a, (S_FACING)
    and 3
    jr nz, +
    ld a, e                     ; north: depth = -dz, lat = dx
    neg
    ld b, a
    ld c, d
    jr ++
+:  dec a
    jr nz, +
    ld b, d                     ; east: depth = dx, lat = dz
    ld c, e
    jr ++
+:  dec a
    jr nz, +
    ld b, e                     ; south: depth = dz, lat = -dx
    ld a, d
    neg
    ld c, a
    jr ++
+:  ld a, d                     ; west: depth = -dx, lat = -dz
    neg
    ld b, a
    ld a, e
    neg
    ld c, a
++: ld a, c
    call abs8
    ld c, a                     ; |lat|
    ld a, b                     ; depth in 1..5, |lat| <= depth
    dec a
    cp 5
    jr nc, _vis_no
    ld a, b
    cp c
    jr c, _vis_no
    ; the cells on the line: (2 * d * s + steps) // (2 * steps), s = 1 .. steps - 1
    ld a, 1
    ld (vis_s), a
-:  ld a, (vis_steps)
    ld hl, vis_s
    cp (hl)
    jr z, +
    jr c, +
    ld a, (vis_dx)
    call vis_step               ; x offset of step s
    ld a, (S_PX)
    add a, l
    push af
    ld a, (vis_dz)
    call vis_step               ; z offset of step s
    ld a, (S_PZ)
    add a, l
    ld c, a
    pop af
    ld b, a
    call wall_at
    jr nz, _vis_no              ; a wall in the way
    call line_at
    cp $FF
    jr nz, _vis_no              ; a closed door in the way
    ld hl, vis_s
    inc (hl)
    jr -
+:  ld a, 1
    or a
    ret
_vis_no:
    xor a
    ret

; a = d (signed) -> hl = (2 * d * s + steps) // (2 * steps)
; (the offset, rounded to the nearest cell, of step s of `steps` along
; a line of length d; s = vis_s, steps = vis_steps). Clobbers a, bc, de.
vis_step:
    call sext                   ; hl = d
    add hl, hl                  ; 2d
    ld a, (vis_s)
    ld b, a
    ex de, hl
    ld hl, 0
-:  add hl, de                  ; * s
    djnz -
    ld a, (vis_steps)
    ld e, a
    ld d, 0
    add hl, de
    ex de, hl
    add hl, hl                  ; (hl = 2 * steps... from de = steps)
    ex de, hl
    jp fdiv16                   ; floor division, as Python's //

; c = the first entity to look at -> c = the next one that is linked,
; alive and not active, within 5 cells in x and z (n_map_ents if none)
; (A cheap test before visible: any kind of entity passes it.) Clobbers
; a, b, de, hl.
av_next:
    ld a, (n_map_ents)
    sub c
    ret z
    ret c
    ld b, a                     ; b = entities left
    ld hl, S_E_FLAGS
    ld a, c
    call add_hl_a
-:  ld a, (hl)
    and F_LINKED | F_ALIVE | F_ACTIVE
    cp F_LINKED | F_ALIVE
    jr nz, ++
    push hl
    ld de, S_E_X - S_E_FLAGS
    add hl, de
    ld a, (S_PX)
    ld e, a
    ld a, (hl)
    sub e
    add a, 5
    cp 11                       ; x - px in -5..5?
    jr nc, +
    pop hl
    push hl
    ld de, S_E_Z - S_E_FLAGS
    add hl, de
    ld a, (S_PZ)
    ld e, a
    ld a, (hl)
    sub e
    add a, 5
    cp 11                       ; z - pz in -5..5?
    jr nc, +
    pop hl
    ret                         ; found: c
+:  pop hl
++: inc hl
    inc c
    djnz -
    ret                         ; none: c = n_map_ents

; every monster that can now see the player becomes active (index order)
; Only the map's entities (not drops). Clobbers every register except
; IX, IY. Called by settle, monster_turn, act_turn and game_init.
activate_visible:
    ld c, 0
-:  call av_next                ; the next candidate from c
    ld a, c
    ld hl, n_map_ents
    cp (hl)
    ret nc                      ; c >= n_map_ents: done
    push bc
    ld a, c
    call etype
    cp ET_ENEMY
    jr nz, +                    ; not a monster
    pop bc
    push bc
    ld hl, S_E_X
    ld a, c
    call add_hl_a
    ld b, (hl)
    ld hl, S_E_Z
    ld a, c
    call add_hl_a
    ld a, c
    ld c, (hl)
    push af
    call visible                ; can it see the player?
    pop bc
    jr z, +
    ld a, b
    call activate               ; yes: it joins the active list
+:  pop bc
    inc c
    jr -

;-----------------------------------------------------------------------
; drops and death
;-----------------------------------------------------------------------
; a = monster entity: maybe drops something (into the next of the 8 drop
; slots). Draws four random numbers.
; (Always draws them, even when nothing drops.) No drop if it already
; dropped (F_DROPPED), for classes 1, 12 and 13, or when the first
; random byte is below 51 (about 1 in 5). Otherwise the kind of drop
; (DD_*) depends on the class and the other random bytes; the slots are
; used in turn (S_DROP_INDEX), so a ninth drop replaces the oldest.
; Clobbers every register except IX, IY. Called by monster_died.
spawn_drop:
    ld (sd_i), a
    call rnd32                  ; l, h, e, d = bytes 0..3
    push de
    push hl
    ld a, (sd_i)
    call esub
    ld (sd_es), a               ; its class
    pop hl
    pop de
    ld a, (sd_i)
    push hl
    ld hl, S_E_FLAGS
    call add_hl_a
    ld a, (hl)
    pop hl
    and F_DROPPED
    ret nz                      ; dropped once already
    ld a, (sd_es)
    cp 1
    ret z
    cp 12
    ret z
    cp 13
    ret z                       ; classes that never drop
    ld a, l
    cp 51
    ret c                       ; byte 0 < 51: nothing
    push hl
    ld a, (sd_i)
    ld hl, S_E_FLAGS
    call add_hl_a
    set 5, (hl)                 ; F_DROPPED
    pop hl
    ; n = rnd >> 8; n5, n6 = the two bytes of n >> 1
    ld a, e
    rra
    ld a, h
    rra
    ld (sd_n5), a
    ld a, d
    rra
    ld a, e
    rra
    ld (sd_n6), a
    bit 0, h                    ; n & 1
    jr z, _sd_even
    ; n odd: health (n5 < 115), armour (< 230) or credits
    ld a, (sd_n5)
    ld b, DD_HEALTH
    cp 115
    jp c, _sd_have
    ld b, DD_ARMOR
    cp 230
    jp c, _sd_have
    ; credits: the richer monsters (parm 294, 467, 640) give 5 more often
    ld a, (sd_i)
    ld hl, S_E_DEF
    call add_hl_a
    ld a, (hl)
    call def_parm               ; dehl = its parm
    ld c, 0
    ld a, d
    or e
    jr nz, ++                   ; a parm >= 65536: no bonus
    ld de, 294
    call sd_parm_is
    ld c, 3
    jr z, ++
    ld de, 467
    call sd_parm_is
    ld c, 8
    jr z, ++
    ld de, 640
    call sd_parm_is
    ld c, 13
    jr z, ++
    ld c, 0
++: ld a, 230
    add a, c
    ld c, a                     ; c = 230 + bonus
    ld a, (sd_n5)
    cp c
    ld b, DD_CREDIT5            ; n5 < c: 5 credits
    jp c, _sd_have
    ld b, DD_CREDIT1            ; else 1 credit
    jp _sd_have

; hl = parm (low word), de = a value -> Z if equal; keeps hl
sd_parm_is:
    push hl
    or a
    sbc hl, de
    pop hl
    ret

_sd_even:
    ; n even: a drop chosen by the class (sd_es)
    ; b3 = n5 < 76, b4 = n6 < 15 (in c: bit 0 = b3, bit 1 = b4)
    ld c, 0
    ld a, (sd_n5)
    cp 76
    jr nc, +
    set 0, c
+:  ld a, (sd_n6)
    cp 15
    jr nc, +
    set 1, c
+:  ld a, (sd_es)
    or a
    jr nz, +
    ; 0: (b4 ? AMMO2 : AMMO) + (b3 ? 2 : 1)
    call sd_ammo_kind           ; b = DD_AMMO or DD_AMMO2
    inc b
    bit 0, c
    jr z, _sd_have
    inc b
    jr _sd_have
+:  cp 2
    jr nz, +
    ; 2: ((b4 || b3) ? AMMO2 : AMMO) + 1
    ld b, DD_AMMO + 1
    ld a, c
    or a
    jr z, _sd_have
    ld b, DD_AMMO2 + 1
    jr _sd_have
+:  cp 6
    jr nz, +
    ; 6: b4 ? LGMED : SMMED
    ld b, DD_SMMED
    bit 1, c
    jr z, _sd_have
    ld b, DD_LGMED
    jr _sd_have
+:  cp 7
    jr nz, +
    ld b, DD_CREDIT5            ; 7: 5 credits
    jr _sd_have
+:  cp 8
    jr nz, +
    call sd_ammo_kind           ; 8: (b4 ? AMMO2 : AMMO) + 3
    ld a, b
    add a, 3
    ld b, a
    jr _sd_have
+:  cp 9
    jr nz, +
    call sd_ammo_kind           ; 9: (b4 ? AMMO2 : AMMO) + 4
    ld a, b
    add a, 4
    ld b, a
    jr _sd_have
+:  cp 10
    jr nz, +
    ; 10: b3 ? (b4 ? LGMED : SMMED) : CREDIT5
    ld b, DD_CREDIT5
    bit 0, c
    jr z, _sd_have
    ld b, DD_SMMED
    bit 1, c
    jr z, _sd_have
    ld b, DD_LGMED
    jr _sd_have
+:  cp 11
    ret nz                      ; nothing for the other kinds
    ; 11: AMMO2 + (b3 ? 4 : 2)
    ld b, DD_AMMO2 + 2
    bit 0, c
    jr z, _sd_have
    ld b, DD_AMMO2 + 4
_sd_have:
    ; b = drop type: into slot n_map_ents + drop_index
    ld a, b
    ld (sd_dt), a
    ld a, (S_DROP_INDEX)
    ld c, a
    inc a
    and 7
    ld (S_DROP_INDEX), a
    ld a, (n_map_ents)
    add a, c
    ld (sd_n5), a               ; (reused: the slot)
    call ent_unlink             ; (if it was linked) the slot's old drop goes
    ld a, (sd_n5)
    ld c, a
    ld b, 0
    LTAB LT_DROP_DEF
    ld a, (sd_dt)
    call add_hl_a_read          ; the level's def for this kind of drop
    ld hl, S_E_DEF
    add hl, bc
    ld (hl), a                  ; (NO_DEF is not checked here)
    ld a, (sd_i)
    ld e, a
    ld d, 0
    ld hl, S_E_X
    add hl, de
    ld a, (hl)
    ld hl, S_E_X
    add hl, bc
    ld (hl), a
    ld hl, S_E_Z
    add hl, de
    ld a, (hl)
    ld hl, S_E_Z
    add hl, bc
    ld (hl), a
    ld hl, S_E_FLAGS
    add hl, bc
    ld (hl), 0                  ; fresh flags
    ld a, c
    jp ent_link                 ; on the dead monster's cell

; c bit 1 = b4 -> b = b4 ? DD_AMMO2 : DD_AMMO; changes only b
sd_ammo_kind:
    ld b, DD_AMMO
    bit 1, c
    ret z
    ld b, DD_AMMO2
    ret

; a = monster entity, e = 1 if it was blown apart (no corpse)
; If it is alive: not any more; the player gets its XP (add_xp, which may
; level up); it leaves the active list and the map; F_CORPSE unless blown
; apart (a corpse an arch-vile can raise); then maybe a drop. Clobbers
; every register except IX, IY.
monster_died:
    ld c, a
    ld hl, S_E_FLAGS
    ld b, 0
    add hl, bc
    bit 0, (hl)                 ; F_ALIVE
    ret z
    res 0, (hl)
    push bc
    push de
    ld a, SFX_MON_DIE
    call sfx
    pop de
    pop bc
    push bc
    push de
    ld a, c
    call cb_exp                 ; a = the XP it is worth
    ld l, a
    ld h, 0
    call add_xp
    pop de
    pop bc
    push bc
    push de
    ld a, c
    call deactivate             ; off the active list
    pop de
    pop bc
    push bc
    push de
    ld a, c
    call ent_unlink             ; off the map
    pop de
    pop bc
    ld a, e
    or a
    jr nz, +
    ld hl, S_E_FLAGS
    ld b, 0
    add hl, bc
    set 6, (hl)                 ; F_CORPSE
+:  ld a, c
    jp spawn_drop               ; maybe something to pick up

;-----------------------------------------------------------------------
; monster AI
;-----------------------------------------------------------------------
; a = monster -> hl = what blocks its moves
; MASK_MONSTER_MOVE, except that classes 4 and 13 walk through both
; kinds of fire (types 10, 11), 6 and 7 through ET_FIRE2, 10 through
; ET_FIRE. Clobbers a.
monster_mask:
    call esub                   ; a = its class
    ld hl, MASK_MONSTER_MOVE
    cp 4
    jr z, +
    cp 13
    jr z, +
    cp 6
    jr z, ++
    cp 7
    jr z, ++
    cp 10
    ret nz
    ld h, $FB                   ; MASK_MONSTER_MOVE without bit 10
    ret
+:  ld h, $F3                   ; without bits 10, 11
    ret
++: ld h, $F7                   ; without bit 11
    ret

; the four steps, in the order the AI tries them
step_x:
    .db 1, -1, 0, 0
step_z:
    .db 0, 0, 1, -1

; hl = a signed word -> hl = its square (low 16 bits, as C's int)
; (The unsigned product's low word is the same as the signed one's.)
; Clobbers a, bc, de, m32c.
sq16:
    ld d, h
    ld e, l
    call mulw                   ; dehl = hl * hl; hl = the low word
    ret

; how far from (cp_sx, cp_sz) to (cp_dx, cp_dz) in two greedy steps:
; 0 or 1 if it gets there (the steps it took), else 2 + the distance^2
; left. -> hl
; Each step tries the four directions (step_x / step_z order) and takes
; the one that brings it closest (squared distance, first wins a tie),
; if a trace with cp_mask (ignoring cp_i itself) says it can move there.
; Moves cp_sx / cp_sz. Clobbers every register except IX, IY.
calc_path:
    ld a, (cp_sx)
    ld hl, cp_dx
    cp (hl)
    jr nz, +
    ld a, (cp_sz)
    ld hl, cp_dz
    cp (hl)
    jr nz, +
    ld hl, 0
    ret
+:  xor a
    ld (cp_k), a                ; k = step 0 or 1
_cp_k:
    ld hl, 65535
    ld (cp_best), hl            ; best squared distance so far
    xor a
    ld (cp_stx), a
    ld (cp_stz), a              ; best step: none yet (0, 0)
    ld (cp_d), a                ; direction 0..3
_cp_d:
    ; nd = (sx + ox - dx)^2 + (sz + oz - dz)^2
    ld hl, step_x
    ld a, (cp_d)
    call add_hl_a_read
    ld b, a
    ld a, (cp_sx)
    ld hl, cp_dx
    call cp_axis                ; x part
    push hl
    ld hl, step_z
    ld a, (cp_d)
    call add_hl_a_read
    ld b, a
    ld a, (cp_sz)
    ld hl, cp_dz
    call cp_axis                ; z part
    pop de
    add hl, de                  ; nd
    ld de, (cp_best)
    push hl
    or a
    sbc hl, de
    pop hl
    jr nc, _cp_next             ; not better
    push hl
    ld hl, step_x               ; can it step there?
    ld a, (cp_d)
    call add_hl_a_read
    ld hl, cp_sx
    add a, (hl)
    ld d, a
    ld hl, step_z
    ld a, (cp_d)
    call add_hl_a_read
    ld hl, cp_sz
    add a, (hl)
    ld e, a
    ld a, (cp_sx)
    ld b, a
    ld a, (cp_sz)
    ld c, a
    ld hl, (cp_mask)
    ld a, (cp_i)
    call trace                  ; anything in the way of that step?
    pop hl
    jr nz, _cp_next
    ld (cp_best), hl            ; the best step so far
    ld hl, step_x
    ld a, (cp_d)
    call add_hl_a_read
    ld (cp_stx), a
    ld hl, step_z
    ld a, (cp_d)
    call add_hl_a_read
    ld (cp_stz), a
_cp_next:
    ld hl, cp_d
    inc (hl)
    ld a, (hl)
    cp 4
    jp nz, _cp_d
    ld a, (cp_stx)              ; take the best step
    ld hl, cp_sx
    add a, (hl)
    ld (hl), a
    ld a, (cp_stz)
    ld hl, cp_sz
    add a, (hl)
    ld (hl), a
    ld a, (cp_sx)
    ld hl, cp_dx
    cp (hl)
    jr nz, +
    ld a, (cp_sz)
    ld hl, cp_dz
    cp (hl)
    jr nz, +
    ld a, (cp_k)                ; there: in k + 1 steps... returns k, as the original
    ld l, a
    ld h, 0
    ret
+:  ld hl, cp_k
    inc (hl)
    ld a, (hl)
    cp 2
    jp nz, _cp_k
    ld b, 0                     ; 2 + what is left
    ld a, (cp_sx)
    ld hl, cp_dx
    call cp_axis                ; (sx - dx)^2
    push hl
    ld b, 0
    ld a, (cp_sz)
    ld hl, cp_dz
    call cp_axis                ; (sz - dz)^2
    pop de
    add hl, de
    inc hl
    inc hl
    ret

; a = s, b = step (signed), hl = &d -> hl = (s + step - d)^2 (s, d bytes)
; Clobbers a, bc, de, m32c.
cp_axis:
    ld c, (hl)                  ; d
    ld l, a
    ld h, 0                     ; s
    ld a, b
    ld e, a
    rla
    sbc a, a
    ld d, a                     ; step, sign-extended
    add hl, de
    ld e, c
    ld d, 0
    or a
    sbc hl, de
    jp sq16

; a = monster, d = x, e = z of where it wants to be -> a = 1 (NZ) if it
; stepped. Of the steps it can take, the one whose two-step path gets
; closest; a step east (the first) wins any tie it is in only when the path
; is unknown (9999), as the original. Ties: one picked at random.
; A step is possible if a trace with the monster's mask (monster_mask)
; to the next cell meets nothing; the path from there is scored by
; calc_path. Draws one random number if any step is possible. Moves the
; monster with ent_move_to. Clobbers every register except IX, IY.
ai_move:
    ld (am_i), a
    ld a, d
    ld (am_dx), a
    ld a, e
    ld (am_dz), a
    ld a, (am_i)
    ld hl, S_E_X
    call add_hl_a_read
    ld (am_sx), a
    ld a, (am_i)
    ld hl, S_E_Z
    call add_hl_a_read
    ld (am_sz), a
    xor a
    ld (am_n), a
    ld (am_d), a
    ld hl, 9999
    ld (am_best), hl
    ld a, (am_i)
    call monster_mask           ; what blocks it
    ld (am_mask), hl
_am_d:                          ; for each direction am_d = 0..3
    ; can it step that way?
    ld hl, step_x
    ld a, (am_d)
    call add_hl_a_read
    ld hl, am_sx
    add a, (hl)
    ld d, a
    ld (cp_sx), a
    ld hl, step_z
    ld a, (am_d)
    call add_hl_a_read
    ld hl, am_sz
    add a, (hl)
    ld e, a
    ld (cp_sz), a
    ld a, (am_sx)
    ld b, a
    ld a, (am_sz)
    ld c, a
    ld hl, (am_mask)
    ld a, (am_i)
    call trace                  ; blocked?
    jr nz, _am_next
    ; and from there?
    ld a, (am_i)
    ld (cp_i), a
    ld a, (am_dx)
    ld (cp_dx), a
    ld a, (am_dz)
    ld (cp_dz), a
    ld hl, (am_mask)
    ld (cp_mask), hl
    call calc_path              ; hl = how far the target is from that cell
    ld (am_pd), hl
    ld de, (am_best)
    ld a, (am_d)
    or a
    jr nz, +
    ; east: better if pd < 9999, joins if pd = 9999
    ld de, 9999
    or a
    sbc hl, de
    jr c, _am_best
    jr z, _am_add
    jr _am_next
+:  or a                        ; the others: better if pd < best, joins if equal
    sbc hl, de
    jr c, _am_best
    jr nz, _am_next
    jr _am_add
_am_best:                       ; a new best: the list of ties restarts
    ld hl, (am_pd)
    ld (am_best), hl
    xor a
    ld (am_n), a
_am_add:                        ; am_order[am_n++] = this direction
    ld hl, am_n
    ld a, (hl)
    inc (hl)
    ld hl, am_order
    call add_hl_a
    ld a, (am_d)
    ld (hl), a
_am_next:
    ld hl, am_d
    inc (hl)
    ld a, (hl)
    cp 4
    jp nz, _am_d
    ld a, (am_n)
    or a
    ret z
    ; pick = order[(rnd & 3) % n]
    call rnd
    and 3
    ld hl, am_n
    ld e, (hl)
    call div8                   ; b = (rnd & 3) % n
    ld a, b
    ld hl, am_order
    call add_hl_a_read
    ld (am_d), a                ; the chosen direction
    ld hl, step_x
    call add_hl_a_read
    ld hl, am_sx
    add a, (hl)
    ld d, a
    ld hl, step_z
    ld a, (am_d)
    call add_hl_a_read
    ld hl, am_sz
    add a, (hl)
    ld e, a
    ld a, (am_i)
    call ent_move_to            ; step there (its cell list follows)
    ld a, 1
    or a
    ret

; a = monster, b = kind (T_*), c = target: queued (newest first), unless
; it already has an attack waiting
; The queue is att_ent / att_kind / att_tgt [0 .. att_n-1]; a new entry
; goes in at index 0 and the others move up (lddr). monster_turn carries
; them out in queue order. Clobbers a, bc, de, hl.
queue_attack:
    ld e, a
    ld a, (att_n)
    or a
    jr z, +
    push bc
    ld b, a
    ld hl, att_ent
-:  ld a, (hl)                  ; already queued?
    cp e
    jr z, ++
    inc hl
    djnz -
    pop bc
    ; make room at the front
    push bc
    push de
    ld a, (att_n)
    ld c, a
    ld b, 0
    ld hl, att_ent-1
    add hl, bc
    ld d, h
    ld e, l
    inc de
    push bc
    lddr
    pop bc
    push bc
    ld hl, att_kind-1
    add hl, bc
    ld d, h
    ld e, l
    inc de
    lddr
    pop bc
    ld hl, att_tgt-1
    add hl, bc
    ld d, h
    ld e, l
    inc de
    lddr
    pop de
    pop bc
+:  ld a, e
    ld (att_ent), a
    ld a, b
    ld (att_kind), a
    ld a, c
    ld (att_tgt), a
    ld hl, att_n
    inc (hl)
    ret
++: pop bc
    ret

; a = monster -> a = the weapon of its attack (the second when F_ATK2)
; (mon_atk[class * 2 + (F_ATK2 ? 1 : 0)].) Clobbers bc, e, hl.
attack_weapon:
    ld c, a
    call esub                   ; a = class
    add a, a
    ld e, a
    ld hl, S_E_FLAGS
    ld b, 0
    add hl, bc
    bit 4, (hl)                 ; F_ATK2
    ld a, e
    jr z, +
    inc a
+:  ld hl, mon_atk
    jp add_hl_a_read

; b = ax, c = az, d = bx, e = bz -> hl = (ax - bx)^2 + (az - bz)^2
; Keeps bc; clobbers a, de.
dist2:
    ld a, b
    sub d
    call abs8
    ld h, a
    push de
    ld e, a
    call mul8                   ; dx^2
    pop de
    push hl
    ld a, c
    sub e
    call abs8
    ld h, a
    ld e, a
    call mul8                   ; dz^2
    pop de
    add hl, de
    ret

; a = monster: its turn's first decision -> a = 1 if it moved
; (otherwise it may have queued an attack)
; The model's aiThink:
;   class 10 (the arch-vile kind): with rnd < 84, looks for a corpse
;     (not being raised) 1..2 cells away (squared distance 1..4) whose
;     cell is free, and queues a T_RAISE at it. Otherwise flips F_ATK2
;     (alternating its two attacks).
;   can hit: in the same row or column as the player, within reach
;     (1 + the weapon's rmin) and with a clear MASK_SHOT line, or a
;     destructible first in that line (then the attack is aimed at it).
;   it moves unless it can hit; a monster with range (reach / 2 > 0)
;     still moves with rnd >= 217. The target cell is the one in front
;     of the player, or one further along its side when it is close and
;     aligned (rnd < 38). If the move fails and it can hit, it attacks.
; Moving sets S_E_MOVE[i] = 0 (steps taken this turn). Clobbers every
; register except IX, IY.
ai_think:
    ld (at_i), a
    ld hl, S_E_X
    call add_hl_a_read
    ld (at_ex), a
    ld a, (at_i)
    ld hl, S_E_Z
    call add_hl_a_read
    ld (at_ez), a
    xor a
    ld (at_canhit), a
    ld (at_kind), a             ; T_PLAYER
    ld (at_tgt), a
    ld a, (at_i)
    call esub
    cp 10
    jp nz, _at_weapon
    ; the arch-vile kind: may raise a corpse within 2 cells
    call rnd
    cp 84
    jp nc, _at_flip
    xor a
    ld (at_j), a
_at_corpse:
    ld a, (at_j)
    ld hl, n_map_ents
    cp (hl)
    jr nc, _at_flip
    ld hl, S_E_FLAGS
    call add_hl_a_read
    bit 6, a                    ; F_CORPSE
    jr z, _at_cnext
    bit 7, a                    ; F_RAISING
    jr nz, _at_cnext
    ld a, (at_j)
    ld hl, S_E_X
    call add_hl_a_read
    ld b, a
    ld a, (at_j)
    ld hl, S_E_Z
    call add_hl_a_read
    ld c, a
    ld a, (at_ex)
    ld d, a
    ld a, (at_ez)
    ld e, a
    call dist2                  ; hl = squared distance to the corpse
    ld a, h
    or a
    jr nz, _at_cnext            ; > 4
    ld a, l
    or a
    jr z, _at_cnext             ; 0
    cp 5
    jr nc, _at_cnext
    ld a, (at_j)                ; is the corpse's cell free to stand on?
    ld hl, S_E_X
    call add_hl_a_read
    ld b, a
    ld d, a
    ld a, (at_j)
    ld hl, S_E_Z
    call add_hl_a_read
    ld c, a
    ld e, a
    ld hl, MASK_RAISE_FREE
    ld a, $FF
    call trace                  ; a one-cell trace: is anything standing there?
    jr nz, _at_cnext
    ld a, (at_j)
    ld hl, S_E_FLAGS
    call add_hl_a
    set 7, (hl)                 ; F_RAISING
    ld a, (at_j)
    ld c, a
    ld b, T_RAISE
    ld a, (at_i)
    call queue_attack           ; the raise happens in the attacks phase
    xor a
    ret
_at_cnext:
    ld hl, at_j
    inc (hl)
    jp _at_corpse
_at_flip:
    ld a, (at_i)
    ld hl, S_E_FLAGS
    call add_hl_a
    ld a, (hl)
    xor F_ATK2
    ld (hl), a
_at_weapon:
    ld a, (at_i)
    call attack_weapon          ; a = the weapon of its current attack
    ld (at_w), a
    ld hl, wpn_rmin
    call add_hl_a_read
    inc a
    ld (at_reach), a            ; reach = 1 + rmin
    srl a
    ld (at_half), a
    ld a, (at_ex)
    ld b, a
    ld a, (at_ez)
    ld c, a
    ld a, (S_PX)
    ld d, a
    ld a, (S_PZ)
    ld e, a
    call dist2                  ; squared distance to the player
    ld (at_d2), hl
    ; aligned: same row or column as the player
    xor a
    ld (at_aligned), a
    ld a, (at_ex)
    ld hl, S_PX
    cp (hl)
    jr z, +
    ld a, (at_ez)
    ld hl, S_PZ
    cp (hl)
    jr nz, ++
+:  ld a, 1
    ld (at_aligned), a
    ; and within reach: a clear shot, or a barrel in the way to hit
    ld a, (at_reach)
    ld h, a
    ld e, a
    call mul8                   ; reach^2
    ld de, (at_d2)
    or a
    sbc hl, de                  ; reach^2 - d2
    jr c, ++                    ; too far
    ld a, (at_ex)
    ld b, a
    ld a, (at_ez)
    ld c, a
    ld a, (S_PX)
    ld d, a
    ld a, (S_PZ)
    ld e, a
    ld hl, MASK_SHOT
    ld a, (at_i)
    call trace                  ; the line of fire to the player
    jr z, +++                   ; clear (the player isn't in MASK_SHOT)
    ld a, (hit_kind)
    cp H_ENT
    jr nz, ++
    ld a, (hit_val)
    call etype                  ; the first thing in the way
    cp ET_DESTRUCT
    jr nz, ++
    ld a, T_ENT
    ld (at_kind), a
    ld a, (hit_val)
    ld (at_tgt), a
+++:
    ld a, 1
    ld (at_canhit), a
++: ; move unless it can hit (and even then, sometimes, if it has range)
    ld a, (at_canhit)
    or a
    jr z, +
    ld a, (at_half)
    or a
    jp z, _at_queue             ; melee: attack
    call rnd
    cp 217
    jr c, _at_queue             ; with range: attack, unless rnd >= 217
+:  ; towards the cell in front of the player (or past it, when close)
    ld a, (S_FACING)
    and 3
    ld c, a
    ld hl, dir_dx
    call add_hl_a_read
    ld hl, S_PX
    add a, (hl)
    ld (at_tx), a
    ld a, c
    ld hl, dir_dz
    call add_hl_a_read
    ld hl, S_PZ
    add a, (hl)
    ld (at_tz), a
    ld a, (at_half)             ; d2 <= half^2 and aligned and rnd < 38
    ld h, a
    ld e, a
    call mul8                   ; half^2
    ld de, (at_d2)
    or a
    sbc hl, de
    jr c, +
    ld a, (at_aligned)
    or a
    jr z, +
    call rnd                    ; (drawn only when the two tests above pass)
    cp 38
    jr nc, +
    ld a, (S_PX)                ; one more cell along its side of the player
    ld b, a
    ld a, (at_ex)
    ld hl, at_tx
    call at_side                ; x
    ld a, (S_PZ)
    ld b, a
    ld a, (at_ez)
    ld hl, at_tz
    call at_side                ; z
+:  ld a, (at_tx)
    ld d, a
    ld a, (at_tz)
    ld e, a
    ld a, (at_i)
    call ai_move                ; try a step towards (at_tx, at_tz)
    jr z, +
    ld a, (at_i)                ; moved
    ld hl, S_E_MOVE
    call add_hl_a
    ld (hl), 0
    ld a, 1
    ret
+:  ld a, (at_canhit)
    or a
    ret z
_at_queue:
    ld a, (at_kind)
    ld b, a
    ld a, (at_tgt)
    ld c, a
    ld a, (at_i)
    call queue_attack           ; attack the player (or the barrel in the way)
    xor a
    ret

; a = monster's coordinate, b = the player's, hl = &target: target -= 1
; if a < b, += 1 if a > b; changes only (hl) and the flags
at_side:
    cp b
    ret z
    jr c, +
    inc (hl)
    ret
+:  dec (hl)
    ret

; a = monster that moved: its next steps this turn -> a = 1 (NZ) if it
; moved again
; S_E_MOVE[i] counts its steps this turn. Classes 4 and 13 take up to 3
; steps, the rest 1. Next to the player (same row or column, distance
; 1) it stops stepping. After its last step, a class that can lunge (1,
; 4, 5, 13) and is next to the player with a clear shot queues an attack
; on the player. Clobbers every register except IX, IY. Called by
; monster_turn.
ai_after_step:
    ld (as_i), a
    call esub                   ; a = its class
    ld c, 1                     ; steps: 3 for the fast kinds (4, 13)
    ld b, 0                     ; can lunge: kinds 5, 4, 1, 13
    cp 4
    jr z, +
    cp 13
    jr nz, ++
+:  ld c, 3
    inc b
    jr +++
++: cp 5
    jr z, +
    cp 1
    jr nz, +++
+:  inc b
+++:
    ld a, c
    ld (as_steps), a
    ld a, b
    ld (as_lunge), a
    ; adjacent: aligned and dist2 <= 1
    xor a
    ld (as_adj), a
    ld a, (as_i)
    ld hl, S_E_X
    call add_hl_a_read
    ld b, a
    ld a, (as_i)
    ld hl, S_E_Z
    call add_hl_a_read
    ld c, a
    ld a, (S_PX)
    ld d, a
    ld a, (S_PZ)
    ld e, a
    ld a, b
    cp d
    jr z, +
    ld a, c
    cp e
    jr nz, ++
+:  call dist2                  ; aligned: how far?
    ld a, h
    or a
    jr nz, ++
    ld a, l
    cp 2
    jr nc, ++
    ld a, 1
    ld (as_adj), a
++: ld a, (as_i)
    ld hl, S_E_MOVE
    call add_hl_a
    inc (hl)                    ; one more step taken
    ld a, (as_steps)
    ld b, a
    ld a, (hl)
    cp b
    jr nc, +                    ; move >= steps
    ld a, (as_adj)
    or a
    jr z, ++
    ld (hl), b                  ; adjacent: no more steps
    jr +
++: push hl
    ld a, (S_PX)
    ld d, a
    ld a, (S_PZ)
    ld e, a
    ld a, (as_i)
    call ai_move                ; a step straight towards the player
    pop hl
    ret nz                      ; moved again
+:  ; after its last step: a lunge at the player, if it can and is next to them
    ld a, (as_steps)
    cp (hl)
    jr nz, +
    ld a, (as_lunge)
    or a
    jr z, +
    ld a, (as_adj)
    or a
    jr z, +
    ld a, (as_i)
    ld hl, S_E_X
    call add_hl_a_read
    ld b, a
    ld a, (as_i)
    ld hl, S_E_Z
    call add_hl_a_read
    ld c, a
    ld a, (S_PX)
    ld d, a
    ld a, (S_PZ)
    ld e, a
    ld hl, MASK_SHOT
    ld a, (as_i)
    call trace                  ; a clear line to the player?
    jr nz, +
    ld b, T_PLAYER
    ld c, 0
    ld a, (as_i)
    call queue_attack           ; the lunge
+:  xor a
    ret

;-----------------------------------------------------------------------
; explosions and destruction (destroy -> run_explosions -> hurt_at ->
; destroy can nest: whatever must survive a nested call is on the stack)
;-----------------------------------------------------------------------
;-----------------------------------------------------------------------
; the cells an explosion reaches: (dx, dz) pairs, the four next to it,
; then the four diagonals
expl_near:
    .db -1, 0,  1, 0,  0, -1,  0, 1
expl_diag:
    .db -1, 1,  1, 1,  1, -1,  -1, -1

; until none are waiting: the first explosion hurts the cells next to it,
; then (half the damage, if any) the diagonals. Draws four random numbers each.
; Damage = rnd / 11 + 5 (5..28), the same for health and armour. The
; explosion's own cell is not hurt. A barrel it destroys pushes another
; explosion, handled in this same loop (destroy calls run_explosions
; again, which simply carries on with the queue). Clobbers every
; register except IX, IY.
run_explosions:
    ld a, (n_pend_expl)
    or a
    ret z
    ld a, (expl_x)              ; take the first
    ld d, a
    ld a, (expl_z)
    ld e, a
    push de
    ld hl, expl_x+1
    ld de, expl_x
    ld bc, 7
    ldir                        ; the queue moves up (x)
    ld hl, expl_z+1
    ld de, expl_z
    ld bc, 7
    ldir                        ; (z)
    ld hl, n_pend_expl
    dec (hl)
    call rnd32                  ; damage = (its first byte) / 11 + 5
    ld a, l
    ld e, 11
    call div8
    add a, 5
    ld c, a
    pop de
    ld hl, expl_near
    call expl_ring              ; the four next to it
    srl c                       ; half the damage
    jr z, run_explosions
    ld hl, expl_diag
    call expl_ring              ; the four diagonals
    jr run_explosions

; hl = 4 (dx, dz) pairs, d = x, e = z, c = damage: hurt_at each; keeps c, d, e
expl_ring:
    ld b, 4
-:  push bc
    push de
    push hl
    ld a, (hl)
    add a, d
    ld d, a
    inc hl
    ld a, (hl)
    add a, e
    ld e, a
    ld b, d                     ; hurt_at(x, z, damage, damage)
    ld a, c
    ld c, e
    ld d, a
    ld e, a
    call hurt_at                ; (may nest: destroy, more explosions)
    pop hl
    pop de
    pop bc
    inc hl
    inc hl
    djnz -
    ret

; b = x, c = z: an explosion waits there (at most 8; more are dropped)
; Clobbers a, de, hl.
push_explosion:
    ld a, (n_pend_expl)
    cp 8
    ret nc
    ld hl, n_pend_expl
    inc (hl)
    ld hl, expl_x
    call add_hl_a
    ld (hl), b
    ld de, 8
    add hl, de                  ; (expl_z follows expl_x)
    ld (hl), c
    ret

; b = x, c = z (may be off the map), d = damage, e = armour damage: the
; player, or the first thing there a shot would hit
; Nothing happens off the map, in a wall, or behind a closed door or prop
; that stops shots. Otherwise the first linked MASK_SHOT entity on the
; cell: nothing if it is not alive; a monster is hurt (and may die);
; fires (ET_FIRE) and destructibles of sub-types 3 and 4 are not
; affected; anything else is destroyed. Clobbers every register except
; IX, IY.
hurt_at:
    ld a, (S_PX)
    cp b
    jr nz, +
    ld a, (S_PZ)
    cp c
    jr nz, +
    ld l, d
    ld h, 0
    ld d, h                     ; hl = damage, de = armour damage
    jp player_pain
+:  ld a, d
    ld (ha_dmg), a
    ld a, e
    ld (ha_arm), a
    ld a, b
    or c
    and $E0
    ret nz                      ; off the map
    call wall_at
    ret nz
    ld a, b
    ld (ha_x), a
    ld a, c
    ld (ha_z), a
    call line_at                ; a closed door that stops shots
    cp $FF
    jr z, +
    LTAB LT_LINE_ETYPE
    call add_hl_a_read          ; the door's kind
    ld hl, MASK_SHOT
    call mask_has
    ret nz
+:  call cell_index             ; a prop that stops shots
    ex de, hl
    LTAB LT_STATIC_BLOCK
    add hl, de
    ld a, (hl)
    or a
    jr z, +
    ld hl, MASK_SHOT
    call mask_has
    ret nz
+:  ld e, 0
-:  ld a, (ha_x)
    ld b, a
    ld a, (ha_z)
    ld c, a
    call ent_at                 ; the next linked entity >= e
    cp $FF
    ret z
    ld (ha_j), a
    call etype
    ld hl, MASK_SHOT
    call mask_has               ; one a shot would hit?
    jr nz, +
    ld a, (ha_j)
    inc a
    ld e, a
    jr -
+:  ld a, (ha_j)
    ld hl, S_E_FLAGS
    call add_hl_a_read
    and F_ALIVE
    ret z
    ld a, (ha_j)
    call etype
    cp ET_ENEMY
    jr nz, +
    ld a, (ha_dmg)
    ld l, a
    ld h, 0
    ld a, (ha_arm)
    ld e, a
    ld d, h
    ld a, (ha_j)
    call monster_pain           ; a monster: hurt
    ld a, (ha_j)
    jp monster_hurt_or_died
+:  cp ET_FIRE
    ret z
    cp ET_DESTRUCT
    jr nz, +
    ld a, (ha_j)
    call esub
    cp 3
    ret z
    cp 4
    ret z
+:  ld a, (ha_j)
    jp destroy

; a = monster just hurt: "<NAME> DIED!" and its death, or the hurt sound
; (e = 1 if it was blown apart: no corpse; entry monster_hurt_or_died: 0)
; Dead means its slot's S_M_HP is 0. Clobbers every register except IX,
; IY.
monster_hurt_or_died:
    ld e, 0
monster_hurt_or_died_e:
    ld c, a
    push de
    push bc
    call ent_mon
    ld hl, S_M_HP
    call add_hl_a_read          ; its health
    pop bc
    pop de
    or a
    jr z, +
    ld a, SFX_MON_HURT
    jp sfx
+:  push de
    push bc
    ld a, c
    call ent_name
    ld de, str_died
    call say2                   ; "<NAME> DIED!"
    pop bc
    pop de
    ld a, c
    jp monster_died

; a = entity: destroyed (a fire put out, a barrel blown up, a crate broken
; open), then any explosions it set off
; Only if alive. It loses F_ALIVE and leaves the map. A fire (ET_FIRE):
; "FIRE CLEARED!" and 2 XP. A destructible of sub-type 1 (a barrel):
; an explosion on its cell. Sub-type 2 (a crate): one random number r:
; r < 2 trapped (an explosion), < 4 5 credits, < 12 1 credit, < 24 a
; small medkit, < 150 armour, < 213 ammo (one more random number for the
; type), else empty; the crate's entity becomes that drop (if the level
; has one) and goes back on the map. Then run_explosions. Uses ds_*
; only before the nested calls. Clobbers every register except IX, IY.
destroy:
    ld (ds_j), a
    ld hl, S_E_FLAGS
    call add_hl_a_read
    and F_ALIVE
    ret z
    ld a, (ds_j)
    call etype
    ld (ds_et), a
    ld a, (ds_j)
    call esub
    ld (ds_es), a
    ld a, (ds_j)
    ld hl, S_E_FLAGS
    call add_hl_a
    res 0, (hl)                 ; F_ALIVE
    ld a, (ds_j)
    call ent_unlink             ; off the map
    ld a, (ds_et)
    cp ET_FIRE
    jr nz, +
    ld hl, str_fire_cleared
    call say
    ld hl, 2
    call add_xp                 ; 2 XP for a fire
    jp run_explosions
+:  cp ET_DESTRUCT
    jp nz, run_explosions
    ld a, (ds_es)
    cp 1
    jr nz, +
    ld hl, str_barrel
    call say
    ld a, SFX_EXPLODE
    call sfx
    call ds_explode             ; queue an explosion on its cell
    jp run_explosions
+:  cp 2
    jp nz, run_explosions
    ; a crate: what was in it
    call rnd
    push af
    ld a, SFX_BREAK
    call sfx
    pop af
    cp 2
    jr nc, +
    ld hl, str_trapped
    call say
    ld a, SFX_EXPLODE
    call sfx
    call ds_explode             ; a trapped crate explodes
    jp run_explosions
+:  ld b, DD_CREDIT5
    cp 4
    jr c, +
    ld b, DD_CREDIT1
    cp 12
    jr c, +
    ld b, DD_SMMED
    cp 24
    jr c, +
    ld b, DD_ARMOR
    cp 150
    jr c, +
    cp 213
    jp nc, run_explosions       ; empty
    ld e, 5                     ; ammo: + rnd % 5
    call rnd_mod
    add a, DD_AMMO
    ld b, a
+:  LTAB LT_DROP_DEF
    ld a, b
    call add_hl_a_read          ; the level's def for that drop
    cp NO_DEF
    jp z, run_explosions        ; the level has none
    ld c, a
    ld a, (ds_j)
    ld hl, S_E_DEF
    call add_hl_a
    ld (hl), c                  ; the crate becomes the drop
    ld a, (ds_j)
    ld hl, S_E_FLAGS
    call add_hl_a
    ld (hl), 0
    ld a, (ds_j)
    call ent_link               ; back on the map
    jp run_explosions

; an explosion where entity ds_j is; clobbers a, bc, de, hl
ds_explode:
    ld a, (ds_j)
    ld hl, S_E_X
    call add_hl_a_read
    ld b, a
    ld a, (ds_j)
    ld hl, S_E_Z
    call add_hl_a_read
    ld c, a
    jp push_explosion

;-----------------------------------------------------------------------
; the monsters' turn
;-----------------------------------------------------------------------
; a = monster, b = kind (T_*), c = target: carries out a queued attack
; Only if the monster is still alive. T_RAISE: _ma_raise. Otherwise
; "<NAME> ATTACKS", its weapon's sound, then mon_shots[class] shots. At
; the player: calc_hit for each (one random number each), calc_damage
; for each hit (one more), a crit only on the first shot; the totals go
; to player_pain, or "DODGED!" if all missed. At an entity (a barrel in
; the way): no rolls; it is destroyed. Clobbers every register except
; IX, IY.
monster_attack:
    ld (ma_i), a
    ld a, b
    ld (ma_kind), a
    ld a, c
    ld (ma_tgt), a
    ld a, (ma_i)
    ld hl, S_E_FLAGS
    call add_hl_a_read
    and F_ALIVE
    ret z
    ld a, (ma_kind)
    cp T_RAISE
    jp z, _ma_raise
    ld a, (ma_i)
    call ent_name
    ld de, str_attacks
    call say2                   ; "<NAME> ATTACKS"
    ld a, (ma_i)
    call attack_weapon
    ld (ma_w), a
    ld hl, wpn_sfx
    call add_hl_a_read
    call sfx                    ; the weapon's sound
    ld a, (ma_i)
    call esub
    ld hl, mon_shots
    call add_hl_a_read
    ld (ma_shots), a
    ld a, (ma_i)
    call cb_monster             ; cb_* = the attacker's stats
    ld hl, 0
    ld (ma_tot), hl
    ld (ma_tarm), hl
    xor a
    ld (ma_crit), a
    ld (ma_s), a
_ma_shot:
    ld a, (ma_shots)
    ld hl, ma_s
    cp (hl)
    jr z, _ma_done
    jr c, _ma_done
    ld a, (ma_kind)
    cp T_PLAYER
    jr z, +
    ld hl, (ma_tot)             ; at a barrel: every shot counts
    inc hl
    ld (ma_tot), hl
    jr _ma_next
+:  ld a, (cb_acc)
    ld b, a
    ld a, (ma_w)
    ld c, a
    ld a, (S_AGILITY)
    ld d, a
    ld e, 0                     ; (no range check)
    call calc_hit
    or a
    jr z, _ma_next              ; missed
    cp HIT_CRIT
    jr nz, +
    ld a, (ma_s)
    or a
    jr nz, +
    ld a, 1
    ld (ma_crit), a
+:  ld a, (cb_str)
    ld (cd_str), a
    ld a, (ma_w)
    ld (cd_w), a
    ld a, (S_DEFENSE)
    ld (cd_def), a
    ld a, $FF
    ld (cd_class), a
    ld a, (ma_crit)
    ld (cd_crit), a
    xor a
    ld (cd_tiles), a
    call calc_damage            ; dmg_hp, dmg_arm for this shot
    ld hl, (ma_tot)
    ld de, (dmg_hp)
    add hl, de
    ld (ma_tot), hl
    ld hl, (ma_tarm)
    ld de, (dmg_arm)
    add hl, de
    ld (ma_tarm), hl
_ma_next:
    ld hl, ma_s
    inc (hl)
    jr _ma_shot
_ma_done:
    ld a, (ma_kind)
    cp T_PLAYER
    jr nz, +
    ld hl, (ma_tot)
    ld de, (ma_tarm)
    ld a, h
    or l
    or d
    or e
    jp nz, player_pain          ; hurt the player with the totals
    ld hl, str_dodged
    call say
    ld a, SFX_DODGE
    jp sfx
+:  ld a, (ma_tgt)
    jp destroy                  ; the barrel in the way

; an arch-vile raises corpse ma_tgt, if its cell is still free: the
; corpse is alive again with full health and armour, back on the map and
; active. Otherwise "RAISE FAILED!" and F_RAISING is cleared.
_ma_raise:
    ld a, (ma_i)
    call ent_name
    ld de, str_casts_raise
    call say2                   ; "<NAME> CASTS RAISE"
    ld a, SFX_RAISE
    call sfx
    ld a, (ma_tgt)
    ld hl, S_E_X
    call add_hl_a_read
    ld b, a
    ld d, a
    ld a, (ma_tgt)
    ld hl, S_E_Z
    call add_hl_a_read
    ld c, a
    ld e, a
    ld hl, MASK_RAISE_FREE
    ld a, $FF
    call trace                  ; anything on the corpse's cell now?
    jr z, +
    ld a, (ma_tgt)              ; something stands there now
    ld hl, S_E_FLAGS
    call add_hl_a
    res 7, (hl)                 ; F_RAISING
    ld hl, str_raise_failed
    jp say
+:  ld a, (ma_tgt)
    ld hl, S_E_FLAGS
    call add_hl_a
    ld a, (hl)
    and $3F                     ; not a corpse, not being raised
    or F_ALIVE
    ld (hl), a
    ld a, (ma_tgt)
    call ent_link               ; back on the map
    ld a, (ma_tgt)
    call ent_mon                ; its stat slot
    ld c, a
    ld b, 0
    ld hl, S_M_MAXHP
    add hl, bc
    ld a, (hl)
    ld hl, S_M_HP
    add hl, bc
    ld (hl), a
    ld hl, S_M_MAXARMOR
    add hl, bc
    ld a, (hl)
    ld hl, S_M_ARMOR
    add hl, bc
    ld (hl), a
    ld a, (ma_tgt)
    call activate               ; it takes turns again
    ld a, (ma_tgt)
    call ent_name
    ld de, str_revived
    jp say2                     ; "<NAME> IS REVIVED!"

; hl = a list, b = its length, a = a value -> Z if it is in the list
; Clobbers a, b, c, hl.
in_list:
    inc b
    dec b
    jr z, +
    ld c, a
-:  ld a, (hl)
    cp c
    ret z
    inc hl
    djnz -
+:  or 1                        ; NZ
    ret

; the monsters' turn (the model's three phases):
;   1. each active monster (as listed at the start) runs ai_think: it
;      moves or queues an attack; those that moved are kept in mt_movers
;   2. rounds of ai_after_step: in active-list order, each mover may
;      step again (fast classes) or queue a lunge; repeat with those
;      that moved again until none did
;   3. the queued attacks, in queue order, until the player is dead
; Then any monster that can now see the player wakes. Uses the shared
; block at $D100 (so strbuf is overwritten). Clobbers every register
; except IX, IY. Called by settle (events.asm).
monster_turn:
    xor a
    ld (att_n), a
    ld (mt_nm), a
    ld a, (S_N_ACTIVE)
    ld (mt_n), a
    or a
    jr z, ++
    ld c, a
    ld b, 0
    ld hl, S_ACTIVE
    ld de, mt_snap
    ldir                        ; a snapshot: the list can change during the turn
    ; each one decides (in the order of the list at the start of the turn)
    xor a
    ld (mt_k), a
-:  ld hl, mt_snap
    ld a, (mt_k)
    call add_hl_a_read
    push af
    call ai_think               ; move, or queue an attack
    pop bc
    or a
    jr z, +
    ld hl, mt_nm                ; it moved: it may move again
    ld a, (hl)
    inc (hl)
    ld hl, mt_movers
    call add_hl_a
    ld (hl), b
+:  ld hl, mt_k
    inc (hl)
    ld a, (mt_n)
    cp (hl)
    jr nz, -
++: ; the movers take their further steps, in active-list order
_mt_steps:
    ld a, (mt_nm)
    or a
    jr z, _mt_attacks
    xor a
    ld (mt_na), a
    ld (mt_k), a
-:  ld a, (mt_k)
    ld hl, S_N_ACTIVE
    cp (hl)
    jr nc, +
    ld hl, S_ACTIVE
    call add_hl_a_read
    ld d, a                     ; i
    ld hl, mt_movers
    ld a, (mt_nm)
    ld b, a
    ld a, d
    push de
    call in_list                ; did it move last round?
    pop de
    jr nz, ++
    ld a, d
    push de
    call ai_after_step          ; its further steps (or a lunge)
    pop de
    jr z, ++
    ld hl, mt_na
    ld a, (hl)
    inc (hl)
    ld hl, mt_again
    call add_hl_a
    ld (hl), d
++: ld hl, mt_k
    inc (hl)
    jr -
+:  ld a, (mt_na)
    ld (mt_nm), a
    or a
    jr z, _mt_attacks
    ld c, a
    ld b, 0
    ld hl, mt_again
    ld de, mt_movers
    ldir                        ; the next round's movers
    jr _mt_steps
_mt_attacks:                    ; phase 3: the queued attacks
    ld a, (att_n)
    ld (mt_n), a
    xor a
    ld (mt_k), a
-:  ld a, (mt_k)
    ld hl, mt_n
    cp (hl)
    jr nc, +
    ld a, (S_DEAD)
    or a
    jr nz, +
    ld a, (mt_k)
    ld hl, att_kind
    call add_hl_a_read
    ld b, a
    ld a, (mt_k)
    ld hl, att_tgt
    call add_hl_a_read
    ld c, a
    ld a, (mt_k)
    ld hl, att_ent
    call add_hl_a_read
    call monster_attack         ; a = monster, b = kind, c = target
    ld hl, mt_k
    inc (hl)
    jr -
+:  xor a
    ld (att_n), a
    jp activate_visible         ; wake those that see the player now

;-----------------------------------------------------------------------
; the sound of each weapon (wpn_* index): the player's and the monsters'
wpn_sfx:
    .db SFX_AXE, SFX_EXTINGUISHER, SFX_PISTOL, SFX_SHOTGUN, SFX_CHAINGUN, SFX_SHOTGUN
    .db SFX_PLASMA, SFX_ROCKET, SFX_BFG, SFX_MON_ATTACK, SFX_MON_ATTACK, SFX_MON_ATTACK
    .db SFX_MON_ATTACK, SFX_MON_ATTACK, SFX_MON_ATTACK, SFX_FIREBALL, SFX_FIREBALL
    .db SFX_FIREBALL, SFX_RAISE

str_gained:       .db "GAINED ", 0
str_health:       .db " HEALTH", 0
str_armor:        .db " ARMOR", 0
str_xp:           .db " XP!", 0
str_level_up:     .db "LEVEL UP! LEVEL ", 0
str_empty:        .db 0
str_damage:       .db " DAMAGE!", 0
str_near_death:   .db "NEAR DEATH!", 0
str_low_health:   .db "LOW HEALTH!", 0
str_armor_gone:   .db "ARMOR GONE!", 0
str_died:         .db " DIED!", 0
str_fire_cleared: .db "FIRE CLEARED!", 0
str_barrel:       .db "BARREL EXPLODES!", 0
str_trapped:      .db "TRAPPED!", 0
str_attacks:      .db " ATTACKS", 0
str_casts_raise:  .db " CASTS RAISE", 0
str_revived:      .db " IS REVIVED!", 0
str_raise_failed: .db "RAISE FAILED!", 0
str_dodged:       .db "DODGED!", 0
