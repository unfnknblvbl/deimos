/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the Game Boy Advance: what the game's code asks of the
 * machine - the SNES game's data (its cartridge image, in the GBA's
 * cartridge as it is: read at its own addresses), the saves (the
 * cartridge's 32 KB battery RAM), the pad (and a test build's script that
 * plays it), the interrupts, the start.
 *
 * Memory: the SNES image sits in the GBA cartridge at $08080000 (after the
 * code's 512 KB) and is read in place, so a SNES address $C0xxxx.. maps to
 * $08080000 + (address - $C00000). "The campaign block" (blk) is
 * The Mega Drive game's data for all the maps, inside that image; view/view.c reads it
 * through the vd_ functions here. "S" is the game state of core/rules.h.
 * Started by crt0.s (main); the game is the Game Boy Color's ui.c
 * (ui_main), which calls gfx.c and these.
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

u16 be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

/* campaign_init: the block's bank from the header's $FFB6, and blk */
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
void far_copy(void) { memcpy(far_dst, far_ptr(), far_len); }
void far_strcpy(void)
{
    const u8 *s = far_ptr();
    u16 i;
    for (i = 0; i < far_len; i++) {
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

/* (fast RAM, ARM: the view reads every wall's 8 words a picture)
   vd_words: n big-endian s16 from block offset a into dst. (build.sh
   renames snes/src/vdata.c's own vd_words so this one is used.) */
IWRAM_CODE void vd_words(u32 a, s16 *dst, u8 n)
{
    const u8 *p = blk + a;
    while (n--) {
        *dst++ = (s16)((p[0] << 8) | p[1]);
        p += 2;
    }
}

/* ------------------------------------------------------------ the saves */
/* sram_rd / sram_wr: save.c's access to the battery RAM, a byte at a time
   (the GBA's SRAM has an 8-bit bus), index wrapped to 32 KB */
u8 sram_rd(u16 i) { return SRAM[i & 0x7FFF]; }
void sram_wr(u16 i, u8 v) { SRAM[i & 0x7FFF] = v; }
#ifdef PLAT_SCRIPT
static void sram_blank(void)            /* (no saved game: the SNES run had none) */
{
    u16 i;
    for (i = 0; i < 8192; i++) SRAM[i] = 0xFF;     /* (the first 8 KB: what the SNES's saves use) */
}
#endif
static const char sram_tag[] __attribute__((used, aligned(4))) = "SRAM_V113";   /* (the emulators' clue) */
/* (emulators and flash carts search the cartridge for this string to
   learn that the game uses 32 KB of SRAM) */

/* ------------------------------------------------------------ the pad */
static u16 script_pad(u16 p);

/* the GBA's buttons -> the SNES pad's bits (ui.c): L and R as they are,
   SELECT with A, B or START the SNES's Y, X and SELECT */
static u16 read_keys(void)
{
    u16 k = (u16)(~REG_KEYINPUT & 0x3FF), p = 0;  /* (the 10 buttons, 1 = pressed) */
    if (k & KEY_L) p |= 0x0020;
    if (k & KEY_R) p |= 0x0010;
    if (k & KEY_SELECT) {
        if (k & KEY_A) p |= 0x4000;             /* Y: medkit */
        if (k & KEY_B) p |= 0x0040;             /* X: notebook */
        if (k & KEY_LEFT) p |= 0x0020;          /* (L, as on the Game Boy Color) */
        if (k & KEY_RIGHT) p |= 0x0010;
        if (k & KEY_START) p |= 0x2000;         /* SELECT */
        return p;
    }
    if (k & KEY_UP) p |= 0x0800;
    if (k & KEY_DOWN) p |= 0x0400;
    if (k & KEY_LEFT) p |= 0x0200;
    if (k & KEY_RIGHT) p |= 0x0100;
    if (k & KEY_A) p |= 0x0080;
    if (k & KEY_B) p |= 0x8000;
    if (k & KEY_START) p |= 0x1000;
    return p;
}

/* pad_read: the pad for this frame (gfx.c's wait_vblank) */
u16 pad_read(void) { return script_pad(read_keys()); }

/* ---- the script mode (a test build: build.sh test): the pad is played
   from the script 32x/tools/make_script.py packs from snes_play.py
   --script (the SNES game's run in step with the model), after the SNES
   image (tools/gba_rom.py): "SCRP", u16 the bytes of S compared; then $1k
   a press of button k, $40 offset value a poke into S, $80 crc16 S
   compared, 0 the end. The results: script_states, script_bad,
   script_done (tools/gba_harness.py reads them). */
#ifdef PLAT_SCRIPT
extern volatile u8 eng_turn_seq, eng_idle, eng_mode;   /* ui.c: turns taken, idle, its UI state */
#define SCRIPT ((const u8 *)(SNES_AT + SNES_SIZE))      /* the script, right after the 4 MB image */
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
   (p itself if the cartridge has no script) */
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
                ((u8 *)&S)[sp[1]] = sp[2];
                sp += 3;
            } else if (c == 0x80) {
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
    case 1:
        if (--s_cnt == 0) {
            s_seq = eng_turn_seq;
            s_st = 2;
            s_cnt = 4;
        }
        return 0;
    case 2:
        if (--s_cnt == 0) s_st = 3;
        return s_key;
    case 3:
        if (eng_turn_seq != s_seq && eng_idle) s_st = 0;
        return 0;
    }
    return 0;
}
#else
static u16 script_pad(u16 p) { return p; }  /* (no script mode: the real pad) */
#endif

/* ------------------------------------------------------------ the interrupts */
void irq_vblank(void);                  /* gfx.c */

/* irq_dispatch: called by crt0.s's irq_entry with the interrupts that came
   (IF bits); only the V-blank (bit 0) is enabled */
void irq_dispatch(u16 f)
{
    if (f & 1) irq_vblank();
}

/* ------------------------------------------------------------ start */
void ui_main(void);                     /* the Game Boy Color's ui.c */

/* plat_init: the campaign block; in the script mode, no saved game */
void plat_init(void)
{
    campaign_init();
#ifdef PLAT_SCRIPT
    sram_blank();
#endif
    (void)sram_tag;                     /* (keeps the string in the cartridge) */
}

/* main: from crt0.s: set up, turn on the V-blank interrupt, run the game */
int main(void)
{
    plat_init();
    gfx_init();
    REG_DISPSTAT = 0x0008;              /* the vertical blank's interrupt */
    REG_IE = 0x0001;                    /* V-blank only */
    REG_IME = 1;                        /* the interrupts on */
    ui_main();                          /* the game (does not return) */
    for (;;) {}
}
