/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the Game Boy Color: the machine's part (plat.c, bank 0). Licence: GPL-2.0-or-later.
 *
 * Included by every Game Boy source: it brings in core_api.h (core/rules.h
 * with the core's functions __banked, written by tools/split_core.py), the
 * campaign block's reader (campaign.h) and the SNES port's glue.h (the
 * message and sound queues the core fills). */
#ifndef GBC_PLAT_H
#define GBC_PLAT_H
#include "core_api.h"
#include "campaign.h"
#include "../../snes/src/glue.h"

/* the cartridge (tools/gb_pack.py): the SNES game's data from bank 64,
   the campaign's block in it, the Game Boy's resources after it; offsets
   ("block offsets") are from the block's start, its bank DATA_BANK0 */
extern u8 data_bank0;                   /* the ROM bank the block starts in (64 until plat_init finds it) */
#define DATA_BANK0 data_bank0
#define SCRIPT_BANK 0xF8                /* (a test build's script: $3E0000 = bank 248) */
void plat_init(void);                   /* find the resources ("GBRES1") and the block's bank */

/* the resources' entries (tools/gb_res.py) */
enum { RES_FONT, RES_FACES, RES_FACE_PAL, RES_TITLE, RES_WEAPONS, RES_MAPS, RES_SOUND };
u32 res_at(u8 e);                       /* resource e's block offset */


/* the ROM from any bank (plat.c): block offsets, big-endian values */
u8 far_u8(u32 off);
void far_copy(u32 off, void *dst, u16 n);
u32 far_u32be(u32 off);
u16 far_u16be(u32 off);
/* the cartridge RAM: bank b at $A000; the saves' bytes (bank 0, offset i) */
void sram_bank(u8 b);
u8 sram_rd(u16 i);
void sram_wr(u16 i, u8 v);
#endif
