/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the SNES: where things are in the ROM, per build (as banks.inc). */
/* The C side of banks.inc: only what C needs. SUPERFX is defined (-D
 * SUPERFX) by the SuperFX build scripts. The cartridge header holds two
 * bytes the build writes (tools/snes_rom.py): HEADER_AT + 6 is the
 * campaign block's first bank, HEADER_AT + 7 the resources' bank. */
#ifndef BANKS_H
#define BANKS_H
#ifdef SUPERFX
/* the GSU cartridge: the header at $7FB0 of the first ROM bank, read
   through the 64 KB-bank mirror at $40 */
#define HEADER_AT 0x407FB0UL
#else
/* HiROM: the header at $FFB0 of bank $C0 */
#define HEADER_AT 0xC0FFB0UL
#endif
#endif
