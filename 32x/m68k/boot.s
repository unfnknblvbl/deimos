| Deimos - Doom RPG on retro consoles
| Copyright (C) 2026 unfnknblvbl and contributors
| SPDX-License-Identifier: GPL-2.0-or-later
| Free software under the GNU General Public License, version 2 or (at your
| option) any later version, with NO WARRANTY: see the LICENSE file.

| Doom RPG for the 32X: the cartridge's start (the 68000's vectors, the
| Mega Drive header, the 32X's jump table and header) and the 68000's
| first code at $3F0, which switches the 32X on and goes on at $880800+
| (the cartridge as the 68000 sees it with the 32X on: m68k/main.c).
| Licence: GPL-2.0-or-later.
|
| Layout of the cartridge's first $400 bytes (this file is linked first, at
| $880000, by m68k/m68k.ld; the ROM offset is the address minus $880000):
|   $000  68000 vector table: initial stack, reset PC, 62 other vectors
|   $100  Mega Drive cartridge header (names, serial, ROM/RAM ranges, the
|         battery RAM's range, region); tools/make_rom.py fills in the
|         checksum ($18E) and the ROM's end ($1A4)
|   $200  the 32X's jump table: with the 32X on, its own vector table points
|         each 68000 exception at $880200 + 6 * n; each entry here is a
|         6-byte absolute JMP
|   $3C0  the 32X header: the "MARS CHECK MODE " name, then where the SH-2
|         code lies and where each SH-2 starts (make_rom.py fills these from
|         the SH-2 link's symbols)
|   $3F0  the 68000's reset code (start), which jumps to main in main.c
| Nothing calls into this file; the CPU's reset does. It calls main.

    .section .boot,"ax"
    .global _boot
_boot:
| $000: the vectors (the 32X's own replace them once it is on).
| Vector 0 is the initial stack pointer (top of work RAM, $FFFFF0); vector 1
| the reset PC ($3F0, start below). The other 62 all point at the jump
| table's first entry ($880200), which is only reachable once the 32X is on;
| the 68000 runs with interrupts masked, so none are expected.
    .long   0x00FFFFF0, 0x000003F0
    .rept   62
    .long   0x00880200
    .endr
| $100: the Mega Drive header (each field at its standard offset)
    .ascii  "SEGA 32X        "  | $100 system name (16 bytes)
    .ascii  "(C)UNFK 2026.OCT"  | $110 copyright and date
    .ascii  "DEIMOS                                          "  | $120 domestic name (48)
    .ascii  "DEIMOS                                          "  | $150 overseas name (48)
    .ascii  "GM 00000032-00"    | $180 type and serial number
    .word   0                   | $18E checksum (make_rom.py writes it)
    .ascii  "J6              "  | $190 I/O support: J pad, 6 the 6-button pad
    .long   0x00000000, 0x003FFFFF  | $1A0 ROM start, end (make_rom.py writes the real end)
    .long   0x00FF0000, 0x00FFFFFF  | $1A8 work RAM start, end
    .ascii  "RA"                | $1B0 external RAM present
    .byte   0xF8, 0x20          | battery-backed RAM on odd bytes only
    .long   0x00200001, 0x00203FFF  | $1B4 its range: 8 KB, one byte per word
    .ascii  "            "      | $1BC modem field (unused)
    .ascii  "Made by AI. Directed by unfnknblvbl.    "  | $1C8 notes (40 bytes)
    .ascii  "JUE             "  | $1F0 regions: Japan, USA, Europe
| $200: the 32X's vectors jump here (6 bytes each): all to a do-nothing
| handler (the 68000 runs with its interrupts off). Each JMP uses the
| absolute address of rte_here as the 68000 sees it with the 32X on.
    .rept   64
    jmp     0x00880000 + rte_here - _boot
    .endr
| rte_here: the shared exception handler. Returns at once; touches nothing.
rte_here:
    rte
    .balign 16
    .org    0x3C0
| $3C0: the 32X's header (m68k/../tools/make_rom.py fills $3D4..$3EF)
    .ascii  "MARS CHECK MODE "
    .long   0                   | $3D0 (left 0)
    .long   0, 0, 0             | the SH-2s' code: ROM offset, SDRAM offset, size
    .long   0, 0                | the master's and the slave's start
    .long   0, 0                | their vector tables
| $3F0: the 68000 starts here
    .org    0x3F0
| start: the reset entry. Inputs: none. Clobbers d0 and sp; never returns.
| It masks interrupts, satisfies the TMSS, turns the 32X adapter on, then
| continues at the same code's $880000 copy and enters main().
start:
    move.w  #0x2700, %sr        | supervisor mode, interrupt mask 7 (all off)
    move.b  0xA10001, %d0       | TMSS: the version register; low 4 bits 0 means no TMSS
    andi.b  #0x0F, %d0
    beq.s   1f                  | an early console: no unlock needed
    move.l  #0x53454741, 0xA14000   | write "SEGA" to the TMSS register to unlock the VDP
1:  move.b  #1, 0xA15101        | the 32X on (the cartridge moves to $880000)
    | (adapter control, low byte: bit 0 turns the adapter on; the SH-2s stay
    | in reset until main.c sets bit 1)
    jmp     0x00880000 + go - _boot | continue in the cartridge's new place
go: lea     0x00FFFFF0, %sp     | stack at the top of work RAM
    jmp     main                | the main loop in m68k/main.c (never returns)
