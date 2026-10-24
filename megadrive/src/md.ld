/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* the Mega Drive game linker script: ROM at 0, work RAM at $FF0000.
 *
 * build.sh links every object of src/ and build/gen/ with this script into
 * build/deimos.elf; objcopy makes the flat binary and
 * tools/fix_header.py writes the checksum (out/deimos.md).
 *
 * The ROM, in order:
 *   $000  the 68000's exception vectors (crt0.s .text.vectors)
 *   $100  the cartridge header (crt0.s .text.header), including at $1C8
 *         the "MDRPGDIR" directory: the campaign block's address, the end
 *         of the space it may use, the areas table's address
 *   $200  the start-up code (crt0.s .text.start), then all code and
 *         read-only data, then the initial values of .data
 *   .assets  big generated tables (compiled scalers, UI graphics, sound)
 *   .pack    the campaign block and the areas table, last
 * The work RAM ($FF0000-$FFFFFF, 64 KB): .data (copied from ROM by
 * crt0.s), then .bss (cleared by crt0.s); the stack grows down from the
 * top ($1000000, the initial stack pointer in the vectors).
 */
OUTPUT_FORMAT("elf32-m68k")
ENTRY(_start)                   /* the reset vector's target (crt0.s) */
MEMORY
{
    rom (rx) : ORIGIN = 0x000000, LENGTH = 0x400000
    ram (rw) : ORIGIN = 0xFF0000, LENGTH = 0x00FE00   /* top 512 bytes: stack */
}
SECTIONS
{
    .text : {
        KEEP(*(.text.vectors))      /* 64 vectors (256 bytes) at address 0 */
        . = 0x100;
        KEEP(*(.text.header))       /* the header must start at $100 */
        . = 0x200;
        KEEP(*(.text.start))        /* _start right after the header */
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(2);
    } > rom
    /* initialised variables: run in RAM, stored in ROM (AT > rom) at
       __data_load; crt0.s copies __data_start..__data_end from there */
    .data : {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(2);
        __data_end = .;
    } > ram AT > rom
    __data_load = LOADADDR(.data);
    /* zeroed variables: crt0.s clears all of work RAM at reset */
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(2);
        __bss_end = .;
    } > ram
    /* big read-only data the tools generate (levels, textures, sprites) */
    .assets : {
        . = ALIGN(2);
        *(.assets .assets.*)
    } > rom
    /* the campaign block (tools/md_campaign.py) and the areas table
       (tools/md_areas.c), both from build/gen/campaign.s, last, on a 64 KB
       boundary: the editor replaces them, so nothing may follow it */
    .pack : {
        . = ALIGN(0x10000);
        KEEP(*(.pack))
    } > rom
    __rom_end = .;                  /* (the end of the ROM image) */
    /DISCARD/ : { *(.comment) *(.note*) }   /* compiler notes: not in the ROM */
}
