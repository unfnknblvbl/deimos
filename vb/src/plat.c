/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the Virtual Boy: what the game's code asks of the machine -
 * the SNES game's data (its cartridge image, in the Virtual Boy's
 * cartridge as it is: read at its own addresses), the saves (the
 * cartridge's battery RAM), the pad (and a test build's script that plays
 * it), the interrupts, the start. The GBA port's plat.c on this machine.
 *
 * Memory: the SNES image sits in the cartridge at $07080000 (after the
 * code's 512 KB) and is read in place, so a SNES address $C0xxxx.. maps to
 * $07080000 + (address - $C00000). "The campaign block" (blk) is the Mega
 * Drive game's data for all the maps, inside that image; view/view.c
 * reads it through the vd_ functions here. "S" is the game state of
 * core/rules.h. Started by crt0.s (main); the game is the Game Boy
 * Color's ui.c (ui_main), which calls gfx.c and these.
 * Licence: GPL-2.0-or-later.
 */
#include "plat.h"
#include "gfx.h"
#include "sound.h"
#include "libc.h"
#include "../../view/vdata.h"

/* ------------------------------------------------------------ the SNES's cartridge */
const u8 *const snes_rom = (const u8 *)SNES_AT;    /* the SNES image's first byte (SNES $C00000) */
const u8 *blk;                          /* the campaign block */
u8 block_bank;                          /* its first SNES bank */

/* snes_ptr: SNES address a -> a pointer into the image; out of range: its first byte */
const u8 *snes_ptr(u32 a)
{
    u32 o = (a & 0xFFFFFF) - 0xC00000;
    return o < SNES_SIZE ? snes_rom + o : snes_rom;
}

u16 be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }    /* (the SNES data's words are big-endian) */
u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

/* campaign_init: the block's bank from the SNES header's $FFB6, and blk */
void campaign_init(void)
{
    block_bank = snes_rom[0xFFB6];
    blk = snes_ptr((u32)block_bank << 16);
}

/* the SNES's far.s: copies from a 24-bit SNES address; the caller sets
   these, then calls far_copy (far_len bytes), far_strcpy (a string, at
   most far_len bytes) or far_u32 (a big-endian u32) */
u8 far_src[4];                          /* the address: low, high, bank */
u8 *far_dst;
u16 far_len;

/* far_ptr: far_src as a pointer */
static const u8 *far_ptr(void) { return snes_ptr((u32)far_src[0] | ((u32)far_src[1] << 8) | ((u32)far_src[2] << 16)); }
void far_copy(void) { memcpy(far_dst, far_ptr(), far_len); }   /* (far_len bytes to far_dst) */
void far_strcpy(void)
{
    const u8 *s = far_ptr();
    u16 i;
    for (i = 0; i < far_len; i++) {     /* (up to and with the NUL, at most far_len) */
        far_dst[i] = s[i];
        if (!s[i]) break;
    }
}
u32 far_u32(void) { return be32(far_ptr()); }

/* the 3D view's reads (view/vdata.h: block offsets, big-endian)
   (a byte, a u16, a u32 at offset a in the block) */
u8 vd_u8(u32 a) { return blk[a]; }
u16 vd_u16(u32 a) { return be16(blk + a); }
u32 vd_u32(u32 a) { return be32(blk + a); }

/* vd_words: n big-endian s16 from block offset a into dst (the view reads
   every wall's 8 words a picture). (build.sh renames snes/src/vdata.c's
   own vd_words so this one is used.) */
void vd_words(u32 a, s16 *dst, u8 n)
{
    const u8 *p = blk + a;
    while (n--) {
        *dst++ = (s16)((p[0] << 8) | p[1]);
        p += 2;
    }
}

/* ------------------------------------------------------------ the saves */
/* sram_rd / sram_wr: save.c's access to the battery RAM, a byte at a time
   (8 bits wide: byte i at the even address SRAM_AT + 2 i), index wrapped
   to 8 KB (what the SNES's saves use) */
u8 sram_rd(u16 i) { return REG8(SRAM_AT + 2u * (i & 0x1FFF)); }
void sram_wr(u16 i, u8 v) { REG8(SRAM_AT + 2u * (i & 0x1FFF)) = v; }
#ifdef PLAT_SCRIPT
/* sram_blank: nothing saved (the SNES run the script was made from had none) */
static void sram_blank(void)
{
    u16 i;
    for (i = 0; i < 8192; i++) sram_wr(i, 0xFF);
}
#endif

/* ------------------------------------------------------------ the pad */
static u16 script_pad(u16 p);

/* read_keys: the Virtual Boy's pad -> the SNES pad's bits (ui.c): the left
   D-pad the D-pad, A and B as they are, the L and R triggers L and R, the
   right D-pad's up and down the SNES's Y (medkit) and X (notebook),
   SELECT and START as they are. The pad is read by the hardware: SCR's
   bit 2 starts it, bit 1 is set until the 16 bits are in SDLR / SDHR. */
static u16 read_keys(void)
{
    u16 k, p = 0;
    HW_SCR = 0x84;                      /* (bit 7: no pad interrupt; bit 2: read the pad) */
    while (HW_SCR & 2) {}               /* (busy: about 32 microseconds) */
    k = (u16)((HW_SDHR << 8) | HW_SDLR);
    if (k & K_LU) p |= 0x0800;
    if (k & K_LD) p |= 0x0400;
    if (k & K_LL) p |= 0x0200;
    if (k & K_LR) p |= 0x0100;
    if (k & K_A) p |= 0x0080;
    if (k & K_B) p |= 0x8000;
    if (k & K_L) p |= 0x0020;
    if (k & K_R) p |= 0x0010;
    if (k & K_RU) p |= 0x4000;          /* Y: medkit */
    if (k & K_RD) p |= 0x0040;          /* X: notebook */
    if (k & K_START) p |= 0x1000;
    if (k & K_SELECT) p |= 0x2000;
    return p;
}

/* pad_read: the pad for this frame (gfx.c's wait_vblank) */
u16 pad_read(void) { return script_pad(read_keys()); }

/* ---- the script mode (a test build: build.sh test): the pad is played
   from the script 32x/tools/make_script.py packs from snes_play.py
   --script (the SNES game's run in step with the model), after the SNES
   image (tools/vb_rom.py): "SCRP", u16 the bytes of S compared; then $1k
   a press of button k, $40 offset value a poke into S, $80 crc16 S
   compared, 0 the end. The results: script_states, script_bad,
   script_done (tools/vb_lockstep.py reads them). The GBA's, as it is. */
#ifdef PLAT_SCRIPT
extern volatile u8 eng_turn_seq, eng_idle, eng_mode;   /* ui.c: turns taken, idle, its UI state */
#define SCRIPT ((const u8 *)SCRIPT_AT)  /* the script, right after the 4 MB image */
static const u8 *sp;                    /* the next script byte (0: not started) */
static u8 s_st, s_cnt, s_seq;           /* state (0 read, 1 released, 2 held, 3 wait, 9 done); frames left; eng_turn_seq at the press */
static u16 s_key, s_n;                  /* the button pressed; the size of S compared */
volatile u32 script_states, script_bad, script_first_bad, script_done;
/* states compared, how many differed, the first that did, 1 at the end
   (the "crc16" compared is the low 16 bits of crc32 below) */

/* crc32: the standard (reflected, $EDB88320) CRC-32 of n bytes */
static u32 crc32(const u8 *p, u16 n)
{
    u32 c = 0xFFFFFFFFu;
    while (n--) {
        int k;
        c ^= *p++;
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

/* script button k -> the SNES pad bit: A, B, X, Y, L, R, Start, Select, up, down, left, right */
static const u16 KEYS[12] = {0x80, 0x8000, 0x40, 0x4000, 0x20, 0x10, 0x1000, 0x2000, 0x800, 0x400, 0x200, 0x100};

/* script_pad: the script's pad for this frame in place of the real one p
   (p itself if the cartridge has no script). A press is held 4 frames
   after 2 released ones, then the next command waits until ui.c has
   handled it (eng_turn_seq moved) and is idle again. */
static u16 script_pad(u16 p)
{
    if (!sp) {
        if (SCRIPT[0] != 'S' || SCRIPT[1] != 'C') return p;
        s_n = be16(SCRIPT + 4);
        sp = SCRIPT + 6;
    }
    switch (s_st) {
    case 0:
        while (eng_idle) {
            u8 c = *sp;
            if (c == 0) {               /* the end */
                script_done = 1;
                s_st = 9;
                return 0;
            }
            if ((c & 0xF0) == 0x10) {   /* a press */
                s_key = KEYS[c & 15];
                sp++;
                s_st = 1;
                s_cnt = 2;
                break;
            } else if (c == 0x40) {     /* a poke into S (the run's random numbers) */
                ((u8 *)&S)[sp[1]] = sp[2];
                sp += 3;
            } else if (c == 0x80) {     /* a comparison: in play, a dialog, a door code or the vendor */
                u16 want;
                if (eng_mode != 0 && eng_mode != 1 && eng_mode != 2 && eng_mode != 6) break;   /* not yet */
                want = be16(sp + 1);
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
    case 1:                             /* released for 2 frames */
        if (--s_cnt == 0) {
            s_seq = eng_turn_seq;
            s_st = 2;
            s_cnt = 4;
        }
        return 0;
    case 2:                             /* held for 4 */
        if (--s_cnt == 0) s_st = 3;
        return s_key;
    case 3:                             /* until handled */
        if (eng_turn_seq != s_seq && eng_idle) s_st = 0;
        return 0;
    }
    return 0;
}
#else
static u16 script_pad(u16 p) { return p; }  /* (no script mode: the real pad) */
#endif

/* ------------------------------------------------------------ the interrupts */
#define TIMER_RELOAD 166                /* the timer: 167 ticks of 100 us, 59.88 a second (the sound's frames) */
static volatile u32 timer_periods;      /* timer interrupts since the start */
void gfx_frame(void);                   /* gfx.c: a display frame begins */

/* timer_irq: the timer's interrupt (crt0.s's irq_timer): the sound's
   frame (sound.c). The timer is stopped before its zero flag is cleared
   (the hardware does not clear it while the timer runs at zero, and the
   interrupt would come again), then started again. */
void timer_irq(void)
{
    HW_TCR = 0x08;                      /* (stopped, the interrupt kept enabled) */
    HW_TCR = 0x0C;                      /* (the zero flag cleared) */
    HW_TCR = 0x09;                      /* (running again, 100 us ticks, its interrupt on) */
    timer_periods++;
    sound_frame();                      /* (the music's and the effects' frame) */
}

/* vip_irq: the VIP's interrupt (crt0.s's irq_vip): only the frame start
   is enabled; gfx.c shows a waiting picture and counts the frame */
void vip_irq(void)
{
    u16 p = VIP_INTPND;
    VIP_INTCLR = p;                     /* (acknowledged) */
    gfx_frame();
}

/* time_100us: the time since the start in 100 microsecond units, from the
   timer's interrupts and its count (for the measurements) */
u32 time_100us(void)
{
    u32 n, c;
    do {
        n = timer_periods;
        c = HW_TLR | ((u32)HW_THR << 8);    /* (counts down from TIMER_RELOAD) */
    } while (n != timer_periods);
    if (c > TIMER_RELOAD) c = TIMER_RELOAD;
    return n * (TIMER_RELOAD + 1) + (TIMER_RELOAD - c);
}

/* ------------------------------------------------------------ start */
void ui_main(void);                     /* the Game Boy Color's ui.c */
void viewtest_main(void);               /* viewtest.c (the view test build) */

/* plat_init: the campaign block; in the script mode, nothing saved */
void plat_init(void)
{
    campaign_init();
#ifdef PLAT_SCRIPT
    sram_blank();
#endif
}

/* main: from crt0.s: set up, start the timer and the VIP's frame
   interrupt, run the game */
int main(void)
{
    VSU_SSTOP = 1;                      /* (the sound's channels power up in any state: silent first) */
    plat_init();
    gfx_init();                         /* (the display on, the VIP's interrupt on) */
    HW_TLR = TIMER_RELOAD & 0xFF;       /* the timer: TIMER_RELOAD + 1 ticks a period */
    HW_THR = TIMER_RELOAD >> 8;
    HW_TCR = 0x09;                      /* (running, 100 us ticks, its interrupt on) */
#ifdef PLAT_VIEWTEST
    viewtest_main();                    /* (the view test: does not return) */
#endif
    ui_main();                          /* the game (does not return) */
    for (;;) {}
}
