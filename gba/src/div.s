@ Deimos - Doom RPG on retro consoles
@ Copyright (C) 2026 unfnknblvbl and contributors
@ SPDX-License-Identifier: GPL-2.0-or-later
@ Free software under the GNU General Public License, version 2 or (at your
@ option) any later version, with NO WARRANTY: see the LICENSE file.

@ Doom RPG for the Game Boy Advance: the divisions the C code calls
@ (__aeabi_uidiv, __aeabi_idiv and their mod forms), in ARM code in the
@ fast RAM: the libgcc ones are Thumb code in the cartridge, several times
@ slower. The divisor is lined up with the dividend (a binary search for
@ the shift), then one step a quotient bit, jumped into at that bit.
@ Licence: GPL-2.0-or-later.
    .section .iwram, "ax"
    .arm
    .align 2

@ r0 = n / d, r1 = n % d (unsigned; d = 0 gives 0, n)
@ __aeabi_uidiv / __aeabi_uidivmod: inputs r0 n, r1 d. Outputs r0 the
@ quotient, r1 the remainder (the EABI's uidivmod pair; uidiv callers just
@ ignore r1). Clobbers r2, r3 and the flags. Called by GCC's code for
@ every unsigned / and %.
    .global __aeabi_uidiv
    .global __aeabi_uidivmod
    .type __aeabi_uidiv, %function
    .type __aeabi_uidivmod, %function
__aeabi_uidiv:
__aeabi_uidivmod:
    cmp     r1, #0                      @ d = 0: 0, n (no exception)
    moveq   r1, r0
    moveq   r0, #0
    bxeq    lr
@ udiv_core: the shared loop (also called by __aeabi_idiv). First shift d
@ left by r2 bits, the largest shift (found 16, 8, 4, 2, 1 at a time) with
@ d still <= n; then r2 + 1 restoring-division steps, each comparing,
@ subtracting and shifting a quotient bit into r3.
udiv_core:                              @ (r0 n, r1 d != 0) -> r0 q, r1 r; r2, r3 used
    cmp     r1, r0
    bhi     2f                          @ d > n: q 0
    mov     r2, #0
    cmp     r1, r0, lsr #16
    movls   r1, r1, lsl #16
    addls   r2, r2, #16
    cmp     r1, r0, lsr #8
    movls   r1, r1, lsl #8
    addls   r2, r2, #8
    cmp     r1, r0, lsr #4
    movls   r1, r1, lsl #4
    addls   r2, r2, #4
    cmp     r1, r0, lsr #2
    movls   r1, r1, lsl #2
    addls   r2, r2, #2
    cmp     r1, r0, lsr #1
    movls   r1, r1, lsl #1
    addls   r2, r2, #1
    mov     r3, #0
    rsb     r2, r2, #31                 @ (r2 + 1 steps: from step 31 - r2)
    add     pc, pc, r2, lsl #4          @ (pc reads 8 ahead, i.e. the first step; each step is 16 bytes)
    nop                                 @ (skipped: pc+8 is past it)
    .rept   32
    cmp     r0, r1                      @ n >= d (carry set)?
    subcs   r0, r0, r1                  @ then take it off
    adc     r3, r3, r3                  @ q = q * 2 + that carry
    mov     r1, r1, lsr #1              @ d to the next bit down
    .endr
    mov     r1, r0                      @ r1 the remainder
    mov     r0, r3                      @ r0 the quotient
    bx      lr
2:  mov     r1, r0
    mov     r0, #0
    bx      lr

@ signed: the quotient towards 0, the remainder with the dividend's sign
@ __aeabi_idiv / __aeabi_idivmod: inputs r0 n, r1 d (signed). Outputs r0
@ the quotient, r1 the remainder. Divides the magnitudes with udiv_core,
@ then fixes the signs. Clobbers r2, r3, flags; saves r4. d = 0 gives 0, n.
    .global __aeabi_idiv
    .global __aeabi_idivmod
    .type __aeabi_idiv, %function
    .type __aeabi_idivmod, %function
__aeabi_idiv:
__aeabi_idivmod:
    cmp     r1, #0
    moveq   r1, r0
    moveq   r0, #0
    bxeq    lr
    stmfd   sp!, {r4, lr}
    eor     r4, r0, r1                  @ bit 31: the quotient's sign
    and     r4, r4, #0x80000000
    orr     r4, r4, r0, lsr #31         @ bit 0: the dividend's
    cmp     r0, #0
    rsblt   r0, r0, #0                  @ |n|
    cmp     r1, #0
    rsblt   r1, r1, #0                  @ |d|
    bl      udiv_core                   @ |n| / |d|, |n| % |d|
    tst     r4, #0x80000000
    rsbne   r0, r0, #0                  @ the quotient's sign
    tst     r4, #1
    rsbne   r1, r1, #0                  @ the remainder's sign
    ldmfd   sp!, {r4, lr}
    bx      lr
