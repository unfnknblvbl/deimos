/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * The Mega Drive game - Mega Drive hardware: types, VDP, DMA, joypad.
 *
 * Every C file of the Mega Drive version includes this header. It gives:
 *   - the fixed-size integer types (the 68000 is big-endian; with
 *     m68k-elf-gcc an int is 32 bits, a short 16, a long 32);
 *   - the VDP (video display processor) ports and the VRAM layout of the
 *     game screen;
 *   - the interrupt mask helpers irq_off / irq_restore;
 *   - the prototypes of src/vdp.c (video set-up, DMA, frame interrupt,
 *     joypad) and the joypad's button bits.
 *
 * Terms used here and in the files that include it:
 *   view      - the 3D first-person picture, 208x128 pixels, drawn by the
 *               renderer (src/render.s) into one of two VRAM tile buffers
 *               and shown in the middle of the screen, with a black
 *               border round it (see VIEW_ROW0 below);
 *   plane A/B - the VDP's two scrolling tile layers; the window - a third,
 *               fixed layer that replaces plane A in part of the screen;
 *   name table - a plane's grid of 16-bit tile entries (see NT_* below);
 *   CRAM      - colour RAM: 4 palettes ("lines") of 16 colours; a CRAM
 *               colour is a 16-bit word 0000BBB0GGG0RRR0 (3 bits a channel).
 *
 * Licence: GPL-2.0-or-later.
 */
#ifndef MD_H
#define MD_H

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef signed short s16;
typedef unsigned long u32;
typedef signed long s32;

/* VDP ports. The data port reads and writes VRAM, CRAM or VSRAM at the
   address last set through the control port (which then advances by the
   auto-increment, register 15). The control port takes register writes
   ($8000 | reg << 8 | value) and address / DMA commands (two words, or one
   long write). The 32-bit versions write two words in one instruction.
   PSG_PORT is the SN76489 sound chip (the PSG: 3 tone voices and a noise
   voice), written one byte at a time. */
#define VDP_DATA (*(volatile u16 *)0xC00000)
#define VDP_DATA32 (*(volatile u32 *)0xC00000)
#define VDP_CTRL (*(volatile u16 *)0xC00004)
#define VDP_CTRL32 (*(volatile u32 *)0xC00004)
#define PSG_PORT (*(volatile u8 *)0xC00011)

/* ---- VRAM layout (H32, 32x32 planes) ----
   $0000 view tiles, buffer 0 (416 tiles, column-major: tile = x*16 + y;
         $3400..$4FFF are free since the view shrank to 208x128)
   $5000 view tiles, buffer 1
   $A000 plane B name table (dialog boxes, over the view)
   $A800 window name table (status bar: rows 21..27)
   $B000 misc tiles 1408..1535 (font, blank)
   $C000 plane A name table 0 (shows buffer 0)
   $C800 misc tiles 1600..1727 (status bar art)
   $D800 sprite table, $DA00 misc 1744..1759, $DC00 h-scroll
   $E000 plane A name table 1 (shows buffer 1)
   $E800 misc tiles 1856..2047 (weapon sprites)
   The view is double-buffered: the renderer draws into the buffer not on
   screen, and the frame interrupt flips plane A's name table (register 2)
   between $C000 and $E000 to show the other one. A tile is 32 bytes (8x8
   pixels, 4 bits a pixel), so tile number n lives at VRAM address n * 32.
   Plane B sits behind plane A and holds the shaded floor and ceiling
   (src/backdrop.c) in the view's rows, and the message line (row 0) and
   dialog boxes (with the priority bit, so they show in front of plane A).

   Screen (32x28 tiles): row 0 the message line; the view in tile rows
   VIEW_ROW0..VIEW_ROW0 + VIEW_TH - 1 (3..18) and tile columns
   VIEW_COL0..VIEW_COL0 + VIEW_TW - 1 (3..28), centred where the old
   256x160 view (rows 1..20, all 32 columns) was; the status bar in rows
   21..27. Everything else (rows 1, 2, 19, 20, the 3 columns each side) is
   the border: both planes blank there, so the backdrop colour (palette 0
   colour 0, kept black) shows. */
#define VIEW_TW 26                  /* the view's size in tiles: 26 wide */
#define VIEW_TH 16                  /*   and 16 high */
#define VIEW_W (VIEW_TW * 8)        /* the view's size in pixels: 208 */
#define VIEW_H (VIEW_TH * 8)        /*   x 128 */
#define VIEW_TILES (VIEW_TW * VIEW_TH)      /* 416 tiles a buffer */
#define VIEW_BYTES (VIEW_TILES * 32)        /* 13312 bytes ($3400) a buffer */
#define VIEW_ROW0 3                 /* first screen tile row of the view */
#define VIEW_COL0 3                 /* first screen tile column of the view */
#define VIEW_LINE0 (VIEW_ROW0 * 8)  /* first screen line of the view (24) */
#define VRAM_BUF0 0x0000            /* the view's tile buffers (tiles 0.. and 640..) */
#define VRAM_BUF1 0x5000
#define VIEW_BUF1_TILE (VRAM_BUF1 / 32)     /* buffer 1's first tile: 640 */
#define VRAM_PLANE_B 0xA000         /* name tables: see the layout above */
#define VRAM_WINDOW 0xA800
#define VRAM_PLANE_A0 0xC000
#define VRAM_PLANE_A1 0xE000
#define VRAM_SAT 0xD800             /* sprite attribute table (8 bytes a sprite) */
#define VRAM_HSCROLL 0xDC00         /* horizontal scroll table (left at 0) */
#define TILE_FONT 1408              /* 96 ASCII glyphs, 32..127 */
#define TILE_BLANK TILE_FONT        /* the space glyph: all transparent */
#define TILE_MISC_B 1600            /* status bar art */
#define TILE_SPR 1856               /* weapon sprite patterns */

/* name table entry bits: priority (bit 15), palette line (bits 14-13),
   vertical flip (bit 12), horizontal flip (bit 11); bits 10-0 are the tile
   number */
#define NT_PAL(n) ((n) << 13)
#define NT_PRIO 0x8000
#define NT_HFLIP 0x0800
#define NT_VFLIP 0x1000

/* VDP access sequences (address, then data) must not be split by the frame
   interrupt, which writes the palette: wrap them in these */

/* irq_off: saves the 68000 status register (SR) and sets it to $2700:
   supervisor mode, interrupt mask 7, so no interrupt (frame or H) can come
   in. Returns the old SR for irq_restore. Clobbers nothing else. */
static inline u16 irq_off(void)
{
    u16 sr;
    __asm__ volatile("move.w %%sr,%0\n\tmove.w #0x2700,%%sr" : "=d"(sr) : : "memory");
    return sr;
}

/* irq_restore: puts back the status register that irq_off returned (and
   with it the interrupt mask that was in force before). */
static inline void irq_restore(u16 sr)
{
    __asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "memory");
}

/* src/vdp.c: see each function's comment there */
void vdp_init(void);                /* registers, clear VRAM / CRAM / VSRAM */
void vdp_set_reg(u8 reg, u8 val);   /* write one VDP register */
void vdp_display(u8 on);            /* screen on (1) or blanked (0) */
void vdp_set_vram_addr(u16 addr);   /* next data port writes go to VRAM addr */
void vdp_set_cram_addr(u16 addr);   /* next data port writes go to CRAM byte addr */
void vdp_write_cram(u8 index, const u16 *cols, u8 n);   /* n colours from colour index */
void vdp_dma_vram(const void *src, u16 vram, u16 words);    /* DMA, waits until done */
void vdp_dma_queue(const void *src, u16 vram, u16 words);   /* (done by the interrupts on the game screen) */
void vdp_dma_wait(void);            /* wait for the DMA queue to empty */
void vdp_fill_vram(u16 vram, u16 bytes, u8 val);    /* fill with a byte, by the CPU */
void vdp_put_nt(u16 nt, u8 x, u8 y, u16 entry);     /* one name table entry */
void wait_vblank(void);             /* wait for the next frame interrupt */

extern volatile u16 frame_count;       /* frames since power-on (frame interrupt) */
extern volatile u8 pending_view_buf;   /* 0/1: flip to this buffer at the next vblank, 0xFF none */
extern u8 shown_view_buf;              /* 0/1: the view buffer on screen now */
extern volatile u16 pending_cram[48];   /* the view's palettes (0..2), applied with the flip */
extern u8 raster_on;                    /* the game screen's raster effects (vdp_raster) */
void vdp_raster(u8 on);
extern volatile u8 flash_frames;        /* red damage flash: frames left (set to 6 when hurt) */

/* joypad (1 = pressed): the bits pad_read returns. Bits 0-7 are the
   3-button pad's (the order the hardware reports them in); bits 8-11 come
   only from a 6-button pad. */
#define PAD_UP 0x01
#define PAD_DOWN 0x02
#define PAD_LEFT 0x04
#define PAD_RIGHT 0x08
#define PAD_B 0x10
#define PAD_C 0x20
#define PAD_A 0x40
#define PAD_START 0x80
#define PAD_X 0x100
#define PAD_Y 0x200
#define PAD_Z 0x400
#define PAD_MODE 0x800
void pad_init(void);                /* port 1's TH line as an output */
u16 pad_read(void);                 /* port 1's buttons (PAD_* bits) */
extern u8 pad_six;                  /* 1: the last pad_read found a 6-button pad */

#endif
