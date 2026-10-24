/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the SNES: the smallest test program (build_test.sh): only
 * crt0.s and hw.s, no game. If the start-up code, the NMI and the C
 * runtime work, the screen's backdrop fades through the greys. */
#include "../../core/types.h"
extern volatile u16 frame_count;
void wait_vblank(void);
void set_backdrop(u16 c);
void screen_on(void);
int main(void)
{
    u16 c = 0;
    screen_on();                        /* display and NMI on */
    for (;;) {
        wait_vblank();                  /* once a frame */
        set_backdrop(c);                /* colour 0, written by the next NMI */
        c += 0x0421;                    /* +1 to red, green and blue (BGR555) */
        c &= 0x7FFF;
    }
}
