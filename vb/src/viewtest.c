/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the Virtual Boy: the view test (build.sh viewtest: the
 * cartridge plays no game, it shows a list of views one after another so
 * tools/vb_view_test.py can compare them, pixel for pixel, with the build
 * machine's renderer).
 *
 * The list (tools/vb_rom.py puts it after the SNES image, SCRIPT_AT):
 * "VIEW", u16 the number of views (big-endian), then 10 bytes a view: the
 * map, the player's x, z, facing, 1 for the free camera, its angle (256
 * a turn), its x and z (big-endian s16, world units). For each view: a
 * new game (core/rules.c's game_init), the map, the player put there, the
 * floor and ceiling, the picture (view.c, then gfx.c's stereo picture);
 * when it is on the screen, vt_done = the view's number (from 1), and the
 * test, having looked (fb in the work RAM, the screen's two eyes), sets
 * vt_go to the same number for the next.
 * Licence: GPL-2.0-or-later.
 */
#include "plat.h"
#include "gfx.h"

volatile u16 vt_done, vt_go;            /* the view shown; the test's go-ahead (tools/vb_view_test.py) */

/* viewtest_main: the views in turn, then nothing (plat.c's main calls it in that build) */
void viewtest_main(void)
{
    const u8 *l = (const u8 *)SCRIPT_AT;
    u16 n, i, f;
    if (l[0] != 'V' || l[1] != 'I') for (;;) wait_vblank();    /* (no list) */
    n = be16(l + 4);
    for (i = 0; i < n; i++) {
        const u8 *v = l + 6 + 10 * i;
        game_init();                    /* (a new game: map 0) */
        if (v[0]) map_change(v[0]);     /* (the view's map) */
        S.px = v[1];
        S.pz = v[2];
        S.facing = v[3];
        gfx_game();
        gfx_backdrop(S.floor_rgb, S.ceil_rgb, 0);
        if (v[4]) {                     /* the free camera */
            view_camera((s16)be16(v + 6), (s16)be16(v + 8), v[5]);
            view_render_cam();
        } else
            view_render();
        gfx_picture();                  /* (both eyes, into the pair not shown) */
        for (f = 0; f < 3; f++) wait_vblank();     /* (on the screen) */
        vt_done = (u16)(i + 1);
        while (vt_go != i + 1) wait_vblank();      /* (the test looks) */
    }
    for (;;) wait_vblank();
}
