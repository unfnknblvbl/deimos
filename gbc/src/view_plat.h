/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the Game Boy Color: view/view.c's divisions (vdraw.c's, in
 * assembler: a 32-bit number by a 16-bit one). Licence: GPL-2.0-or-later.
 *
 * build.sh preprocesses view/view.c with -DVIEW_PLAT -Isrc, so view.c
 * includes this file; its DIV_L16, DIV_U16 and MUL_W macros then call the
 * routines below (in vdraw.c) instead of SDCC's slower generic 32-bit
 * helpers. */
#ifndef GBC_VIEW_PLAT_H
#define GBC_VIEW_PLAT_H
#include "../../core/types.h"
s32 gb_div_l16(s32 n, s16 d);           /* n / d, truncated toward zero; |d| at most $8000 */
#define DIV_L16(n, d) gb_div_l16((n), (d))
u16 gb_div_u16(u16 a, u16 b);           /* a / b; b non-zero, at most $8000 */
#define DIV_U16(a, b) gb_div_u16((a), (b))
s32 gb_mul_w(s16 a, s16 b);             /* a * b, the full 32-bit product */
#define MUL_W(a, b) gb_mul_w((a), (b))
u8 gb_div8(u8 m, u8 n);                 /* (m << 8) / n for m < n <= 255: view.c's t8_of */
#define DIV_T8(m, n) gb_div8((m), (n))
#endif
