/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the SNES: the floor and ceiling (the backdrop's colour per
 * line, by HDMA): the level's colour times 0.3 + 0.9 t, t = 0 at the
 * horizon .. 1 at the top / bottom of the view (as the Mega Drive game's backdrop,
 * here per line and in 15 bits: no dither needed).
 * Licence: GPL-2.0-or-later. */
/* The 3D view's floor and ceiling are not drawn: the view's picture leaves
 * them colour 0, which shows the backdrop, and HDMA channel 5 (set up by
 * video.s's video_game) rewrites colour 0 at the start of every line from
 * the table grad_hdma built here. Called by ui.c (and test_view.c) when a
 * map's colours are loaded. Colours are BGR555: bits 0-4 red, 5-9 green,
 * 10-14 blue.
 *
 * The view's size is view/view.h's (VIEW_H rows, shown on VIEW_LINES = 2
 * VIEW_H screen lines: 176 at the full 88 rows); a smaller view is centred
 * in the 176 lines below the message line, the VIEW_PAD_Y lines above and
 * below it black (as video.s's view.inc). */
#include "video.h"
#include "../../view/view.h"

#define VIEW_LINES (2 * VIEW_H)        /* the screen lines the view takes */
#define VIEW_PAD_Y (88 - VIEW_H)        /* black lines above and below it */
#if VIEW_LINES <= 127 || VIEW_LINES > 254
#error "video.c: the gradient's table needs 128 to 254 lines of view"
#endif

/* colour c darkened or brightened for line (0 .. VIEW_LINES - 1 of the
   view's lines; VIEW_H, 88 at the full size, is the horizon): t (0..255)
   grows from the horizon out, and each channel is multiplied by f = 77 +
   230 t / 256 (8.8 fixed point: 0.3 .. 1.2), clamped at 31 */
static u16 shade(u16 c, u16 line)
{
    u16 t = line < VIEW_H ? (u16)(((VIEW_H - 1 - line) * 2 + 1) * 256 / VIEW_LINES) : (u16)(((line - VIEW_H) * 2 + 1) * 256 / VIEW_LINES);
    u16 f = 77 + ((t * 230) >> 8), out = 0, v;
    u8 k;
    for (k = 0; k < 3; k++) {           /* red, green, blue */
        v = (u16)((((c >> (5 * k)) & 31) * f) >> 8);
        out |= (v > 31 ? 31 : v) << (5 * k);
    }
    return out;
}

/* build grad_hdma for the level's floor and ceiling colours. Each HDMA
   entry is a line-count byte and, per line, 4 bytes for $2121 $2121 $2122
   $2122 (colour index 0 twice, then the colour's low and high bytes).
   A count with bit 7 set ("repeat") takes a new 4 bytes every line, for
   at most 127 lines, so the VIEW_LINES lines of the view (176 at the full
   size) are two entries. The message line and the border above the view
   are black, and so is everything from the line after the view on. */
void view_gradient(u16 floor, u16 ceiling)
{
    u8 *p = grad_hdma;
    u16 line, c;
    *p++ = 8 + VIEW_PAD_Y;     /* the message line (and the border above the view): black */
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;
    for (line = 0; line < VIEW_LINES; line++) {
        if (line == 0) *p++ = 0x80 | 127;           /* lines 0..126, one colour each */
        else if (line == 127) *p++ = 0x80 | (VIEW_LINES - 127);     /* lines 127 .. VIEW_LINES - 1 */
        c = shade(line < VIEW_H ? ceiling : floor, line);   /* the top half is ceiling */
        *p++ = 0;
        *p++ = 0;
        *p++ = (u8)c;
        *p++ = (u8)(c >> 8);
    }
    *p++ = 1;                  /* the border below the view, the status bar: black (held until the frame ends) */
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;
    *p = 0;                    /* the table's end */
}
