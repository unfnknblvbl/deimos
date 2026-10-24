/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG portable core: integer types for every compiler it is built
 * with (cc65 for the SNES's 65816, gcc for the 3DS and the tests).
 *
 * Included by core/rules.h, view/view.h and most of the ports' C files.
 * It gives the fixed-size integer names the rest of the code uses (u8 /
 * s8, u16 / s16, u32 / s32: unsigned / signed, 8, 16 or 32 bits) and the
 * macros that read and write big-endian numbers in byte arrays: the game
 * state and the level data keep the Mega Drive's (68000) byte order, most
 * significant byte first, whatever the CPU's own order is.
 * Licence: GPL-2.0-or-later. */
#ifndef CORE_TYPES_H
#define CORE_TYPES_H

#ifdef __CC65__
/* cc65 has no <stdint.h> here: its int is 16 bits, its long 32 */
typedef unsigned char u8;
typedef signed char s8;
typedef unsigned int u16;
typedef signed int s16;
typedef unsigned long u32;
typedef signed long s32;
#else
/* gcc and SDCC: the standard fixed-size types */
#include <stdint.h>
typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
#endif

/* big-endian fields of the state and the level data:
   G16(p) reads the 16-bit number at bytes p[0] (high), p[1] (low);
   P16(p, v) writes v there; G32 / P32 the same for 32 bits (4 bytes).
   p is a byte (u8) pointer. P16 and P32 evaluate p more than once. */
#define G16(p) ((u16)(((u16)(p)[0] << 8) | (p)[1]))
#define P16(p, v) ((p)[0] = (u8)((u16)(v) >> 8), (p)[1] = (u8)(v))
/* (a function: cc65 2.19's optimiser drops the top byte of the
   ((u32)G16(p) << 16) form)
   rd32be is defined in core/rules.c. */
u32 rd32be(const u8 *p);
#define G32(p) rd32be(p)
#define P32(p, v) (P16((p), (u32)(v) >> 16), P16((p) + 2, (v)))

#endif
