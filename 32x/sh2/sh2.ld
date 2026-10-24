/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * The SH-2s' link script for the 32X. Two memories:
 *   rom    the cartridge as the SH-2 sees it ($02000000 is cartridge offset
 *          0); the SH-2 code's image starts at offset $4000 (after the
 *          68000's code) and must end before $80000, where the game's data
 *          starts (tools/make_rom.py)
 *   sdram  the 32X's 256 KB of SDRAM at $06000000; the top $4000 bytes are
 *          left for the stacks and the go flag (sh2/crt0.s)
 * Sections:
 *   .sdram  code and data that run from SDRAM: crt0.s's start
 *           (.sdram_start, which also holds the vector table), any code
 *           put in a .sdram_text section (no C file does so at present),
 *           and all initialised data. It is stored in the cartridge at the
 *           start of rom (AT> rom) and the 32X boot ROM copies it to SDRAM
 *           (make_rom.py puts its ROM offset and size in the 32X header).
 *   .text   all other code and the constants, run straight from the
 *           cartridge (slower: the cartridge bus is shared with the 68000)
 *   .bss    zeroed by crt0.s; not in the ROM image (build.sh drops it)
 * Symbols for other files: __sdram_lma and __sdram_size (make_rom.py),
 * __bss_start and __bss_end (crt0.s), __rom_end (the end of the code in
 * the cartridge; nothing reads it at present).
 */
OUTPUT_FORMAT("elf32-sh")
ENTRY(_master_start)
MEMORY {
    rom (rx) : ORIGIN = 0x02004000, LENGTH = 0x7C000
    sdram (rwx) : ORIGIN = 0x06000000, LENGTH = 0x3C000
}
SECTIONS {
    .sdram : {
        *(.sdram_start)
        *(.sdram_text*)
        *(.data*)
        *(.sdata*)
        . = ALIGN(16);
    } > sdram AT> rom
    __sdram_lma = LOADADDR(.sdram);
    __sdram_size = SIZEOF(.sdram);
    .text : { *(.text*) *(.rodata*) . = ALIGN(16); } > rom
    __rom_end = .;
    .bss (NOLOAD) : { __bss_start = .; *(.bss*) *(.sbss*) *(COMMON) . = ALIGN(4); __bss_end = .; } > sdram
}
