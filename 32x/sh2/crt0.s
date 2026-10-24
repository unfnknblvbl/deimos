! Deimos - Doom RPG on retro consoles
! Copyright (C) 2026 unfnknblvbl and contributors
! SPDX-License-Identifier: GPL-2.0-or-later
! Free software under the GNU General Public License, version 2 or (at your
! option) any later version, with NO WARRANTY: see the LICENSE file.

! Doom RPG for the 32X: the SH-2s' start (the boot ROM copies the SDRAM
! image, this included, and jumps here). The master clears the .bss and
! runs master_main; the slave waits for it and runs slave_main.
! Licence: GPL-2.0-or-later.
!
! Where it fits: the 32X's boot ROM reads the cartridge's 32X header (set by
! tools/make_rom.py from this file's symbols), copies the SDRAM image (the
! .sdram section of sh2/sh2.ld, which starts with this file's code) to
! SDRAM at $06000000, sets each SH-2's VBR to _master_vbr or _slave_vbr and
! jumps to _master_start or _slave_start. master_main is in sh2/plat.c,
! slave_main in sh2/sound32x.c.
!
! Memory used here (SDRAM is $06000000..$0603FFFF, 256 KB):
!   up to $0603C000  code, data and .bss (sh2.ld's limit)
!   $0603F800        the master's stack top (grows down)
!   $06040000        the slave's stack top (grows down: 2 KB above the
!                    master's)
!   $0603FFF0        the "go" flag, written through the cache-through
!                    alias $2603FFF0 so that both CPUs see it at once
!   $FFFFFE92        CCR, each SH-2's own cache control register
! Address bit 29 ($20000000) selects the cache-through view of the same
! memory on the SH-2.
    .section .sdram_start,"ax"
    .global _master_start, _slave_start, _master_vbr, _slave_vbr

! _master_start: the master SH-2's reset entry. Inputs: none. Sets r15 (the
! stack), turns the cache on, zeroes .bss a long at a time
! (__bss_start..__bss_end, from sh2.ld), raises the go flag for the slave,
! then calls master_main. Clobbers r0..r2. Never returns: if master_main
! did, it would spin at 3.
_master_start:
    mov.l   m_stack, r15
    mov.l   ccr, r1
    mov     #0x10, r0               ! the cache: purged, then on
    mov.b   r0, @r1                 ! (CCR $10: CP, purge all entries)
    mov     #1, r0
    mov.b   r0, @r1                 ! (CCR $01: CE, cache enabled)
    mov.l   bss_s, r0
    mov.l   bss_e, r1
    mov     #0, r2
1:  cmp/hs  r1, r0                  ! r0 >= the end: done
    bt      2f
    mov.l   r2, @r0
    bra     1b
    add     #4, r0                  ! (in the branch's delay slot: runs before the branch)
2:  mov.l   go_flag, r1
    mov.l   go_val, r0
    mov.l   r0, @r1                 ! "GOGO": the slave may start
    mov.l   m_main, r0
    jsr     @r0                     ! master_main (sh2/plat.c): the game
    nop                             ! (delay slot)
3:  bra     3b
    nop

! _slave_start: the slave SH-2's reset entry. Inputs: none. Sets r15,
! turns its cache on (purge then enable, as the master), waits until the
! master has cleared .bss and written "GOGO" to the go flag, then calls
! slave_main. Clobbers r0..r2. Never returns.
_slave_start:
    mov.l   s_stack, r15
    mov.l   ccr, r1
    mov     #0x10, r0
    mov.b   r0, @r1
    mov     #1, r0
    mov.b   r0, @r1
    mov.l   go_flag, r1
    mov.l   go_val, r2
4:  mov.l   @r1, r0                 ! spin until the go flag holds "GOGO"
    cmp/eq  r0, r2
    bf      4b
    mov.l   s_main, r0
    jsr     @r0                     ! slave_main (sh2/sound32x.c): the sound and half the 3D view
    nop
5:  bra     5b
    nop

! an exception: stop here (every vector of both tables points here; the
! CPU hangs so that a debugger shows where)
_trap:
    bra     _trap
    nop

! the constants the code above loads with mov.l (PC-relative)
    .align  2
m_stack: .long  0x0603F800
s_stack: .long  0x06040000
ccr:    .long   0xFFFFFE92           ! CCR, the cache control register
bss_s:  .long   __bss_start
bss_e:  .long   __bss_end
go_flag: .long  0x2603FFF0           ! (cache-through)
go_val: .long   0x474F474F           ! "GOGO"
m_main: .long   _master_main
s_main: .long   _slave_main

! both SH-2s' vector tables: one table of 128 entries, shared (both labels
! name the same address). It is in SDRAM, so it can be changed: every entry
! starts as _trap, and slave_main (sh2/sound32x.c) later puts its PWM
! interrupt handler in entry 67 (level-6 auto-vector). Because the table is
! shared, the master's entry 67 changes too; the master never enables that
! interrupt, so it does not matter.
    .align  2
_master_vbr:
_slave_vbr:
    .rept   128
    .long   _trap
    .endr
