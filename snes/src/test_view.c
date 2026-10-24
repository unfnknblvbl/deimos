/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the SNES: the 3D view driven through a mailbox (the test,
 * tools/snes_view_test.py, compares the frame buffer with the build
 * machine's). Licence: GPL-2.0-or-later. */
/* Built by build_view_test.sh (the CPU's view) and build_fx_view_test.sh
 * (the GSU's, with SUPERFX and VIEW_FX defined). The tester puts the
 * player somewhere through the mailbox, waits for done, and reads the
 * picture out of the emulator's RAM; test/view_host draws the same view
 * on the build machine from the same view/view.c. dbg records how far
 * the program got, for debugging a hang. */
#include "../../core/rules.h"
#include "../../view/view.h"
#include "campaign.h"
#include "video.h"
#include "gfx.h"

void wait_vblank(void);
void screen_on(void);

/* cmd: 11 map change (arg), 13 put the player at text[0..2] (x, z,
   facing), couplings text[3]; each then draws the view */
struct mailbox {
    u8 magic[4];                /* "MBOX" */
    u8 cmd, arg, seq, done;     /* done: 0xEE once ready, then the seq last finished */
    char text[24];              /* the command's data (see above) */
    u16 frames;                 /* frames the picture took */
};
struct mailbox mbox = {{'M', 'B', 'O', 'X'}, 0, 0, 0, 0, {0}, 0};
extern u16 frame_count;
static u8 shown_map = 0xFF;     /* the map whose colours are loaded (0xFF: none) */
u8 grin_frames;                 /* (glue.c's sfx sets it; ui.c, not linked here, owns it) */

u8 dbg;                         /* progress marker for debugging */

/* draw the view from the player's place, load the map's colours if the map
   changed, upload the picture and wait until it is in VRAM */
static void picture(void)
{
    u16 f0 = frame_count;
    dbg = 1;
    view_render();                              /* draw (view/view.c) */
    dbg = 2;
    if (shown_map != S.map) {
        shown_map = S.map;
        view_gradient(view_floor, view_ceiling);    /* the floor and ceiling */
        cgram_load();                               /* the map's colours */
    }
    view_upload();                              /* queue the picture */
    dbg = 3;
    while (view_busy()) wait_vblank();          /* until it is up */
    dbg = 4;
    mbox.frames = frame_count - f0;
}

int main(void)
{
    u8 last = 0;                /* the last seq handled */
    gfx_init();
    video_init();
    video_game();
#ifdef SUPERFX
    {
        void gsu_init(void);
        gsu_init();             /* the GSU and its work RAM routines */
    }
#endif
    dbg = 10;
    campaign_init();            /* find the campaign block */
    game_init();                /* a new game, the first map */
    dbg = 11;
    screen_on();
    picture();
    mbox.done = 0xEE;           /* ready */
    for (;;) {
        wait_vblank();
        if (mbox.seq == last) continue;     /* no new command */
        switch (mbox.cmd) {
        case 11: map_change(mbox.arg); break;
        case 13:
            S.px = mbox.text[0];
            S.pz = mbox.text[1];
            S.facing = mbox.text[2];
            S.couplings = mbox.text[3];
            break;
        }
#ifdef VIEW_FX
        if (mbox.cmd == 14) {               /* the free camera: text[0..1] x, [2..3] z (cells * 64 + ...), [4] angle */
            view_camera((s16)(mbox.text[0] | (mbox.text[1] << 8)), (s16)(mbox.text[2] | (mbox.text[3] << 8)),
                        (u8)mbox.text[4]);
            view_render_cam();
            view_upload();
            while (view_busy()) wait_vblank();
            last = mbox.seq;
            mbox.done = last;
            continue;
        }
#endif
        picture();
        last = mbox.seq;
        mbox.done = last;
    }
}
