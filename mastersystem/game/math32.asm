; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

;=======================================================================
; Wider arithmetic for the combat formulas (calc_damage): 24-, 32- and
; 40-bit products, unsigned unless the name says otherwise. m32a is the
; 32-bit accumulator (little-endian); m32b and m32c are scratch.
;
; Bank 0. Called from combat.asm (calc_damage, calc_hit, setup_enemy);
; calls only mul8 (core.asm). m32a/m32b/m32c live in game.asm's
; variables; m40 and the counters below. calc_damage works in 8.8 fixed
; point (256 = 1.0), so most results are shifted right by 8 afterwards.
;=======================================================================

.ENUM $D3B0
m40             dsb 5               ; a 40-bit product (mul32_8), little-endian
mm_k            db                  ; mul32_8: the multiplier
mm_n            db                  ; mul32_8: the carry byte into the next column
math_ram_end    db
.ENDE

; hl * a -> a:hl (24-bit: a = the top byte); clobbers bc, de (de = the
; old hl). Shift-and-add, multiplier bits from the top.
mul_hl_a:
    ex de, hl                   ; de = multiplicand
    ld c, a                     ; c = multiplier
    xor a
    ld h, a
    ld l, a                     ; a:hl = 0
    ld b, 8
-:  add hl, hl                  ; a:hl <<= 1
    rla
    sla c                       ; next multiplier bit, most significant first
    jr nc, +
    add hl, de                  ; a:hl += multiplicand
    adc a, 0
+:  djnz -
    ret

; hl * de -> dehl (32-bit, unsigned); clobbers a, bc, m32c (bytes 0..2).
; Two 16 x 8 products: x * e, plus x * d one byte higher.
mulw:
    push hl
    push de
    ld a, e
    call mul_hl_a               ; x * e
    ld (m32c), hl
    ld (m32c+2), a
    pop de
    pop hl
    ld a, d
    call mul_hl_a               ; x * d, added one byte up
    ld b, a
    ld a, (m32c+1)
    add a, l
    ld c, a                     ; byte 1
    ld a, (m32c+2)
    adc a, h
    ld e, a                     ; byte 2
    ld a, b
    adc a, 0
    ld d, a                     ; byte 3
    ld a, (m32c)
    ld l, a                     ; byte 0
    ld h, c
    ret

; m32a * a -> m40 (40-bit, unsigned); m32a unchanged; clobbers a, bc,
; de, hl. Byte by byte (schoolbook), from the lowest, with the high byte
; of each partial product carried into the next.
mul32_8:
    ld (mm_k), a
    xor a
    ld (mm_n), a                ; the carry into the next byte
    ld hl, m32a
    ld de, m40
    ld b, 4                     ; four bytes of m32a
-:  push bc
    push hl
    push de
    ld h, (hl)
    ld a, (mm_k)
    ld e, a
    call mul8                   ; hl = byte * k (at most $FE01)
    ld a, (mm_n)
    add a, l                    ; + the carry from the byte below
    ld l, a
    ld a, h
    adc a, 0                    ; (cannot overflow: $FE01 + $FF < $10000)
    ld (mm_n), a                ; the high byte carries into the next column
    pop de
    ld a, l
    ld (de), a                  ; this column's byte of m40
    inc de
    pop hl
    inc hl
    pop bc
    djnz -
    ld a, (mm_n)
    ld (de), a                  ; the fifth byte
    ret

; m32a = (m32a * de) >> 8, the low 32 bits:
; (x * e) >> 8 + x * d, from two 40-bit products. Clobbers a, bc, de,
; hl, m32b, m40. Used to multiply by an 8.8 fixed-point factor in de.
; (The bits that (x * e) >> 8 drops are lost: the same truncation as the
; model's.)
mul32x16_shr8:
    push de
    ld a, e
    call mul32_8                ; m40 = x * e
    ld hl, m40+1                ; m32b = (x * e) >> 8
    ld de, m32b
    ld bc, 4
    ldir
    pop de
    ld a, d
    call mul32_8                ; m40 = x * d
    ld hl, m40                  ; m32a = x * d (low 32 bits) ...
    ld de, m32a
    ld bc, 4
    ldir
    ; ... + m32b (falls through into add32)
; m32a += m32b (32-bit, carry out lost); clobbers a, b, de, hl
add32:
    ld hl, m32a
    ld de, m32b
    ld b, 4
    or a                        ; no carry into the lowest byte
-:  ld a, (de)
    adc a, (hl)
    ld (hl), a
    inc hl
    inc de
    djnz -
    ret

; m32a -= m32b (32-bit two's complement); clobbers a, bc, de, hl
sub32:
    ld hl, m32a
    ld de, m32b
    ld b, 4
    or a                        ; no borrow into the lowest byte
-:  ld a, (de)
    ld c, a
    ld a, (hl)
    sbc a, c
    ld (hl), a
    inc hl
    inc de
    djnz -
    ret

; m32a >>= 8 (unsigned): bytes 1..3 move down a byte, the top becomes 0;
; clobbers a, bc, de, hl. Turns an 8.8 x 8.8 product back into 8.8.
shr8_32:
    ld hl, m32a+1
    ld de, m32a
    ld bc, 3
    ldir
    xor a
    ld (m32a+3), a
    ret

; m32a = dehl (hl the low word); changes nothing else
set32:
    ld (m32a), hl
    ld (m32a+2), de
    ret
