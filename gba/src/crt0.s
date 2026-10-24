@ Deimos - Doom RPG on retro consoles
@ Copyright (C) 2026 unfnknblvbl and contributors
@ SPDX-License-Identifier: GPL-2.0-or-later
@ Free software under the GNU General Public License, version 2 or (at your
@ option) any later version, with NO WARRANTY: see the LICENSE file.

@ Doom RPG for the Game Boy Advance: the start. The cartridge's header
@ (its logo and checksum: tools/gba_rom.py), then the copies into the
@ memories (.iwram, .data from the ROM; .bss, .ewram_bss cleared) and
@ main. The interrupts: irq_entry, then plat.c's irq_dispatch.
@ Licence: GPL-2.0-or-later.
@
@ Linked first in the cartridge ($08000000) by src/gba.ld: the BIOS checks
@ the header and jumps to _start in ARM state. Memory used here:
@   $03007F00  the game's stack top (system mode; grows down into the fast
@              RAM above what .iwram and .iwram_bss use)
@   $03007FA0  the IRQ mode's stack top (small: irq_entry switches stacks)
@   $03007FF8  the BIOS's interrupt flags for IntrWait (irq_entry sets them)
@   $03007FFC  the BIOS's pointer to the game's interrupt handler
@   $04000200  IE (enabled interrupts) and IF (requested; write 1 to clear)
@   $04000204  WAITCNT (the cartridge's wait states)
    .section .crt0, "ax"
    .arm
    .global _start
_start:
    b       start
    .fill   156, 1, 0                   @ (the logo: gba_rom.py)
    .ascii  "DEIMOS      "              @ the title (12)
    .ascii  "BDRE"                      @ the game code
    .ascii  "01"                        @ the maker
    .byte   0x96, 0, 0                  @ the fixed $96; unit code; device type
    .fill   7, 1, 0                     @ reserved
    .byte   0                           @ version
    .byte   0                           @ checksum (gba_rom.py)
    .byte   0, 0                        @ reserved
@ start: the reset code. Inputs: none. Sets both stacks and the wait
@ states, fills the RAM sections, installs irq_entry, then calls main
@ (plat.c); if main returned it would spin here. Clobbers r0..r3, lr.
start:
    mov     r0, #0x12                   @ IRQ mode: its stack
    msr     cpsr_c, r0
    ldr     sp, =0x03007FA0
    mov     r0, #0x1F                   @ system mode: the game's stack
    msr     cpsr_c, r0
    ldr     sp, =0x03007F00
    @ the waits: ROM 3/1, prefetch on
    ldr     r0, =0x04000204
    ldr     r1, =0x4317                 @ (SRAM 8 cycles; ROM WS0 3 then 1; WS2 8; prefetch)
    strh    r1, [r0]
    @ .iwram and .data from the ROM
    ldr     r0, =__iwram_lma
    ldr     r1, =__iwram_start
    ldr     r2, =__iwram_end
    bl      copy                        @ the fast RAM's code and data
    ldr     r0, =__data_lma
    ldr     r1, =__data_start
    ldr     r2, =__data_end
    bl      copy                        @ the work RAM's initialised variables
    @ the cleared parts
    ldr     r1, =__bss_start
    ldr     r2, =__bss_end
    bl      clear
    ldr     r1, =__iwram_bss_start
    ldr     r2, =__iwram_bss_end
    bl      clear
    ldr     r1, =__ewram_bss_start
    ldr     r2, =__ewram_bss_end
    bl      clear
    ldr     r0, =0x03007FFC
    ldr     r1, =irq_entry
    str     r1, [r0]                    @ the BIOS calls irq_entry on an interrupt
    ldr     r0, =main
    mov     lr, pc                      @ (lr = the instruction after the bx)
    bx      r0                          @ main (plat.c), in Thumb: bx switches state
1:  b       1b

@ copy: words from r0 to r1 until r1 reaches r2. Advances r0 and r1;
@ clobbers r3. Called by start.
copy:                                   @ r0 source, r1..r2 destination (words)
    cmp     r1, r2
    bxhs    lr
    ldr     r3, [r0], #4
    str     r3, [r1], #4
    b       copy

@ clear: zero the words from r1 up to r2. Advances r1; clobbers r3.
clear:                                  @ r1..r2 (words)
    mov     r3, #0
1:  cmp     r1, r2
    bxhs    lr
    str     r3, [r1], #4
    b       1b

@ the interrupts (the BIOS calls this in IRQ mode, ARM): irq_dispatch(IF)
@ in system mode (the game's stack: the IRQ mode's is small), the
@ interrupts still off
    .section .iwram, "ax"
    .arm
    .global irq_entry
@ irq_entry: inputs IE and IF; output r0 (the interrupts taken) passed to
@ irq_dispatch; clears them in IF and adds them to the BIOS's flags. The
@ BIOS has saved r0..r3, r12 and lr; this saves SPSR and the IRQ lr, and
@ the system-mode lr. Code in the fast RAM (ARM).
irq_entry:
    mov     r3, #0x04000000
    ldr     r2, [r3, #0x200]!           @ IE | IF << 16
    and     r0, r2, r2, lsr #16         @ the ones that came
    strh    r0, [r3, #2]                @ acknowledged
    ldr     r1, =0x03007FF8
    ldrh    r2, [r1]
    orr     r2, r2, r0
    strh    r2, [r1]                    @ (the BIOS's IntrWait)
    mrs     r2, spsr                    @ the interrupted code's status
    stmfd   sp!, {r2, lr}               @ (on the IRQ stack)
    mov     r2, #0x9F                   @ system mode, IRQs still off
    msr     cpsr_c, r2
    stmfd   sp!, {lr}                   @ (the game's lr, on the game's stack)
    ldr     r1, =irq_dispatch
    mov     lr, pc
    bx      r1                          @ irq_dispatch(r0) in plat.c
    ldmfd   sp!, {lr}
    mov     r2, #0x92                   @ back to IRQ mode
    msr     cpsr_c, r2
    ldmfd   sp!, {r2, lr}
    msr     spsr_cxsf, r2
    bx      lr                          @ back to the BIOS, which returns from the interrupt
    .pool                               @ (the ldr = constants)
