/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the 32X: what the SNES port's game code (snes/src/ui.c,
 * glue.c, campaign.c, save.c and the core) asks of its machine, done on
 * the 32X's master SH-2 - as the 3DS port does it (3ds/src/plat.c), so the
 * game, its menus and boxes are the SNES port's own.
 *
 *  - the cartridge: the SNES build's ROM image, at $80000 in the 32X's
 *    cartridge (tools/make_rom.py), read at its SNES addresses;
 *  - the picture chip, as much of it as the UI uses: VRAM, CGRAM, OAM,
 *    BG1 (Mode 1, 4bpp), 16x16 / 32x32 sprites, drawn into the 32X's frame
 *    buffer (8 bits a pixel; the 32X's palette is the SNES's CGRAM, the
 *    same 15-bit colours) in the middle 256 of its 320 columns;
 *  - the 3D view: r32x.c, 320 x 176 under the message line;
 *  - the pad (the 68000 reads it: m68k/main.c), the frames, the saves
 *    (the 68000 keeps them in the cartridge's battery RAM), the sounds
 *    (sound32x.c, on the slave SH-2).
 *
 * Words used below:
 *  - "the chip" / "the picture chip": a software model of the SNES PPU's
 *    state (VRAM, CGRAM, OAM) that the SNES UI code writes to as if it
 *    were the real chip; this file draws a picture from it.
 *  - "gen": a counter bumped at every change to that state, so a frame
 *    buffer is redrawn only when what it shows is out of date.
 *  - "S": the game state struct of core/rules.h (compared in script mode).
 *  - "the script mode": a test build (PLAT_SCRIPT) that plays recorded
 *    button presses and checks S against a recording from the SNES game.
 *
 * Called from: sh2/crt0.s (master_main), and the SNES port's code (every
 * public function here replaces an SNES assembler routine of the same
 * name: vdata.s, far.s, gfx.s, video.s, ...). Calls: r32x.c for the 3D
 * view, sound32x.c for the sound, and the 68000 (through the COMM
 * registers) for the pad, the frame count and the battery RAM.
 *
 * The 32X frame buffer (MARS_FB, two of them, swapped by MARS_VDP_FBCTL):
 * 256 line-table words first, then 224 lines of 320 bytes from byte $200.
 * Rows 0..7 are the message line, 8..183 the 3D view, 184..223 the status
 * bar; in the menus (MODE_FULL) all 224 rows are the SNES picture.
 *
 * Licence: GPL-2.0-or-later.
 */
#include "mars.h"
#include "libc.h"
#include "r32x.h"
#include "sound32x.h"
#include "../../core/rules.h"
#include "../../snes/src/gfx.h"
#include "../../snes/src/video.h"
#include "../../snes/src/campaign.h"
#include "../../snes/src/glue.h"

#define HEADER_AT 0xC0FFB0UL                    /* the SNES image's header block (SNES address) */
#define ROM_DATA ((const u8 *)0x02080000)       /* the SNES image in the 32X cartridge ($80000), SH-2 view */
#define ROM_DATA_SIZE 0x380000                  /* its largest size: $80000..$3FFFFF */

/* ------------------------------------------------------------ the cartridge */
const u8 *rom_image = ROM_DATA;         /* the SNES image's start, for code that reads it directly */
u8 block_bank;                          /* the campaign block's first SNES bank (campaign.h) */

/* rom_ptr: SNES address a (HiROM, banks $C0..$FF: image offset = a - $C00000)
 * -> a pointer into the cartridge. An address outside the image gives the
 * image's first byte rather than a fault. */
static const u8 *rom_ptr(u32 a)
{
    u32 o = (a & 0xFFFFFF) - 0xC00000;  /* (below $C00000 wraps to a huge offset: out of range) */
    return o < ROM_DATA_SIZE ? ROM_DATA + o : ROM_DATA;
}

/* rom_u8 / rom_u16le: a byte, or a little-endian word, of the SNES image at
 * SNES address a (vdata.s on the SNES). */
u8 rom_u8(u32 a) { return *rom_ptr(a); }
u16 rom_u16le(u32 a) { const u8 *p = rom_ptr(a); return (u16)(p[0] | (p[1] << 8)); }
/* campaign_init: read the campaign block's bank from header byte 6
 * ($C0FFB6, written by tools/snes_rom.py) into block_bank. */
void campaign_init(void) { block_bank = rom_u8(HEADER_AT + 6); }

/* far.s: the SNES code's copies from a "far" (24-bit) ROM address. The
 * caller sets these globals, then calls one of the functions. */
u8 far_src[4];                          /* the source address: low, high, bank (byte 3 unused) */
u8 *far_dst;                            /* where to copy to */
u16 far_len;                            /* how many bytes (at most, for far_strcpy) */

/* far_ptr: far_src as a pointer into the cartridge */
static const u8 *far_ptr(void) { return rom_ptr((u32)far_src[0] | ((u32)far_src[1] << 8) | ((u32)far_src[2] << 16)); }
/* far_copy: copy far_len bytes from far_src to far_dst */
void far_copy(void) { memcpy(far_dst, far_ptr(), far_len); }
/* far_strcpy: copy a 0-terminated string from far_src to far_dst, the 0
 * included, stopping after far_len bytes if no 0 comes first */
void far_strcpy(void)
{
    const u8 *s = far_ptr();
    u16 i;
    for (i = 0; i < far_len; i++) {
        far_dst[i] = s[i];
        if (!s[i]) break;
    }
}
/* far_u32: the big-endian 32-bit value at far_src */
u32 far_u32(void)
{
    const u8 *p = far_ptr();
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* ------------------------------------------------------------ the picture chip */
/* The SNES PPU's memories, kept here in the SNES's own formats. */
static u16 vram[32768];                 /* VRAM: 64 KB as 32K words, by SNES word address */
u16 cgram[256];                         /* CGRAM: 256 colours, BGR555 (bit 15 unused) */
/* the SNES's work RAM bank $7F, as much as the UI uses: $3000..$7FFF (the
   text layer's tiles, the tiles being made) and $D000..$DFFF (the font) */
static u8 w7f_lo[0x5000], w7f_hi[0x1000];
u8 oam[544];                            /* the sprite table: 128 x 4 bytes, then 32 bytes of 2 bits each */
u8 oam_dirty;                           /* set by the UI when it has changed oam */
u32 vq_src;                             /* vq_add's arguments: source (SNES address or plat_ramaddr's), */
u16 vq_dst, vq_len, vq_mode;            /* destination word address, length in bytes, VQ_VRAM or VQ_CGRAM */
u32 mt_src;                             /* md_tiles' arguments: source (ROM), */
u16 mt_dst, mt_n;                       /* destination in bank $7F, number of tiles */
char txt_chars[16][24];                 /* the text layer: 16 rows of 24 characters (0 = see-through) */
u8 grad_hdma[1];                        /* (the SNES's floor/ceiling HDMA table: unused here, the
                                           3D view draws its own colours) */

#define MODE_GAME 0                     /* the game screen: message line, 3D view, status bar */
#define MODE_FULL 1                     /* a full-screen SNES picture (title, menus) */
static u8 mode = MODE_FULL, blank = 1, flash_n, bg1_tiles = 0;
/* mode: MODE_GAME or MODE_FULL; blank: 1 while the screen is off (forced
   blank); flash_n: frames of the damage flash left; bg1_tiles: BG1's tile
   base in MODE_FULL, in units of $1000 words */
static u16 gen = 1;                     /* the picture chip's state: +1 at each change */
static u8 pal_dirty = 1;                /* the 32X's palette must be reloaded */

/* w7f: SNES work RAM address $7Fxxxx (a = xxxx) -> a pointer here. Only
 * $3000..$7FFF and $D000..$DFFF are kept (and $E000..$FFFF, aliased onto
 * $D000 by the & $FFF); anything else gets a 32-byte scratch buffer
 * whose contents do not matter. */
static u8 *w7f(u16 a)
{
    static u8 none[32];
    if (a >= 0x3000 && a < 0x8000) return w7f_lo + (a - 0x3000);
    if (a >= 0xD000) return w7f_hi + (a & 0xFFF);
    return none;
}

/* RAMADDR (ui.c): a pointer into the program's memory as a fake address.
 * The SNES code passes 24-bit addresses around; a pointer into SH-2 memory
 * does not fit, so plat_ramaddr parks it in one of 4 slots (used round
 * robin) and returns $FE00000k, which src_ptr turns back into the pointer.
 * A fake address is only good until 4 more have been made. */
static const void *ram_slot[4];
static u8 ram_next;                     /* the next slot to use (mod 4) */
u32 plat_ramaddr(const void *p)
{
    u8 k = ram_next++ & 3;
    ram_slot[k] = p;
    return 0xFE000000UL | k;
}

/* src_ptr: any source address the SNES code may give (a fake RAM address,
 * bank $7F work RAM, or the ROM) -> a pointer */
static const u8 *src_ptr(u32 a)
{
    if ((a >> 24) == 0xFE) return (const u8 *)ram_slot[a & 3];
    if (((a >> 16) & 0xFF) == 0x7F) return w7f((u16)a);
    return rom_ptr(a);
}

/*
 * vq_add: on the SNES, queue a DMA of vq_len bytes from vq_src to VRAM or
 * CGRAM (vq_mode) at word address vq_dst. Here it copies at once into
 * vram[] or cgram[] (the address wraps within each). Data from the ROM or
 * bank $7F is little-endian, as on the SNES; data from the program's own
 * memory (fake addresses) is already u16 words in the SH-2's big-endian
 * order, so it is copied as words. Sets pal_dirty for CGRAM and bumps gen.
 */
void vq_add(void)
{
    const u8 *s = src_ptr(vq_src);
    u16 i;
    if ((vq_src >> 24) == 0xFE) {               /* (the program's own words: the SH-2's byte order) */
        const u16 *w = (const u16 *)s;
        for (i = 0; i + 1 < vq_len; i += 2) {
            if (vq_mode == VQ_CGRAM) cgram[(vq_dst + i / 2) & 255] = w[i / 2];
            else vram[(vq_dst + i / 2) & 32767] = w[i / 2];
        }
        if (vq_mode == VQ_CGRAM) pal_dirty = 1;
        gen++;
        return;
    }
    if (vq_mode == VQ_CGRAM) {
        for (i = 0; i + 1 < vq_len; i += 2) cgram[(vq_dst + i / 2) & 255] = (u16)(s[i] | (s[i + 1] << 8));
        pal_dirty = 1;
    } else
        for (i = 0; i + 1 < vq_len; i += 2) vram[(vq_dst + i / 2) & 32767] = (u16)(s[i] | (s[i + 1] << 8));
    gen++;
}

/* vq_busy / vq_flush: the queue is always empty (vq_add copies at once) */
u8 vq_busy(void) { return 0; }
void vq_flush(void) {}
/* gfx_init: clear the sprite table (all sprites at 0,0, tile 0) */
void gfx_init(void) { memset(oam, 0, sizeof oam); }

/* Mega Drive 4bpp (packed, the left pixel high) -> SNES 4bpp (planar).
 * md_tiles converts mt_n tiles of 32 bytes from ROM address mt_src into
 * bank $7F at mt_dst. An SNES 4bpp tile is 32 bytes: rows 0..7 of planes
 * 0 and 1 interleaved (2 bytes a row), then the same for planes 2 and 3;
 * in each plane byte bit 7 is the leftmost pixel. */
void md_tiles(void)
{
    const u8 *s = rom_ptr(mt_src);
    u16 t, dst = mt_dst;
    int r, x, k;
    for (t = 0; t < mt_n; t++, s += 32, dst += 32) {
        u8 *d = w7f(dst);
        memset(d, 0, 32);
        for (r = 0; r < 8; r++)
            for (x = 0; x < 8; x++) {
                u8 p = (s[r * 4 + (x >> 1)] >> ((x & 1) ? 0 : 4)) & 15;     /* pixel x of row r */
                for (k = 0; k < 4; k++)         /* its bit k into plane k's byte for row r */
                    if ((p >> k) & 1) d[(k >> 1) * 16 + r * 2 + (k & 1)] |= (u8)(0x80 >> x);
            }
    }
}

/* the text layer's characters into its sprite tiles at $7F3000 (gfx.s's).
 * Each 2 x 2 block of characters is one 16 x 16 sprite (s numbers them,
 * 12 per pair of rows). The sprites' tiles are laid out as in the SNES's
 * sprite tile table: 16 tiles a row, a 16 x 16 sprite using tiles t, t+1,
 * t+16, t+17; here 8 sprites fill one such pair of tile rows (32 tiles).
 * The font is 96 tiles of 32 bytes at $7FD000: glyph ft is character
 * 32 + ft, glyph 0 is blank (also used for a 0 character, see-through) and
 * glyph 95 stands in for anything unknown. Lower case is shown as upper. */
void txt_render(void)
{
    int r, c;
    for (r = 0; r < 16; r++)
        for (c = 0; c < 24; c++) {
            u8 ch = (u8)txt_chars[r][c], ft;        /* ft: the font glyph */
            int s = (c >> 1) + 12 * (r >> 1), tile;
            if (!ch) ft = 0;
            else {
                if (ch >= 'a' && ch <= 'z') ch -= 32;
                ft = ch >= 33 ? (u8)(ch - 32) : 95;     /* (a space, 32, also gets 95) */
                if (ft >= 96) ft = 95;
            }
            tile = (s % 8) * 2 + (s / 8) * 32 + (r & 1) * 16 + (c & 1);
            memcpy(w7f((u16)(0x3000 + tile * 32)), w7f((u16)(0xD000 + ft * 32)), 32);   /* the glyph's 32 bytes */
        }
}

/* fill7f: zero len bytes of bank $7F from address at */
void fill7f(u16 at, u16 len)
{
    u16 i;
    for (i = 0; i < len; i++) *w7f((u16)(at + i)) = 0;
}

/* video.s: the SNES's screen set-up. Each records the new state and bumps
 * gen; the picture is redrawn by wait_vblank. */
/* video_init: VRAM cleared */
void video_init(void)
{
    memset(vram, 0, sizeof vram);
    gen++;
}
/* video_game: the game screen (3D view plus BG1's message line and status bar) */
void video_game(void) { mode = MODE_GAME; gen++; pal_dirty = 1; }
/* video_full: a full-screen BG1 picture with tiles at word $1000 * t;
 * colour 0 (the backdrop) black */
void video_full(u8 t)
{
    mode = MODE_FULL;
    bg1_tiles = t;
    cgram[0] = 0;
    gen++;
    pal_dirty = 1;
}
/* screen_on / screen_off: leave or enter forced blank (a black screen) */
void screen_on(void) { blank = 0; gen++; }
void screen_off(void) { blank = 1; gen++; }
/* view_upload, view_busy, cgram_load: SNES transfers with nothing to do here */
void view_upload(void) {}
u8 view_busy(void) { return 0; }
void cgram_load(void) {}
/* flash: the damage flash (a red tint, palette_out) for the next frames frames */
void flash(u8 frames) { flash_n = frames; pal_dirty = 1; }
/* view_gradient: the floor's and ceiling's colours (BGR555) -> r32x.c */
void view_gradient(u16 floor, u16 ceiling) { r32x_colours(floor, ceiling); }
/* view_render: the game asks for a new view: start the camera moving to the player */
void view_render(void) { r32x_target(); }

/* ---- drawing it: BG1 and the sprites, a line at a time */
static u8 xpand[256][4];                /* (unused: an earlier lookup table, never filled or read) */
static u32 plane_lut[4][256][2];        /* bitplane k's byte -> 8 pixels (bit k set where 1) */
/* plane_lut[k][b][0] holds pixels 0..3 (from bits 7..4 of b) and [1]
   pixels 4..7 (bits 3..0), one pixel a byte, the leftmost in the long's
   top byte: OR-ing the four planes' entries gives 8 colour indexes. */

/* ppu_init: fill plane_lut. Called once by master_main. */
static void ppu_init(void)
{
    int k, b, x;
    (void)xpand;                        /* (silences the unused-variable warning) */
    for (k = 0; k < 4; k++)
        for (b = 0; b < 256; b++) {
            u32 hi = 0, lo = 0;
            for (x = 0; x < 4; x++) {
                if (b & (0x80 >> x)) hi |= (u32)(1 << k) << (24 - 8 * x);
                if (b & (0x08 >> x)) lo |= (u32)(1 << k) << (24 - 8 * x);
            }
            plane_lut[k][b][0] = hi;
            plane_lut[k][b][1] = lo;
        }
}

/* tile t's row y (0..7) at base (words): 8 colour indexes, left first, into d (big-endian longs)
 * (a 4bpp tile is 16 words: word y has plane 0 in its low byte and plane 1
 * in its high byte, word 8 + y planes 2 and 3) */
static void tile_row(u32 base, u32 t, u32 y, u32 *d)
{
    const u16 *p = &vram[(base + t * 16) & 32767];
    u16 a = p[y], b = p[8 + y];         /* a: planes 0/1, b: planes 2/3 */
    d[0] = plane_lut[0][a & 255][0] | plane_lut[1][a >> 8][0] | plane_lut[2][b & 255][0] | plane_lut[3][b >> 8][0];
    d[1] = plane_lut[0][a & 255][1] | plane_lut[1][a >> 8][1] | plane_lut[2][b & 255][1] | plane_lut[3][b >> 8][1];
}

/* a sprite to draw, decoded from the OAM */
struct spr {
    s16 x, y;                           /* top-left on the SNES screen (x -256..255, y -16..239) */
    u8 sz, at, t, k;                    /* size (16 or 32), OAM attribute byte, first tile, OAM index */
};
static struct spr sl[128];              /* the sprites of the rows being drawn (sprites_list) */
static int nsl;                         /* how many */

/* the sprites lo..hi-1 that touch rows y0..y1-1 (the last first: lower numbers in front)
 * Each OAM entry is 4 bytes: x (low 8 bits), y, tile, attributes (bit 0
 * the tile's 9th bit, i.e. the second tile table; bits 1..3 the palette;
 * bits 4..5 the priority; bit 6 h-flip; bit 7 v-flip). The 32-byte high
 * table at oam[512] gives each sprite 2 bits: x's 9th bit, and the size
 * (0 small = 16 x 16, 1 large = 32 x 32, as this game sets the PPU).
 * Fills sl[] and nsl. */
static void sprites_list(int lo, int hi, int y0, int y1)
{
    int k;
    nsl = 0;
    for (k = hi - 1; k >= lo; k--) {
        const u8 *o = &oam[k * 4];
        u8 hb = (u8)(oam[512 + (k >> 2)] >> ((k & 3) * 2));
        int sz = (hb & 2) ? 32 : 16, sx = o[0] | ((hb & 1) << 8), sy = o[1] + 1;    /* (the SNES shows a sprite one line below its y) */
        if (sx >= 256) sx -= 512;       /* 9-bit x: 256..511 are -256..-1 */
        if (sy >= 240) sy -= 256;       /* y near 255: partly above the top */
        if (sy >= y1 || sy + sz <= y0 || sx <= -sz || sx >= 256) continue;  /* off the rows or the screen */
        sl[nsl].x = (s16)sx;
        sl[nsl].y = (s16)sy;
        sl[nsl].sz = (u8)sz;
        sl[nsl].at = o[3];
        sl[nsl].t = o[2];
        sl[nsl].k = (u8)k;
        nsl++;
    }
}

/* one line: BG1 (if bg) and the listed sprites into px (colour indexes, 0 none)
 * y is the screen row (0..223), px 256 bytes. BG1's map is 32 x 32 entries
 * at VRAM word $5C00; each entry: bits 0..9 the tile, 10..12 the palette
 * (16 colours each), 13 the priority, 14 h-flip, 15 v-flip. In MODE_GAME
 * BG1 is scrolled 8 rows down (the map's row 0 is the screen's row 8 ...
 * wrapping, so the message line on rows 0..7 is the map's last row) and its
 * tiles are at word $4000; in MODE_FULL at bg1_tiles * $1000. Sprites use
 * palettes 8..15 (colours 128..255) and tile tables at word $6000 / $7000.
 * Called by ppu_rows. */
static void ppu_line(int y, int bg, u8 *px)
{
    static u8 prio[256];                /* 1 where BG1's pixel is a high-priority tile's */
    int x, i;
    if (bg) {
        int by = mode == MODE_GAME ? ((y - 8) & 255) : y;  /* the row within BG1's map */
        u32 base = mode == MODE_GAME ? 0x4000 : (u32)bg1_tiles * 0x1000;
        const u16 *row = &vram[(0x5C00 + (by >> 3) * 32) & 32767];
        for (x = 0; x < 32; x++) {      /* the 32 tiles across */
            u16 e = row[x];
            u32 d[2];
            u8 c[8], pal = (u8)(((e >> 10) & 7) << 4), pr = (e & 0x2000) != 0;
            int ty = by & 7, j;
            if (e & 0x8000) ty = 7 - ty;        /* v-flip */
            tile_row(base, e & 1023, (u32)ty, d);
            for (j = 0; j < 4; j++) {   /* the two longs -> 8 bytes */
                c[j] = (u8)(d[0] >> (24 - 8 * j));
                c[4 + j] = (u8)(d[1] >> (24 - 8 * j));
            }
            for (j = 0; j < 8; j++) {
                u8 v = c[(e & 0x4000) ? 7 - j : j];     /* h-flip */
                px[x * 8 + j] = v ? (u8)(pal + v) : 0;
                prio[x * 8 + j] = (u8)(v ? pr : 0);
            }
        }
    } else {                            /* no BG1: an empty line */
        memset(px, 0, 256);
        memset(prio, 0, 256);
    }
    for (i = 0; i < nsl; i++) {         /* the sprites, back to front (sl is in that order) */
        const struct spr *s = &sl[i];
        int row = y - s->y, sz = s->sz, c;
        u8 at = s->at, pal, pr;
        u32 base;
        if (row < 0 || row >= sz) continue;     /* not on this line */
        base = 0x6000 + ((at & 1) ? 0x1000 : 0);
        pal = (u8)(128 + ((at >> 1) & 7) * 16);
        pr = (at >> 4) & 3;
        if (at & 0x80) row = sz - 1 - row;      /* v-flip */
        for (c = 0; c < sz; c += 8) {   /* each 8-pixel tile across the sprite */
            int col = (at & 0x40) ? sz - 8 - c : c, j;  /* h-flip: the tiles in the other order */
            u32 d[2];
            u8 v8[8];
            /* (the sprite's tiles: 16 a row in the tile table) */
            tile_row(base, (u32)((s->t + (col >> 3) + ((row >> 3) << 4)) & 255), (u32)(row & 7), d);
            for (j = 0; j < 4; j++) {
                v8[j] = (u8)(d[0] >> (24 - 8 * j));
                v8[4 + j] = (u8)(d[1] >> (24 - 8 * j));
            }
            for (j = 0; j < 8; j++) {
                int px_x = s->x + c + j;
                u8 v = v8[(at & 0x40) ? 7 - j : j];
                if (!v || px_x < 0 || px_x >= 256) continue;
                /* Mode 1: sprites of priority 3 over BG1's high tiles, the rest under */
                if (prio[px_x] && pr != 3 && px[px_x]) continue;
                px[px_x] = (u8)(pal + v);
            }
        }
    }
}

/* the listed sprites straight into the frame buffer's rows y0..y1-1 (over
   the 3D view: no BG1 there), back to front
   Only the sprites' non-zero pixels are written, so the 3D view r32x_draw
   left in those rows shows between them. Draws into the back buffer
   (whichever frame buffer the 32X VDP lets the SH-2 write). */
static void sprites_over(int y0, int y1)
{
    int i;
    for (i = 0; i < nsl; i++) {
        const struct spr *s = &sl[i];
        int sz = s->sz, ra = s->y < y0 ? y0 - s->y : 0, rb = s->y + sz > y1 ? y1 - s->y : sz, row, c;
        u8 at = s->at, pal = (u8)(128 + ((at >> 1) & 7) * 16);
        u32 base = 0x6000 + ((at & 1) ? 0x1000 : 0);
        /* ra..rb-1: the sprite's rows inside y0..y1-1 */
        for (row = ra; row < rb; row++) {
            int r = (at & 0x80) ? sz - 1 - row : row;       /* v-flip */
            volatile u8 *d = MARS_FB8 + 0x200 + (s->y + row) * 320 + 32;   /* SNES x 0 is frame buffer column 32 */
            for (c = 0; c < sz; c += 8) {
                int col = (at & 0x40) ? sz - 8 - c : c, j;
                u32 dd[2];
                tile_row(base, (u32)((s->t + (col >> 3) + ((r >> 3) << 4)) & 255), (u32)(r & 7), dd);
                if (!(dd[0] | dd[1])) continue;    /* an empty tile row: nothing to draw */
                for (j = 0; j < 8; j++) {
                    int jj = (at & 0x40) ? 7 - j : j, px = s->x + c + j;
                    u8 v = (u8)((jj < 4 ? dd[0] : dd[1]) >> (24 - 8 * (jj & 3)));
                    if (v && px >= 0 && px < 256) d[px] = (u8)(pal + v);
                }
            }
        }
    }
}

/* ---- the frame buffer: 320 x 224, 8 bits a pixel; line y at byte 0x200 + 320 y */
#define FB_LINE(y) ((volatile u32 *)(MARS_FB8 + 0x200 + (y) * 320))

/* fb_lines: the frame buffer's line table (its first 256 words): line y
 * starts at word $100 + 160 y (byte $200 + 320 y); lines 224..255, never
 * shown, repeat line 223. Fills the buffer the SH-2 can write now. */
static void fb_lines(void)
{
    int y;
    for (y = 0; y < 256; y++) MARS_FB[y] = (u16)(0x100 + (y < 224 ? y : 223) * 160);
}

/* rows y0..y1-1 of the chip's picture (BG1 if bg, sprites lo..hi-1) into
   the frame buffer's middle 256 columns; the sides colour 0
   With keep_zero set it only draws the sprites over what is already there
   (sprites_over: for the rows of the 3D view), and does so once for all
   the rows, then returns. Callers: wait_vblank. */
static void ppu_rows(int y0, int y1, int bg, int lo, int hi, int keep_zero)
{
    static u8 px[256] __attribute__((aligned(4)));  /* one line's colour indexes (aligned: copied as longs) */
    int y, x;
    sprites_list(lo, hi, y0, y1);       /* the sprites on these rows */
    for (y = y0; y < y1; y++) {
        volatile u32 *d = FB_LINE(y);
        if (keep_zero) {
            sprites_over(y0, y1);       /* all the rows at once */
            return;
        }
        ppu_line(y, bg, px);            /* this line's pixels */
        for (x = 0; x < 8; x++) d[x] = 0;               /* columns 0..31: colour 0 */
        for (x = 0; x < 64; x++) d[8 + x] = ((const u32 *)px)[x];  /* 32..287: the SNES's 256 */
        for (x = 72; x < 80; x++) d[x] = 0;             /* 288..319: colour 0 */
    }
}

/* ------------------------------------------------------------ the frames */
u16 pad, pad_edge;                      /* the buttons held (SNES bits), and those newly pressed this frame */
volatile u16 frame_count;               /* the 68000's frame count, as of the last wait_vblank */
static u16 drawn_gen[2] = {0, 0};       /* per frame buffer: the gen it shows (~gen: blanked) */
static u8 back;                         /* the frame buffer being drawn (0/1) */
static u16 last_frame;                  /* the frame count when the last wait_vblank ended */


static void save_tick(void);
static u16 script_pad(u16 p);

/* palette_out: write all 256 colours of the 32X's palette: cgram, except
 * that on the game screen colours 32..223 are the 3D view's (r32x.c). While
 * flash_n is non-zero every colour gets the damage flash: red halfway to
 * full, green and blue halved. Waits for the palette to be accessible
 * (FBCTL bit 13, PEN). Called by wait_vblank. */
static void palette_out(void)
{
    int i;
    const u16 *vp = r32x_palette();
    while (!(MARS_VDP_FBCTL & 0x2000)) {}           /* (the palette: in a blank) */
    for (i = 0; i < 256; i++) {
        u16 c = (mode == MODE_GAME && i >= 32 && i < 224) ? vp[i] : cgram[i];
        if (flash_n) c = (u16)((((c & 31) + 31) >> 1) | (((c >> 6) & 15) << 5) | (((c >> 11) & 15) << 10));
        MARS_PALETTE[i] = (u16)(c & 0x7FFF);        /* (bit 15, the 32X's per-colour priority bit, kept clear) */
    }
}

/* frames_now: the 68000's frame count (COMM+4, m68k/main.c) */
static int frames_now(void) { return MARS_COMM(4); }

/*
 * wait_vblank: the SNES code's "wait for the next frame", called once per
 * game frame (by ui.c and its helpers). It:
 *  1. redraws the back frame buffer if what it shows is out of date (the
 *     chip's state changed, the camera moved, or the 3D view in that
 *     buffer is stale): on the game screen the message line and status bar
 *     (only if the chip's state changed), the 3D view (r32x_draw, both
 *     SH-2s), then the sprites over the view; on a full screen all rows;
 *     while blanked, colour 0 everywhere (once per buffer);
 *  2. if it drew, shows that buffer (the swap takes effect at the next
 *     vertical blank) and waits for it; else waits for the next frame;
 *  3. reloads the palette if needed, reads the pad (pad, pad_edge) and the
 *     frame count, and runs the sound and save ticks.
 * Inputs: the chip's state, oam_dirty, the 68000's COMM registers. Outputs:
 * the frame buffers, the palette, pad, pad_edge, frame_count.
 */
void wait_vblank(void)
{
    int redraw = 0;
    if (oam_dirty) {
        oam_dirty = 0;
        gen++;
    }
    if (!blank) {
        int moved = mode == MODE_GAME && r32x_step(frames_now());     /* the camera for this frame */
        /* (oam_dirty is always 0 here: it was cleared just above) */
        if (drawn_gen[back] != gen || moved || oam_dirty || r32x_changed(back)) redraw = 1;
    }
    if (redraw) {
        while (MARS_VDP_FBCTL & 2) {}               /* (the frame buffer: free) */
        if (mode == MODE_GAME) {
            if (drawn_gen[back] != gen) {
                ppu_rows(0, 8, 1, 0, 96, 0);        /* the message line (sprites 0..95) */
                ppu_rows(184, 224, 1, 0, 96, 0);    /* the status bar */
            }
            r32x_draw(back);                        /* rows 8..183 */
#ifdef R32X_BENCH
            /* (the benchmark build: once r32x.c has its results, time 60
               passes of the sprite drawing and store the frames taken) */
            {
                extern volatile u32 bench_res[6];
                static int once;
                if (!once && bench_res[5]) {
                    u16 f0 = MARS_COMM(4);
                    int k;
                    once = 1;
                    for (k = 0; k < 60; k++) ppu_rows(8, 184, 0, 0, 128, 1);
                    *(volatile u32 *)UNCACHED(&bench_res[4]) = (u16)(MARS_COMM(4) - f0);
                }
            }
#endif
            ppu_rows(8, 184, 0, 0, 128, 1);         /* the boxes, the weapon over it */
        } else
            ppu_rows(0, 224, 1, 0, 128, 0);
        drawn_gen[back] = gen;
    } else if (blank && drawn_gen[back] != (u16)~gen) {    /* blanked, and this buffer not yet cleared */
        int y, x;
        while (MARS_VDP_FBCTL & 2) {}               /* (FEN: wait until the buffer may be written) */
        for (y = 0; y < 224; y++)
            for (x = 0; x < 80; x++) FB_LINE(y)[x] = 0;
        drawn_gen[back] = (u16)~gen;
        redraw = 1;
    }
    /* the next frame: a new picture is shown from its start (the swap waits
       for the blank), else the frame counter moves on */
    if (redraw) {
        MARS_VDP_FBCTL = back;                      /* FS: show the buffer just drawn */
        while ((MARS_VDP_FBCTL & 1) != back) {}     /* (the swap happens in the vertical blank) */
        back ^= 1;                                  /* draw into the other one next */
    } else
        while (frames_now() == last_frame) {}       /* nothing new: wait for the 68000's next frame */
    if (pal_dirty || flash_n) {
        palette_out();
        pal_dirty = flash_n ? 1 : 0;                /* (during a flash: again next frame) */
        if (flash_n) flash_n--;
    }
    last_frame = (u16)frames_now();
    {
        u16 p = MARS_COMM(2);                       /* the pad, as the 68000 read it */
#ifdef PLAT_SCRIPT
        p = script_pad(p);
#endif
        pad_edge = p & ~pad;                        /* pressed now, not before */
        pad = p;
    }
    frame_count = (u16)frames_now();
    sound_tick();                                   /* the sounds the game queued -> the slave */
    save_tick();                                    /* write the saves out once they settle */
}

/* ticks: the frame count (the game's clock, 60 a second on NTSC) */
u16 ticks(void) { return frame_count; }
/* set_backdrop: the SNES sets its backdrop colour here; nothing to do */
void set_backdrop(void) {}

/* ------------------------------------------------------------ the saves:
   the 68000 reads and writes the battery RAM a byte at a time (COMM 8..12) */
/* The game reads and writes a copy in SDRAM (sram); save_tick sends the
   bytes that differ from what the battery RAM holds (sram_out) once the
   game has stopped writing for half a second. */
#define SRAM_SIZE 8192
static u8 sram[SRAM_SIZE], sram_out[SRAM_SIZE];    /* the game's copy; the battery RAM's contents */
static u16 save_wait;                   /* frames until the write-out (0: nothing pending) */

/* sram_io: one request to the 68000: cmd 1 read byte i, 2 write v to byte i.
 * Waits until the 68000 has cleared COMM+8 (done); returns COMM+12 (the
 * byte read, for a read). */
static u8 sram_io(u16 cmd, u16 i, u8 v)
{
    MARS_COMM(10) = i;
    MARS_COMM(12) = v;
    MARS_COMM(8) = cmd;                 /* (last: the 68000 acts when it sees this) */
    while (MARS_COMM(8)) {}
    return (u8)MARS_COMM(12);
}

/* sram_rd / sram_wr: the save code's (snes/src/save.c) byte access to the
   battery RAM, here through the SDRAM copy */
u8 sram_rd(u16 i) { return sram[i % SRAM_SIZE]; }
void sram_wr(u16 i, u8 v)
{
    sram[i % SRAM_SIZE] = v;
    save_wait = 30;                     /* written out once the writes stop */
}

/* save_tick: once a frame (wait_vblank): count save_wait down, and when it
   reaches 0 send every changed byte to the 68000 (this takes a frame or
   more of the 68000's time per 6000 bytes). */
static void save_tick(void)
{
    u16 i;
    if (!save_wait || --save_wait) return;  /* nothing pending, or not yet */
    for (i = 0; i < SRAM_SIZE; i++)
        if (sram[i] != sram_out[i]) {
            sram_io(2, i, sram[i]);
            sram_out[i] = sram[i];
        }
}

#ifdef PLAT_SCRIPT
/* ---- the script mode (a test build: build.sh test): the pad is played
   from the script tools/make_script.py packs from snes_play.py --script
   (the SNES game's run in step with the model) at the cartridge's $3E0000:
   "SCRP", u16 the bytes of S compared; then $1k a press of button k (when
   the game is idle: 2 frames released, 4 held, then until it has answered
   and is idle again), $40 offset value a poke into S, $80 crc16 S
   compared (when idle in play, a dialog, a code or the vendor), 0 the
   end. The results: script_states, script_bad (tools read them).
   (The "crc16" is the low 16 bits of the CRC-32 below.) The results are
   written through the cache-through alias (UNCACHED) so that a tool
   reading memory sees them at once. */
extern volatile u8 eng_turn_seq, eng_idle, eng_mode;   /* ui.c: turns taken, idle flag, its UI state */
#define SCRIPT ((const u8 *)0x023E0000)                 /* the script at cartridge offset $3E0000 */
static const u8 *sp;                    /* the next script byte (0: not started) */
static int s_st, s_cnt;                 /* the state (0 read, 1 released, 2 held, 3 wait, 9 done); frames left */
static u16 s_key, s_n;                  /* the button being pressed; the size of S compared */
static u8 s_seq;                        /* eng_turn_seq when the press began */
volatile u32 script_states, script_bad, script_done, script_first_bad;
/* states compared, how many differed, 1 at the end, the first that differed */

/* crc32: the standard (reflected, $EDB88320) CRC-32 of n bytes at p */
static u32 crc32(const u8 *p, u32 n)
{
    u32 c = 0xFFFFFFFFu;
    while (n--) {
        int k;
        c ^= *p++;
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));  /* (0 - bit: all ones if set) */
    }
    return ~c;
}

/* script button k -> the SNES pad bit: A, B, X, Y, L, R, Start, Select,
   up, down, left, right */
static const u16 KEYS[12] = {0x80, 0x8000, 0x40, 0x4000, 0x20, 0x10, 0x1000, 0x2000, 0x800, 0x400, 0x200, 0x100};

/* script_pad: in script mode, replaces the real pad p with the script's
   presses (the pad itself is ignored once a script is found). Runs the
   script's state machine one frame. Called by wait_vblank. */
static u16 script_pad(u16 p)
{
    (void)p;
    if (!sp) {
        if (SCRIPT[0] != 'S' || SCRIPT[1] != 'C') return p;    /* no script: the real pad */
        s_n = (u16)(SCRIPT[4] << 8 | SCRIPT[5]);
        sp = SCRIPT + 6;
    }
    switch (s_st) {
    case 0:                             /* run the commands while the game is idle */
        while (eng_idle) {
            u8 c = *sp;
            if (c == 0) {
                *(volatile u32 *)UNCACHED(&script_done) = 1;
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
                want = (u16)(sp[1] << 8 | sp[2]);
                sp += 3;
                *(volatile u32 *)UNCACHED(&script_states) = script_states + 1;
                if ((u16)crc32((const u8 *)&S, s_n) != want) {
                    if (!script_bad) *(volatile u32 *)UNCACHED(&script_first_bad) = script_states;
                    *(volatile u32 *)UNCACHED(&script_bad) = script_bad + 1;
                }
            } else {                    /* an unknown command: stop */
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
    case 2:                             /* held for 4 frames */
        if (--s_cnt == 0) s_st = 3;
        return s_key;
    case 3:                             /* released until the game has acted and is idle again */
        if (eng_turn_seq != s_seq && eng_idle) s_st = 0;
        return 0;
    }
    return 0;
}
#endif

/* ------------------------------------------------------------ start */
int ui_main(void);                      /* snes/src/ui.c's main (renamed by build.sh) */

/*
 * master_main: the master SH-2's program, called by sh2/crt0.s. Waits for
 * the slave and the 68000, sets up the 32X's picture (packed-pixel mode,
 * both frame buffers' line tables, cleared), tells the 68000 to start its
 * loop, loads the battery RAM, starts the 3D view and the sound, and runs
 * the game (ui_main), which does not return.
 */
void master_main(void)
{
    u16 i;
    while (MREG32(0x24) != 0x535F4F4B) {}           /* the slave's S_OK (the boot ROM's) */
    while (!(MARS_SYS & 0x8000)) {}                 /* the picture is ours */
    MARS_VDP_MODE = 1;                              /* packed pixels, the 32X in front */
    MARS_VDP_SHIFT = 0;                             /* no 1-pixel shift */
    ppu_init();
    back = (u8)((MARS_VDP_FBCTL & 1) ^ 1);          /* the buffer not shown */
    for (i = 0; i < 2; i++) {                       /* both buffers: line table, then cleared, then shown */
        int y, x;
        while (MARS_VDP_FBCTL & 2) {}
        fb_lines();
        for (y = 0; y < 224; y++)
            for (x = 0; x < 80; x++) FB_LINE(y)[x] = 0;
        MARS_VDP_FBCTL = back;
        while ((MARS_VDP_FBCTL & 1) != back) {}
        back ^= 1;
    }
    MARS_COMM(0) = 0x5348;                          /* "SH": going (the 68000 starts its loop) */
    while (MARS_COMM(4) == 0x535F) {}               /* (its frame count replaces the slave's S_OK) */
    /* the saves */
    for (i = 0; i < SRAM_SIZE; i++) sram[i] = sram_out[i] = sram_io(1, i, 0);   /* all 8 KB, a byte at a time */
#ifdef PLAT_SCRIPT
    memset(sram, 0xFF, sizeof sram);                /* (nothing saved: the SNES run had none) */
#endif
    r32x_init();                                    /* the 3D view's tables, the slave's half */
    sound_start();                                  /* the slave starts playing */
    last_frame = (u16)frames_now();
    ui_main();                                      /* the game (does not return) */
    for (;;) {}
}
