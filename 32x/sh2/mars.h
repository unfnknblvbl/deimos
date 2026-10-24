/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the 32X: the 32X's registers, as the SH-2s see them. Licence: GPL-2.0-or-later. */
/* The 32X's own registers sit at $20004000 on the SH-2 side (the
 * cache-through view of $00004000). Offsets used here:
 *   $00   adapter / interrupt control (bit 15 FM; low bits: this SH-2's
 *         interrupt mask, bit 0 the PWM's)
 *   $1C   PWM interrupt clear (write)
 *   $20   the 8 COMM words shared with the 68000 (see m68k/main.c)
 *   $30.. the PWM sound: control, cycle (period), left, right, mono
 *   $100+ the 32X VDP: mode, shift, fill length/address/data, frame
 *         buffer control
 *   $200  the palette (256 x BGR555, bit 15 a priority bit)
 * The frame buffer the SH-2s may write is at $24000000 (128 KB); writes
 * to the overwrite image at $24020000 skip 0 bytes.
 * MREG16/MREG32: a 16- or 32-bit register at offset o. */
#ifndef MARS_H
#define MARS_H
#include "../../core/types.h"

#define MREG16(o) (*(volatile u16 *)(0x20004000 + (o)))
#define MREG32(o) (*(volatile u32 *)(0x20004000 + (o)))
#define MARS_SYS MREG16(0x00)               /* bit 15 FM: the SH-2s have the picture */
#define MARS_COMM(n) MREG16(0x20 + (n))      /* the COMM word at byte offset n (0..14) */
#define MARS_PWM_CTRL MREG16(0x30)          /* channel routing; interrupt every n samples (bits 8..11) */
#define MARS_PWM_CYCLE MREG16(0x32)         /* the sample period in SH-2 clocks, + 1 */
#define MARS_PWM_LEFT MREG16(0x34)          /* write: the next left value (its FIFO); read bit 15: FIFO full */
#define MARS_PWM_RIGHT MREG16(0x36)         /* the next right value */
#define MARS_PWM_MONO MREG16(0x38)          /* both at once (unused) */
#define MARS_VDP_MODE MREG16(0x100)         /* 1 packed pixels; bit 7 the Mega Drive in front */
#define MARS_VDP_SHIFT MREG16(0x102)        /* bit 0: the picture one pixel left */
/* the VDP's fill: length, word address, value (unused by this port) */
#define MARS_VDP_FILLLEN MREG16(0x104)
#define MARS_VDP_FILLADR MREG16(0x106)
#define MARS_VDP_FILLDAT MREG16(0x108)
#define MARS_VDP_FBCTL MREG16(0x10A)        /* bit 0 FS, bit 1 FEN (busy), bit 13 PEN, bit 14 HBLK, bit 15 VBLK */
#define MARS_PALETTE ((volatile u16 *)0x20004200)      /* 256 colours (writable while FBCTL's PEN is set) */
#define MARS_FB ((volatile u16 *)0x24000000)          /* the frame buffer as words (its line table) */
#define MARS_FB8 ((volatile u8 *)0x24000000)          /* ... as bytes (the pixels) */
#define MARS_OVERWRITE8 ((volatile u8 *)0x24020000)   /* the overwrite image (unused) */
/* UNCACHED(p): the cache-through address of p (bit 29 set): reads and
   writes go straight to memory, seen at once by the other SH-2 */
#define UNCACHED(p) ((void *)((u32)(p) | 0x20000000))

/* CCR: this SH-2's cache control register ($10 purge, $01 on) */
#define CCR (*(volatile u8 *)0xFFFFFE92)
#endif
