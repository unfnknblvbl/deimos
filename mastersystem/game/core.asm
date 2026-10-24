; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

;=======================================================================
; Core helpers: the level blob, random numbers, small maths, the map's
; cells, the per-cell entity lists and the line trace.
;
; Everything else in game/ builds on these. Lives in bank 0 ($1000-$3FFF,
; always paged in). Nothing here calls the engine.
;
; The map is a grid of 32 x 32 cells; a cell is (x, z), both 0..31, and
; its index is z * 32 + x (0..1023). x grows to the east, z to the south.
; Per-cell bit sets in the level blob (wall_bits, line_cells) hold 4 bytes
; per row of 32 cells, the lowest bit of each byte for the lowest x.
;
; The level blob (bank 2 at $A000, docs/rom_format.md) is the rules' copy
; of the level: a 16-byte header, then a u16 offset per table. level_load
; turns those offsets into addresses in lv_tab, and the LTAB macro reads
; them back. Bank 2 must be in slot 2 when any of these tables is read;
; the engine's services page it back in, and only level_string (ui.asm)
; pages anything else there, briefly.
;
; Per-cell entity lists: cell_head[cell] holds 1 + the index of the first
; linked entity on that cell (0 = none), and ent_next[e] holds 1 + the
; index of the next one (0 = end). The lists are kept sorted by entity
; index, because the model visits entities in index order. An entity is
; "linked" (flag F_LINKED) when it is in its cell's list: it can then be
; seen, hit, touched or traced. Dead, hidden and picked-up entities are
; unlinked.
;
; The trace walks a straight row or column of cells and records what it
; meets (hit_n, hit_kind[], hit_val[]); a mask says which entity types
; count (see the MASK_* constants in game.asm).
;=======================================================================

.ENUM $D3A0                         ; (after the UI's scratch; actions.asm checks ui_ram_end <= $D3A0)
tr_x            db                  ; trace: current cell
tr_z            db
tr_dx           db                  ; last cell
tr_dz           db
tr_st           dw                  ; low byte = z step, high byte = x step (each 0, 1 or -1)
tr_mask         dw                  ; which entity types count (bit n = type n; bit 8 = the player)
tr_self         db                  ; an entity the trace ignores (the mover itself), $FF none
tr_tmp          db                  ; a door or prop type being tested
core_ram_end    db
.ENDE

.IF shared_end > $D3A0
.FAIL "the shared block runs into the trace variables at $D3A0"
.ENDIF

;-----------------------------------------------------------------------
; the level blob (docs/rom_format.md): a 16-byte header with the counts
; and spawn point, then a u16 offset (from $A000) for each table
;-----------------------------------------------------------------------
; level_load: copies the blob's counts and spawn point into the game
; variables (n_defs .. spawn_f, in the order of game.asm's .ENUM) and
; fills lv_tab with the absolute address of each table.
; In: the blob paged in at LEVEL_ADDR ($A000).
; Out: n_defs, n_map_ents, n_ents (= n_map_ents + N_DROPS), n_mons,
;      n_lines, n_events, n_cmds, n_strings, spawn_x/z/f, lv_tab[].
; Clobbers a, b, c, d, e, h, l. Called by game_init (actions.asm).
level_load:
    ld hl, LEVEL_ADDR+5         ; header: "LVL1" (4), version (1), then the counts
    ld de, n_defs
    ld a, (hl)                  ; defs
    ld (de), a
    inc hl
    inc de
    ld a, (hl)                  ; map entities
    ld (de), a
    inc de
    add a, N_DROPS              ; + the drop slots
    ld (de), a                  ; n_ents
    inc hl
    inc de
    ld bc, 8                    ; monsters, lines, events, cmds, strings, spawn x, z, f
    ldir
    ; table addresses = $A000 + their offsets
    ld hl, LEVEL_ADDR+16
    ld de, lv_tab
    ld b, LEVEL_N_TABLES
-:  ld a, (hl)                  ; low byte of the offset + low byte of $A000
    add a, <LEVEL_ADDR
    ld (de), a
    inc hl
    inc de
    ld a, (hl)                  ; high byte, with the carry
    adc a, >LEVEL_ADDR
    ld (de), a
    inc hl
    inc de
    djnz -
    ret

; LTAB which: hl = the address of level blob table `which` (an LT_*
; constant, a macro argument, not a register). Reads lv_tab; changes
; only hl.
.MACRO LTAB ARGS which
    ld hl, (lv_tab + 2*which)
.ENDM

; hl += a (a unsigned). Leaves a = the new l; keeps bc, de.
; Used everywhere to index a byte array: hl = array, a = index.
add_hl_a:
    add a, l
    ld l, a
    ret nc
    inc h
    ret

; a = entity -> a = its type (def_etype[e_def[i]], an ET_* value); keeps
; bc, de; clobbers hl. Note: a drop slot that was never used has def
; NO_DEF ($FF), which reads past the table; callers only ask about linked
; entities or map entities.
etype:
    ld hl, S_E_DEF
    call add_hl_a               ; hl = &e_def[i]
    ld a, (hl)
    LTAB LT_DEF_ETYPE
    jp add_hl_a_read            ; a = def_etype[def]

; a = entity -> a = its sub-type (def_esub[e_def[i]]); keeps bc, de;
; clobbers hl. For a monster the sub-type is its class (the row of
; mon_base, mon_atk, mon_shots and dmg_mult); for a pickup, its kind.
esub:
    ld hl, S_E_DEF
    call add_hl_a               ; hl = &e_def[i]
    ld a, (hl)
    LTAB LT_DEF_ESUB
; add_hl_a_read: hl = a byte array, a = index -> a = hl[a], hl = &hl[a];
; keeps bc, de. (Shared tail of etype / esub; called on its own too.)
add_hl_a_read:
    add a, l
    ld l, a
    jr nc, +
    inc h
+:  ld a, (hl)
    ret

; a = entity -> a = its monster slot (ent_mon[i], 0..MAX_MONS-1, or $FF
; for a non-monster); keeps bc, de; clobbers hl. The slot indexes the
; S_M_* stat arrays.
ent_mon:
    LTAB LT_ENT_MON
    jr add_hl_a_read

; a = def -> hl = its name (NUL-terminated upper case, in the level blob);
; keeps bc; clobbers de. def_name starts with a u16 offset per def,
; counted from the start of the table.
def_name:
    ld e, a
    ld d, 0
    LTAB LT_DEF_NAME
    push hl
    add hl, de
    add hl, de                  ; hl = &offset[def]
    ld e, (hl)
    inc hl
    ld d, (hl)
    pop hl
    add hl, de                  ; table start + offset
    ret

; a = entity -> hl = the name of its def; keeps bc; clobbers de
ent_name:
    ld hl, S_E_DEF
    call add_hl_a               ; hl = &e_def[i]
    ld a, (hl)
    jr def_name

; a = def -> dehl = def_parm[def] (signed 32-bit, little-endian in the
; blob: hl = low word, de = high word); keeps bc. The "parm" is the
; def's amount: a monster's stat scale (x/256), a pickup's amount, a key
; number, a destructible's weapon mask...
def_parm:
    LTAB LT_DEF_PARM
    ld e, a
    ld d, 0
    add hl, de
    add hl, de
    add hl, de
    add hl, de                  ; hl = &parm[def] (4 bytes each)
    ld e, (hl)
    inc hl
    ld d, (hl)
    inc hl
    push de                     ; (the low word)
    ld e, (hl)
    inc hl
    ld d, (hl)                  ; de = the high word
    pop hl
    ret

;-----------------------------------------------------------------------
; random numbers: xorshift16, x ^= x << 7; x ^= x >> 9; x ^= x << 8
; The state is the 16-bit word S_RNG (part of the compared game state;
; game_init seeds it with $1234). The model uses the same generator, so
; every call here must happen in the same order as in the model.
;-----------------------------------------------------------------------
; -> a = the next byte (the low byte of the new state), S_RNG updated;
; keeps bc, de; clobbers hl
rnd:
    ld hl, (S_RNG)
    ; x ^= x << 7: the shifted word is (h0 l7..l1, l0 0000000)
    ld a, h
    rra                         ; carry = bit 0 of h
    ld a, l
    rra                         ; a = high byte of x << 7 (h bit 0, then l bits 7..1)
    xor h
    ld h, a
    ld a, l
    rrca
    and $80                     ; low byte of x << 7
    xor l
    ld l, a
    ; x ^= x >> 9
    ld a, h
    srl a                       ; (x >> 9 = h >> 1, in the low byte only)
    xor l
    ld l, a
    ; x ^= x << 8
    xor h                       ; (x << 8 = the new l, into the high byte)
    ld h, a
    ld (S_RNG), hl
    ld a, l
    ret

; four random bytes, the first drawn in the lowest -> dehl (l = 1st,
; h = 2nd, e = 3rd, d = 4th); keeps bc
rnd32:
    call rnd                    ; byte 0
    ld l, a
    push hl
    call rnd                    ; byte 1 (rnd clobbers hl)
    pop hl
    ld h, a
    push hl
    call rnd                    ; byte 2
    ld e, a
    push de
    call rnd                    ; byte 3
    pop de
    ld d, a
    pop hl
    ret

;-----------------------------------------------------------------------
; small maths
;-----------------------------------------------------------------------
; a / e -> a = quotient, b = remainder (unsigned 8-bit; e > 0); keeps c,
; hl, e; clobbers d. Restoring shift-and-subtract division, 8 rounds.
div8:
    ld b, a
    ld d, 8
    xor a                       ; a = remainder, b shifts into the quotient
-:  sla b                       ; next dividend bit (top first) into the remainder
    rla
    cp e
    jr c, +                     ; remainder < divisor: quotient bit 0
    sub e
    inc b                       ; quotient bit 1 (b's low bit, just freed by the shift)
+:  dec d
    jr nz, -
    ld d, b                     ; swap: a = quotient, b = remainder
    ld b, a
    ld a, d
    ret

; h * e -> hl (unsigned 8 x 8); keeps bc, e; clobbers a, d (= 0).
; Shift-and-add: h's bits come out at the top of hl as the product
; shifts in at the bottom.
mul8:
    ld d, 0
    ld l, d
    ld a, 8
-:  add hl, hl                  ; product <<= 1, carry = next bit of the multiplier
    jr nc, +
    add hl, de
+:  dec a
    jr nz, -
    ret

; hl, de signed: hl = floor(hl / de), de > 0 (Python's //, which rounds
; towards minus infinity, unlike C's division). Clobbers a, bc, de.
fdiv16:
    bit 7, h
    jr nz, +
    ld b, d
    ld c, e
    jp div16                    ; non-negative: plain division
+:  ; negative: -( (-hl + de - 1) / de )
    call negw                   ; hl = -hl
    add hl, de
    dec hl
    ld b, d
    ld c, e
    call div16                  ; (-hl + de - 1) / de, rounded down
    jp negw                     ; negated back: floor of the original

; hl / bc -> hl = quotient, de = remainder (unsigned 16-bit, bc > 0);
; keeps bc; clobbers a. Restoring division, 16 rounds: the dividend
; shifts out of de's top while the quotient bits shift in at its bottom.
div16:
    ex de, hl                   ; de = dividend
    ld hl, 0                    ; hl = the running remainder
    ld a, 16
-:  sla e
    rl d
    adc hl, hl                  ; remainder = remainder * 2 + next dividend bit
    or a
    sbc hl, bc
    jr nc, +                    ; it fits: quotient bit 1
    add hl, bc                  ; it doesn't: undo the subtraction
    jr ++
+:  inc e                       ; set the quotient bit
++: dec a
    jr nz, -
    ex de, hl                   ; hl = quotient, de = remainder
    ret

; hl = -hl (two's complement); clobbers a; keeps bc, de
negw:
    xor a
    sub l
    ld l, a
    sbc a, a                    ; a = -borrow
    sub h
    ld h, a
    ret

; a = |a| for a signed byte (-128 stays -128); changes only a and flags
abs8:
    bit 7, a
    ret z
    neg
    ret

; hl = signed byte a, sign-extended; a = $FF if negative, else 0
sext:
    ld l, a
    rla                         ; carry = the sign bit
    sbc a, a                    ; a = $FF or 0
    ld h, a
    ret

; bit numbers -> masks: bit_mask[n] = 1 << n (n = 0..7)
bit_mask:
    .db 1, 2, 4, 8, 16, 32, 64, 128

; hl = a 16-bit mask, a = bit (0..255) -> a = bit set (0/1), NZ if set;
; bits 16 and up always read as clear; keeps bc, de; clobbers hl.
; Used with the MASK_* constants: a = an entity type (ET_*), a door's
; kind or a prop type.
mask_has:
    cp 16
    jr nc, +++                  ; past the mask: clear
    cp 8
    jr c, +
    sub 8
    ld l, h                     ; the high byte
+:  push hl
    ld hl, bit_mask
    call add_hl_a               ; hl = &bit_mask[bit & 7]
    ld a, (hl)
    pop hl
    and l
    ret z
    ld a, 1
    ret
+++: xor a
    ret

;-----------------------------------------------------------------------
; the map's cells
;-----------------------------------------------------------------------
; b = x, c = z (both < 32) -> hl = z * 32 + x (0..1023); keeps bc, de;
; clobbers a
cell_index:
    ld l, c
    ld h, 0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl                  ; z * 32
    ld a, l
    or b                        ; + x (x < 32 fits in the free low 5 bits)
    ld l, a
    ret

; hl = a bit per cell (128 bytes: 4 per row), b = x, c = z (both < 32)
; -> a = the cell's bit (0/1), NZ if set; keeps bc; clobbers de, hl.
; Bit (x & 7) of byte z * 4 + x / 8.
bit_of:
    ld a, c
    add a, a
    add a, a                    ; z * 4 bytes a row
    ld e, a
    ld a, b
    rrca
    rrca
    rrca
    and 3                       ; x / 8
    add a, e
    ld e, a
    ld d, 0
    add hl, de                  ; hl = the byte
    ld a, b
    and 7
    ld e, (hl)
    jr z, +
-:  srl e                       ; shift the cell's bit down to bit 0
    dec a
    jr nz, -
+:  ld a, e
    and 1
    ret

; b = x, c = z -> a = 1 (NZ) if a wall fills the cell (off the map counts); keeps bc
; (x or z >= 32: either has a bit in $E0, so the cell is off the map;
; negative coordinates wrap to 224..255 and count as off too.)
wall_at:
    ld a, b
    or c
    and $E0
    jr nz, +
    LTAB LT_WALL_BITS
    jp bit_of                   ; the level's solid-cell bits
+:  ld a, 1
    or a
    ret

; b = x, c = z -> a = the closed door / line on the cell, or $FF; keeps bc;
; clobbers de, hl. line_cells (1 bit per cell) is a quick test before the
; linear search through the lines; an open door (L_OPEN) does not count.
line_at:
    ld a, b
    or c
    and $E0
    jr nz, ++                   ; off the map
    LTAB LT_LINE_CELLS
    call bit_of                 ; any line on this cell at all?
    jr z, ++
    ld a, (n_lines)
    or a
    jr z, ++
    ld d, a                     ; d = lines left to look at
    ld e, 0                     ; e = line
-:  LTAB LT_LINE_X
    ld a, e
    call add_hl_a_read          ; line_x[e]
    cp b
    jr nz, +
    LTAB LT_LINE_Z
    ld a, e
    call add_hl_a_read          ; line_z[e]
    cp c
    jr nz, +
    ld hl, S_L_FLAGS
    ld a, e
    call add_hl_a_read          ; its flags
    and L_OPEN
    ld a, e
    ret z                       ; closed: this one
+:  inc e
    dec d
    jr nz, -
++: ld a, $FF
    ret

;-----------------------------------------------------------------------
; linked entities: per-cell lists sorted by entity index (the model goes
; through entities in index order). Every change of an entity's cell or
; of its F_LINKED flag goes through ent_link / ent_unlink / ent_move_to.
;-----------------------------------------------------------------------
; c = entity -> hl = &cell_head[its cell]; keeps bc (b is set to 0);
; clobbers a, de
ent_cell:
    ld b, 0
    ld hl, S_E_Z
    add hl, bc
    ld a, (hl)                  ; z
    ld hl, S_E_X
    add hl, bc
    ld e, (hl)                  ; x
    ld l, a
    ld h, 0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl                  ; z * 32
    ld a, l
    or e                        ; + x
    ld l, a
    ld de, cell_head
    add hl, de
    ret

; c = entity: into its cell's list, keeping the list sorted by index;
; keeps c; clobbers a, b, de, hl. Does not touch F_LINKED (ent_link does).
list_insert:
    call ent_cell               ; hl = &cell_head[cell]
-:  ld a, (hl)                  ; p = &cell_head: walk while *p - 1 < i
    or a
    jr z, +                     ; end of the list
    dec a
    cp c
    jr nc, +                    ; the next one's index >= i: insert here
    ld hl, ent_next
    call add_hl_a               ; p = &ent_next[that one]
    jr -
+:  ld a, (hl)                  ; ent_next[i] = *p, *p = i + 1
    ex de, hl
    ld hl, ent_next
    ld b, 0
    add hl, bc
    ld (hl), a
    ld a, c
    inc a
    ld (de), a
    ret

; c = entity: out of its cell's list (nothing happens if it is not in
; it); keeps c; clobbers a, b (= 0), de, hl. Does not touch F_LINKED.
list_remove:
    call ent_cell               ; hl = &cell_head[cell]
-:  ld a, (hl)
    or a
    ret z                       ; end of the list: not there
    dec a
    cp c
    jr z, +                     ; *p points at i
    ld hl, ent_next
    call add_hl_a               ; p = &ent_next[that one]
    jr -
+:  ex de, hl                   ; *p = ent_next[i], ent_next[i] = 0
    ld hl, ent_next
    ld b, 0
    add hl, bc
    ld a, (hl)
    ld (hl), b
    ld (de), a
    ret

; a = entity: linked (in its cell's list, F_LINKED set), if it isn't;
; -> c = entity; clobbers a, b, de, hl
ent_link:
    ld c, a
    ld hl, S_E_FLAGS
    ld b, 0
    add hl, bc
    bit 2, (hl)                 ; F_LINKED
    ret nz
    set 2, (hl)
    jr list_insert

; a = entity: unlinked (out of its cell's list, F_LINKED clear), if it
; is; -> c = entity; clobbers a, b, de, hl
ent_unlink:
    ld c, a
    ld hl, S_E_FLAGS
    ld b, 0
    add hl, bc
    bit 2, (hl)                 ; F_LINKED
    ret z
    res 2, (hl)
    jr list_remove

; a = entity, d = x, e = z: moved there (S_E_X / S_E_Z); a linked entity
; is taken out of its old cell's list and put into the new one's.
; -> c = entity; clobbers a, b, de, hl
ent_move_to:
    ld c, a
    ld hl, S_E_FLAGS
    ld b, 0
    add hl, bc
    bit 2, (hl)                 ; F_LINKED
    push af                     ; (Z = not linked)
    push de
    call nz, list_remove        ; out of the old cell's list
    pop de
    ld b, 0
    ld hl, S_E_X
    add hl, bc
    ld (hl), d
    ld hl, S_E_Z
    add hl, bc
    ld (hl), e
    pop af
    ret z
    jr list_insert              ; into the new cell's list

; b = x, c = z, e = from -> a = the first linked entity >= from on the
; cell, or $FF (also for a cell off the map); keeps bc, e; clobbers d, hl.
; To visit every entity on a cell: e = 0, then e = the last result + 1.
ent_at:
    ld a, b
    or c
    and $E0
    jr nz, ++                   ; off the map
    call cell_index
    ld a, l
    add a, <cell_head
    ld l, a
    ld a, h
    adc a, >cell_head
    ld h, a                     ; hl = &cell_head[cell]
    ld a, (hl)
-:  or a
    jr z, ++                    ; end of the list
    dec a
    cp e
    ret nc                      ; index >= from
    ld hl, ent_next
    call add_hl_a
    ld a, (hl)                  ; the next one (+1)
    jr -
++: ld a, $FF
    ret

;-----------------------------------------------------------------------
; Game_trace: walks a row or a column from (sx, sz) to (dx, dz) and lists
; what it meets (hit_kind / hit_val, at most 8): stops at a wall or, if
; the mask's bit 8 is set, the player.
; b = sx, c = sz, d = dx, e = dz, hl = mask, a = an entity to ignore ($FF
; none) -> a = hit_n (Z if nothing)
;
; The start cell is examined too. If x differs the trace steps along x
; only (z stays sz), otherwise along z: callers only trace along a row or
; a column. In each cell, in this order:
;   a wall (or off the map): recorded as H_WALL (no value), the trace ends
;   the player (only if mask bit 8 is set): H_PLAYER, the trace ends
;   a closed door whose kind (line_etype) is in the mask: H_LINE, value =
;     the line
;   a blocking prop (static_block, a type) in the mask: H_STATIC, value =
;     its type
;   every linked entity whose type is in the mask, except the ignored
;     one, in index order: H_ENT, value = the entity
; The trace also ends after the cell (dx, dz) or when 8 hits are listed.
; Clobbers every register except IX, IY.
;-----------------------------------------------------------------------
trace:
    ld (tr_mask), hl
    ld (tr_self), a
    ld a, b
    ld (tr_x), a
    ld a, c
    ld (tr_z), a
    ld a, d
    ld (tr_dx), a
    ld a, e
    ld (tr_dz), a
    xor a
    ld (hit_n), a
    ; like the model: a column trace when x differs, else a row trace
    ld hl, 0                    ; h = x step, l = z step
    ld a, b
    cp d
    jr z, +
    ld h, 1                     ; sx < dx: east
    jr c, ++
    ld h, $FF                   ; sx > dx: west (-1)
    jr ++
+:  ld a, c
    cp e
    jr z, ++                    ; the same cell: no step (only the start is looked at)
    ld l, 1                     ; sz < dz: south
    jr c, ++
    ld l, $FF                   ; north (-1)
++: ld (tr_st), hl
_tr_cell:                       ; look at cell (tr_x, tr_z)
    ld a, (tr_x)
    ld b, a
    ld a, (tr_z)
    ld c, a
    call wall_at
    jr z, +
    ld e, H_WALL                ; a wall ends it
    jp _tr_last
+:  ld a, (tr_mask+1)           ; the player (mask bit 8)?
    rra                         ; carry = bit 8
    jr nc, +
    ld a, (S_PX)
    cp b
    jr nz, +
    ld a, (S_PZ)
    cp c
    jr nz, +
    ld e, H_PLAYER
    jp _tr_last
+:  call line_at                ; a closed door
    cp $FF
    jr z, +
    ld (tr_tmp), a
    LTAB LT_LINE_ETYPE
    call add_hl_a_read          ; its kind (0 door, 9 wall, 12 breakable)
    ld hl, (tr_mask)
    call mask_has               ; does that kind count?
    jr z, +
    ld a, (tr_tmp)
    ld e, H_LINE
    call hit_add                ; record the line
+:  call cell_index             ; a blocking prop
    ex de, hl
    LTAB LT_STATIC_BLOCK
    add hl, de                  ; hl = &static_block[cell]
    ld a, (hl)
    or a
    jr z, +                     ; 0: no prop
    ld (tr_tmp), a
    ld hl, (tr_mask)
    call mask_has               ; does the prop's type count?
    jr z, +
    ld a, (tr_tmp)
    ld e, H_STATIC
    call hit_add                ; record the prop's type
+:  ld e, 0                     ; the entities on the cell
-:  call ent_at                 ; the next linked one with index >= e
    cp $FF
    jr z, ++
    ld e, a
    ld hl, tr_self
    cp (hl)
    jr z, +                     ; the one to ignore
    push de
    call etype
    ld hl, (tr_mask)
    call mask_has               ; does its type count?
    pop de
    jr z, +
    ld a, e
    push de
    ld e, H_ENT
    call hit_add                ; record the entity
    pop de
+:  inc e
    jr -
++: ld a, (hit_n)
    cp 8
    ret nc                      ; (8: full)
    ld hl, (tr_st)
    ld a, h
    or a
    jr z, +
    ld a, (tr_dx)               ; column trace: done at the last x
    cp b
    jr nz, ++
    ld a, (hit_n)
    or a
    ret
+:  ld a, (tr_dz)               ; row trace: done at the last z
    cp c
    jr nz, ++
    ld a, (hit_n)
    or a
    ret
++: ld a, b                     ; step to the next cell
    add a, h
    ld (tr_x), a
    ld a, c
    add a, l
    ld (tr_z), a
    jp _tr_cell
_tr_last:                       ; e = kind: the last hit (no value)
    ; (room is certain: a full list returns before the next cell)
    ld hl, hit_n
    ld a, (hl)
    inc (hl)
    ld hl, hit_kind
    call add_hl_a
    ld (hl), e                  ; hit_val[] is left as it was
    ld a, (hit_n)
    or a
    ret

; a = value, e = kind: one more hit (hit_kind[hit_n] = e, hit_val[hit_n]
; = a, hit_n + 1) if there is room for it (at most 8); keeps bc, e;
; clobbers a, d, hl. Called only by trace.
hit_add:
    ld d, a
    ld a, (hit_n)
    cp 8
    ret nc
    ld hl, hit_kind
    call add_hl_a
    ld (hl), e
    ld a, (hit_n)
    ld hl, hit_val
    call add_hl_a
    ld (hl), d
    ld hl, hit_n
    inc (hl)
    ret
