/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * The Mega Drive game - VDP set-up, DMA, the frame interrupt, joypad.
 *
 * The hardware layer under ui.c, render.s, weapon.c and backdrop.c:
 *   - VDP registers, VRAM / CRAM addresses, name table writes;
 *   - DMA from ROM or RAM to VRAM, either at once or through a queue that
 *     the interrupts run (see the DMA queue below);
 *   - the frame interrupt's C part (vblank_c, called from crt0.s): the
 *     view's buffer flip with its palettes, the red damage flash, the
 *     floor / ceiling colours of the first band, the queued DMAs;
 *   - the raster effects switch (vdp_raster): H interrupts every 8 lines
 *     on the game screen (crt0.s hblank_handler);
 *   - the DMA budgets: how many words the interrupts may send before the
 *     next raster effect is due (they follow the view's place on the
 *     screen: md.h VIEW_LINE0, VIEW_H);
 *   - the joypad on port 1, 3- or 6-button.
 *
 * Palettes on the game screen (CRAM: 4 lines of 16 colours):
 *   0, 1   the view's (two of the area's three palettes, src/area.c)
 *   2      the view's third palette while the view is drawn; the status
 *          bar's (ui_hud_pal) above the view (the message line) and below
 *          it (the H interrupt swaps them)
 *   3      text and dialog boxes, the weapon (slots 4..12), the floor and
 *          ceiling shades (slots 13, 14, rewritten per 16-line band)
 *
 * Screen timing (NTSC, 224 lines): the V counter (vcount) counts the
 * lines; lines 0..223 are drawn, the vertical blank is the rest of the 262.
 * The view is lines VIEW_LINE0..VIEW_LINE0 + VIEW_H - 1 (24..151), the
 * status bar lines 168..223; lines 8..23 and 152..167 are the black
 * border round the view (palette 0's colour 0, the backdrop colour, kept
 * black even during the damage flash).
 *
 * Licence: GPL-2.0-or-later.
 */
#include "md.h"
#include "backdrop.h"

volatile u16 frame_count;                    /* frames since start-up (wraps) */
volatile u8 pending_view_buf = 0xFF;         /* the buffer to flip to (md.h) */
u8 shown_view_buf;
volatile u16 pending_cram[48];               /* the next picture's palettes 0..2 (render.s fills it) */
volatile u8 flash_frames;                    /* damage flash frames left */
static u16 shown_pal[48];                    /* the shown picture's palettes 0..2 */
static u8 flashing;                          /* nonzero: CRAM must be (re)written this frame (no raster) */
u8 raster_on;                                /* 1 on the game screen (vdp_raster) */
volatile u8 hud_done;                        /* the status bar's palette is in (this frame) */
volatile u8 top_done;                        /* the view's palette 2 is in (this frame) */
u16 pal2_view[16];                           /* (what goes in at the view's top) */
const u32 *volatile bd_rows_p;               /* the backdrop's colours per row (backdrop.c) */
void sound_update(void);                     /* sound.c */
void music_update(void);                     /* music.s */

/* the VDP's registers at start-up (vdp_init). Addresses of tables are
   given in units: plane A / window in $400s, plane B in $2000s, sprites in
   $200s, h-scroll in $400s. */
static const u8 vdp_regs[24] = {
    0x04,       /* 0: no H interrupt, normal colour */
    0x24,       /* 1: display OFF, V interrupt on, DMA off, 224 lines, mode 5 */
    0x30,       /* 2: plane A = $C000 */
    0x2A,       /* 3: window = $A800 */
    0x05,       /* 4: plane B = $A000 */
    0x6C,       /* 5: sprites = $D800 */
    0x00,
    0x00,       /* 7: backdrop = palette 0 colour 0 */
    0x00, 0x00,
    0xFF,       /* 10: H interrupt counter */
    0x00,       /* 11: full-screen scroll */
    0x00,       /* 12: H32, no shadow/highlight, no interlace */
    0x37,       /* 13: h-scroll table = $DC00 */
    0x00,
    0x02,       /* 15: auto-increment 2 */
    0x00,       /* 16: planes 32x32 */
    0x00,       /* 17: window H = none */
    0x95,       /* 18: window V: rows >= 21 (bottom) */
    0x00, 0x00, 0x00, 0x00, 0x00    /* 19..23: DMA length and source (set per DMA) */
};

/* write VDP register reg (0..23) with val: one control port word
   $8000 | reg << 8 | val. Callers must keep the frame interrupt out if it
   could come between an address write and its data. */
void vdp_set_reg(u8 reg, u8 val)
{
    VDP_CTRL = 0x8000 | ((u16)reg << 8) | val;
}

/* a copy of register 1 (the VDP's registers cannot be read back): DMA
   and the display bit are set by changing it; crt0.s's H interrupt and
   dma.s use it too */
u8 vdp_reg1 = 0x24;                          /* (the H interrupt blanks with it) */
#define reg1 vdp_reg1

/* the display on (register 1 bit 6 set) or blanked (clear: the border
   colour only, and VRAM can be written at any time) */
void vdp_display(u8 on)
{
    reg1 = on ? (reg1 | 0x40) : (reg1 & ~0x40);
    vdp_set_reg(1, reg1);
}

/* the address commands: two words, the first 01 (VRAM write), 11 (CRAM
   write) or 01 + bit 4 of the second (VSRAM write) in the top bits with
   address bits 13..0, the second with address bits 15..14 in its low bits.
   The data port then writes there. Interrupts must be kept out by the
   caller (irq_off) until the data is written. */

/* next data port writes go to VRAM byte address addr */
void vdp_set_vram_addr(u16 addr)
{
    VDP_CTRL32 = 0x40000000UL | ((u32)(addr & 0x3FFF) << 16) | (addr >> 14);
}

/* next data port writes go to CRAM byte address addr (colour n at 2n) */
void vdp_set_cram_addr(u16 addr)
{
    VDP_CTRL32 = 0xC0000000UL | ((u32)(addr & 0x3FFF) << 16) | (addr >> 14);
}

/* next data port writes go to VSRAM (the vertical scroll values) */
static void vdp_set_vsram_addr(u16 addr)
{
    VDP_CTRL32 = 0x40000010UL | ((u32)(addr & 0x3FFF) << 16);
}

/* n colours (CRAM words) from cols into CRAM from colour number index
   (0..63: palette line * 16 + slot), with the interrupts held off */
void vdp_write_cram(u8 index, const u16 *cols, u8 n)
{
    u16 sr = irq_off();
    vdp_set_cram_addr(index * 2);
    while (n--) VDP_DATA = *cols++;
    irq_restore(sr);
}

/* the V counter (lines 0..234 as themselves, then 229..255 again for the
   rest of the frame in 224-line NTSC mode) */
static u8 vcount(void)
{
    return *(volatile u8 *)0xC00008;
}

/* one DMA RAM/ROM -> VRAM, n words (not across a 128 KB boundary of the
   source); the 68000 stops while it runs */
static void dma_now(u32 s, u16 vram, u16 n)
{
    u32 a = s >> 1;                 /* the VDP takes the source in words */
    VDP_CTRL = 0x8100 | reg1 | 0x10;                               /* DMA enable */
    VDP_CTRL32 = ((0x9300UL | (n & 0xFF)) << 16) | (0x9400 | (n >> 8));            /* length */
    VDP_CTRL32 = ((0x9500UL | (a & 0xFF)) << 16) | (0x9600 | ((a >> 8) & 0xFF));   /* source */
    VDP_CTRL = 0x9700 | ((a >> 16) & 0x7F);         /* (bits 7-6 = 0: memory to VRAM) */
    {
        /* the command word that starts the DMA must come from RAM
           on real hardware: write it as two 16-bit halves */
        volatile u16 cmd[2];
        cmd[0] = 0x4000 | (vram & 0x3FFF);          /* VRAM write, address 13..0 */
        cmd[1] = 0x80 | (vram >> 14);               /* + DMA (bit 7), address 15..14 */
        VDP_CTRL = cmd[0];
        VDP_CTRL = cmd[1];
    }
    VDP_CTRL = 0x8100 | reg1;                       /* DMA disable again */
}

/* While the raster effects run (the floor and ceiling's colours set every
   tile row of the view, the status bar's palette swapped in below it), a
   DMA may not hold the 68000 when they are due, or they would come late.
   So then DMAs are queued and the interrupts do them, as much as fits
   before the next effect: the frame interrupt in the vertical blank (to
   line ~258), the status bar's after the swap (to line 222), each tile
   row's 32 words (4 lines; 16 bytes a line while the screen is drawn,
   ~160 in the blank). The view ends at line VIEW_LINE0 + VIEW_H (152), so
   the status bar's interrupt (and its DMAs) come at line ~151, with the
   border below the view to spare.
   The queue is a ring of DQ_N entries: dq_tail is where the next one goes
   (vdp_dma_queue), dq_head the next to run (dma.s advances it); empty
   when they are equal. An entry with words = 0 is a flip: dma.s sets
   pending_view_buf to its vram field once the DMAs before it are done, so
   the picture is shown only when it is all in VRAM. */
#define DQ_N 48
struct dq_entry {                            /* (dma.s reads them: 8 bytes) */
    u32 src;
    u16 vram, words;
};
struct dq_entry dq[DQ_N];
volatile u8 dq_head, dq_tail;               /* (crt0.s, dma.s look at them) */
volatile u8 dq_hold;                        /* (the queue being filled: wait) */

void dma_run(u16 budget);                    /* (dma.s) */

/* the first line of the status bar's H interrupt: the interrupt at the
   end of the view's last line (crt0.s; it was 166 for the 160-line view) */
#define HUD_IRQ_LINE (VIEW_LINE0 + VIEW_H - 2)

/* the status bar's H interrupt (crt0.s): to line ~220 (each DMA's set-up
   costs about a line: 6 words a line). Budget: 7 words per line left
   before line 219; nothing if the interrupt came too late (line 212 on). */
void dma_run_hud(void)
{
    u8 v = vcount();
    if (v >= HUD_IRQ_LINE && v < 212) dma_run((u16)(219 - v) * 7);
}

/* 1 while queued DMAs are waiting */
u8 dma_busy(void)
{
    return dq_head != dq_tail;
}

/* wait until the queue is empty (the interrupts run it; they must be on) */
void vdp_dma_wait(void)
{
    while (dq_head != dq_tail) {
    }
}

/* queue a DMA (done now when there are no raster effects, or the
   interrupts are off); words = 0: flip to buffer `vram` when the DMAs
   before it are done.
   "The interrupts are off" means the caller's interrupt mask was 4 or more
   (SR bits 10-8), so the H interrupt could not run the queue. */
void vdp_dma_queue(const void *src, u16 vram, u16 words)
{
    u16 sr = irq_off();
    /* a flip with nothing to wait for: ask the frame interrupt for it */
    if (!words && (!raster_on || (sr & 0x0700) >= 0x0400)) {
        pending_view_buf = (u8)vram;
        irq_restore(sr);
        return;
    }
    /* no raster effects: the DMA now, cut where the source crosses a
       128 KB boundary (one DMA cannot cross one) */
    if (!raster_on || (sr & 0x0700) >= 0x0400) {
        u32 s = (u32)src;
        while (words) {
            u32 room = (0x20000UL - (s & 0x1FFFF)) >> 1;   /* words to the boundary */
            u16 n = words;
            if (room < n) n = (u16)room;
            dma_now(s, vram, n);
            s += (u32)n * 2;
            vram += n * 2;
            words -= n;
        }
        irq_restore(sr);
        return;
    }
    /* the raster effects run: add it to the queue (dma.s splits it) */
    {
        u8 t = dq_tail, n = t + 1 < DQ_N ? t + 1 : 0;
        struct dq_entry *e = &dq[t];
        while (n == dq_head) {                           /* full: wait */
            irq_restore(sr);            /* (let the interrupts run some) */
            sr = irq_off();
        }
        e->src = (u32)src;
        e->vram = vram;
        e->words = words;
        dq_tail = n;                    /* published last: the entry is complete */
    }
    irq_restore(sr);
}

/* DMA RAM/ROM -> VRAM, done when it returns */
void vdp_dma_vram(const void *src, u16 vram, u16 words)
{
    vdp_dma_queue(src, vram, words);
    vdp_dma_wait();
}

/* fill `bytes` bytes of VRAM from vram with val, a word at a time by the
   CPU (bytes rounded down to even) */
void vdp_fill_vram(u16 vram, u16 bytes, u8 val)
{
    u16 w = ((u16)val << 8) | val;
    u16 sr = irq_off();
    vdp_set_vram_addr(vram);
    bytes >>= 1;
    while (bytes--) VDP_DATA = w;
    irq_restore(sr);
}

/* one entry of a 32-column name table at VRAM nt: column x, row y */
void vdp_put_nt(u16 nt, u8 x, u8 y, u16 entry)
{
    u16 sr = irq_off();
    vdp_set_vram_addr(nt + ((u16)y * 32 + x) * 2);
    VDP_DATA = entry;
    irq_restore(sr);
}

/* start-up (main): the registers from vdp_regs (display off), then all of
   VRAM, CRAM (64 colours) and VSRAM (40 words) cleared, and a sprite
   table with one empty entry */
void vdp_init(void)
{
    u8 i;
    (void)VDP_CTRL;                         /* reset the command latch */
    for (i = 0; i < 24; i++) vdp_set_reg(i, vdp_regs[i]);
    reg1 = vdp_regs[1];             /* display off, V interrupt on */
    vdp_set_vram_addr(0);
    {
        u16 n = 0x8000;
        while (n--) VDP_DATA = 0;           /* all 64 KB */
    }
    vdp_set_cram_addr(0);
    for (i = 0; i < 64; i++) VDP_DATA = 0;
    vdp_set_vsram_addr(0);
    for (i = 0; i < 40; i++) VDP_DATA = 0;
    /* sprite table: one terminating entry, off screen */
    vdp_set_vram_addr(VRAM_SAT);
    VDP_DATA = 0;
    VDP_DATA = 0;
    VDP_DATA = 0;
    VDP_DATA = 0;
}

/* wait for the next frame interrupt (frame_count to change); the
   interrupts must be on */
void wait_vblank(void)
{
    u16 f = frame_count;
    while (f == frame_count) {
    }
}

/* the raster effects on (the game's screen) or off: H interrupts every
   8 lines (crt0.s hblank_handler) */
void vdp_raster(u8 on)
{
    u16 sr = irq_off();
    raster_on = on;
    hud_done = 0;
    vdp_set_reg(10, on ? 7 : 0xFF);     /* H interrupt every 8 lines (counter 7) */
    vdp_set_reg(0, on ? 0x14 : 0x04);   /* bit 4: the H interrupt enabled */
    irq_restore(sr);
}

/* the frame interrupt: flips the view buffer (and its palettes) when a new
   picture has been uploaded; with the raster effects, puts the view's
   palettes back (the status bar's took palette 2) and the floor and
   ceiling's first row. The sound is done after, with the H interrupts
   let in (crt0.s).
   Called by crt0.s's vblank_handler at the start of the vertical blank,
   with the interrupts masked. */
void vblank_c(void)
{
    u8 i, red;
    frame_count++;
    hud_done = 0;
    if (pending_view_buf != 0xFF) {
        /* register 2: plane A's name table, $C000 (buffer 0) or $E000 (1) */
        vdp_set_reg(2, pending_view_buf ? (VRAM_PLANE_A1 >> 10) : (VRAM_PLANE_A0 >> 10));
        for (i = 0; i < 48; i++) shown_pal[i] = pending_cram[i];
        shown_view_buf = pending_view_buf;
        pending_view_buf = 0xFF;
        flashing = 0xFF;                    /* (re)load the palettes below */
    }
    /* damage flash: the view's colours (and the floor and ceiling behind
       it) pushed to red for a few frames */
    red = 0;
    if (flash_frames) {
        flash_frames--;
        red = 1;
        flashing = 0xFF;
    }
    if (raster_on) {
        /* palettes 0, 1 the view's; 2 the status bar's above the view (the
           message line), the view's from its first band (crt0.s) */
        u16 or_ = red ? 0x00E : 0;      /* the flash: red channel full */
        extern const u16 ui_hud_pal[16];
        vdp_set_cram_addr(0);
        /* colour 0 of palette 0 is the backdrop colour: the border round
           the view, black (not flashed); the view never shows it */
        VDP_DATA = 0;
        for (i = 1; i < 32; i++) VDP_DATA = shown_pal[i] | or_;
        for (i = 0; i < 16; i++) {
            VDP_DATA = ui_hud_pal[i];
            pal2_view[i] = shown_pal[32 + i] | or_;
        }
        flashing = red;
        top_done = 0;
    } else if (flashing) {
        /* no raster effects: palettes 0..2 the view's, written only when
           they changed or the flash starts / ends */
        u16 or_ = red ? 0x00E : 0;
        vdp_set_cram_addr(0);
        for (i = 0; i < 48; i++) VDP_DATA = shown_pal[i] | or_;
        flashing = red;
    }
    if (raster_on) {
        /* the floor and ceiling's colours: this frame's set (red during
           the flash), and band 0's into palette 3, slots 13 and 14 (CRAM
           byte 96 = colour 48, palette 3's first) */
        bd_rows_p = red ? bd_rows_red : bd_rows;
        vdp_set_cram_addr(96 + BD_ROW_SLOT * 2);
        VDP_DATA32 = bd_rows_p[0];
        /* then the queued DMAs, through the blank and the top border to
           line ~VIEW_LINE0 - 4 (20). The first H interrupt (end of line 7)
           then comes late, in the border, where it loads the view's
           palette 2 (the message line above keeps the status bar's) and
           band 0's colours before the view starts at VIEW_LINE0. The V
           counter: lines up to 234 as themselves, then 229..255 for
           235..261; here, at the start of the blank, below 229 */
        if (dq_head != dq_tail) {
            u8 v = vcount();
            /* about 56 words a line to the end of the blank (262), plus
               VIEW_LINE0 - 4 lines of 7 words into the next frame */
            if (v >= 224 && v < 229) dma_run((u16)(262 - v) * 56 + (VIEW_LINE0 - 4) * 7);
        }
    }
}

/* the frame interrupt's second part (crt0.s, with the H interrupts let
   in): the sound effects, then the music */
void vblank_sound(void)
{
    sound_update();
    music_update();
}

/* ---- joypad: 6-button protocol (TH toggled; the third low phase reads
   0000 in the direction bits on a 6-button pad, the fourth high phase
   gives Mode X Y Z) ---- */
#define PAD_DATA (*(volatile u8 *)0xA10003)     /* port 1's data: bit 6 = TH, bits 5-0 buttons (0 pressed) */
#define PAD_CTRL (*(volatile u8 *)0xA10009)     /* port 1's direction: 1 bits are outputs */

u8 pad_six;                                   /* 1 = a 6-button pad answered */

/* port 1: TH (bit 6) an output, set high (the pad's resting state) */
void pad_init(void)
{
    PAD_CTRL = 0x40;
    PAD_DATA = 0x40;
}

/* a short pause (four nops) after changing TH, for the pad to answer */
#define PAD_WAIT() __asm__ volatile("nop\n nop\n nop\n nop")

/* reads port 1: the PAD_* bits of the buttons held (1 = pressed), with X,
   Y, Z and Mode on a 6-button pad; sets pad_six. Toggles TH four times
   high-low (the 6-button pad's sequence), with the interrupts held off so
   the sequence keeps its timing; the pad resets its counter by itself
   after about 1.5 ms without toggles, so reads one frame apart start
   afresh. */
u16 pad_read(void)
{
    u8 b1, a1, a3, b4;
    u16 sr = irq_off();
    PAD_DATA = 0x40; PAD_WAIT(); b1 = PAD_DATA;     /* ? 1 C B R L D U */
    PAD_DATA = 0x00; PAD_WAIT(); a1 = PAD_DATA;     /* ? 0 S A 0 0 D U */
    PAD_DATA = 0x40; PAD_WAIT();
    PAD_DATA = 0x00; PAD_WAIT();
    PAD_DATA = 0x40; PAD_WAIT();
    PAD_DATA = 0x00; PAD_WAIT(); a3 = PAD_DATA;     /* ? 0 S A 0 0 0 0 on a 6-button pad */
    PAD_DATA = 0x40; PAD_WAIT(); b4 = PAD_DATA;     /* ? 1 C B M X Y Z */
    PAD_DATA = 0x00; PAD_WAIT();
    PAD_DATA = 0x40;
    irq_restore(sr);
    {
        /* bits 0-5 U D L R B C from the TH-high read, 6-7 A and Start from
           the TH-low read, inverted (the pad reads 0 when pressed) */
        u16 r = (u8)(~((b1 & 0x3F) | ((a1 & 0x30) << 2)));
        pad_six = (a3 & 0x0F) == 0;
        if (pad_six) {
            u8 e = ~b4;
            if (e & 1) r |= PAD_Z;
            if (e & 2) r |= PAD_Y;
            if (e & 4) r |= PAD_X;
            if (e & 8) r |= PAD_MODE;
        }
        return r;
    }
}
