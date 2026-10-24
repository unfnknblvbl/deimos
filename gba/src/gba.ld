/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the Game Boy Advance: where things go. The code and the
 * constants in the cartridge (from $08000000; the SNES game's data after
 * them, tools/gba_rom.py); the variables in the 256 KB work RAM; the
 * pixel loops and the busiest tables in the 32 KB fast RAM (.iwram,
 * .iwram_data, .iwram_bss: gba.h's IWRAM_CODE, IWRAM_DATA).
 * Licence: GPL-2.0-or-later. */
/* Sections, in order: .text (crt0.s's header and start first, then all
 * code and constants) in the cartridge; .iwram (ARM code and data for the
 * fast RAM, stored in the cartridge, copied by crt0.s); .iwram_bss and .bss
 * / .ewram_bss (zeroed by crt0.s); .data (stored in the cartridge, copied to
 * the work RAM). __rom_end: where the code's image ends in the cartridge.
 * The code must stay within the cartridge's first 512 KB: the SNES data
 * starts there (tools/gba_rom.py). The fast RAM's last $200 bytes are left
 * for the stacks and the BIOS's variables (crt0.s). */
OUTPUT_ARCH(arm)
ENTRY(_start)
MEMORY {
    rom (rx) : ORIGIN = 0x08000000, LENGTH = 512K
    ewram (rwx) : ORIGIN = 0x02000000, LENGTH = 256K
    iwram (rwx) : ORIGIN = 0x03000000, LENGTH = 0x7E00
}
SECTIONS {
    .text : {
        KEEP(*(.crt0))
        *(.text .text.* .glue_7 .glue_7t .vfp11_veneer .v4_bx)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > rom
    .ARM.exidx : { *(.ARM.exidx*) } > rom     /* (unwind tables, if any) */
    .iwram : {
        __iwram_start = .;
        *(.iwram .iwram.*)
        *(.iwram_data)
        . = ALIGN(4);
        __iwram_end = .;
    } > iwram AT > rom
    __iwram_lma = LOADADDR(.iwram);
    .iwram_bss (NOLOAD) : {
        __iwram_bss_start = .;
        *(.iwram_bss)
        . = ALIGN(4);
        __iwram_bss_end = .;
    } > iwram
    .data : {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end = .;
    } > ewram AT > rom
    __data_lma = LOADADDR(.data);
    __rom_end = __data_lma + SIZEOF(.data);
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > ewram
    .ewram_bss (NOLOAD) : {
        __ewram_bss_start = .;
        *(.ewram_bss)
        . = ALIGN(4);
        __ewram_bss_end = .;
    } > ewram
    /DISCARD/ : { *(.comment) *(.note*) *(.ARM.attributes) }
}
