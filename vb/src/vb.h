/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the Virtual Boy: the machine's registers and memories.
 *
 * The Virtual Boy's address space (27 bits; it repeats every 128 MB):
 *   $00000000  the VIP (video): its 256 KB of memory and its registers
 *              $00000 left eye's frame buffer 0, $08000 left 1,
 *              $10000 right 0, $18000 right 1 (24 KB each: 384 columns
 *              of 64 bytes, 4 pixels a byte, 2 bits a pixel, the top
 *              pixel in the low bits); between them the characters;
 *              $20000..$3D7FF the background maps (unused here: general
 *              memory, vb.ld's .vbss), $3DC00 / $3DE00 the left / right
 *              column tables (the display's timing per column),
 *              $5F800.. the registers
 *   $01000000  the VSU (sound): the 5 waves, then the 6 channels' registers
 *   $02000000  the hardware control: the pad, the timer, the wait states
 *   $05000000  the work RAM, 64 KB
 *   $06000000  the cartridge's battery RAM (8 bits wide: a byte at every
 *              even address)
 *   $07000000  the cartridge ROM (mirrored to fill 16 MB; the CPU starts
 *              at $FFFFFFF0, the ROM's last 16 bytes)
 * Licence: GPL-2.0-or-later. */
#ifndef VB_H
#define VB_H
#include "../../core/types.h"

/* REG8 / REG16: an 8- or 16-bit hardware register at address a */
#define REG8(a) (*(volatile u8 *)(a))
#define REG16(a) (*(volatile u16 *)(a))

/* ---- the VIP's registers ($0005F800..) ---- */
#define VIP_INTPND REG16(0x0005F800)    /* interrupts pending (bits: see VIP_INT_) */
#define VIP_INTENB REG16(0x0005F802)    /* interrupts enabled */
#define VIP_INTCLR REG16(0x0005F804)    /* write 1s: clear those pending */
#define VIP_DPSTTS REG16(0x0005F820)    /* display status (read) */
#define VIP_DPCTRL REG16(0x0005F822)    /* display control: bit 1 DISP (on), 8 RE (refresh), 9 SYNCE */
#define VIP_BRTA REG16(0x0005F824)      /* brightness: shade 1 = A, 2 = B, 3 = A + B + C */
#define VIP_BRTB REG16(0x0005F826)
#define VIP_BRTC REG16(0x0005F828)
#define VIP_REST REG16(0x0005F82A)      /* the rest time between columns */
#define VIP_FRMCYC REG16(0x0005F82E)    /* display frames per game frame, minus 1 */
#define VIP_XPSTTS REG16(0x0005F840)    /* the drawing's status (read) */
#define VIP_XPCTRL REG16(0x0005F842)    /* drawing control: bit 1 XPEN (the VIP draws its worlds), bit 0 XPRST */
#define VIP_INT_FRAMESTART 0x0010       /* a display frame starts (50 a second) */

/* the frame buffers: eye e (0 left, 1 right) of buffer b (0, 1) */
#define FB_ADDR(b, e) (0x00000000u + (u32)(b) * 0x8000u + (u32)(e) * 0x10000u)
#define COLTAB_L ((volatile u16 *)0x0003DC00)  /* the left eye's column table: 256 halfwords */
#define COLTAB_R ((volatile u16 *)0x0003DE00)  /* the right eye's */

/* ---- the VSU ($01000000..): every register a byte at a word address ---- */
#define VSU_WAVE(n, i) REG8(0x01000000u + (u32)(n) * 0x80u + (u32)(i) * 4u)   /* wave n's sample i (6 bits) */
#define VSU_CH(c, r) REG8(0x01000400u + (u32)(c) * 0x40u + (u32)(r) * 4u)     /* channel c's register r */
#define VSU_INT 0                       /* bit 7 play, bit 5 stop after the interval, bits 0..4 the interval */
#define VSU_LRV 1                       /* bits 4..7 the left volume, 0..3 the right */
#define VSU_FQL 2                       /* the frequency's low 8 bits */
#define VSU_FQH 3                       /* its high 3 bits */
#define VSU_EV0 4                       /* bits 4..7 the envelope's start, bit 3 up, bits 0..2 its step time */
#define VSU_EV1 5                       /* bit 0 the envelope on; the noise: bits 4..6 its tap */
#define VSU_RAM 6                       /* the wave played (0..4) */
#define VSU_SSTOP REG8(0x01000580)      /* bit 0: stop every channel */

/* ---- the hardware control ($02000000..) ---- */
#define HW_SDLR REG8(0x02000010)        /* the pad's low 8 bits (after a read) */
#define HW_SDHR REG8(0x02000014)        /* its high 8 bits */
#define HW_TLR REG8(0x02000018)         /* the timer's reload / count, low byte */
#define HW_THR REG8(0x0200001C)         /* high byte */
#define HW_TCR REG8(0x02000020)         /* timer control: bit 0 on, 2 clear the zero flag, 3 its interrupt, 4 the 20 us clock */
#define HW_WCR REG8(0x02000024)         /* wait control: bit 0 the ROM with 1 wait state (not 2), bit 1 the expansion */
#define HW_SCR REG8(0x02000028)         /* the pad's serial control: bit 2 read, bit 1 busy, bit 7 no interrupt */

/* the pad's bits (HW_SDHR << 8 | HW_SDLR) */
#define K_A 0x0004
#define K_B 0x0008
#define K_R 0x0010                      /* the R trigger */
#define K_L 0x0020                      /* the L trigger */
#define K_RU 0x0040                     /* the right D-pad: up, right, left, down */
#define K_RR 0x0080
#define K_RL 0x4000
#define K_RD 0x8000
#define K_LR 0x0100                     /* the left D-pad: right, left, down, up */
#define K_LL 0x0200
#define K_LD 0x0400
#define K_LU 0x0800
#define K_START 0x1000
#define K_SELECT 0x2000

/* the battery RAM: save byte i at SRAM_AT + 2 i */
#define SRAM_AT 0x06000000u

/* VBSS: a variable in the VIP's spare memory (vb.ld's .vbss, $20000..),
   for the big buffers the 64 KB of work RAM has no room for */
#define VBSS __attribute__((section(".vbss")))
#endif
