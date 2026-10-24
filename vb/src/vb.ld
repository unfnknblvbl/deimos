/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the Virtual Boy: where things go.
 *   rom    the code and the constants, from the cartridge's start
 *          ($07000000), at most 512 KB (the SNES game's data follows:
 *          tools/vb_rom.py)
 *   wram   the 64 KB work RAM: .data (stored after the code, copied by
 *          crt0.s), .bss (zeroed by crt0.s), the stack at the top
 *          (__stack_top, growing down; 4 KB are kept free for it)
 *   vram   the VIP's memory the game does not show ($20000..$3D7FF, the
 *          background maps' place: the VIP's own drawing is off): .vbss,
 *          the big buffers (vb.h's VBSS; zeroed by crt0.s)
 *   vec    the interrupt vectors (crt0.s's .vectors), the cartridge's
 *          last 512 bytes: tools/vb_rom.py takes them from their own
 *          binary and puts them at the end of the ROM
 * Licence: GPL-2.0-or-later. */
OUTPUT_ARCH(v810)
ENTRY(_start)
MEMORY {
    rom (rx) : ORIGIN = 0x07000000, LENGTH = 512K
    wram (rwx) : ORIGIN = 0x05000000, LENGTH = 64K - 4K
    vram (rw) : ORIGIN = 0x00020000, LENGTH = 0x1D800
    vec (rx) : ORIGIN = 0x07FFFE00, LENGTH = 512
}
SECTIONS {
    .text : {
        KEEP(*(.text.start))
        *(.text .text.*)
        *(.rodata .rodata.* .rdata)
        . = ALIGN(4);
    } > rom
    .data : {
        __data_start = .;
        *(.data .data.* .sdata .sdata.*)
        . = ALIGN(4);
        __data_end = .;
    } > wram AT > rom
    __data_lma = LOADADDR(.data);
    __rom_end = __data_lma + SIZEOF(.data);
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > wram
    .vbss (NOLOAD) : {
        __vbss_start = .;
        *(.vbss .vbss.*)
        . = ALIGN(4);
        __vbss_end = .;
    } > vram
    .vectors : { KEEP(*(.vectors)) } > vec
    __stack_top = 0x05010000;
    __gp = 0x05000000;
    /DISCARD/ : { *(.comment) *(.note*) }
}
