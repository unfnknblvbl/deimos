/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the Game Boy Color: the pad (and a test build's script that
 * plays it). Licence: GPL-2.0-or-later.
 *
 * gfx.c's wait_vblank calls pad_read once a frame; ui.c sees the result as
 * the SNES pad's bits (ui.c's PAD_*), so the SNES port's menus and button
 * handling work unchanged.
 */
#pragma bank 255
#include "plat.h"
#include "gfx.h"

static u16 script_pad(u16 p);           /* (the test build's script, below; else p as it is) */

/* the Game Boy's buttons -> the SNES pad's bits (ui.c): SELECT with another
   button makes the SNES's X, Y, L, R */
static u16 read_pad(void)
{
    u8 j = joypad();                            /* (GBDK: the buttons pressed, J_* bits) */
    u16 p = 0;
    if (j & J_SELECT) {
        if (j & J_A) p |= 0x4000;               /* Y: medkit */
        if (j & J_B) p |= 0x0040;               /* X: notebook */
        if (j & J_LEFT) p |= 0x0020;            /* L */
        if (j & J_RIGHT) p |= 0x0010;           /* R */
        if (j & J_START) p |= 0x2000;           /* SELECT */
        return p;
    }
    if (j & J_UP) p |= 0x0800;                  /* UP */
    if (j & J_DOWN) p |= 0x0400;                /* DOWN */
    if (j & J_LEFT) p |= 0x0200;                /* LEFT */
    if (j & J_RIGHT) p |= 0x0100;               /* RIGHT */
    if (j & J_A) p |= 0x0080;                   /* A: use / attack */
    if (j & J_B) p |= 0x8000;                   /* B: pass the turn */
    if (j & J_START) p |= 0x1000;               /* START: pause */
    return p;
}

/* the pad this frame, as SNES bits (gfx.c's wait_vblank) */
u16 pad_read(void) BANKED
{
    return script_pad(read_pad());
}

/* ---- the script mode (a test build: build.sh test): the pad is played
   from the script tools/make_script.py packs (32x/tools) from snes_play.py
   --script (the SNES game's run in step with the model), at the data's
   $3E0000: "SCRP", u16 the bytes of S compared; then $1k a press of
   button k, $40 offset value a poke into S, $80 crc16 S compared, 0 the
   end. The results: script_states, script_bad, script_done (tools read them).
   ("crc16" is the low 16 bits of the CRC-32 of S's first n bytes, n the
   header's u16; button k indexes KEYS. All values big-endian.)
   The game's turn counter eng_turn_seq and idle flag eng_idle (ui.c) say
   when a press has been handled: a press is held for 4 frames after 2
   released ones, then the script waits for a new turn and idle. "Lockstep"
   means the game state S is compared after every move with the reference
   run's (the SNES game played in step with the Mega Drive game's model). */
#ifdef PLAT_SCRIPT
extern volatile u8 eng_turn_seq, eng_idle, eng_mode;   /* (ui.c) */
static u32 sp;                          /* the script's next byte (a block offset) */
static u8 s_st, s_cnt, s_seq, s_on;     /* the state (0 read, 1 released, 2 held, 3 wait, 9 done), its frame count, the turn seen, the script found */
static u16 s_key, s_n;                  /* the button being pressed (SNES bit); S's bytes compared */
u16 script_states, script_bad, script_first_bad;    /* states compared, how many differed, the first that did (1-based) */
u8 script_done;                         /* 1 at the script's end */

/* the standard CRC-32 (reflected, polynomial $EDB88320) of n bytes, as
   Python's zlib.crc32 */
static u32 crc32(const u8 *p, u16 n)
{
    u32 c = 0xFFFFFFFFUL;
    while (n--) {
        u8 k;
        c ^= *p++;
        for (k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320UL : c >> 1;
    }
    return ~c;
}

/* the script's buttons 0..11 -> SNES bits: A, B, X, Y, L, R, START,
   SELECT, UP, DOWN, LEFT, RIGHT */
static const u16 KEYS[12] = {0x80, 0x8000, 0x40, 0x4000, 0x20, 0x10, 0x1000, 0x2000, 0x800, 0x400, 0x200, 0x100};

/* the pad as the script plays it (the real pad p is ignored once a script
   is found); without a script at $3E0000, p */
static u16 script_pad(u16 p)
{
    if (!s_on) {
        u32 at = (u32)(u8)(SCRIPT_BANK - DATA_BANK0) << 14;    /* ($3E0000 as a block offset) */
        if (far_u8(at) != 'S' || far_u8(at + 1) != 'C') return p;
        s_n = far_u16be(at + 4);
        sp = at + 6;
        s_on = 1;
    }
    switch (s_st) {
    case 0:                             /* read commands while the game is idle */
        while (eng_idle) {
            u8 c = far_u8(sp);
            if (c == 0) {
                script_done = 1;
                s_st = 9;
                return 0;
            }
            if ((c & 0xF0) == 0x10) {
                s_key = KEYS[c & 15];
                sp++;
                s_st = 1;
                s_cnt = 2;
                break;
            } else if (c == 0x40) {
                ((u8 *)&S)[far_u8(sp + 1)] = far_u8(sp + 2);   /* (the test's god mode: health set) */
                sp += 3;
            } else if (c == 0x80) {
                u16 want;
                if (eng_mode != 0 && eng_mode != 1 && eng_mode != 2 && eng_mode != 6) break;   /* not yet */
                want = far_u16be(sp + 1);
                sp += 3;
                script_states++;
                if ((u16)crc32((const u8 *)&S, s_n) != want) {
                    if (!script_bad) script_first_bad = script_states;
                    script_bad++;
                }
            } else {
                s_st = 9;
                return 0;
            }
        }
        return 0;
    case 1:                             /* 2 frames released, the turn noted */
        if (--s_cnt == 0) {
            s_seq = eng_turn_seq;
            s_st = 2;
            s_cnt = 4;
        }
        return 0;
    case 2:                             /* 4 frames held */
        if (--s_cnt == 0) s_st = 3;
        return s_key;
    case 3:                             /* until the game has handled it */
        if (eng_turn_seq != s_seq && eng_idle) s_st = 0;
        return 0;
    }
    return 0;
}
#else
static u16 script_pad(u16 p) { return p; }     /* (the game: the real pad) */
#endif
