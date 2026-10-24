/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the Game Boy Advance: the machine's part (plat.c), as the
 * game's code (the Game Boy Color's ui.c, the SNES port's glue.c,
 * campaign.c, save.c) asks for it. Licence: GPL-2.0-or-later. */
#ifndef GBA_PLAT_H
#define GBA_PLAT_H
#include "../../core/rules.h"
#include "../../snes/src/campaign.h"
#include "../../snes/src/glue.h"
#include "gba.h"

/* the SNES game's cartridge image (from $08080000: tools/gba_rom.py), its
   HiROM addresses ($C00000..) */
extern const u8 *const snes_rom;
#define SNES_AT 0x08080000
#define SNES_SIZE 0x400000
const u8 *snes_ptr(u32 a);              /* a SNES address ($C00000..$FFFFFF) */
extern const u8 *blk;                   /* the campaign's block */
u16 be16(const u8 *p);                  /* a big-endian u16 at p */
u32 be32(const u8 *p);                  /* a big-endian u32 at p */

void plat_init(void);                   /* the campaign block (and, in the script mode, blank saves) */
u8 sram_rd(u16 i);                      /* battery RAM byte i (0..32767) */
void sram_wr(u16 i, u8 v);
#endif
