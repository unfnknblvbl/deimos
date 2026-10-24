/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the Game Boy Advance: the machine's registers. Licence: GPL-2.0-or-later. */
#ifndef GBA_H
#define GBA_H
#include "../../core/types.h"

/* REG16 / REG32: a 16- or 32-bit hardware register at address a */
#define REG16(a) (*(volatile u16 *)(a))
#define REG32(a) (*(volatile u32 *)(a))

#define REG_DISPCNT REG16(0x04000000)       /* display control: mode, page (bit 4), layers */
#define REG_DISPSTAT REG16(0x04000004)      /* display status; bit 3 enables the V-blank interrupt */
#define REG_VCOUNT REG16(0x04000006)        /* the line being drawn (0..227) */
#define REG_BG2PA REG16(0x04000020)         /* BG2's affine x scale (8.8; $100 = 1) */
#define REG_BG2PD REG16(0x04000026)         /* BG2's affine y scale */

#define REG_SOUNDCNT_L REG16(0x04000080)    /* the PSG channels' volumes and routing */
#define REG_SOUNDCNT_H REG16(0x04000082)    /* Direct Sound A/B: volume, sides, timer, FIFO reset */
#define REG_SOUNDCNT_X REG16(0x04000084)    /* bit 7: the sound hardware on */
#define REG_SOUNDBIAS REG16(0x04000088)     /* the output's bias and PWM resolution */
#define REG_FIFO_A 0x040000A0               /* Direct Sound A's FIFO (an address: DMA writes it) */
#define REG_FIFO_B 0x040000A4               /* Direct Sound B's */

/* the 4 DMA channels: source, destination, count (low half) and control (high half) */
#define REG_DMA0SAD REG32(0x040000B0)
#define REG_DMA0DAD REG32(0x040000B4)
#define REG_DMA0CNT REG32(0x040000B8)
#define REG_DMA1SAD REG32(0x040000BC)
#define REG_DMA1DAD REG32(0x040000C0)
#define REG_DMA1CNT REG32(0x040000C4)
#define REG_DMA2SAD REG32(0x040000C8)
#define REG_DMA2DAD REG32(0x040000CC)
#define REG_DMA2CNT REG32(0x040000D0)
#define REG_DMA3SAD REG32(0x040000D4)
#define REG_DMA3DAD REG32(0x040000D8)
#define REG_DMA3CNT REG32(0x040000DC)

#define REG_TM0CNT_L REG16(0x04000100)     /* timer 0: reload value (the sound's sample rate) */
#define REG_TM0CNT_H REG16(0x04000102)     /* timer 0: control */

#define REG_KEYINPUT REG16(0x04000130)     /* the buttons, 0 = pressed (KEY_ bits) */
#define REG_IE REG16(0x04000200)           /* interrupts enabled (bit 0 V-blank) */
#define REG_IF REG16(0x04000202)           /* interrupts requested; write 1 to clear */
#define REG_WAITCNT REG16(0x04000204)      /* the cartridge's wait states */
#define REG_IME REG16(0x04000208)          /* the interrupts' master switch */
#define IRQ_HANDLER (*(void (**)(void))0x03007FFC)  /* the BIOS's pointer to our handler */
#define BIOS_IF REG16(0x03007FF8)          /* the BIOS's copy of handled interrupts (IntrWait) */

#define PAL_BG ((volatile u16 *)0x05000000)    /* the background palette: 256 BGR555 colours */
#define VRAM ((volatile u16 *)0x06000000)      /* video RAM (Mode 4: two 240 x 160 pages, $A000 apart) */
#define SRAM ((volatile u8 *)0x0E000000)       /* the cartridge's battery RAM (byte access only) */

/* DMA control (the high half of xCNT) */
#define DMA_ON 0x80000000u              /* enable */
#define DMA_HBLANK 0x20000000u          /* start at each H-blank */
#define DMA_SPECIAL 0x30000000u         /* start on request (DMA1/2: the sound FIFOs) */
#define DMA_REPEAT 0x02000000u          /* again at each start */
#define DMA_32 0x04000000u              /* 32-bit units (else 16) */
#define DMA_DST_FIXED 0x00400000u       /* the destination does not move */
#define DMA_DST_RELOAD 0x00600000u      /* the destination returns to its start on each repeat */

/* the buttons in REG_KEYINPUT */
#define KEY_A 0x0001
#define KEY_B 0x0002
#define KEY_SELECT 0x0004
#define KEY_START 0x0008
#define KEY_RIGHT 0x0010
#define KEY_LEFT 0x0020
#define KEY_UP 0x0040
#define KEY_DOWN 0x0080
#define KEY_R 0x0100
#define KEY_L 0x0200

/* where to put things (src/gba.ld): IWRAM_CODE a function as ARM code in
   the 32 KB fast RAM (long_call: it is too far for a plain bl from the
   cartridge); IWRAM_DATA / IWRAM_BSS variables there (initialised /
   zeroed); EWRAM_BSS zeroed variables in the 256 KB work RAM */
#define IWRAM_CODE __attribute__((section(".iwram"), long_call, target("arm"), noinline))
#define IWRAM_DATA __attribute__((section(".iwram_data")))
#define EWRAM_BSS __attribute__((section(".ewram_bss")))
#define IWRAM_BSS __attribute__((section(".iwram_bss")))
#endif
