/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the Game Boy Color: the screen.
 *
 * The game's screen (BG scrolled: SCY 112, SCX 240, so the view's tiles
 * start a map row and the maps' rows 0..13 are free for the window):
 *   row 0        the message line (20 tiles, 26 characters of 6 pixels)
 *   rows 1..12   the view: 16 x 12 tiles (128 x 96), between two black
 *                columns; drawn twice over, VRAM bank 0 (map $9800) and
 *                bank 1 (map $9C00), the one not shown filled, then shown
 *   rows 13..17  the face (5 x 5 tiles) and the status bar (5 lines of 20
 *                characters, 15 tiles each)
 *   the window   the boxes (dialogs, menus): up to 14 rows of 18 tiles at
 *                the bottom, its map in $9C00's rows 0..13, its tiles in
 *                bank 1 where the view's second picture and the status
 *                bar's lines 1..4 are (the view then stays on bank 0's,
 *                the status bar is hidden)
 *   sprites      the weapon (8 x 16, OBJ tiles 0..127 of bank 0)
 * Tiles (BG: $8800 addressing): view 0..191 from $8800 (numbers 128..255,
 * 0..63), then (numbers 64..) bank 0: face 64, message 89, status line 0
 * 109, blank 124; bank 1: status lines 1..4 from 64.
 * Palettes: 0 the text (black, dark blue, yellow, white), 1 the face,
 * 2..4 the view above the horizon (colour 0 the ceiling), 5..7 below it
 * (the floor); the title's are its own (7: the menu box's).
 *
 * The video RAM is written in the vertical blank only: a queue of jobs
 * (DMA from the cartridge RAM's bank 7, where the text is drawn, or bank
 * 6, the view's tiles) that wait_vblank works through.
 *
 * Who calls what: ui.c calls the gfx_* functions and wait_vblank (once a
 * frame); gfx_view runs view.c's view_render (which calls vdraw.c's
 * draw_walls / draw_spr) and vdraw.c's vd_tiles, then queues the upload.
 * Registers used: LCDC ($FF40: bit 7 LCD on, 6 window map $9C00, 5 window
 * on, 4 tiles at $8000 (else $8800), 3 BG map $9C00, 2 8x16 sprites, 1
 * sprites on, 0 BG on), SCY / SCX / WY / WX (scroll, window position), VBK
 * ($FF4F: VRAM bank), the HDMA registers ($FF51-$FF55: VRAM DMA) and the
 * colour palette ports ($FF68-$FF6B). A BG map attribute byte (VRAM bank
 * 1) holds the palette in bits 0..2 and the tile's VRAM bank in bit 3.
 * Licence: GPL-2.0-or-later.
 */
#pragma bank 255
#include <gb/gb.h>
#include <gb/cgb.h>
#include <string.h>
#include "plat.h"
#include "gfx.h"
#include "vdraw.h"
#include "version.h"
#include "view_api.h"                   /* (view_gb.c's functions, banked: view.h's prototypes are not) */
#include "save_api.h"                    /* (cfg: the settings) */
#ifdef VIEW_PROF
#include "../../view/vdata.h"
#include "view_ram.h"
#endif

u16 pad;                                /* the pad (SNES bits), read by wait_vblank */
#ifdef VIEW_PROF
u16 prof_t[4], prof_v[6];               /* (profiling: frames per part, read by the tools) */
#endif
volatile u16 frame_count;               /* frames since power-on */
u16 pad_read(void) BANKED;              /* (pad.c) */
void view_ram_init(void) BANKED;        /* (view_gb.c, made by tools/view_ram.py) */

/* the CGB's VRAM DMA: HDMA1/2 the source (high, low; a multiple of 16),
   HDMA3/4 the destination in VRAM, HDMA5 the length / 16 - 1 and the mode
   (bit 7 clear: general-purpose, all at once) */
#define R_HDMA1 (*(volatile u8 *)0xFF51)
#define R_HDMA2 (*(volatile u8 *)0xFF52)
#define R_HDMA3 (*(volatile u8 *)0xFF53)
#define R_HDMA4 (*(volatile u8 *)0xFF54)
#define R_HDMA5 (*(volatile u8 *)0xFF55)

#define ST_BANK 7                       /* the cartridge RAM's staging bank */
#define ST_BOX 0xA000                   /* 18 x 14 tiles */
#define ST_HUD 0xB000                   /* 5 x 15 tiles */
#define ST_MSG 0xB500                   /* 20 tiles */
#define ST_FACE 0xB640                  /* 25 tiles */
#define ST_HUDTXT 0xB7D0                /* the status bar's text (5 x 21) */
#define ST_WP 0xB800                    /* up to 128 tiles */
#define TILE_BANK 6                     /* the cartridge RAM bank vdraw.c writes the view's tiles to */

/* tile numbers (BG, $8800 addressing: 64 is $9400) */
#define T_FACE 64                       /* bank 0: the face, 25 tiles */
#define T_MSG 89                        /* bank 0: the message line, 20 tiles */
#define T_HUD0 109                      /* bank 0: the status bar's line 0, 15 tiles */
#define T_BLANK 124                     /* bank 0: an empty tile */
#define T_HUD1 64                       /* bank 1: the status bar's lines 1..4, 15 tiles each */
#define VT_ADDR(k) (0x8800 + (u16)(k) * 16)     /* tile k of the view's (or the box's) */
#define MAP_ROW0 14                     /* the screen's row 0 in the maps */

static u8 scr_mode;                         /* 0 nothing yet, 1 the title, 2 the game */
static u8 shown;                        /* the view's picture shown: 0 bank 0 ($9800), 1 */
static u8 box_up, box_rows, pic_pending, hud_hidden;    /* a box on the window, its rows; a picture waiting for the box to go; status lines 1..4 not shown */
static u8 flash_n;                      /* frames of the red flash left */
static u8 pal_map = 0xFF;               /* the map whose colours palettes 2..7 hold */
static u16 bgpal[8][4], objpal[2][4];   /* the palettes (BGR555): 8 BG, 2 OBJ */
static u8 pal_dirty;                    /* palbuf to be written at the next vertical blank */

/* ------------------------------------------------------------ the jobs */
/* A job is one change of the video RAM or the LCD registers, done in the
   vertical blank: a ring of NJ (a power of two), jq_head the next to run.
   J_DMA   len bytes from cartridge RAM bank sbank's src to VRAM bank
           vbank's dst
   J_ATTR  the view's palettes (bank 6's VD_ATTR) into map arg (0 $9800,
           1 $9C00)
   J_FLIP  show the BG map arg (0 $9800, 1 $9C00): LCDC bit 3
   J_WIN   the window at line arg (0: off)
   J_PAL   palbuf to the palette RAM
   J_ACOL  the view's palettes of one column of tiles (src: 0..15) into
           map arg (the outward drawing, a column at a time) */
enum { J_DMA = 1, J_ATTR, J_FLIP, J_WIN, J_PAL, J_ACOL };
struct job {
    u8 kind, sbank, vbank, arg;
    u16 src, dst, len;
};
#define NJ 16
static struct job jq[NJ];
static u8 jq_head, jq_tail;

/* a general-purpose VRAM DMA: len bytes (a multiple of 16) from src to
   dst in the VRAM bank in place; the CPU stops until it is done */
static void gdma(u16 src, u16 dst, u16 len)
{
    R_HDMA1 = (u8)(src >> 8);
    R_HDMA2 = (u8)src & 0xF0;
    R_HDMA3 = (u8)(dst >> 8) & 0x1F;
    R_HDMA4 = (u8)dst & 0xF0;
    R_HDMA5 = (u8)((len >> 4) - 1);     /* (general: the CPU waits) */
}

/* the palettes: made ready in palbuf (BG then OBJ), written in the
   vertical blank by pals_out (about a line and a half) */
static u8 palbuf[80];

/* bgpal and objpal into palbuf, as the palette ports take them (low byte
   first); while flash_n runs, the view's palettes (2..7) get full red */
static void prep_pals(void)
{
    u8 p, c, *d = palbuf;
    for (p = 0; p < 8; p++)
        for (c = 0; c < 4; c++) {
            u16 v = bgpal[p][c];
            if (flash_n && p >= 2) v |= 0x1F;   /* (red: bits 0..4) */
            *d++ = (u8)v;
            *d++ = (u8)(v >> 8);
        }
    for (p = 0; p < 2; p++)
        for (c = 0; c < 4; c++) {
            *d++ = (u8)objpal[p][c];
            *d++ = (u8)(objpal[p][c] >> 8);
        }
}

/* palbuf to the palette RAM: BCPS ($FF68) = $80 (index 0, auto-increment),
   64 bytes to BCPD ($FF69); OCPS ($FF6A) = $80, 16 bytes to OCPD ($FF6B).
   Unrolled so it fits in the vertical blank. Clobbers a, c, hl */
static void pals_out(void) __naked
{
    __asm
    ld  hl, #_palbuf
    ld  a, #0x80
    ldh (0x68), a
    ld  c, #0x69
    .rept 64
    ld  a, (hl+)
    ldh (c), a
    .endm
    ld  a, #0x80
    ldh (0x6A), a
    ld  c, #0x6B
    .rept 16
    ld  a, (hl+)
    ldh (c), a
    .endm
    ret
    __endasm;
}

/* the palettes written now (the screen is off: gfx_title / gfx_game) */
static void write_pals(void)
{
    prep_pals();
    pals_out();
}

/* the jobs, as many as the vertical blank has time for (LY 144..153; a
   DMA of 16 bytes takes 1/8 of a line). A DMA job too big for what is left
   is done in part and finished next frame. Called by wait_vblank. Leaves
   VRAM bank 0 and cartridge RAM bank 1 in place */
static void run_jobs(void)
{
    while (jq_head != jq_tail) {
        struct job *j = &jq[jq_head];
        u8 ly = LY_REG;
        if (ly < 144 || ly > 151) break;    /* (LY, $FF44: past the blank's start, or too near its end) */
        if (j->kind == J_DMA) {
            u16 room = (u16)(152 - ly) * 112, n = j->len < room ? j->len : room;    /* (112 bytes a line left before line 152) */
            n &= ~15;                   /* (whole 16-byte blocks) */
            if (!n) break;
            SWITCH_RAM(j->sbank);
            VBK_REG = j->vbank;
            gdma(j->src, j->dst, n);    /* (the CPU waits for it) */
            j->src += n;
            j->dst += n;
            j->len -= n;
            if (j->len) break;          /* (the rest next frame) */
        } else if (j->kind == J_ATTR) {     /* the view's palettes: 12 rows of 16 */
            u8 r;
            u16 src = 0xA000 + VD_ATTR + (j->arg ? 192 : 0), dst = (j->arg ? 0x9C00 : 0x9800) + MAP_ROW0 * 32 + 32;
            if (ly > 146) break;        /* (12 DMAs: it needs about a line) */
            SWITCH_RAM(TILE_BANK);
            VBK_REG = 1;
            for (r = 0; r < 12; r++, src += 16, dst += 32) gdma(src, dst, 16);  /* (a map row is 32 bytes) */
        } else if (j->kind == J_ACOL) {     /* a column of the view's palettes: 12 bytes */
            u8 r, *src = (u8 *)0xA000 + VD_ATTR + (j->arg ? 192 : 0) + j->src;
            u8 *dst = (u8 *)((j->arg ? 0x9C00 : 0x9800) + MAP_ROW0 * 32 + 32 + j->src);
            if (ly > 150) break;
            SWITCH_RAM(TILE_BANK);
            VBK_REG = 1;
            for (r = 0; r < 12; r++, src += 16, dst += 32) *dst = *src;
        } else if (j->kind == J_FLIP) {
            if (j->arg) LCDC_REG |= 0x08;   /* (LCDC bit 3: the BG map at $9C00) */
            else LCDC_REG &= ~0x08;
        } else if (j->kind == J_WIN) {
            if (j->arg) {
                WY_REG = j->arg;
                LCDC_REG |= 0x20;       /* (LCDC bit 5: the window on) */
            } else
                LCDC_REG &= ~0x20;
        } else if (j->kind == J_PAL) {
            if (ly > 150) break;        /* (about a line and a half) */
            pals_out();
        }
        jq_head = (jq_head + 1) & (NJ - 1);
    }
    VBK_REG = 0;
    SWITCH_RAM(1);
}

/* the frame: waits for the vertical blank, writes pending palettes, runs
   the jobs, counts down the flash (its end rewrites the palettes next
   frame) and reads the pad into pad. Everything that waits calls this */
void wait_vblank(void) BANKED
{
    vsync();                            /* (GBDK: wait for the vertical blank interrupt) */
    frame_count++;
    if (pal_dirty) {
        pal_dirty = 0;
        pals_out();
    }
    run_jobs();
    if (flash_n && !--flash_n) {
        prep_pals();
        pal_dirty = 1;
    }
    pad = pad_read();                   /* (pad.c: the buttons as SNES bits, or the test's script) */
}


/* a job queued (see enum above); when the ring is full, frames pass until
   there is room */
static void job(u8 kind, u8 sbank, u16 src, u8 vbank, u16 dst, u16 len, u8 arg)
{
    struct job *j;
    while (((jq_tail + 1) & (NJ - 1)) == jq_head) wait_vblank();
    j = &jq[jq_tail];
    j->kind = kind;
    j->sbank = sbank;
    j->src = src;
    j->vbank = vbank;
    j->dst = dst;
    j->len = len;
    j->arg = arg;
    jq_tail = (jq_tail + 1) & (NJ - 1);
}

/* frames pass until every queued job is done */
static void jobs_flush(void)
{
    while (jq_head != jq_tail) wait_vblank();
}

/* ------------------------------------------------------------ text */
static u32 font_at;                     /* the font's block offset (0 until init_common) */
static u8 g8[8];                        /* the glyph glyph() read: 8 rows, pixels in bits 6..1 */

/* character ch's glyph into g8 (outside 32..127: a space) */
static void glyph(char ch)
{
    u8 c = (u8)ch;
    if (c < 32 || c > 127) c = 32;
    far_copy(font_at + (u16)(c - 32) * 8, g8, 8);
}

/* a line of text into tiles (in the staging bank, mapped): white on black.
   dst: ntiles tiles side by side (16 bytes each), cleared first; s drawn
   from pixel x0, 6 pixels a character, as far as it fits */
static void text_line(u8 *dst, u8 ntiles, const char *s, u8 x0)
{
    u8 px = x0, r, t;
    memset(dst, 0, (u16)ntiles * 16);
    for (; *s; s++, px += 6) {
        if (px + 6 > ntiles * 8) break;
        if (*s == ' ') continue;
        glyph(*s);
        t = px >> 3;
        for (r = 0; r < 8; r++) {
            u16 w = ((u16)(u8)(g8[r] << 1) << 8) >> (px & 7);   /* (the row's 6 pixels at bits 15..10, moved to the pixel's place in two tiles) */
            u8 *p = dst + (u16)t * 16 + r * 2;
            p[0] |= (u8)(w >> 8);       /* (both bit planes: colour 3) */
            p[1] |= (u8)(w >> 8);
            if ((u8)w && t + 1 < ntiles) {  /* (what spills into the next tile) */
                p[16] |= (u8)w;
                p[17] |= (u8)w;
            }
        }
    }
}

/* ------------------------------------------------------------ the maps */
/* screen tile (row, col) of BG map map ($9800 / $9C00): its tile number
   (VRAM bank 0) and attributes (bank 1), with the game's scroll (SCY
   MAP_ROW0 rows, SCX 240 = 30 columns) taken into account. The screen
   must be off */
static void put_map(u16 map, u8 row, u8 col, u8 tile, u8 attr)
{
    u16 a = map + (u16)((row + MAP_ROW0) & 31) * 32 + ((col + 30) & 31);    /* (SCX 240) */
    VBK_REG = 0;
    *(u8 *)a = tile;
    VBK_REG = 1;
    *(u8 *)a = attr;
    VBK_REG = 0;
}

/* BG palette p: the text's colours (0 black, 1 dark blue, 2 yellow, 3 white) */
static void ui_palette(u8 p)
{
    bgpal[p][0] = RGB(0, 0, 0);
    bgpal[p][1] = RGB(2, 3, 10);
    bgpal[p][2] = RGB(31, 26, 0);
    bgpal[p][3] = RGB(31, 31, 31);
}

/* the window's map ($9C00 rows 0..13): the box's tiles, row by row. 18
   tiles a row between two blank columns (tile blank, attributes
   blank_attr); box tile k (row by row) is VRAM bank 1's tile 128 + k for
   k < 192 (the $8800 area), then 64.. (attributes: bank 1, palette pal).
   The screen must be off */
static void window_map(u8 blank, u8 blank_attr, u8 pal)
{
    u8 r, c;
    for (r = 0; r < 14; r++)
        for (c = 0; c < 20; c++) {
            u16 a = 0x9C00 + (u16)r * 32 + c;
            u8 k = r * 18 + c - 1;
            VBK_REG = 0;
            *(u8 *)a = (c == 0 || c == 19) ? blank : (u8)(k < 192 ? 128 + k : 64 + (k - 192));
            VBK_REG = 1;
            *(u8 *)a = (c == 0 || c == 19) ? blank_attr : (u8)(0x08 | pal);
        }
    VBK_REG = 0;
}

/* both VRAM banks' tiles and maps cleared (the screen must be off) */
static void vram_clear(void)
{
    VBK_REG = 1;
    memset((u8 *)0x8000, 0, 0x2000);
    VBK_REG = 0;
    memset((u8 *)0x8000, 0, 0x2000);
}

static u8 hud_txt[5][21];               /* the status bar's lines as drawn (only changed lines are redrawn) */
static u8 cur_face = 0xFF, wp_frame = 0xFF;     /* the face and weapon frame in VRAM (255: none) */

/* what the title and the game screen start with: the first time, the font,
   the view's RAM (bank 3) and vdraw.c's picture set up; then the jobs
   finished, the screen off, VRAM cleared, every sprite hidden */
static void init_common(void)
{
    if (!font_at) {
        font_at = res_at(RES_FONT);
        SWITCH_RAM(3);
        view_ram_init();                /* (view.c's variables: their first values) */
        SWITCH_RAM(1);
        vd_init();                      /* (the picture's rows outside the view: cleared) */
    }
    jobs_flush();
    box_up = 0;
    hud_hidden = 0;
    flash_n = 0;
    DISPLAY_OFF;                        /* (GBDK: waits for the vertical blank, LCD off: VRAM free to write) */
    vram_clear();
    {
        u8 i;
        for (i = 0; i < 40; i++) move_sprite(i, 0, 0);  /* (y 0: off the screen) */
    }
    wp_frame = 0xFF;
}

/* the title: the DEIMOS logo and its subtitle (the resources': 20 x 10
   tiles, palettes 0..6), the version and PRESS START; palette 7 the text's
   (and the menu box's) */

/* a centred line of text on the title's screen row (12..17), in VRAM
   bank 1's tiles for that row; yellow clears the low bit plane, making the
   white (colour 3) yellow (colour 2). The screen must be off */
static void title_line(u8 row, const char *s, u8 yellow)  /* bank 1's tiles 0..119: rows 12..17 */
{
    u8 n = 0, *t = (u8 *)0x8000 + (u16)(row - 12) * 20 * 16;
    u16 k;
    while (s[n]) n++;
    VBK_REG = 1;
    text_line(t, 20, s, (160 - n * 6) / 2);     /* (straight into VRAM: the screen is off) */
    if (yellow)
        for (k = 0; k < 20 * 16; k += 2) t[k] = 0;
    VBK_REG = 0;
}

/* the title screen: rows 0..1 blank, rows
   2..11 the logo (the resource's tiles from 0, its map and attributes),
   rows 12..17 text (bank 1's tiles). $8000 tile addressing, no scroll. The
   menu box (gfx_box) comes up on the window */
void gfx_title(void) BANKED
{
    u32 at = res_at(RES_TITLE);
    u16 n = far_u16be(at);
    u8 r, c;
    init_common();
    scr_mode = 1;
    VBK_REG = 0;
    far_copy(at + 2, (void *)0x8000, n * 16);       /* (at most 200 tiles) */
    at += 2 + n * 16;                   /* (now the map: 200 tile numbers, then 200 attributes, then 7 palettes) */
    for (r = 0; r < 18; r++)
        for (c = 0; c < 20; c++) {
            u16 a = 0x9800 + (u16)r * 32 + c;
            u8 t = 127, attr = 0x0F;                    /* (bank 1's tile 127: blank) */
            if (r < 2) {
                /* (rows 0..1: blank, above the logo) */
            } else if (r < 12) {
                t = far_u8(at + (r - 2) * 20 + c);
                attr = far_u8(at + 200 + (r - 2) * 20 + c);
            } else {
                t = (r - 12) * 20 + c;
                attr = 0x0F;
            }
            VBK_REG = 0;
            *(u8 *)a = t;
            VBK_REG = 1;
            *(u8 *)a = attr;
        }
    VBK_REG = 0;
    far_copy(at + 400, bgpal, 56);      /* (the logo's palettes 0..6, little-endian: as bgpal holds them) */
    ui_palette(7);
    title_line(13, "A Doom RPG Engine", 0);
    title_line(15, "Version " GB_VERSION, 0);
    title_line(17, "PRESS START", 1);
    window_map(127, 0x0F, 7);           /* (the menu box's map, palette 7) */
    write_pals();
    SCY_REG = 0;
    SCX_REG = 0;
    WX_REG = 7;                         /* (the window's left edge at x 0) */
    WY_REG = 144;                       /* (below the screen until a box comes up) */
    LCDC_REG = 0x80 | 0x40 | 0x10 | 0x04 | 0x01;          /* window map $9C00, $8000 tiles, 8x16 sprites */
}

/* the backdrop's and the families' colours into palettes 2..7 */
static u16 floor_c, ceil_c;             /* the backdrop's colours (BGR555, gfx_backdrop) */

/* palettes 2..4 (above the horizon: colour 0 the ceiling) and 5..7 (below:
   colour 0 the floor); colours 1..3 the map's three families' shades
   (resource RES_MAPS, 18 bytes after the map's 512-byte table). Queued for
   the next vertical blank */
static void view_palettes(void)
{
    u8 p, c;
    for (p = 0; p < 3; p++) {
        bgpal[2 + p][0] = ceil_c;
        bgpal[5 + p][0] = floor_c;
    }
    if (pal_map != 0xFF) {
        u8 b[18];
        far_copy(res_at(RES_MAPS) + 530UL * pal_map + 512, b, 18);
        for (p = 0; p < 3; p++)
            for (c = 0; c < 3; c++) {
                u16 v = b[6 * p + 2 * c] | ((u16)b[6 * p + 2 * c + 1] << 8);
                bgpal[2 + p][1 + c] = v;
                bgpal[5 + p][1 + c] = v;
            }
    }
    prep_pals();                        /* (palbuf now) */
    job(J_PAL, 0, 0, 0, 0, 0, 0);       /* (written in a vertical blank) */
}

/* the game screen: both BG maps filled (the view's tiles from tile 128,
   column by column: tile k = column * 12 + row; $9C00's view tiles from
   VRAM bank 1), the window's map, the palettes, the scroll. The pictures
   come later (gfx_view) */
void gfx_game(void) BANKED
{
    u8 r, c;
    init_common();
    scr_mode = 2;
    for (r = 0; r < 18; r++)
        for (c = 0; c < 20; c++) {
            u8 t = T_BLANK, a = 0, a1;
            if (r == 0) t = T_MSG + c;
            else if (r <= 12) {
                if (c >= 2 && c < 18) {
                    u8 k = (c - 2) * 12 + (r - 1);
                    t = 128 + k;            /* (wraps past 255 to 0..63) */
                    a = 2;
                }
            } else if (c < 5) {
                t = T_FACE + (r - 13) * 5 + c;
                a = 1;
            } else if (r == 13)
                t = T_HUD0 + c - 5;
            else {
                t = T_HUD1 + (r - 14) * 15 + c - 5;
                a = 0x08;
            }
            a1 = a;
            if (r >= 1 && r <= 12 && c >= 2 && c < 18) a1 |= 0x08;     /* (the second picture: VRAM bank 1) */
            put_map(0x9800, r, c, t, a);
            put_map(0x9C00, r, c, t, a1);
        }
    window_map(T_BLANK, 0, 0);
    ui_palette(0);
    {
        u8 b[8];
        far_copy(res_at(RES_FACE_PAL), b, 8);  /* (palette 1: the face's 4 colours) */
        for (c = 0; c < 4; c++) bgpal[1][c] = b[2 * c] | ((u16)b[2 * c + 1] << 8);
    }
    view_palettes();
    write_pals();
    shown = 0;
    pic_pending = 0;
    cur_face = 0xFF;
    memset(hud_txt, 0, sizeof hud_txt);
    SCY_REG = MAP_ROW0 * 8;             /* (map row 14 at the screen's top) */
    SCX_REG = 240;                      /* (map column 30 at the screen's left) */
    WX_REG = 7;
    WY_REG = 144;
    LCDC_REG = 0x80 | 0x40 | 0x04 | 0x02 | 0x01;          /* window map $9C00, $8800 tiles, 8x16 sprites on */
}

/* ------------------------------------------------------------ the message line, the status bar, the face */
/* the message line: s (at most 26 characters) centred, drawn in the
   staging bank, queued to tiles T_MSG.. */
void gfx_message(const char *s) BANKED
{
    u8 n = 0;
    if (scr_mode != 2) return;
    while (s[n] && n < 26) n++;
    SWITCH_RAM(ST_BANK);
    {
        char b[27];
        memcpy(b, s, n);
        b[n] = 0;
        text_line((u8 *)ST_MSG, 20, b, (160 - n * 6) / 2);
    }
    SWITCH_RAM(1);
    job(J_DMA, ST_BANK, ST_MSG, 0, 0x9000 + T_MSG * 16, 320, 0);
}

/* the status bar's line l (staged at ST_HUD + 240 l) queued to its tiles:
   line 0 in bank 0, lines 1..4 in bank 1 */
static void hud_upload(u8 l)
{
    if (l == 0) job(J_DMA, ST_BANK, ST_HUD, 0, 0x9000 + T_HUD0 * 16, 240, 0);
    else job(J_DMA, ST_BANK, ST_HUD + l * 240, 1, 0x9000 + (T_HUD1 + (l - 1) * 15) * 16, 240, 0);
}

/* the status bar: each line that changed is drawn and queued (lines 1..4
   wait while a box hides them: gfx_box_off uploads them) */
void gfx_hud(char lines[5][21]) BANKED
{
    u8 l;
    if (scr_mode != 2) return;
    for (l = 0; l < 5; l++) {
        if (!memcmp(hud_txt[l], lines[l], 20)) continue;
        memcpy(hud_txt[l], lines[l], 20);
        hud_txt[l][20] = 0;
        SWITCH_RAM(ST_BANK);
        text_line((u8 *)ST_HUD + l * 240, 15, (const char *)hud_txt[l], 0);
        SWITCH_RAM(1);
        if (l == 0 || !hud_hidden) hud_upload(l);
    }
}

/* face f: its 25 tiles (400 bytes, resource RES_FACES) staged and queued */
void gfx_face(u8 f) BANKED
{
    if (scr_mode != 2 || f == cur_face) return;
    cur_face = f;
    SWITCH_RAM(ST_BANK);
    far_copy(res_at(RES_FACES) + 400UL * f, (void *)ST_FACE, 400);
    SWITCH_RAM(1);
    job(J_DMA, ST_BANK, ST_FACE, 0, 0x9000 + T_FACE * 16, 400, 0);
}

/* ------------------------------------------------------------ the boxes */
/* the empty box: rows x 18 tiles at st, colour 1 filled, a frame of colour
   2 one pixel wide (low plane byte, then high plane byte, per pixel row) */
static void box_pixel_rows(u8 *st, u8 rows)
{
    /* the background (colour 1), a yellow frame (colour 2) */
    u16 k, n = (u16)rows * 18;
    u8 r, *p;
    for (k = 0; k < n; k++) {
        p = st + k * 16;
        for (r = 0; r < 8; r++) {
            p[2 * r] = 0xFF;
            p[2 * r + 1] = 0x00;
        }
    }
    for (k = 0; k < 18; k++) {          /* top and bottom lines */
        p = st + k * 16;
        p[0] = 0x00;
        p[1] = 0xFF;
        p = st + ((u16)(rows - 1) * 18 + k) * 16 + 14;
        p[0] = 0x00;
        p[1] = 0xFF;
    }
    for (k = 0; k < rows; k++)
        for (r = 0; r < 8; r++) {      /* left and right */
            p = st + (k * 18) * 16 + 2 * r;
            p[0] &= 0x7F;
            p[1] |= 0x80;
            p = st + (k * 18 + 17) * 16 + 2 * r;
            p[0] &= 0xFE;
            p[1] |= 0x01;
        }
}

/* a box of h lines of text (24 characters of 6 pixels, lines 7 pixels
   apart) on the window, from the screen's bottom: rows = h * 7 / 8 tile
   rows rounded up (5..14), the text centred in them. Its tiles go to VRAM
   bank 1 from $8800, where the view's second picture and status lines
   1..4 are: so in the game the shown picture is first moved to bank 0 */
void gfx_box(char box[16][24], u8 y, u8 h) BANKED
{
    u8 rows = (u8)(((u16)h * 7 + 7) >> 3), r, c, off;
    u8 *st = (u8 *)ST_BOX;
    (void)y;
    if (rows < 5) rows = 5;
    if (rows > 14) rows = 14;
    off = (u8)((rows * 8 - h * 7) >> 1);    /* (the text's first pixel row) */
    if (scr_mode == 2 && !box_up) {
        jobs_flush();                   /* (nothing may still be writing bank 1) */
        hud_hidden = 1;                 /* (status lines 1..4 are about to be overwritten) */
        if (shown) {                    /* the picture onto bank 0, shown from there */
            job(J_DMA, TILE_BANK, 0xA000, 0, 0x8800, 3072, 0);  /* (bank 6's tiles to VRAM bank 0) */
            job(J_ATTR, 0, 0, 0, 0, 0, 0);                      /* (their palettes into $9800) */
            job(J_FLIP, 0, 0, 0, 0, 0, 0);                      /* ($9800 shown) */
            shown = 0;
            jobs_flush();               /* (done before the box's tiles go to bank 1) */
        }
    }
    SWITCH_RAM(ST_BANK);                /* (the box drawn in the staging bank at ST_BOX) */
    box_pixel_rows(st, rows);
    for (r = 0; r < h && r < 16; r++)
        for (c = 0; c < 24; c++) {
            char ch = box[r][c];
            u8 px = c * 6, py, gr, t;
            if (!ch || ch == ' ') continue;
            glyph(ch);
            for (gr = 1; gr < 8; gr++) {    /* (the glyph's rows 1..7: 7-pixel lines) */
                u8 m = (u8)(g8[gr] << 1);
                u16 w;
                u8 *p;
                if (!m) continue;
                py = off + r * 7 + gr - 1;
                if (py >= rows * 8) continue;
                w = ((u16)m << 8) >> (px & 7);
                t = px >> 3;
                p = st + ((u16)(py >> 3) * 18 + t) * 16 + (py & 7) * 2;
                p[0] |= (u8)(w >> 8);
                p[1] |= (u8)(w >> 8);
                if ((u8)w && t < 17) {
                    p[16] |= (u8)w;
                    p[17] |= (u8)w;
                }
            }
        }
    SWITCH_RAM(1);
    job(J_DMA, ST_BANK, ST_BOX, 1, 0x8800, (u16)rows * 18 * 16, 0);    /* (the box's tiles to bank 1) */
    job(J_WIN, 0, 0, 0, 0, 0, 144 - rows * 8);                          /* (then the window up) */
    box_up = 1;
    box_rows = rows;
}

static void upload_picture(void);

/* the box gone: the window off; in the game, the status lines 1..4
   uploaded again and the latest picture shown if one waited */
void gfx_box_off(void) BANKED
{
    u8 l;
    if (!box_up) return;
    box_up = 0;
    job(J_WIN, 0, 0, 0, 0, 0, 0);       /* (the window off) */
    if (scr_mode == 2) {
        hud_hidden = 0;
        for (l = 1; l < 5; l++) hud_upload(l);  /* (bank 1's status lines, overwritten by the box) */
        if (pic_pending) upload_picture();      /* (the picture drawn while the box was up) */
    }
}

/* ------------------------------------------------------------ the weapon */
/* the weapon hidden: every sprite moved off the screen */
void gfx_weapon_off(void) BANKED
{
    u8 i;
    for (i = 0; i < 40; i++) move_sprite(i, 0, 0);
    wp_frame = 0xFF;
}

/* weapon w's frame (2 w + firing; resource RES_WEAPONS, tools/gb_res.py):
   its 8 x 16 tiles to OBJ tiles from 0, its two OBJ palettes, then its
   sprites placed. Waits until the tiles are in VRAM (jobs_flush) */
void gfx_weapon(u8 w, u8 firing) BANKED
{
    u8 f = w * 2 + (firing ? 1 : 0), hd[18], i, n, nt;
    u32 at;
    if (scr_mode != 2 || w > 8) return;
    if (f == wp_frame) return;
    at = res_at(RES_WEAPONS);
    at += far_u16be(at + 2 * f);        /* (the frame's offset in the table) */
    far_copy(at, hd, 18);               /* (u8 sprites, u8 tiles, 8 colours) */
    n = hd[0];
    nt = hd[1];
    for (i = 0; i < 40; i++) move_sprite(i, 0, 0);
    SWITCH_RAM(ST_BANK);
    far_copy(at + 18 + 4 * (u16)n, (void *)ST_WP, (u16)nt * 16);
    SWITCH_RAM(1);
    job(J_DMA, ST_BANK, ST_WP, 0, 0x8000, (u16)nt * 16, 0);    /* (OBJ tiles from $8000, VRAM bank 0) */
    for (i = 0; i < 8; i++) objpal[i >> 2][i & 3] = hd[2 + 2 * i] | ((u16)hd[3 + 2 * i] << 8);   /* (little-endian) */
    prep_pals();
    job(J_PAL, 0, 0, 0, 0, 0, 0);
    jobs_flush();                       /* (tiles and palettes in before the sprites show) */
    for (i = 0; i < n && i < 40; i++) {
        u8 s[4];
        far_copy(at + 18 + 4 * (u16)i, s, 4);  /* (y, x in the view's pixels, tile, palette) */
        set_sprite_tile(i, s[2] * 2);   /* (8 x 16 sprites: tiles in pairs) */
        set_sprite_prop(i, s[3] & 7);   /* (the CGB palette, OBJ VRAM bank 0) */
        move_sprite(i, 8 + 16 + s[1], 16 + 8 + s[0]);   /* (OAM's offsets 8, 16, plus the view's place: x 16, y 8) */
    }
    wp_frame = f;
}

/* ------------------------------------------------------------ the picture */
/* the view's palettes red for frames frames (wait_vblank counts down) */
void gfx_flash(u8 frames) BANKED
{
    flash_n = frames;
    prep_pals();
    pal_dirty = 1;
}

/* an 8-bit RGB colour (the core's) -> BGR555 at 3/4 brightness; berserk
   pulls it toward red */
static u16 rgb15(const u8 *rgb, u8 berserk)
{
    u16 r = rgb[0], g = rgb[1], b = rgb[2];
    if (berserk) {
        r = (r + 255) >> 1;
        g >>= 1;
        b >>= 1;
    }
    r = r * 3 >> 5;                     /* (3/4: the SNES's backdrop darkens to the horizon) */
    g = g * 3 >> 5;
    b = b * 3 >> 5;
    return r | (g << 5) | (b << 10);
}

/* the floor's and ceiling's colours (palettes 2..7's colour 0) */
void gfx_backdrop(const u8 *floor_rgb, const u8 *ceil_rgb, u8 berserk) BANKED
{
    floor_c = rgb15(floor_rgb, berserk);
    ceil_c = rgb15(ceil_rgb, berserk);
    view_palettes();
}

/* the picture in bank 6 queued to the VRAM bank not shown, its palettes
   to the map not shown, then that map shown (double buffering: the flip
   comes after the whole picture is in) */
static void upload_picture(void)
{
    u8 back = shown ^ 1;
    pic_pending = 0;
    job(J_DMA, TILE_BANK, 0xA000, back, 0x8800, 3072, 0);
    job(J_ATTR, 0, 0, 0, 0, 0, back);
    job(J_FLIP, 0, 0, 0, 0, 0, back);
    shown = back;
}

/* the 3D view drawn (view.c with vdraw.c, the view's RAM in bank 3) and
   made into tiles, then shown (or kept until the box goes). A new map's
   colours go to the palettes with its first picture */
void gfx_view(void) BANKED
{
    static const u8 OUTWARD[16] = {7, 8, 6, 9, 5, 10, 4, 11, 3, 12, 2, 13, 1, 14, 0, 15};
    if (scr_mode != 2) return;
    jobs_flush();                       /* (bank 6's tiles: no job reads them now) */
    /* drawn outward (the option, as the Master System's): from the middle
       column of tiles out, each into the picture shown as soon as it is
       done - not with a box up, nor for a new map (its colours change) */
    if (cfg.outward && !box_up && pal_map == (u8)S.map) {
        u8 k;
        SWITCH_RAM(3);                  /* (the view's variables) */
        view_prepare();                 /* (view.c: the walls and the things worked out) */
        for (k = 0; k < 16; k++) {
            u8 tx = OUTWARD[k];
            view_draw(tx * 8, tx * 8 + 8);  /* (its 8 columns) */
            vd_tiles_col(tx);               /* (its 12 tiles and palettes, bank 6) */
            job(J_DMA, TILE_BANK, 0xA000 + (u16)tx * 192, shown, 0x8800 + (u16)tx * 192, 192, 0);
            job(J_ACOL, 0, tx, 0, 0, 0, shown);
            SWITCH_RAM(3);              /* (a full job ring waited a frame: run_jobs leaves bank 1) */
        }
        SWITCH_RAM(1);                  /* (the level back for the core) */
        return;
    }
#ifdef VIEW_PROF                        /* (build.sh XDEFS=-DVIEW_PROF: the frames each part takes) */
    {
        u16 t0 = sys_time;
        u8 i;
        SWITCH_RAM(3);
        view_render();
        prof_t[0] = sys_time - t0;
        for (i = 0; i < 6; i++) prof_v[i] = VR.vprof[i];
        t0 = sys_time;
        vd_tiles();
        prof_t[1] = sys_time - t0;
        SWITCH_RAM(1);
    }
#else
    SWITCH_RAM(3);                      /* (the view's variables) */
    view_render();                      /* (view.c: the walls and sprites into the picture, banks 4, 5) */
    vd_tiles();                         /* (vdraw.c: the picture into tiles, bank 6) */
    SWITCH_RAM(1);                      /* (the level back for the core) */
#endif
    if (box_up) pic_pending = 1;
    else upload_picture();
    if (pal_map != vd_conv_map) {       /* (a new map's colours: with its first picture) */
        pal_map = vd_conv_map;
        view_palettes();
    }
}
