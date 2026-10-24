/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the Virtual Boy: the machine's part (plat.c), as the
 * game's code (the Game Boy Color's ui.c, the SNES port's glue.c,
 * campaign.c, save.c) asks for it. Licence: GPL-2.0-or-later. */
#ifndef VB_PLAT_H
#define VB_PLAT_H
#include "../../core/rules.h"
#include "../../snes/src/campaign.h"
#include "../../snes/src/glue.h"
#include "vb.h"

/* the SNES game's cartridge image (from $07080000: tools/vb_rom.py), read
   at its HiROM addresses ($C00000..) */
extern const u8 *const snes_rom;
#define SNES_AT 0x07080000u
#define SNES_SIZE 0x400000u
#define SCRIPT_AT (SNES_AT + SNES_SIZE)  /* a test build's script or views (tools/vb_rom.py) */
const u8 *snes_ptr(u32 a);              /* a SNES address ($C00000..$FFFFFF) */
extern const u8 *blk;                   /* the campaign's block */
u16 be16(const u8 *p);                  /* a big-endian u16 at p */
u32 be32(const u8 *p);                  /* a big-endian u32 at p */

void plat_init(void);                   /* the campaign block (and, in the script mode, blank saves) */
u8 sram_rd(u16 i);                      /* battery RAM byte i (0..8191) */
void sram_wr(u16 i, u8 v);
u16 pad_read(void);                     /* the pad as the SNES's bits (or the test script's) */
u32 time_100us(void);                   /* a clock in 100 microsecond units (the timer: measurements) */
#endif
