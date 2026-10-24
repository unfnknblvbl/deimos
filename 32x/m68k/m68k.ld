/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * The 68000's link script for the 32X cartridge. Everything is linked at
 * $880000: with the 32X adapter on, the 68000 sees the cartridge's first
 * 512 KB there (m68k/boot.s switches it on and jumps up). The result is
 * flattened by objcopy to obj/m68k.bin, which tools/make_rom.py places at
 * the cartridge's offset 0; it must stay under $4000 bytes, where the SH-2
 * code starts.
 *
 * Sections: .boot (m68k/boot.s: vectors, headers, reset code) must come
 * first so that its fixed offsets ($100, $200, $3C0, $3F0) land right; then
 * all code and constants. main.c keeps no variables (its loop state lives in
 * registers), so .data and .bss are expected to be empty: anything placed
 * there would be in ROM and not writable.
 */
OUTPUT_FORMAT("elf32-m68k")
ENTRY(_boot)                /* the cartridge's first byte (boot.s) */
SECTIONS {
    . = 0x880000;           /* the cartridge with the 32X on */
    .boot : { *(.boot) }
    .text : { *(.text*) *(.rodata*) *(.data*) *(.bss*) *(COMMON) }
}
