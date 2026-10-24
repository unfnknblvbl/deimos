/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the Game Boy Advance: the screen.
 *
 * Mode 4 (240 x 160, a byte a pixel, two pages): the picture is made in
 * the work RAM (scr) and copied onto the page not shown, which is shown
 * from the next vertical blank. The game's screen:
 *   y 0..7      the message line
 *   y 8..117    the 3D view: view/view.c's 128 x 88 (fb, drawn by
 *               view_draw.c) at 1.25 times (160 x 110, from x 40), the
 *               weapon over it (the Mega Drive's frames at 5/8 of the
 *               SNES's size: the same proportion to the view)
 *   y 120..159  the face (40 x 40, x 40) and the status bar (5 lines of
 *               20 characters, x 82)
 *   the boxes   over it all (24 characters of 6 pixels, 8-pixel lines)
 * Colours: 0 the backdrop (the floor and ceiling's shades: a colour per
 * line, by H-blank DMA, as the SNES's HDMA; black off the view), 1..6
 * the text's, 16..31 the face (the title: the title's), 32..223 the view
 * (view.c's slots, as the SNES's), 224..239 the weapon.
 * The title: the SNES title's picture cut to
 * 240 x 160, the lines under it drawn with the font.
 *
 * Where it fits: the Game Boy Color's ui.c calls the gfx_ functions (gfx.h)
 * to say what the screen shows and wait_vblank once a frame; gfx_view
 * runs view/view.c (which calls view_draw.c) to make a picture of the 3D
 * view. plat.c's interrupt dispatch calls irq_vblank, which flips the
 * pages, loads the palette, starts the backdrop's H-blank DMA and mixes
 * the sound (sound.c). The game's data comes through plat.c (snes_ptr,
 * blk: the campaign block, the Mega Drive game's map data).
 * Licence: GPL-2.0-or-later.
 */
#include "plat.h"
#include "gfx.h"
#include "sound.h"
#include "libc.h"
#include "../../view/view.h"

#ifndef VERSION_STR
#define VERSION_STR "?"   /* (build.sh: VERSION) */
#endif

u16 pad;                                /* the buttons (SNES bits), read once a frame */
volatile u16 frame_count;               /* V-blanks since the start (irq_vblank) */
u16 pad_read(void);                     /* plat.c */

/* the SNES's resources (snes_ui_res.py: "DRPGRES1", entries) */
enum { E_FONT, E_FACES, E_ICONS, E_HUD_PAL, E_DLG_PAL, E_TITLE_TILES, E_TITLE_MAP, E_TITLE_PAL };
static const u8 *res_base;              /* the resources' start (the bank in the header's $FFB7) */

/* res: resource e's data: its 32-bit little-endian offset from res_base is
   at res_base + 12 + 8 e */
static const u8 *res(u8 e)
{
    const u8 *p = res_base + 12 + 8 * e;
    return res_base + (p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24));
}

/* a Mega Drive colour level (0..7) -> 5 bits */
static const u8 LEV5[8] = {0, 6, 10, 14, 17, 21, 25, 31};
/* md_colour: a Mega Drive colour (0000 BBB0 GGG0 RRR0) -> BGR555 */
static u16 md_colour(u16 c) { return LEV5[(c >> 1) & 7] | ((u16)LEV5[(c >> 5) & 7] << 5) | ((u16)LEV5[(c >> 9) & 7] << 10); }

/* a Mega Drive tile's pixel (4 bits, two a byte, the left one high) */
static inline u8 md_px(const u8 *t, u32 x, u32 y) { return (t[y * 4 + (x >> 1)] >> ((x & 1) ? 0 : 4)) & 15; }

/* palette slots */
#define C_BLACK 1                       /* the text's colours (ui_colours) */
#define C_BOX 2
#define C_YELLOW 3
#define C_WHITE 4
#define C_GREY 5
#define FACE_SLOT 16                    /* 16..31: the face (or the title picture) */
#define WP_SLOT 224                     /* 224..239: the weapon */

#define SCR_W 240                       /* the screen */
#define SCR_H 160
#define VIEW_X 40                       /* the 3D view on the screen: left, top, width, height */
#define VIEW_Y 8
#define VW 160
#define VH 110
#define HUD_Y 120                       /* the status bar's top */

static u8 scr[SCR_W * SCR_H] __attribute__((aligned(4))) EWRAM_BSS;    /* the picture being made */
static u8 bg[SCR_W * SCR_H] __attribute__((aligned(4))) EWRAM_BSS;     /* the title's */
u8 fb[VIEW_W * VIEW_H] __attribute__((aligned(4), section(".iwram_bss")));   /* the view, row by row (view_draw.c) */
static u8 wp_img[VW * VH] __attribute__((aligned(4))) EWRAM_BSS;       /* the weapon at the screen's size (0: none) */
static u8 wp_y0, wp_y1;                 /* its rows */

/* ------------------------------------------------------------ the frames */
static u16 pal[256];                    /* the colours (flash: added red) */
static u16 pal_next[192];               /* the view's colours for the next picture */
static u16 grad[SCR_H], grad_out[SCR_H] IWRAM_BSS;     /* colour 0 per line */
/* (grad: as made; grad_out: what the DMA copies, flashed if need be) */
static volatile u8 flip_pending, pal_dirty, pal_with_flip, flash_n, flash_was;
/* flip_pending: a picture is on the hidden page, to be shown at the next
   V-blank; pal_dirty: load pal into the hardware then; pal_with_flip:
   pal_next goes into pal with the flip; flash_n: frames of the flash left;
   flash_was: a flash was on (the palette must be put back after it) */
static u8 shown_page;                   /* the page on the screen */
static u8 dirty, scr_mode;              /* scr_mode: 0 none, 1 the title, 2 the game */
/* dirty: the parts to make again */
#define D_MSG 1
#define D_VIEW 2
#define D_HUD 4
#define D_ALL 0xFF
static u8 sides_done;                   /* (the view's black sides: made once) */

/* flashed: colour c in the damage flash: red halfway to full, green and blue halved */
static u16 flashed(u16 c) { return (u16)((((c & 31) + 31) >> 1) | (((c >> 6) & 15) << 5) | (((c >> 11) & 15) << 10)); }

/* pal_out: pal (flashed or not) into the hardware palette, all but colour
   0, and the backdrop's colours into grad_out. Called by irq_vblank. */
static void pal_out(void)
{
    u32 i;
    if (flash_n)
        for (i = 1; i < 256; i++) PAL_BG[i] = flashed(pal[i]);
    else
        for (i = 1; i < 256; i++) PAL_BG[i] = pal[i];
    for (i = 0; i < SCR_H; i++) grad_out[i] = flash_n ? flashed(grad[i]) : grad[i];
}

/* irq_vblank: the V-blank interrupt (from plat.c's irq_dispatch): count
 * the frame, show the waiting picture (with its view colours), update the
 * palette and the flash, restart the backdrop's H-blank DMA for the new
 * frame, and make the next frame's sound. */
void irq_vblank(void)
{
    REG_DMA0CNT = 0;                    /* stop last frame's backdrop DMA */
    frame_count++;
    if (flip_pending) {
        REG_DISPCNT ^= 0x0010;          /* (bit 4: Mode 4's page shown) */
        shown_page ^= 1;
        flip_pending = 0;
        if (pal_with_flip) {
            memcpy(&pal[VIEW_SLOT0], pal_next, sizeof pal_next);
            pal_with_flip = 0;
            pal_dirty = 1;
        }
    }
    if (flash_n) {
        flash_n--;
        pal_dirty = 1;
    } else if (flash_was) {
        flash_was = 0;
        pal_dirty = 1;
    }
    if (pal_dirty) {
        pal_dirty = 0;
        pal_out();
    }
    /* the backdrop: line 0's now, the others in each H-blank */
    PAL_BG[0] = grad_out[0];
    REG_DMA0SAD = (u32)&grad_out[1];
    REG_DMA0DAD = (u32)&PAL_BG[0];
    REG_DMA0CNT = 1 | DMA_ON | DMA_HBLANK | DMA_REPEAT | DMA_DST_FIXED;   /* one halfword an H-blank */
    sound_mix();                        /* the next frame's samples */
}

/* vblank_wait: the BIOS's VBlankIntrWait (SWI 5): halt until the next
   V-blank interrupt (the BIOS clobbers r0..r3) */
static void vblank_wait(void)
{
    __asm__ volatile("swi 0x05" ::: "r0", "r1", "r2", "r3", "memory");
}

static void compose(void);

/* the picture onto the page not shown, shown from the next blank */
static void present(void)
{
    while (flip_pending) vblank_wait();
    compose();                          /* make scr up to date */
    REG_DMA3SAD = (u32)scr;
    REG_DMA3DAD = 0x06000000u + (shown_page ? 0 : 0xA000);     /* the hidden page */
    REG_DMA3CNT = (SCR_W * SCR_H / 4) | DMA_ON | DMA_32;        /* 9600 words, now (the CPU waits) */
    flip_pending = 1;
    dirty = 0;
}

/* wait_vblank: the game's once-a-frame call (ui.c): show any change, wait
   for the next V-blank, read the pad */
void wait_vblank(void)
{
    if (dirty) present();
    vblank_wait();
    pad = pad_read();                   /* (plat.c: the buttons, or the script's) */
    sound_tick();
}

/* ticks: the frame count */
u16 ticks(void) { return frame_count; }

/* ------------------------------------------------------------ text */
static u8 font[96][8];                  /* each glyph's rows: bit 7 - x the pixel x (0..7) */

/* font_init: the 96 glyphs (characters 32..127) from the SNES resources'
   Mega Drive font tiles: colour 1 is the ink */
static void font_init(void)
{
    const u8 *t = res(E_FONT);
    u32 g, y, x;
    for (g = 0; g < 96; g++)
        for (y = 0; y < 8; y++) {
            u8 b = 0;
            for (x = 0; x < 8; x++)
                if (md_px(t + 32 * g, x, y) == 1) b |= (u8)(0x80 >> x);
            font[g][y] = b;
        }
}

/* a character (6 pixels: the glyph's columns 1..6) at x, y
   in colour into scr, clipped to the screen; with shadow, a C_BLACK pixel
   down and right of each (unless that pixel is already the colour) */
static void put_char(int x, int y, char ch, u8 colour, u8 shadow)
{
    u8 c = (u8)ch;
    int r, k;
    if (c < 32 || c > 127) c = 32;
    for (r = 0; r < 8; r++) {
        u8 b = font[c - 32][r];
        if (!b || y + r < 0 || y + r >= SCR_H) continue;
        for (k = 1; k < 7; k++)
            if (b & (0x80 >> k)) {
                int px = x + k - 1;
                if (px < 0 || px >= SCR_W) continue;
                if (shadow && px + 1 < SCR_W && y + r + 1 < SCR_H) {
                    u8 *s = &scr[(y + r + 1) * SCR_W + px + 1];
                    if (*s != colour) *s = C_BLACK;
                }
                scr[(y + r) * SCR_W + px] = colour;
            }
    }
}

/* put_text: a string at x, y, 6 pixels a character */
static void put_text(int x, int y, const char *s, u8 colour, u8 shadow)
{
    for (; *s; s++, x += 6) put_char(x, y, *s, colour, shadow);
}

/* text_len: a string's length */
static int text_len(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* fill: a w x h rectangle of colour c in scr (not clipped) */
static void fill(int x, int y, int w, int h, u8 c)
{
    int r;
    for (r = 0; r < h; r++) memset(&scr[(y + r) * SCR_W + x], c, (size_t)w);
}

/* ------------------------------------------------------------ the title */
/* title_picture: the SNES title (a 32-wide map of Mega Drive tiles: tile
   number bits 0..10, h-flip bit 11, v-flip bit 12) into bg, from SNES
   column 8 and the rows below, with its 16 colours at FACE_SLOT */
static void title_picture(void)
{
    const u8 *tiles = res(E_TITLE_TILES), *map = res(E_TITLE_MAP), *tp = res(E_TITLE_PAL);
    int y, x, i;
    for (i = 0; i < 16; i++) pal[FACE_SLOT + i] = md_colour(be16(tp + 2 * i));
    for (y = 0; y < SCR_H; y++) {
        /* the SNES title's lines 30..189: the DEIMOS logo, its subtitle,
           the stars and the top of Mars (its version and PRESS START lines
           are below: the GBA's own text goes over the picture) */
        int sy = y + 30;
        for (x = 0; x < SCR_W; x++) {
            int sx = x + 8;
            u16 e = be16(map + 2 * ((sy >> 3) * 32 + (sx >> 3)));
            const u8 *t = tiles + 32 * (u32)(e & 0x7FF);
            u32 tx = sx & 7, ty = sy & 7;
            if (e & 0x0800) tx = 7 - tx;
            if (e & 0x1000) ty = 7 - ty;
            bg[y * SCR_W + x] = (u8)(FACE_SLOT + md_px(t, tx, ty));
        }
    }
}

/* ------------------------------------------------------------ the game's parts */
static char msg[41];                    /* the message line */
static char hud[5][21];                 /* the status bar's lines */
static u8 face_img[40 * 40];            /* the face picture (colour indexes) */
static u8 cur_face = 0xFF;              /* which face it is ($FF none) */
static char box_txt[16][24];            /* the box's text */
static u8 box_h, box_up;                /* its lines; shown */
static s8 wp_frame = -1;                /* the weapon frame in wp_img (-1 none) */

static void compose_view(void);

/* compose_box: the box into scr: a dark rectangle with a yellow border, 24
   characters wide, centred on the view if it fits there, else on the screen */
static void compose_box(void)
{
    int w = 24 * 6 + 8, h = box_h * 8 + 8, x0 = (SCR_W - w) / 2, y0, r, c;
    y0 = h <= VH ? VIEW_Y + (VH - h) / 2 : (SCR_H - h) / 2;
    fill(x0, y0, w, h, C_BOX);
    fill(x0, y0, w, 1, C_YELLOW);
    fill(x0, y0 + h - 1, w, 1, C_YELLOW);
    fill(x0, y0, 1, h, C_YELLOW);
    fill(x0 + w - 1, y0, 1, h, C_YELLOW);
    for (r = 0; r < box_h && r < 16; r++)
        for (c = 0; c < 24; c++) {
            char ch = box_txt[r][c];
            if (ch && ch != ' ') put_char(x0 + 4 + c * 6, y0 + 4 + r * 8, ch, C_WHITE, 0);
        }
}

/* compose: remake the parts of scr that dirty names (everything on the
   title, or when D_ALL), then the box over them. Called by present. */
static void compose(void)
{
    if (dirty == D_ALL) sides_done = 0;
    int l, y;
    u8 d = dirty;
    /* (the first test is covered by the last: not the game screen) */
    if (scr_mode == 1 || (d & ~(D_MSG | D_VIEW | D_HUD)) || scr_mode != 2) d = D_ALL;
    if (scr_mode == 1) {
        memcpy(scr, bg, sizeof scr);
        put_text((SCR_W - 11 * 6) / 2, 151, "PRESS START", C_YELLOW, 1);
        put_text(SCR_W - 6 * 6 - 2, 151, "v" VERSION_STR, C_GREY, 1);
    } else if (scr_mode == 2) {
        if (d & D_MSG) {
            fill(0, 0, SCR_W, VIEW_Y, 0);
            put_text((SCR_W - text_len(msg) * 6) / 2, 0, msg, C_WHITE, 0);
        }
        if (d & D_VIEW) compose_view();
        if (d & D_HUD) {
            fill(0, VIEW_Y + VH, SCR_W, SCR_H - VIEW_Y - VH, 0);
            for (y = 0; y < 40; y++) memcpy(&scr[(HUD_Y + y) * SCR_W + 40], &face_img[y * 40], 40);
            for (l = 0; l < 5; l++) put_text(82, HUD_Y + l * 8, hud[l], C_WHITE, 0);
        }
    } else
        memset(scr, 0, sizeof scr);
    if (box_up) compose_box();
}

/* ------------------------------------------------------------ the view's composition */
static u8 ys[VH];                       /* each screen row of the view -> its row in fb (x 4/5) */

/* (fast RAM, ARM) the view at 1.25 times, the weapon over it
   rows y0..y1-1 of the 160 x 110 view into dst (the screen, SCR_W a
   row), from src (fb) and wp (wp_img: 0 is see-through). Byte lanes are
   little-endian: each output word takes 4 bytes from the source words. */
IWRAM_CODE static void view_rows(u8 *dst, const u8 *src, const u8 *wp, u32 y0, u32 y1)
{
    u32 y, x, i;
    for (y = y0; y < y1; y++) {
        u32 *d = (u32 *)(dst + y * SCR_W);
        const u32 *s = (const u32 *)(src + (u32)ys[y] * VIEW_W);
        const u8 *w = wp + y * VW;
        /* 4 pixels -> 5 (the first twice): 16 -> 20, as 5 words */
        for (i = 0; i < VIEW_W / 16; i++, s += 4, d += 5) {
            u32 a = s[0], b = s[1], c = s[2], e = s[3];
            d[0] = (a & 0xFF) | (a << 8);
            d[1] = (a >> 24) | ((b & 0xFF) << 8) | (b << 16);
            d[2] = (b >> 16) | ((c & 0xFF) * 0x01010000u);
            d[3] = (c >> 8) | (e << 24);
            d[4] = e;
        }
        if (y >= wp_y0 && y < wp_y1) {
            u8 *db = dst + y * SCR_W;
            for (x = 0; x < VW; x++)
                if (w[x]) db[x] = w[x];
        }
    }
}

/* compose_view: the view into scr, and (once after a full redraw) black
   at its sides */
static void compose_view(void)
{
    u32 y;
    view_rows(scr + VIEW_Y * SCR_W + VIEW_X, fb, wp_img, 0, VH);
    if (sides_done) return;
    sides_done = 1;
    for (y = VIEW_Y; y < VIEW_Y + VH; y++) {
        memset(&scr[y * SCR_W], C_BLACK, VIEW_X);
        memset(&scr[y * SCR_W + VIEW_X + VW], C_BLACK, SCR_W - VIEW_X - VW);
    }
}

/* ------------------------------------------------------------ the API (ui.c's) */
/* ui_colours: the text's colours (BGR555) */
static void ui_colours(void)
{
    pal[C_BLACK] = 0;
    pal[C_BOX] = 2 | (3 << 5) | (10 << 10);
    pal[C_YELLOW] = 31 | (26 << 5);
    pal[C_WHITE] = 0x7FFF;
    pal[C_GREY] = 21 | (21 << 5) | (21 << 10);
}

/* screen_reset: no box, no weapon, a black backdrop, the text colours
   (after the last picture has been shown) */
static void screen_reset(void)
{
    u32 i;
    while (flip_pending) vblank_wait();
    box_up = 0;
    wp_frame = -1;
    wp_y0 = wp_y1 = 0;
    for (i = 0; i < SCR_H; i++) grad[i] = 0;
    ui_colours();
}

void gfx_init(void)
{
    u32 i;
    res_base = snes_ptr((u32)(snes_rom[0xFFB7] & 0x3F) << 16 | 0xC00000);   /* (the resources' bank) */
    font_init();
    for (i = 0; i < VH; i++) ys[i] = (u8)(i * 4 / 5);
    REG_DISPCNT = 0x0404;               /* mode 4, BG2 */
    shown_page = 0;
    ui_colours();
    pal_dirty = 1;
}

/* gfx_title: the title screen */
void gfx_title(void)
{
    screen_reset();
    title_picture();
    scr_mode = 1;
    pal_dirty = 1;
    dirty = D_ALL;
}

/* gfx_game: the game screen, cleared, with the face's palette */
void gfx_game(void)
{
    screen_reset();
    scr_mode = 2;
    msg[0] = 0;
    memset(hud, 0, sizeof hud);
    memset(face_img, 0, sizeof face_img);
    memset(fb, 0, sizeof fb);
    cur_face = 0xFF;
    {
        const u8 *hp = res(E_HUD_PAL);
        u32 i;
        for (i = 0; i < 16; i++) pal[FACE_SLOT + i] = md_colour(be16(hp + 2 * i));
    }
    pal_dirty = 1;
    dirty = D_ALL;
}

/* gfx_message: the message line's text (cut to 40 characters) */
void gfx_message(const char *s)
{
    u32 n = 0;
    if (scr_mode != 2) return;
    while (s[n] && n < 40) {
        msg[n] = s[n];
        n++;
    }
    msg[n] = 0;
    dirty |= D_MSG;
}

/* gfx_hud: the status bar's lines (redrawn only if they changed) */
void gfx_hud(char lines[5][21])
{
    if (scr_mode != 2 || !memcmp(hud, lines, sizeof hud)) return;
    memcpy(hud, lines, sizeof hud);
    dirty |= D_HUD;
}

/* gfx_face: face f (0..8): 25 Mega Drive tiles (5 x 5, column by column)
   into face_img; colour 0 becomes C_BLACK */
void gfx_face(u8 f)
{
    const u8 *t;
    u32 x, y;
    if (scr_mode != 2 || f == cur_face || f > 8) return;
    cur_face = f;
    t = res(E_FACES) + (u32)f * 25 * 32;
    for (y = 0; y < 40; y++)
        for (x = 0; x < 40; x++) {
            u8 v = md_px(t + 32 * ((x >> 3) * 5 + (y >> 3)), x & 7, y & 7);
            face_img[y * 40 + x] = v ? (u8)(FACE_SLOT + v) : C_BLACK;
        }
    dirty |= D_HUD;
}

/* gfx_box: show a box of h lines (y, the Game Boy Color's position, is ignored) */
void gfx_box(char box[16][24], u8 y, u8 h)
{
    (void)y;
    memcpy(box_txt, box, sizeof box_txt);
    box_h = h > 16 ? 16 : h;
    box_up = 1;
    dirty = D_ALL;
}

/* gfx_box_off: hide the box (the whole screen is redrawn) */
void gfx_box_off(void)
{
    if (!box_up) return;
    box_up = 0;
    dirty = D_ALL;
}

/* ------------------------------------------------------------ the weapon:
   the block's Mega Drive frames (as the SNES's ui.c shows them, in its
   screen's pixels), each pixel of ours the SNES's at 8/5 */
/* The block's weapon tables (offset at block +16, 8 bytes a table):
   0 the frames (10 bytes: first tile, -, palette, first sprite, count),
   1 the sprites (8 bytes: x, y, Mega Drive size, tile), 2 the palettes
   (16 colours), 3 the tiles. */
/* wp_table: weapon table k's offset in the block */
static u32 wp_table(u8 k) { return be32(blk + be32(blk + 16) + 8UL * k); }

/* gfx_weapon_off: no weapon (its rows of wp_img cleared) */
void gfx_weapon_off(void)
{
    if (wp_frame < 0 && wp_y1 == 0) return;
    wp_frame = -1;
    if (wp_y1) {
        memset(wp_img + wp_y0 * VW, 0, (size_t)(wp_y1 - wp_y0) * VW);
        wp_y0 = wp_y1 = 0;
        dirty |= D_VIEW;
    }
}

/* gfx_weapon: weapon w's frame (2 w, + 1 firing) into wp_img, scaled by
   5/8, its colours at WP_SLOT; wp_y0..wp_y1 its rows */
void gfx_weapon(u8 w, u8 firing)
{
    s8 id = (s8)(w * 2 + (firing ? 1 : 0));
    const u8 *f, *sp, *tiles, *pp;
    u32 n, k, i;
    int ymin = VH, ymax = 0;
    if (scr_mode != 2 || w > 8) return;
    if (id == wp_frame) return;
    gfx_weapon_off();
    wp_frame = id;
    f = blk + wp_table(0) + (u32)id * 10;
    n = be16(f + 8);
    sp = blk + wp_table(1) + be16(f + 6) * 8UL;
    tiles = blk + wp_table(3) + (u32)be16(f) * 32;
    pp = blk + wp_table(2) + be16(f + 4) * 32UL;
    for (i = 0; i < 16; i++) pal[WP_SLOT + i] = md_colour(be16(pp + 2 * i));
    pal_dirty = 1;
    for (k = 0; k < n && k < 16; k++) {
        const u8 *s = sp + k * 8;
        int x = be16(s) & 0x1FF, y = (s16)be16(s + 2), sz = be16(s + 4), tile = be16(s + 6);
        int w8 = ((sz >> 2) & 3) + 1, h8 = (sz & 3) + 1, dx, dy;  /* the size in tiles (Mega Drive: width bits 2..3, height 0..1) */
        if (x >= 256) x -= 512;         /* 9-bit x */
        y += 16;                        /* (the SNES's view rows: the sprite's line y + 24, the view's first 8) */
        /* our pixels whose SNES pixel (8/5 of ours, from its middle) is in the sprite */
        for (dy = (y * 5 + 7) / 8 - 1; dy <= ((y + h8 * 8) * 5) / 8 + 1; dy++) {
            int sy = (dy * 8 + 4) / 5 - y;
            if (dy < 0 || dy >= VH || sy < 0 || sy >= h8 * 8) continue;
            for (dx = (x * 5 + 7) / 8 - 1; dx <= ((x + w8 * 8) * 5) / 8 + 1; dx++) {
                int sx = (dx * 8 + 4) / 5 - x;
                u8 v;
                if (dx < 0 || dx >= VW || sx < 0 || sx >= w8 * 8) continue;
                /* (a Mega Drive sprite's tiles go down each column first) */
                v = md_px(tiles + (u32)(tile + (sx >> 3) * h8 + (sy >> 3)) * 32, (u32)(sx & 7), (u32)(sy & 7));
                if (!v) continue;
                wp_img[dy * VW + dx] = (u8)(WP_SLOT + v);
                if (dy < ymin) ymin = dy;
                if (dy > ymax) ymax = dy;
            }
        }
    }
    if (ymin <= ymax) {
        wp_y0 = (u8)ymin;
        wp_y1 = (u8)(ymax + 1);
    }
    dirty |= D_VIEW;
}

/* ------------------------------------------------------------ the picture */
/* gfx_flash: the red damage flash for frames frames (irq_vblank counts it down) */
void gfx_flash(u8 frames)
{
    flash_n = frames;
    flash_was = 1;
    pal_dirty = 1;
}

/* rgb15: 8-bit r, g, b -> BGR555; berserk tints it red (red halfway to
   full, green and blue halved) */
static u16 rgb15(const u8 *rgb, u8 berserk)
{
    u16 r = rgb[0], g = rgb[1], b = rgb[2];
    if (berserk) {
        r = (r + 255) >> 1;
        g >>= 1;
        b >>= 1;
    }
    return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10);
}

/* the SNES's shade (snes/src/video.c): 0.3 + 0.9 t, t = 0 at the horizon
   .. 1 at the view's top and bottom, over its 176 lines */
static u16 shade(u16 c, u16 line)
{
    u16 t = line < 88 ? (u16)(((87 - line) * 2 + 1) * 256 / 176) : (u16)(((line - 88) * 2 + 1) * 256 / 176);
    u16 f = 77 + ((t * 230) >> 8), out = 0, v;    /* 0.3 + 0.9 t, x 256 */
    u8 k;
    for (k = 0; k < 3; k++) {
        v = (u16)((((c >> (5 * k)) & 31) * f) >> 8);
        out |= (v > 31 ? 31 : v) << (5 * k);
    }
    return out;
}

/* gfx_backdrop: colour 0 for each screen line: in the view the ceiling's
   or floor's shade for that line (as the SNES's 176-line view would have
   it), black elsewhere */
void gfx_backdrop(const u8 *floor_rgb, const u8 *ceil_rgb, u8 berserk)
{
    u16 fl = rgb15(floor_rgb, berserk), ce = rgb15(ceil_rgb, berserk);
    u32 y;
    for (y = 0; y < SCR_H; y++) {
        u16 c = 0;
        if (y >= VIEW_Y && y < VIEW_Y + VH) {
            u16 line = (u16)(((y - VIEW_Y) * 2 + 1) * 176 / (2 * VH));     /* the SNES's line at our line's middle */
            c = shade(line < 88 ? ce : fl, line);
        }
        grad[y] = c;
    }
    pal_dirty = 1;
}

/* a step or a quarter turn shown in VIEW_STEPS pictures (the camera in
   between: view.c's free camera, the SNES SuperFX build's way) */
#define VIEW_STEPS 4
static s16 lc_x, lc_z;                  /* the last picture's camera (world units; angle 256 a turn) */
static u8 lc_a, lc_map = 0xFF;

/* show_picture: the picture view.c has just drawn in fb, with its colours,
   onto the screen at the next V-blank */
static void show_picture(void)
{
    while (flip_pending) vblank_wait();     /* (pal_next: the last picture's are in) */
    memcpy(pal_next, view_cgram, sizeof pal_next);
    pal_with_flip = 1;
    dirty |= D_VIEW;
    present();
}

/* gfx_view: the view where the player now is. If that is one step or a
   quarter turn from the last picture (same map, no box), first show the
   VIEW_STEPS - 1 pictures in between. */
void gfx_view(void)
{
    s16 tx = (s16)S.px * 64 + 32, tz = (s16)S.pz * 64 + 32, dx, dz;
    u8 ta = (u8)((S.facing & 3) << 6), k;
    s8 da;
    if (scr_mode != 2) return;
    if (lc_map == S.map && !box_up) {
        dx = tx - lc_x;
        dz = tz - lc_z;
        da = (s8)(u8)(ta - lc_a);
        if ((da == 0 && ((dx == 0 && (dz == 64 || dz == -64)) || (dz == 0 && (dx == 64 || dx == -64)))) ||
            (dx == 0 && dz == 0 && (da == 64 || da == -64)))
            for (k = 1; k < VIEW_STEPS; k++) {     /* (drawn while the last picture waits for its blank) */
                view_camera(lc_x + dx / VIEW_STEPS * k, lc_z + dz / VIEW_STEPS * k, (u8)(lc_a + da / VIEW_STEPS * k));
                view_render_cam();
                show_picture();
            }
    }
    lc_x = tx;
    lc_z = tz;
    lc_a = ta;
    lc_map = S.map;
    view_render();                      /* (view.c: from S, the player's own position) */
    show_picture();
}
