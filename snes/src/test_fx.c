/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the SNES with the SuperFX: a first test (the GSU fills the
 * picture). Licence: GPL-2.0-or-later. */
/* Built by build_fx_test.sh with gsu/test.s (a GSU program that fills the
 * frame buffer with a pattern) instead of the real view. It checks the
 * CPU-GSU handshake (gsu_cpu.s), the picture's upload and the screen
 * set-up; fx_frames says how long the GSU took. It provides the few
 * globals the linked modules expect from the game (view_cgram,
 * view_n_colours, the sound queue). */
#include "../../core/types.h"
#include "video.h"
#include "gfx.h"
#define VIEW_SLOTS 192                  /* the view's colours: CGRAM 32..223 */
u16 view_cgram[VIEW_SLOTS];             /* (normally view/view.c's) */
u8 view_n_colours;
u8 sfx_q[8], sfx_head, sfx_tail;        /* (normally glue.c's; sound.s reads them) */

void wait_vblank(void);
void gsu_init(void);
void gsu_run(u16 entry);
extern volatile u16 frame_count;
u16 fx_frames;                          /* frames the GSU program took (read by the tester) */

int main(void)
{
    u16 i, f0;
    gfx_init();                         /* sprites hidden, upload queue empty */
    video_init();                       /* VRAM cleared, Mode 7 map */
    video_game();                       /* the game screen's modes and HDMA */
    gsu_init();                         /* the GSU and its work RAM routines */
    screen_on();                        /* display, NMI, pad */
    /* a made-up palette: red from the low bits, green and blue from others */
    for (i = 0; i < VIEW_SLOTS; i++) view_cgram[i] = (u16)((i & 31) | ((i >> 3) << 5) | (((i * 3) & 31) << 10));
    view_n_colours = VIEW_SLOTS;
    cgram_load();                       /* queue the colours */
    view_gradient(0x0421, 0x1084);      /* grey floor and ceiling */
    f0 = frame_count;
    gsu_run(0);                         /* the GSU fills the frame buffer */
    fx_frames = frame_count - f0;
    view_upload();                      /* show it */
    for (;;) wait_vblank();
}
