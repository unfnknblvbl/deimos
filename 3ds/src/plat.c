/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the 3DS: what the SNES port's game code (snes/src/ui.c,
 * glue.c, campaign.c, save.c and the core) asks of its machine, done on
 * the 3DS - so the game, its menus and boxes, are the SNES port's own.
 *
 *  - the cartridge: the SNES build's ROM image (romfs:/deimos.sfc), read
 *    at its SNES addresses (the resources, the campaign's block);
 *  - the picture chip, as much of it as the UI uses: VRAM, CGRAM, OAM,
 *    BG1 (Mode 1, 4bpp), 16x16 / 32x32 sprites - drawn into textures: the
 *    status bar, the message line and the boxes on the bottom screen, the
 *    weapon (sprites 96..) over the 3D view on the top screen, the title
 *    (the full-screen mode) on the top screen;
 *  - the 3D view: r3d.c (view_render: the camera goes to the player);
 *  - the pad, the frames, the saves (a file on the SD card), the sounds.
 *
 * Words used below: "the chip" is a software model of the SNES PPU's
 * memories (VRAM, CGRAM, OAM) that the SNES UI code writes as if it were
 * the real one; "S" is the game state of core/rules.h; "the script mode"
 * is a test build that plays recorded presses and checks S against a
 * recording of the SNES game.
 *
 * Every public function here stands in for the SNES routine of the same
 * name (vdata.s, far.s, gfx.s, video.s ...). main() starts the 3DS
 * (graphics, romfs, citro3d, render targets), loads the game data and the
 * save, starts the sound (sound3ds.c) and runs the SNES UI (ui_main), which
 * calls wait_vblank once a frame; wait_vblank draws both screens through
 * r3d.c.
 *
 * Screens: the top screen is 400 x 240 (one render target per eye), the
 * bottom 320 x 240. Render targets are 240 wide and 400 or 320 tall because
 * the 3DS's screens are mounted rotated; citro3d's "Tilt" matrices undo it.
 *
 * Licence: GPL-2.0-or-later.
 */
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "r3d.h"
#include "../../core/rules.h"
#include "../../snes/src/gfx.h"
#include "../../snes/src/video.h"
#include "../../snes/src/campaign.h"
#include "../../snes/src/glue.h"
#include "sound3ds.h"

#define SAVE_DIR "sdmc:/3ds/deimos"   /* the save, and the script mode's files */

#define HEADER_AT 0xC0FFB0UL            /* the SNES image's header block (SNES address) */

/* ------------------------------------------------------------ the cartridge */
const u8 *rom_image;                    /* the whole SNES image, read into memory by main */
static u32 rom_size;                    /* its size in bytes */
u8 block_bank;                          /* the campaign block's first SNES bank (campaign.h) */

/* rom_ptr: SNES address a (HiROM, banks $C0..: offset a - $C00000) -> a
 * pointer into the image; out of range gives the image's first byte */
static const u8 *rom_ptr(u32 a)
{
    u32 o = (a & 0xFFFFFF) - 0xC00000;
    return o < rom_size ? rom_image + o : rom_image;
}

/* rom_u8 / rom_u16le: a byte, or a little-endian word, at SNES address a */
u8 rom_u8(u32 a) { return *rom_ptr(a); }
u16 rom_u16le(u32 a) { const u8 *p = rom_ptr(a); return (u16)(p[0] | (p[1] << 8)); }
/* campaign_init: the campaign block's bank from header byte 6 ($C0FFB6) */
void campaign_init(void) { block_bank = rom_u8(HEADER_AT + 6); }

/* far.s: copies from a 24-bit ROM address; the caller sets the globals
 * and calls far_copy (far_len bytes), far_strcpy (a 0-terminated string,
 * at most far_len bytes) or far_u32 (a big-endian u32) */
u8 far_src[4];                          /* the address: low, high, bank */
u8 *far_dst;                            /* the destination */
u16 far_len;                            /* the length (the limit, for far_strcpy) */

/* far_ptr: far_src as a pointer */
static const u8 *far_ptr(void) { return rom_ptr((u32)far_src[0] | ((u32)far_src[1] << 8) | ((u32)far_src[2] << 16)); }
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
u32 far_u32(void)
{
    const u8 *p = far_ptr();
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* ------------------------------------------------------------ the picture chip */
static u16 vram[32768];                 /* VRAM: 32K words, by SNES word address */
static u16 cgram[256];                  /* CGRAM: 256 BGR555 colours */
static u8 wram7f[65536];                /* the SNES's work RAM bank $7F (tiles being made) */
u8 oam[544];                            /* the sprite table: 128 x 4 bytes, then 32 bytes of 2 bits each */
u8 oam_dirty;                           /* set by the UI when it changes oam */
u32 vq_src;                             /* vq_add's arguments: source address, */
u16 vq_dst, vq_len, vq_mode;            /* destination word address, length in bytes, VQ_VRAM or VQ_CGRAM */
u32 mt_src;                             /* md_tiles' arguments: source (ROM), */
u16 mt_dst, mt_n;                       /* destination in bank $7F, number of tiles */
char txt_chars[16][24];                 /* the text layer: 16 rows of 24 characters (0 = see-through) */
u8 grad_hdma[1];                        /* (the SNES's floor/ceiling HDMA table: unused here) */

#define MODE_GAME 0                     /* the game screen: message line, 3D view, status bar */
#define MODE_FULL 1                     /* a full-screen SNES picture (title, menus) */
static u8 mode = MODE_FULL, blank = 1, flash_n, bg1_tiles = 0;
/* blank: the screen is off; flash_n: frames of the red damage flash left;
   bg1_tiles: BG1's tile base in MODE_FULL, in units of $1000 words */
static u8 dirty = 1;                    /* the chip's state changed: tex_ppu must be redrawn */

/* RAMADDR (ui.c): a pointer into the program's memory as a fake address
 * (the SNES code passes 24-bit addresses; a 3DS pointer is parked in one
 * of 4 slots, round robin, and $FE00000k stands for it) */
static const void *ram_slot[4];
static u8 ram_next;
u32 plat_ramaddr(const void *p)
{
    u8 k = ram_next++ & 3;
    ram_slot[k] = p;
    return 0xFE000000UL | k;
}

/* src_ptr: a source address (fake RAM address, bank $7F, or ROM) -> a pointer */
static const u8 *src_ptr(u32 a)
{
    if ((a >> 24) == 0xFE) return (const u8 *)ram_slot[a & 3];
    if (((a >> 16) & 0xFF) == 0x7F) return wram7f + (a & 0xFFFF);
    return rom_ptr(a);
}

/* vq_add: the SNES queues a DMA to VRAM or CGRAM; here the copy is made at
 * once, the source read as little-endian words (the 3DS's ARM is
 * little-endian too, so the program's own u16 arrays come out right) */
void vq_add(void)
{
    const u8 *s = src_ptr(vq_src);
    u16 i;
    if (vq_mode == VQ_CGRAM)
        for (i = 0; i + 1 < vq_len; i += 2) cgram[(vq_dst + i / 2) & 255] = (u16)(s[i] | (s[i + 1] << 8));
    else
        for (i = 0; i + 1 < vq_len; i += 2) vram[(vq_dst + i / 2) & 32767] = (u16)(s[i] | (s[i + 1] << 8));
    dirty = 1;
}

/* vq_busy / vq_flush: the queue is always empty */
u8 vq_busy(void) { return 0; }
void vq_flush(void) {}
/* gfx_init: clear the sprite table */
void gfx_init(void) { memset(oam, 0, sizeof oam); }

/* Mega Drive 4bpp (packed, the left pixel high) -> SNES 4bpp (planar)
 * mt_n tiles of 32 bytes from mt_src to bank $7F at mt_dst. An SNES 4bpp
 * tile: rows 0..7 of planes 0 and 1 interleaved (2 bytes a row), then the
 * same for planes 2 and 3; bit 7 of a plane byte is the leftmost pixel.
 * (Nothing checks that the tiles stay inside the 64 KB bank.) */
void md_tiles(void)
{
    const u8 *s = rom_ptr(mt_src);
    u8 *d = wram7f + mt_dst;
    u16 t;
    int r, x, k;
    for (t = 0; t < mt_n; t++, s += 32, d += 32) {
        memset(d, 0, 32);
        for (r = 0; r < 8; r++)
            for (x = 0; x < 8; x++) {
                u8 p = (s[r * 4 + (x >> 1)] >> ((x & 1) ? 0 : 4)) & 15;     /* pixel x of row r */
                for (k = 0; k < 4; k++)         /* its bit k into plane k's byte */
                    if ((p >> k) & 1) d[(k >> 1) * 16 + r * 2 + (k & 1)] |= (u8)(0x80 >> x);
            }
    }
    /* (mt_src moves on as the 65816's does: callers set it each time) */
}

/* the text layer's characters into its sprite tiles at $7F3000 (gfx.s's)
 * Each 2 x 2 block of characters is one 16 x 16 sprite (s numbers them, 12
 * a pair of rows); a 16 x 16 sprite uses tiles t, t+1, t+16, t+17 of the
 * sprite tile table, 8 sprites to a pair of tile rows. The font: 96 tiles
 * of 32 bytes at $7FD000, glyph ft for character 32 + ft, glyph 0 blank and
 * 95 for anything unknown; lower case is shown as upper. */
void txt_render(void)
{
    int r, c;
    for (r = 0; r < 16; r++)
        for (c = 0; c < 24; c++) {
            u8 ch = (u8)txt_chars[r][c], ft;
            int s = (c >> 1) + 12 * (r >> 1), tile;
            if (!ch) ft = 0;
            else {
                if (ch >= 'a' && ch <= 'z') ch -= 32;
                ft = ch >= 33 ? (u8)(ch - 32) : 95;     /* (a space, 32, also gets 95) */
                if (ft >= 96) ft = 95;
            }
            tile = (s % 8) * 2 + (s / 8) * 32 + (r & 1) * 16 + (c & 1);
            memcpy(wram7f + 0x3000 + tile * 32, wram7f + 0xD000 + ft * 32, 32);    /* the glyph */
        }
}

/* fill7f: zero len bytes of bank $7F from at */
void fill7f(u16 at, u16 len) { memset(wram7f + at, 0, len); }

/* video.s: the SNES's screen set-up; each records the state for wait_vblank */
/* video_init: VRAM cleared */
void video_init(void)
{
    memset(vram, 0, sizeof vram);
    dirty = 1;
}
/* video_game: the game screen */
void video_game(void) { mode = MODE_GAME; dirty = 1; }
/* video_full: a full-screen BG1 picture, tiles at word $1000 * t, the backdrop black */
void video_full(u8 t)
{
    mode = MODE_FULL;
    bg1_tiles = t;
    cgram[0] = 0;
    dirty = 1;
}
/* screen_on / screen_off: leave or enter forced blank (both screens black) */
void screen_on(void) { blank = 0; }
void screen_off(void) { blank = 1; }
/* view_upload, view_busy, cgram_load: SNES transfers with nothing to do here */
void view_upload(void) {}
u8 view_busy(void) { return 0; }
void cgram_load(void) {}
/* flash: the damage flash for the next frames frames */
void flash(u8 frames) { flash_n = frames; }
/* view_gradient: the floor's and ceiling's colours (BGR555) -> r3d.c */
void view_gradient(u16 floor, u16 ceiling) { r3d_colours(floor, ceiling); }
/* view_render: a new view: the camera goes to the player */
void view_render(void) { r3d_target(); }

/* ---- drawing it: BG1 and the sprites into an RGBA8 texture (tiled) */
/* rgba: a BGR555 colour -> the GPU's RGBA8 (red in the top byte), opaque;
   each 5-bit level widened to 8 bits by repeating its top bits */
static u32 rgba(u16 c)
{
    u32 r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3;
    return (r | r >> 5) << 24 | (g | g >> 5) << 16 | (b | b >> 5) << 8 | 0xFF;
}

/* tiled256: texel (x, y) of a 256-wide texture in the GPU's tiled order
   (8 x 8 tiles row by row, Morton order inside; see r3d.c's tiled) */
static u32 tiled256(u32 x, u32 y)
{
    u32 m = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
    return ((y >> 3) * 32 + (x >> 3)) * 64 + m;
}

/* the colour index of 4bpp tile t's pixel (x, y), tiles at base (words)
 * (word y holds planes 0 and 1 of row y, low byte and high byte; word 8 + y
 * planes 2 and 3; bit 7 - x of each plane is pixel x) */
static u8 pix4(u32 base, u32 t, u32 x, u32 y)
{
    const u16 *p = &vram[(base + t * 16) & 32767];
    u16 a = p[y], b = p[8 + y];
    u8 s = (u8)(7 - x);
    return (u8)(((a >> s) & 1) | (((a >> (8 + s)) & 1) << 1) | (((b >> s) & 1) << 2) | (((b >> (8 + s)) & 1) << 3));
}

/* line y's pixels: BG1 (if shown there) and sprites lo..hi-1; out: colour + 1 or 0 (none)
 * out has 256 entries, each a CGRAM index + 1. OAM entries: x, y, tile,
 * attributes (bit 0 the second tile table, 1..3 palette, 4..5 priority, 6
 * h-flip, 7 v-flip); the high table gives x's 9th bit and the size (16 or
 * 32). Sprites use palettes 8..15 and tiles at word $6000 / $7000. BG1's
 * map is 32 x 32 entries at word $5C00 (bits 0..9 tile, 10..12 palette,
 * 13 priority, 14 h-flip, 15 v-flip); in MODE_GAME it is scrolled 8 rows
 * down and its tiles are at word $4000, otherwise at bg1_tiles * $1000. */
static void line_pixels(int y, int bg, int lo, int hi, u16 *out)
{
    static u16 sp[256];                 /* the sprites' pixels on this line (colour + 1) */
    static u8 spri[256];                /* and their priorities */
    int x, k;
    memset(sp, 0, sizeof sp);
    memset(spri, 0, sizeof spri);
    for (k = hi - 1; k >= lo; k--) {    /* the last first, so lower numbers end up in front */
        const u8 *o = &oam[k * 4];
        u8 hb = (u8)(oam[512 + (k >> 2)] >> ((k & 3) * 2));
        int sz = (hb & 2) ? 32 : 16, sx = o[0] | ((hb & 1) << 8), sy = o[1] + 1, row;  /* (one line below its y, as the SNES shows it) */
        if (sx >= 256) sx -= 512;       /* 9-bit x: 256..511 are -256..-1 */
        if (sy >= 240) sy -= 256;       /* partly above the top */
        row = y - sy;
        if (row < 0 || row >= sz) continue;     /* not on this line */
        {
            u8 at = o[3];
            u32 base = 0x6000 + ((at & 1) ? 0x1000 : 0);
            int pal = (at >> 1) & 7, pr = (at >> 4) & 3, c;
            if (at & 0x80) row = sz - 1 - row;      /* v-flip */
            for (c = 0; c < sz; c++) {
                int px = sx + c, col = (at & 0x40) ? sz - 1 - c : c;   /* h-flip */
                u32 t = (o[2] + (col >> 3) + ((row >> 3) << 4)) & 255;   /* (16 tiles a row in the table) */
                u8 i;
                if (px < 0 || px >= 256) continue;
                i = pix4(base, t, col & 7, row & 7);
                if (i) {
                    sp[px] = (u16)(128 + pal * 16 + i + 1);
                    spri[px] = (u8)pr;
                }
            }
        }
    }
    for (x = 0; x < 256; x++) {         /* BG1 under or over them */
        u16 b = 0;
        int bprio = 0;
        if (bg) {
            int by = mode == MODE_GAME ? ((y - 8) & 255) : y;
            u16 e = vram[(0x5C00 + (by >> 3) * 32 + (x >> 3)) & 32767];
            int tx = x & 7, ty = by & 7;
            u8 i;
            if (e & 0x4000) tx = 7 - tx;
            if (e & 0x8000) ty = 7 - ty;
            i = pix4((mode == MODE_GAME ? 0x4000 : (u32)bg1_tiles * 0x1000), e & 1023, (u32)tx, (u32)ty);
            if (i) {
                b = (u16)(((e >> 10) & 7) * 16 + i + 1);
                bprio = (e & 0x2000) != 0;
            }
        }
        /* Mode 1: sprites of priority 3 over BG1's high tiles, the rest under */
        if (sp[x] && (spri[x] == 3 || !(b && bprio))) out[x] = sp[x];
        else out[x] = b;
    }
}

static C3D_Tex tex_ppu, tex_wpn;        /* 256 x 256 RGBA8: the chip's picture; the weapon */
static u8 wpn_dirty = 1;                /* tex_wpn must be redrawn */

/* render_ppu: the chip's 256 x 224 picture into tex_ppu: on the game
 * screen BG1 only on rows 0..7 and 184..223 (the message line and the
 * status bar) and sprites 0..95; elsewhere all of BG1 and all sprites.
 * The 3D view's rows are left see-through (alpha 0) for the automap. */
static void render_ppu(void)
{
    u32 *d = (u32 *)tex_ppu.data;
    u16 px[256];
    int y, x;
    for (y = 0; y < 224; y++) {
        int bg = mode == MODE_FULL || y < 8 || y >= 184;
        line_pixels(y, bg, 0, mode == MODE_GAME ? 96 : 128, px);
        for (x = 0; x < 256; x++) {
            u32 c;
            if (px[x]) c = rgba(cgram[px[x] - 1]);
            else if (mode == MODE_GAME && y >= 8 && y < 184) c = 0;          /* the automap shows through */
            else c = rgba(cgram[0]);
            d[tiled256((u32)x, (u32)y)] = c;
        }
    }
    C3D_TexFlush(&tex_ppu);             /* (the CPU cache out to memory, for the GPU) */
}

/* render_weapon: sprites 96..127 (the weapon) of rows 8..183 into tex_wpn,
   see-through elsewhere; shown over the 3D view on the top screen */
static void render_weapon(void)
{
    u32 *d = (u32 *)tex_wpn.data;
    u16 px[256];
    int y, x;
    for (y = 8; y < 184; y++) {
        line_pixels(y, 0, 96, 128, px);
        for (x = 0; x < 256; x++) d[tiled256((u32)x, (u32)y)] = px[x] ? rgba(cgram[px[x] - 1]) : 0;
    }
    C3D_TexFlush(&tex_wpn);
}

/* ------------------------------------------------------------ the frames */
u16 pad, pad_edge;                      /* the buttons held (SNES bits), and those newly pressed */
volatile u16 frame_count;               /* frames since the start (the game's clock) */
static C3D_RenderTarget *top_l, *top_r, *bottom;   /* the top screen's left and right eye, the bottom */

/* the display transfer from a render target to the screen: no flip, not
   tiled, RGBA8 in, RGB8 out (the screens' format), no scaling */
#define DT_FLAGS (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static void save_tick(void);

#ifdef PLAT_SCRIPT
/* ---- the script mode (a test build: ./build.sh with EXTRA=-DPLAT_SCRIPT):
   the pad is played from sdmc:/3ds/deimos/script.txt, written by
   tools/snes_play.py --script from the SNES game's run in step with the
   model: "k <button>" a press (when the game is idle: 2 frames released,
   4 held, then until it has answered and is idle again), "w <offset>
   <value>" a poke into S, "c <bytes> <crc32>" S's first bytes compared
   (when idle in play, a dialog, a code or the vendor). The result goes to
   sdmc:/3ds/deimos/script.log. Every frame is drawn unless PLAT_FAST. */
extern volatile u8 eng_turn_seq, eng_idle, eng_mode;
static FILE *scr, *slog;
static int s_st, s_cnt, s_steps, s_bad;
static u16 s_key;
static u8 s_seq;

/* crc32: the standard (reflected, $EDB88320) CRC-32 of n bytes */
static u32 crc32(const u8 *p, u32 n)
{
    u32 c = 0xFFFFFFFFu;
    while (n--) {
        int k;
        c ^= *p++;
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

/* key_bits: a script's button name -> the SNES pad bit (0 if unknown) */
static u16 key_bits(const char *k)
{
    static const char *const N[] = {"a", "b", "x", "y", "l", "r", "start", "select", "up", "down", "left", "right"};
    static const u16 B[] = {0x80, 0x8000, 0x40, 0x4000, 0x20, 0x10, 0x1000, 0x2000, 0x800, 0x400, 0x200, 0x100};
    int i;
    for (i = 0; i < 12; i++)
        if (!strcmp(k, N[i])) return B[i];
    return 0;
}

/* script_pad: the pad for this frame from the script's state machine
   (0 reading lines while idle, 1 released, 2 held, 3 waiting for the
   game's answer, 9 done) */
static u16 script_pad(void)
{
    char ln[64];
    switch (s_st) {
    case 0:                                     /* the next line, when idle */
        while (eng_idle) {
            long at = ftell(scr);
            if (!fgets(ln, sizeof ln, scr)) {
                fprintf(slog, "END: %d compared states, %d different, %u frames\n", s_steps, s_bad, frame_count);
                fclose(slog);
                s_st = 9;
                return 0;
            }
            if (ln[0] == 'k') {
                char k[16];
                sscanf(ln + 2, "%15s", k);
                s_key = key_bits(k);
                s_st = 1;
                s_cnt = 2;
                break;
            } else if (ln[0] == 'w') {
                int o, v;
                sscanf(ln + 2, "%d %d", &o, &v);
                ((u8 *)&S)[o] = (u8)v;
            } else if (ln[0] == 'c') {
                unsigned n, want, got;
                if (eng_mode != 0 && eng_mode != 1 && eng_mode != 2 && eng_mode != 6) {
                    fseek(scr, at, SEEK_SET);   /* not yet */
                    break;
                }
                sscanf(ln + 2, "%u %x", &n, &want);
                got = crc32((const u8 *)&S, n);
                s_steps++;
                if (got != want) {
                    s_bad++;
                    fprintf(slog, "MISMATCH at state %d (frame %u): %08x, the SNES %08x\n", s_steps, frame_count, got, want);
                    if (s_bad == 1) {   /* the first difference: S kept for a look */
                        FILE *d = fopen(SAVE_DIR "/script_S.bin", "wb");
                        if (d) {
                            fwrite(&S, 1, n, d);
                            fclose(d);
                        }
                    }
                    fflush(slog);
                }
                if (s_steps % 500 == 0) {
                    fprintf(slog, "%d states (frame %u), %d different\n", s_steps, frame_count, s_bad);
                    fflush(slog);
                }
            }
        }
        return 0;
    case 1:                                     /* released */
        if (--s_cnt == 0) {
            s_seq = eng_turn_seq;
            s_st = 2;
            s_cnt = 4;
        }
        return 0;
    case 2:                                     /* held */
        if (--s_cnt == 0) s_st = 3;
        return s_key;
    case 3:                                     /* answered */
        if (eng_turn_seq != s_seq && eng_idle) s_st = 0;
        return 0;
    }
    return 0;
}
#endif

/* read_pad: the 3DS's buttons (hidScanInput must have run) -> the SNES
   pad's bits ($8000 B, $4000 Y, $2000 Select, $1000 Start, $0800 up,
   $0400 down, $0200 left, $0100 right, $0080 A, $0040 X, $0020 L,
   $0010 R); ZL and ZR count as L and R */
static u16 read_pad(void)
{
    u32 k = hidKeysHeld();
    u16 p = 0;
    if (k & KEY_A) p |= 0x0080;
    if (k & KEY_B) p |= 0x8000;
    if (k & KEY_X) p |= 0x0040;
    if (k & KEY_Y) p |= 0x4000;
    if (k & (KEY_L | KEY_ZL)) p |= 0x0020;
    if (k & (KEY_R | KEY_ZR)) p |= 0x0010;
    if (k & KEY_START) p |= 0x1000;
    if (k & KEY_SELECT) p |= 0x2000;
    if (k & KEY_UP) p |= 0x0800;
    if (k & KEY_DOWN) p |= 0x0400;
    if (k & KEY_LEFT) p |= 0x0200;
    if (k & KEY_RIGHT) p |= 0x0100;
    return p;
}

/*
 * wait_vblank: the SNES code's "wait for the next frame", called once a
 * frame by ui.c. Redraws the chip's textures if they changed, then draws
 * the frame: on the game screen the 3D view (stereo when the 3D slider is
 * up) with the weapon over it on the top screen, and the automap under
 * the status bar and boxes on the bottom screen; on a full screen the
 * picture on the top screen; the red flash over both. C3D_FrameEnd waits
 * for the vertical blank (C3D_FRAME_SYNCDRAW). Then reads the pad, counts
 * the frame, and runs the sound and save ticks. Exits the program when the
 * system asks it to close (aptMainLoop false).
 */
void wait_vblank(void)
{
    static const float WHITE4[4] = {1, 1, 1, 1}, RED[4] = {0.45f, 0, 0, 1};
    float slider;
    u16 p;
    if (!aptMainLoop()) exit(0);
#ifdef PLAT_FAST
    if (frame_count & 63) goto input;   /* (a fast test: draw only every 64th frame) */
#endif
    if (dirty || oam_dirty) {
        if (oam_dirty) wpn_dirty = 1;
        render_ppu();
        dirty = 0;
        oam_dirty = 0;
    }
    if (wpn_dirty && mode == MODE_GAME) {
        render_weapon();
        wpn_dirty = 0;
    }
    slider = osGet3DSliderState();      /* 0..1: the 3D effect's strength (0 = no stereo) */
    r3d_frame_start();
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_RenderTargetClear(top_l, C3D_CLEAR_ALL, 0x000000FF, 0);
    C3D_RenderTargetClear(top_r, C3D_CLEAR_ALL, 0x000000FF, 0);
    C3D_RenderTargetClear(bottom, C3D_CLEAR_ALL, 0x000000FF, 0);
    if (!blank) {
        if (mode == MODE_GAME) {
            int e;
            r3d_draw(top_l, slider > 0.0f ? top_r : 0, slider);
            for (e = 0; e < (slider > 0.0f ? 2 : 1); e++) {
                C3D_RenderTarget *t = e ? top_r : top_l;
                float w = 256.0f * 240.0f / 176.0f;     /* the view's 176 rows stretched to 240: the width to match */
                /* the weapon: texture rows 8..183 (v = 1 - row / 256) over the whole height */
                r3d_flat(t, 400, &tex_wpn, (400.0f - w) / 2, 0, w, 240, 0, 248.0f / 256, 1, 72.0f / 256, WHITE4, 0);
                if (flash_n) r3d_flat(t, 400, 0, 0, 0, 400, 240, 0, 0, 0, 0, RED, 1);    /* red, added */
            }
            r3d_automap(bottom, 320, 32, 16, 256, 176);  /* under the picture's see-through middle */
            r3d_flat(bottom, 320, &tex_ppu, 32, 8, 256, 224, 0, 1, 1, 32.0f / 256, WHITE4, 0);    /* rows 0..223 */
            if (flash_n) r3d_flat(bottom, 320, 0, 0, 0, 320, 240, 0, 0, 0, 0, RED, 1);
        } else {
            int e;
            for (e = 0; e < (slider > 0.0f ? 2 : 1); e++)
                r3d_flat(e ? top_r : top_l, 400, &tex_ppu, 72, 8, 256, 224, 0, 1, 1, 32.0f / 256, WHITE4, 0);   /* centred */
        }
    }
    if (flash_n) flash_n--;
    C3D_FrameEnd(0);                    /* submit, then show at the vertical blank */
#ifdef PLAT_FAST
input:
#endif
    hidScanInput();
    p = read_pad();
#ifdef PLAT_SCRIPT
    if (scr) p = script_pad();
#endif
#ifdef PLAT_DEBUG
    if (p != pad) {
        char m[32];
        sprintf(m, "pad %04x @%u", p, frame_count);
        svcOutputDebugString(m, strlen(m));
    }
#endif
    pad_edge = p & ~pad;                /* pressed now, not before */
    pad = p;
    frame_count++;
    sound_tick();                       /* the effects the game queued -> sound3ds.c */
    save_tick();                        /* the save written out once it settles */
}

/* ticks: the frame count */
u16 ticks(void) { return frame_count; }
/* set_backdrop: nothing to do here */
void set_backdrop(void) {}

/* ------------------------------------------------------------ the saves */
/* The SNES's 8 KB battery RAM, kept in memory and written whole to a file
   half a second (30 frames) after the game's last write. */
#define SRAM_SIZE 8192
#define SAVE_FILE SAVE_DIR "/deimos.sav"
static u8 sram[SRAM_SIZE];              /* the battery RAM's contents */
static u16 save_wait;                   /* frames until the write-out (0: nothing pending) */

/* sram_rd / sram_wr: save.c's byte access to the battery RAM */
u8 sram_rd(u16 i) { return sram[i % SRAM_SIZE]; }
void sram_wr(u16 i, u8 v)
{
    sram[i % SRAM_SIZE] = v;
    save_wait = 30;                     /* written out once the writes stop */
}

/* save_tick: once a frame; when the countdown ends, write the file
   (making its directories first; failures are ignored) */
static void save_tick(void)
{
    FILE *f;
    if (!save_wait || --save_wait) return;
    mkdir("sdmc:/3ds", 0777);
    mkdir(SAVE_DIR, 0777);
    f = fopen(SAVE_FILE, "wb");
    if (f) {
        fwrite(sram, 1, SRAM_SIZE, f);
        fclose(f);
    }
}

/* ------------------------------------------------------------ start */
int ui_main(void);                      /* snes/src/ui.c's main (renamed by build.sh) */

/* fail: show msg on the bottom screen's text console, wait for START (or
   the system closing the program), and exit */
static void fail(const char *msg)
{
    consoleInit(GFX_BOTTOM, NULL);
    printf("Deimos: %s\n\nPress START.\n", msg);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
    exit(1);
}

/* main: the program's start (from libctru's start-up code); never returns
   normally: ui_main runs the game */
int main(void)
{
    FILE *f;
    gfxInitDefault();                   /* the screens' frame buffers */
    gfxSet3D(true);                     /* the top screen in stereo mode */
    romfsInit();                        /* romfs: the files built into the .3dsx */
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 2);  /* citro3d, with a double-size command buffer */
    /* the render targets: colour RGBA8 with a 24-bit depth buffer */
    top_l = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(top_l, GFX_TOP, GFX_LEFT, DT_FLAGS);
    top_r = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(top_r, GFX_TOP, GFX_RIGHT, DT_FLAGS);
    bottom = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(bottom, GFX_BOTTOM, GFX_LEFT, DT_FLAGS);
    r3d_init();                         /* the shader and the vertex buffer */
    C3D_TexInit(&tex_ppu, 256, 256, GPU_RGBA8);
    C3D_TexInit(&tex_wpn, 256, 256, GPU_RGBA8);
    C3D_TexSetFilter(&tex_ppu, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetFilter(&tex_wpn, GPU_NEAREST, GPU_NEAREST);
    memset(tex_wpn.data, 0, 256 * 256 * 4);    /* all see-through */
    /* the cartridge */
    f = fopen("romfs:/deimos.sfc", "rb");
    if (!f) f = fopen(SAVE_DIR "/deimos.sfc", "rb");  /* (else a copy on the SD card) */
    if (!f) fail("no game data (romfs:/deimos.sfc)");
    fseek(f, 0, SEEK_END);
    rom_size = (u32)ftell(f);
    fseek(f, 0, SEEK_SET);
    rom_image = malloc(rom_size);
    if (!rom_image || fread((void *)rom_image, 1, rom_size, f) != rom_size) fail("the game data could not be read");
    fclose(f);
    /* the saves */
    memset(sram, 0xFF, sizeof sram);    /* (no file: as blank battery RAM) */
    f = fopen(SAVE_FILE, "rb");
#ifdef PLAT_SCRIPT
    mkdir("sdmc:/3ds", 0777);
    mkdir(SAVE_DIR, 0777);
    scr = fopen(SAVE_DIR "/script.txt", "r");
    slog = fopen(SAVE_DIR "/script.log", "w");
    {
        char m[64];
        snprintf(m, sizeof m, "script %p log %p", (void *)scr, (void *)slog);
        svcOutputDebugString(m, strlen(m));
    }
    if (scr) {                                  /* (nothing saved: the SNES run had none) */
        if (f) fclose(f);
        f = 0;
    }
#endif
    if (f) {
        fread(sram, 1, SRAM_SIZE, f);
        fclose(f);
    }
    sound_init3ds();                    /* (empty: ui.c's sound_init starts the sound) */
    return ui_main();                   /* the game */
}
