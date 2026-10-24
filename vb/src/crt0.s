/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the Virtual Boy: the start-up and the interrupt vectors.
 *
 * _start (the CPU arrives from the reset vector at $FFFFFFF0): the stack
 * at the top of the work RAM, the ROM's wait states down to one, a moment
 * for the work RAM to settle, the instruction cache on, .data copied from
 * the cartridge, .bss and .vbss cleared (vb.ld's symbols), then main
 * (plat.c), which does not return.
 *
 * The vectors (.vectors: the last 512 bytes of the cartridge, mirrored at
 * $FFFFFE00 where the CPU looks for them; tools/vb_rom.py puts them there):
 * 16 bytes each. The timer's ($FE10) and the VIP's ($FE40) push r1 and
 * jump to irq_timer / irq_vip below; the others return at once (reti).
 * irq_timer / irq_vip save the registers a C function may change (r1 was
 * saved by the vector; r6..r19, r30, r31), call plat.c's timer_irq or
 * vip_irq, put them back and return from the interrupt (reti restores
 * the PC and PSW the CPU saved in EIPC / EIPSW).
 *
 * The compiler (v810-gcc) puts an underscore before every C name: main is
 * _main here. Registers: sp = r3, gp = r4 (unused: no small data area),
 * lp = r31 the return address.
 * Licence: GPL-2.0-or-later. */

	.section .text.start, "ax"
	.global _start
_start:
	movhi	hi(__stack_top), r0, sp		/* the stack: down from the work RAM's top */
	movea	lo(__stack_top), sp, sp
	movhi	hi(__gp), r0, gp
	movea	lo(__gp), gp, gp
	movhi	0x0200, r0, r6			/* r6 = $02000000: the hardware control */
	mov	3, r7
	st.b	r7, 0x24[r6]			/* WCR = 3: one wait state for the ROM and the expansion */
	movea	0x2000, r0, r6			/* the work RAM settles (as Nintendo's start-up waits) */
1:	add	-1, r6
	bne	1b
	mov	2, r6				/* CHCW = 2: ICE, the 1 KB instruction cache on */
	ldsr	r6, chcw
	/* .data: from its load address in the cartridge to the work RAM */
	movhi	hi(__data_lma), r0, r6
	movea	lo(__data_lma), r6, r6
	movhi	hi(__data_start), r0, r7
	movea	lo(__data_start), r7, r7
	movhi	hi(__data_end), r0, r8
	movea	lo(__data_end), r8, r8
2:	cmp	r8, r7
	bnl	3f				/* (r7 >= r8: done) */
	ld.w	0[r6], r9
	st.w	r9, 0[r7]
	add	4, r6
	add	4, r7
	br	2b
	/* .bss (the work RAM) and .vbss (the VIP's spare memory) zeroed */
3:	movhi	hi(__bss_start), r0, r7
	movea	lo(__bss_start), r7, r7
	movhi	hi(__bss_end), r0, r8
	movea	lo(__bss_end), r8, r8
	jal	zero_words
	movhi	hi(__vbss_start), r0, r7
	movea	lo(__vbss_start), r7, r7
	movhi	hi(__vbss_end), r0, r8
	movea	lo(__vbss_end), r8, r8
	jal	zero_words
	ldsr	r0, psw				/* PSW = 0: out of the reset's NMI state, interrupts on, any level */
	jal	_main				/* the game (plat.c) */
4:	br	4b

/* zero_words: zero the words from r7 up to r8 (both word-aligned); changes r7 */
zero_words:
	cmp	r8, r7
	bnl	5f
	st.w	r0, 0[r7]
	add	4, r7
	br	zero_words
5:	jmp	[lp]

/* the interrupts' common part: called from a vector with r1 pushed; r1 =
   the C function to call */
	.macro	IRQ_ENTRY name, handler
	.global	\name
\name:
	addi	-64, sp, sp			/* room for 16 registers */
	st.w	r6, 0[sp]
	st.w	r7, 4[sp]
	st.w	r8, 8[sp]
	st.w	r9, 12[sp]
	st.w	r10, 16[sp]
	st.w	r11, 20[sp]
	st.w	r12, 24[sp]
	st.w	r13, 28[sp]
	st.w	r14, 32[sp]
	st.w	r15, 36[sp]
	st.w	r16, 40[sp]
	st.w	r17, 44[sp]
	st.w	r18, 48[sp]
	st.w	r19, 52[sp]
	st.w	r30, 56[sp]
	st.w	lp, 60[sp]
	jal	\handler			/* the C handler (plat.c) */
	ld.w	0[sp], r6
	ld.w	4[sp], r7
	ld.w	8[sp], r8
	ld.w	12[sp], r9
	ld.w	16[sp], r10
	ld.w	20[sp], r11
	ld.w	24[sp], r12
	ld.w	28[sp], r13
	ld.w	32[sp], r14
	ld.w	36[sp], r15
	ld.w	40[sp], r16
	ld.w	44[sp], r17
	ld.w	48[sp], r18
	ld.w	52[sp], r19
	ld.w	56[sp], r30
	ld.w	60[sp], lp
	addi	64, sp, sp
	ld.w	0[sp], r1			/* (pushed by the vector) */
	add	4, sp
	reti
	.endm

	.section .text
	IRQ_ENTRY irq_timer, _timer_irq
	IRQ_ENTRY irq_vip, _vip_irq

/* ---- the vectors: 16 bytes each, from $FFFFFE00 ---- */
	.macro	VEC_RETI			/* an interrupt not used: straight back */
	reti
	.fill	14, 1, 0
	.endm
	.macro	VEC_JUMP target			/* push r1, jump to target (16 bytes) */
	add	-4, sp
	st.w	r1, 0[sp]
	movhi	hi(\target), r0, r1
	movea	lo(\target), r1, r1
	jmp	[r1]
	.endm

	.section .vectors, "ax"
	VEC_RETI				/* $FE00 the pad */
	VEC_JUMP irq_timer			/* $FE10 the timer: the sound's frame */
	VEC_RETI				/* $FE20 the expansion port */
	VEC_RETI				/* $FE30 the link port */
	VEC_JUMP irq_vip			/* $FE40 the VIP: a frame starts */
	.fill	0xFF60 - 0xFE50, 1, 0		/* (unused) */
	VEC_RETI				/* $FF60 a floating-point exception */
	.fill	16, 1, 0
	VEC_RETI				/* $FF80 a division by zero */
	VEC_RETI				/* $FF90 an illegal opcode */
	VEC_RETI				/* $FFA0 trap 0..15 */
	VEC_RETI				/* $FFB0 trap 16..31 */
	VEC_RETI				/* $FFC0 an address trap */
	VEC_RETI				/* $FFD0 a duplexed exception */
	VEC_RETI				/* $FFE0 (unused) */
	movhi	hi(_start), r0, r1		/* $FFF0 the reset: to _start */
	movea	lo(_start), r1, r1
	jmp	[r1]
	.fill	6, 1, 0
