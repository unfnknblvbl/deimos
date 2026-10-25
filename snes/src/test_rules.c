/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the SNES: the rules driven through a mailbox in RAM (the
 * lockstep test, tools/snes_lockstep.py, pokes a command, the game runs
 * it and answers). Licence: GPL-2.0-or-later. */
/* Built by build_rules_test.sh: the rules (core/rules.c), glue.c,
 * campaign.c and this, with no screen or sound. "Lockstep": the test runs
 * the ROM in an emulator next to the Mega Drive game's Python model of the game,
 * gives both the same action, and compares the ROM's game state S with
 * the model's after every action. The tester finds mbox through the
 * linker's label file. */
#include "../../core/rules.h"
#include "glue.h"

void wait_vblank(void);
void screen_on(void);
void set_backdrop(u16 c);

/* the mailbox: cmd (1 move, 2 turn, 3 use, 4 pass, 5 cycle, 6 heal,
   7 item, 8 resume, 9 store close, 10 store buy, 11 map change, 12 a fresh
   game), arg, then seq: the test writes seq last; done = seq when finished */
struct mailbox {
    u8 magic[4];                /* "MBOX": lets the tester check it found it */
    u8 cmd, arg, seq, done;     /* done: 0xEE once ready, then the seq last finished */
    char text[24];              /* a password for cmd 8 (NUL-terminated) */
    u16 frames;                 /* frames the last command took */
};
struct mailbox mbox = {{'M', 'B', 'O', 'X'}, 0, 0, 0, 0, {0}, 0};
extern u16 frame_count;         /* hw.s: NMIs so far */

int main(void)
{
    u8 last = 0;                /* the last seq handled */
    u16 f0;
    screen_on();                /* (for the NMI: frame_count, wait_vblank) */
    set_backdrop(0x1084);       /* dark grey: it is running */
    game_init();                /* a fresh game, the first map */
    mbox.done = 0xEE;           /* ready */
    for (;;) {
        wait_vblank();
        if (mbox.seq == last) continue;     /* no new command */
        f0 = frame_count;
        switch (mbox.cmd) {     /* the rules' actions (core/rules.h) */
        case 1: act_move(mbox.arg); break;
        case 2: act_turn((s8)mbox.arg); break;
        case 3: act_use(); break;
        case 4: act_pass(); break;
        case 5: act_cycle_weapon((s8)mbox.arg); break;
        case 6: act_health_item(); break;
        case 7: use_item(mbox.arg); break;
        case 8: resume(mbox.arg ? mbox.text : 0); break;
        case 9: store_close(); break;
        case 10: store_buy(mbox.arg); break;
        case 11: map_change(mbox.arg); break;
        case 12: game_init(); break;
        }
        mbox.frames = frame_count - f0;
        last = mbox.seq;
        mbox.done = last;
    }
}
