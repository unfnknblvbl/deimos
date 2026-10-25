/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the Virtual Boy: the screen.
 *
 * The VIP's own drawing (its worlds, characters and sprites) is off
 * (XPCTRL's XPEN never set): the CPU draws every pixel itself. With the
 * drawing off, which of the two frame buffer pairs (left and right eye
 * each) the display shows is not something to rely on (the emulators and
 * the hardware differ), so both pairs always hold the same picture: the
 * screen's middle 256 columns (the view, the status bar, the boxes, the
 * title) are made in a back buffer in the VIP's spare memory (bb), and
 * once made copied into both pairs at the start of a display frame
 * (publish), before the display reaches the left eye's columns; the
 * message line is drawn into both pairs directly. A frame buffer is 384
 * columns of 64 bytes, 2 bits a pixel (shades 0 black .. 3 the brightest
 * red; the brightness registers set how bright each is), the top pixel
 * of each byte in its low bits; this file writes them a halfword (8
 * pixels down) at a time.
 *
 * The game's screen (384 x 224, the same in both eyes but for the view):
 *   y 0..7      the message line (8 x 8 characters, centred)
 *   y 8..183    the 3D view: view/view.c's 128 x 88 (fb, drawn by
 *               view_draw.c) at twice the size (256 x 176, from x 64),
 *               the weapon over it (the Mega Drive's frames at 1.5 times
 *               the SNES's size, which is this view's: gfx_weapon)
 *   y 184..223  the face (40 x 40, x 68) and the status bar (5 lines of
 *               25 characters, x 116: the SNES's layout, tools/vb_ui.py)
 *   the boxes   over the view (24 characters of 8 pixels, 8-pixel lines)
 * The title: the SNES title's picture (256 x 224, its text drawn in by
 * megadrive/tools/build_md_ui.py), from x 64.
 *
 * Shades. Each colour (the view's slots, the backdrop's rows, the face's,
 * the weapon's, the title's) is turned into one of 13 levels (LUMA_LEVEL:
 * its brightness, in the display's linear light), and a level into a 2 x
 * 2 pattern of two neighbouring shades (level L: shade L / 4 everywhere
 * and one more in L % 4 of the four pixels, in the order of a 2 x 2 Bayer
 * matrix), fixed to the screen's pixels: so a pixel of the 128 x 88 view,
 * which covers 2 x 2 screen pixels, shows its whole level.
 *
 * Stereo. The view is drawn once; each eye's picture shifts each column
 * sideways by an amount from its depth (col_depth: the nearest of the
 * column's wall and the things drawn in it, from their heights in pixels):
 * 1 pixel between the eyes for the nearest, up to 8 for the farthest, so
 * the view is behind the screen and things stand out from the walls
 * behind them. The message line, the status bar, the boxes and the weapon
 * have no shift (they are at the screen, in front of the view). stereo_map
 * says, for each of the 256 screen columns of each eye, the view column
 * shown there; viewtest.c and tools/vb_view_test.py check it pixel for
 * pixel.
 *
 * Where it fits: the Game Boy Color's ui.c calls the gfx_ functions (gfx.h)
 * to say what the screen shows and wait_vblank once a frame; gfx_view
 * runs view/view.c (which calls view_draw.c) to make a picture of the 3D
 * view. plat.c's interrupt dispatch calls gfx_frame at each display frame
 * (50.27 a second), which swaps the frame buffers and counts the frames.
 * The game's data comes through plat.c (snes_ptr, blk).
 * Licence: GPL-2.0-or-later.
 */
#include "plat.h"
#include "gfx.h"
#include "sound.h"
#include "libc.h"

u16 pad;                                /* the buttons (SNES bits), read once a frame */
volatile u16 frame_count;               /* display frames since the start (gfx_frame) */
volatile u32 prof_last[4];              /* the last step: its time, one picture's render, one picture's
                                           stereo composition (100 us units), its pictures */

/* the SNES's resources (snes_ui_res.py's entries, in its order) */
enum { E_FONT, E_FACES, E_ICONS, E_HUD_PAL, E_DLG_PAL, E_TITLE_TILES, E_TITLE_MAP, E_TITLE_PAL };
static const u8 *res_base;              /* the resources' start (the bank in the SNES header's $FFB7) */

/* res: resource e's data: its 32-bit little-endian offset from res_base is
   at res_base + 12 + 8 e */
static const u8 *res(u8 e)
{
    const u8 *p = res_base + 12 + 8 * e;
    return res_base + (p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24));
}

/* a Mega Drive tile's pixel (4 bits, two a byte, the left one high) */
static inline u8 md_px(const u8 *t, u32 x, u32 y) { return (t[y * 4 + (x >> 1)] >> ((x & 1) ? 0 : 4)) & 15; }

#define SCR_W 384                       /* the screen */
#define SCR_H 224
#define VIEW_X 64                       /* the 3D view on the screen: left, top, width, height */
#define VIEW_Y 8
#define VW 256
#define VH 176
#define HUD_Y 184                       /* the status bar's top */
#define FACE_X 68                       /* the face's left (the face, a gap and the text: 248 pixels under the view) */
#define HUD_TX 116                      /* the status bar's text (HUD_COLS characters) */
#define VIEW_HW (VH / 8)                /* the view's halfwords down a column (22) */

/* ------------------------------------------------------------ levels and shades */
/* LUMA_LEVEL: a colour's brightness (luma, 0..63: 77 r + 150 g + 29 b over
   128 for 5-bit r, g, b) -> its level 0..12, the nearest of the levels'
   light (0, 1/16 .. 1/2 in sixteenths, then 5/8 .. 1 in eighths: the mixes
   of two neighbouring shades of 0, 1/4, 1/2 and all the light) to
   (luma / 62) ^ 2.2 (the colour's light) */
static const u8 LUMA_LEVEL[64] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3,
    4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 7, 7, 8, 8, 8, 8, 9, 9, 9, 9, 9, 10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12};

/* level15: a BGR555 colour's level */
static u8 level15(u16 c)
{
    u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return LUMA_LEVEL[(77 * r + 150 * g + 29 * b) >> 7];
}

/* a Mega Drive colour level (0..7) -> 5 bits (as the GBA shows them) */
static const u8 LEV5[8] = {0, 6, 10, 14, 17, 21, 25, 31};
/* md_level: a Mega Drive colour (0000 BBB0 GGG0 RRR0)'s level */
static u8 md_level(u16 c) { return level15(LEV5[(c >> 1) & 7] | ((u16)LEV5[(c >> 5) & 7] << 5) | ((u16)LEV5[(c >> 9) & 7] << 10)); }

/* shade_at: the shade of level l at screen pixel (x, y): l / 4, one more
   where l % 4 passes the 2 x 2 Bayer matrix's threshold (0 2 / 3 1) */
static u8 shade_at(u8 l, u32 x, u32 y)
{
    static const u8 BAYER[2][2] = {{0, 2}, {3, 1}};
    return (u8)((l >> 2) + ((l & 3) > BAYER[y & 1][x & 1]));
}

/* NIB[p][l]: the two pixels (a column's even row, then the odd one below
   it: 2 bits each) that level l gives in an even (p 0) or odd (p 1)
   screen column: a view pixel's half of its 2 x 2 block */
static u8 NIB[2][13];

/* nib_init: NIB from shade_at */
static void nib_init(void)
{
    u8 p, l;
    for (p = 0; p < 2; p++)
        for (l = 0; l < 13; l++) NIB[p][l] = (u8)(shade_at(l, p, 0) | (shade_at(l, p, 1) << 2));
}

/* the halfword of a shade's 8 pixels */
static const u16 SHADE_HW[4] = {0x0000, 0x5555, 0xAAAA, 0xFFFF};

/* the back buffer: the screen's columns VIEW_X .. VIEW_X + 255 of each
   eye, all 28 halfwords (224 rows) of each (28 KB, in the VIP's spare
   memory) */
#define BB_ROWS (SCR_H / 8)
static u16 bb[2][VW][BB_ROWS] VBSS;
#define TO_BB 2                         /* (the "pair" that is the back buffer, for fb_hw) */

/* fb_hw: the halfword of pair b (0, 1; TO_BB: the back buffer, x from
   VIEW_X to VIEW_X + 255), eye e, column x, rows 8 r .. 8 r + 7 */
static inline volatile u16 *fb_hw(u32 b, u32 e, u32 x, u32 r)
{
    if (b == TO_BB) return (volatile u16 *)&bb[e][x - VIEW_X][r];
    return (volatile u16 *)(FB_ADDR(b, e) + x * 64) + r;
}

/* on_target: column x can be drawn in pair b (the back buffer has only the middle 256) */
static inline int on_target(u32 b, int x)
{
    return b == TO_BB ? x >= VIEW_X && x < VIEW_X + VW : x >= 0 && x < SCR_W;
}

/* ------------------------------------------------------------ the frames */
static volatile u8 flash_n, flash_on;   /* the damage flash's frames left; it is on */
static u8 dirty, scr_mode;              /* the parts to draw again; 0 none, 1 the title, 2 the game */
static u8 need_clear;                   /* the screen changed: both pairs cleared first */
#define D_MSG 1
#define D_VIEW 2
#define D_HUD 4
#define D_ALL 0xFF
/* mark: parts d to be drawn again */
static void mark(u8 d) { dirty |= d; }

/* brightness: the brightness registers: normal, or the damage flash's
   (every shade brighter) */
static void brightness(u8 flash)
{
    VIP_BRTA = flash ? 56 : 32;         /* shade 1 */
    VIP_BRTB = flash ? 72 : 64;         /* shade 2 */
    VIP_BRTC = flash ? 0 : 32;          /* shade 3: A + B + C */
}

/* gfx_frame: a display frame starts (plat.c's VIP interrupt): count it;
   the flash counted down */
void gfx_frame(void)
{
    frame_count++;
    if (flash_n) {
        if (!flash_on) brightness(1);
        flash_on = 1;
        flash_n--;
    } else if (flash_on) {
        brightness(0);
        flash_on = 0;
    }
}

/* frame_wait: halt until the next display frame (other interrupts, the
   timer's, wake the CPU too: it halts again) */
static void frame_wait(void)
{
    u16 f = frame_count;
    while (frame_count == f) __asm__ volatile("halt");
}

static void compose(u8 d);

/* present: the changed parts drawn (compose) and shown */
static void present(void)
{
    u8 d = dirty;
    dirty = 0;
    compose(d);
}

/* wait_vblank: the game's once-a-frame call (ui.c): show any change, wait
   for the next frame, read the pad, the sound's part */
void wait_vblank(void)
{
    if (dirty) present();
    frame_wait();
    pad = pad_read();                   /* (plat.c: the buttons, or the script's) */
    sound_tick();
}

/* ticks: the frame count */
u16 ticks(void) { return frame_count; }

/* ------------------------------------------------------------ text */
static u16 font_hw[96][8];              /* each glyph's 8 columns: a halfword with 11 at each ink pixel */

/* font_init: the 96 glyphs (characters 32..127) from the SNES resources'
   Mega Drive font tiles: colour 1 is the ink */
static void font_init(void)
{
    const u8 *t = res(E_FONT);
    u32 g, y, x;
    for (g = 0; g < 96; g++)
        for (x = 0; x < 8; x++) {
            u16 m = 0;
            for (y = 0; y < 8; y++)
                if (md_px(t + 32 * g, x, y) == 1) m |= (u16)(3 << (2 * y));
            font_hw[g][x] = m;
        }
}

/* put_char: character ch at column x, rows 8 r .. 8 r + 7, in shade s, in
   both eyes of pair b or the back buffer (clipped to its width) */
static void put_char(u32 b, int x, u32 r, char ch, u8 s)
{
    u8 c = (u8)ch;
    u32 k, e;
    if (c < 32 || c > 127) c = 32;
    for (k = 0; k < 8; k++) {
        u16 m = font_hw[c - 32][k];
        if (!m || !on_target(b, x + (int)k)) continue;
        for (e = 0; e < 2; e++) {
            volatile u16 *h = fb_hw(b, e, (u32)x + k, r);
            *h = (u16)((*h & ~m) | (m & SHADE_HW[s]));
        }
    }
}

/* put_text: a string at column x, character row r, 8 pixels a character */
static void put_text(u32 b, int x, u32 r, const char *s, u8 shade)
{
    for (; *s; s++, x += 8) put_char(b, x, r, *s, shade);
}

/* text_len: a string's length */
static int text_len(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* fill: the rectangle x..x+w-1, y..y+h-1 in shade s, both eyes of pair b */
static void fill(u32 b, u32 x, u32 y, u32 w, u32 h, u8 s)
{
    u32 r, c, e;
    for (r = y >> 3; r <= (y + h - 1) >> 3; r++) {
        /* the rows of this halfword inside the rectangle */
        u32 lo = r * 8 < y ? y - r * 8 : 0, hi = r * 8 + 8 > y + h ? y + h - r * 8 : 8;
        u16 m = (u16)(((1u << (2 * hi)) - 1) & ~((1u << (2 * lo)) - 1));
        for (c = x; c < x + w; c++)
            for (e = 0; e < 2; e++) {
                volatile u16 *p = fb_hw(b, e, c, r);
                *p = (u16)((*p & ~m) | (m & SHADE_HW[s]));
            }
    }
}

/* clear_pair: both eyes of pair b black */
static void clear_pair(u32 b)
{
    u32 e, i;
    for (e = 0; e < 2; e++) {
        volatile u32 *p = (volatile u32 *)FB_ADDR(b, e);
        for (i = 0; i < 0x6000 / 4; i++) p[i] = 0;
    }
}

/* ------------------------------------------------------------ the title */
static u16 title_hw[VW][SCR_H / 8] VBSS;   /* the title picture's 256 columns, ready to copy */
static u8 title_done;                   /* title_hw made */

/* title_picture: the SNES title (a 32-wide map of Mega Drive tiles: tile
   number bits 0..10, h-flip bit 11, v-flip bit 12; 16 Mega Drive colours)
   into title_hw, its colours as levels shaded on the screen's pixels */
static void title_picture(void)
{
    const u8 *tiles = res(E_TITLE_TILES), *map = res(E_TITLE_MAP), *tp = res(E_TITLE_PAL);
    u8 lev[16];
    u32 x, y, i;
    for (i = 0; i < 16; i++) lev[i] = md_level(be16(tp + 2 * i));
    for (x = 0; x < VW; x++)
        for (y = 0; y < SCR_H; y++) {
            u16 e = be16(map + 2 * ((y >> 3) * 32 + (x >> 3)));
            const u8 *t = tiles + 32 * (u32)(e & 0x7FF);
            u32 tx = x & 7, ty = y & 7;
            if (e & 0x0800) tx = 7 - tx;
            if (e & 0x1000) ty = 7 - ty;
            if (!(y & 7)) title_hw[x][y >> 3] = 0;
            title_hw[x][y >> 3] |= (u16)(shade_at(lev[md_px(t, tx, ty)], x + VIEW_X, y) << (2 * (y & 7)));
        }
    title_done = 1;
}

/* ------------------------------------------------------------ the game's parts */
static char msg[41];                    /* the message line */
static char hud[5][HUD_COLS + 1];       /* the status bar's lines */
static u16 face_hw[40][5];              /* the face's 40 columns (5 halfwords each) */
static u8 cur_face = 0xFF;              /* which face it is ($FF none) */
static char box_txt[16][24];            /* the box's text */
static u8 box_h, box_up;                /* its lines; shown */
static s8 wp_frame = -1;                /* the weapon frame in wp_pix (-1 none) */

/* the weapon over the view (no stereo shift): per view column its pixels
   and its mask (3 at each of its pixels), the view's 22 halfwords;
   columns wp_x0..wp_x1-1, halfwords wp_r0..wp_r1-1 hold it */
static u16 wp_pix[VW][VIEW_HW] VBSS, wp_msk[VW][VIEW_HW] VBSS;
static u16 wp_x0, wp_x1, wp_r0, wp_r1;

/* the view's shading: each slot's two-pixel pattern for even / odd
   columns, the backdrop's for each of the view's 88 rows */
static u8 slot_nib[2][256], bg_nib[2][VIEW_H];
/* the stereo: each eye's view column at each of the 256 screen columns
   (255: black), and each view column's depth (1 near .. 8 far) */
static u8 stereo_src[2][VW], col_depth[VIEW_W];

/* view_column: one screen column of the view (0..255, eye e) into pair b:
   view column s at twice the size, shaded by the column's parity, the
   weapon over it */
static void view_column(u32 b, u32 e, u32 x)
{
    volatile u16 *d = fb_hw(b, e, VIEW_X + x, VIEW_Y / 8);
    u32 s = stereo_src[e][x], r;
    if (s == 255) {                     /* (past the picture's edge: black) */
        for (r = 0; r < VIEW_HW; r++) d[r] = 0;
    } else {
        const u8 *col = fb + s * VIEW_H, *nib = slot_nib[x & 1], *bg = bg_nib[x & 1];
        for (r = 0; r < VIEW_HW; r++, col += 4) {
            /* 4 view rows -> 8 screen rows: a halfword */
            u32 h = (u32)(col[0] ? nib[col[0]] : bg[4 * r]) | ((u32)(col[1] ? nib[col[1]] : bg[4 * r + 1]) << 4) |
                    ((u32)(col[2] ? nib[col[2]] : bg[4 * r + 2]) << 8) |
                    ((u32)(col[3] ? nib[col[3]] : bg[4 * r + 3]) << 12);
            d[r] = (u16)h;
        }
    }
    if (x >= wp_x0 && x < wp_x1)        /* the weapon */
        for (r = wp_r0; r < wp_r1; r++) d[r] = (u16)((d[r] & ~wp_msk[x][r]) | wp_pix[x][r]);
}

/* compose_view: the view, both eyes, into pair b */
static void compose_view(u32 b)
{
    u32 e, x;
    for (e = 0; e < 2; e++)
        for (x = 0; x < VW; x++) view_column(b, e, x);
}

/* compose_box: the box into pair b: black with a grey border, 24
   characters wide, centred on the view (on the title, under the logo),
   its text lines on character rows */
static void compose_box(u32 b)
{
    u32 w = 24 * 8 + 8, h = box_h * 8 + 8, x0 = VIEW_X + (VW - w) / 2, y0, r, c;
    y0 = scr_mode == 1 ? 108 : VIEW_Y + (VH - h) / 2;     /* (the title's menu: under the logo and its subtitle) */
    y0 = ((y0 + 4) & ~7u) - 4;          /* (the text's rows, y0 + 4.., on character rows) */
    fill(b, x0, y0, w, h, 0);
    fill(b, x0, y0, w, 1, 2);
    fill(b, x0, y0 + h - 1, w, 1, 2);
    fill(b, x0, y0, 1, h, 2);
    fill(b, x0 + w - 1, y0, 1, h, 2);
    for (r = 0; r < box_h && r < 16; r++)
        for (c = 0; c < 24; c++) {
            char ch = box_txt[r][c];
            if (ch && ch != ' ') put_char(b, (int)(x0 + 4 + c * 8), (y0 + 4) / 8 + r, ch, 3);
        }
}

/* publish: rows r0..r1-1 (halfwords) of the back buffer's columns into
   both eyes of both pairs, from the start of a display frame (the copy,
   about 5 ms for the view, ends before the display reaches the left
   eye's columns, so no frame shows half of it) */
static void publish(u32 r0, u32 r1)
{
    u32 e, x, r;
    if (r0 >= r1) return;
    frame_wait();
    for (e = 0; e < 2; e++)
        for (x = 0; x < VW; x++) {
            const volatile u16 *src = bb[e][x];
            volatile u16 *d0 = fb_hw(0, e, VIEW_X + x, 0), *d1 = fb_hw(1, e, VIEW_X + x, 0);
            for (r = r0; r < r1; r++) {
                u16 h = src[r];
                d0[r] = h;
                d1[r] = h;
            }
        }
}

/* compose: remake the parts d names (everything on the title, or when
   the screen changed): the middle columns in the back buffer (the view,
   the status bar, the box over them), then into both pairs; the message
   line into both pairs directly. Called by present. */
static void compose(u8 d)
{
    u32 l, x, e, r, r0 = BB_ROWS, r1 = 0;
    if (need_clear) {                   /* (a new screen: both pairs black, then all of it) */
        need_clear = 0;
        clear_pair(0);
        clear_pair(1);
        d = D_ALL;
    }
    if (scr_mode == 1) {
        if (!title_done) title_picture();
        for (e = 0; e < 2; e++)
            for (x = 0; x < VW; x++)
                for (r = 0; r < BB_ROWS; r++) bb[e][x][r] = title_hw[x][r];
        if (box_up) compose_box(TO_BB);
        r0 = 0;
        r1 = BB_ROWS;
    } else if (scr_mode == 2) {
        if (d & D_VIEW) {
            compose_view(TO_BB);
            if (box_up) compose_box(TO_BB);
            r0 = VIEW_Y / 8;
            r1 = HUD_Y / 8;
        }
        if (d & D_HUD) {
            fill(TO_BB, VIEW_X, HUD_Y, VW, SCR_H - HUD_Y, 0);
            for (e = 0; e < 2; e++)
                for (x = 0; x < 40; x++)
                    for (r = 0; r < 5; r++) *fb_hw(TO_BB, e, FACE_X + x, HUD_Y / 8 + r) = face_hw[x][r];
            for (l = 0; l < 5; l++) put_text(TO_BB, HUD_TX, HUD_Y / 8 + l, hud[l], 3);
            if (r0 > HUD_Y / 8) r0 = HUD_Y / 8;
            r1 = BB_ROWS;
        }
    }
    publish(r0, r1);
    if (scr_mode == 2 && (d & D_MSG))   /* the message line, in both pairs */
        for (x = 0; x < 2; x++) {
            fill(x, 0, 0, SCR_W, 8, 0);
            put_text(x, (SCR_W - text_len(msg) * 8) / 2, 0, msg, 3);
        }
}

/* ------------------------------------------------------------ the API (ui.c's) */
/* screen_reset: no box, no weapon, both pairs to be cleared */
static void screen_reset(void)
{
    need_clear = 1;
    box_up = 0;
    wp_frame = -1;
    wp_x0 = wp_x1 = 0;
}

/* gfx_init: the display: the VIP's drawing off, the column tables, the
   brightness, both pairs black, the display on, the frame interrupt;
   the font, the shading table */
void gfx_init(void)
{
    u32 i;
    res_base = snes_ptr((u32)(snes_rom[0xFFB7] & 0x3F) << 16 | 0xC00000);   /* (the resources' bank) */
    font_init();
    nib_init();
    VIP_INTENB = 0;                     /* (no VIP interrupts while it is set up) */
    VIP_INTCLR = 0xFFFF;
    VIP_XPCTRL = 0;                     /* the VIP's drawing off: the CPU draws the frame buffers */
    for (i = 0; i < 256; i++) {         /* the column tables: the display's timing, every column the same */
        COLTAB_L[i] = 0x00FE;
        COLTAB_R[i] = 0x00FE;
    }
    brightness(0);
    VIP_REST = 0;
    VIP_FRMCYC = 0;                     /* (a game frame every display frame) */
    clear_pair(0);
    clear_pair(1);
    VIP_DPCTRL = 0x0302;                /* the display on (DISP), with refresh (RE) and sync (SYNCE) */
    VIP_INTENB = VIP_INT_FRAMESTART;    /* the frame start's interrupt (gfx_frame) */
}

/* gfx_title: the title screen */
void gfx_title(void)
{
    screen_reset();
    scr_mode = 1;
    mark(D_ALL);
}

/* gfx_game: the game screen, cleared */
void gfx_game(void)
{
    screen_reset();
    scr_mode = 2;
    msg[0] = 0;
    memset(hud, 0, sizeof hud);
    memset(face_hw, 0, sizeof face_hw);
    memset(fb, 0, VIEW_W * VIEW_H);
    memset(stereo_src, 255, sizeof stereo_src);
    cur_face = 0xFF;
    mark(D_ALL);
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
    mark(D_MSG);
}

/* gfx_hud: the status bar's lines (redrawn only if they changed) */
void gfx_hud(char lines[5][HUD_COLS + 1])
{
    if (scr_mode != 2 || !memcmp(hud, lines, sizeof hud)) return;
    memcpy(hud, lines, sizeof hud);
    mark(D_HUD);
}

/* gfx_face: face f (0..8): 25 Mega Drive tiles (5 x 5, column by column,
   the status bar's 16 colours) into face_hw, shaded on the screen's pixels
   (colour 0 black) */
void gfx_face(u8 f)
{
    const u8 *t, *hp = res(E_HUD_PAL);
    u8 lev[16];
    u32 x, y, i;
    if (scr_mode != 2 || f == cur_face || f > 8) return;
    cur_face = f;
    for (i = 0; i < 16; i++) lev[i] = md_level(be16(hp + 2 * i));
    t = res(E_FACES) + (u32)f * 25 * 32;
    for (x = 0; x < 40; x++)
        for (y = 0; y < 40; y++) {
            u8 v = md_px(t + 32 * ((x >> 3) * 5 + (y >> 3)), x & 7, y & 7);
            if (!(y & 7)) face_hw[x][y >> 3] = 0;
            if (v) face_hw[x][y >> 3] |= (u16)(shade_at(lev[v], FACE_X + x, HUD_Y + y) << (2 * (y & 7)));
        }
    mark(D_HUD);
}

/* gfx_box: show a box of h lines (y, the Game Boy Color's position, is ignored) */
void gfx_box(char box[16][24], u8 y, u8 h)
{
    (void)y;
    memcpy(box_txt, box, sizeof box_txt);
    box_h = h > 16 ? 16 : h;
    box_up = 1;
    mark(D_VIEW);                       /* (the view with the box over it) */
}

/* gfx_box_off: hide the box (the view is redrawn) */
void gfx_box_off(void)
{
    if (!box_up) return;
    box_up = 0;
    mark(D_VIEW);                       /* (the view again, without it) */
}

/* ------------------------------------------------------------ the weapon:
   the block's Mega Drive frames (as the SNES's ui.c shows them, in its
   screen's pixels: this view's) */
/* The block's weapon tables (offset at block +16, 8 bytes a table):
   0 the frames (10 bytes: first tile, -, palette, first sprite, count),
   1 the sprites (8 bytes: x, y, Mega Drive size, tile), 2 the palettes
   (16 colours), 3 the tiles. */
/* wp_table: weapon table k's offset in the block */
static u32 wp_table(u8 k) { return be32(blk + be32(blk + 16) + 8UL * k); }

/* gfx_weapon_off: no weapon (its columns cleared) */
void gfx_weapon_off(void)
{
    u32 x;
    if (wp_frame < 0 && wp_x1 == 0) return;
    wp_frame = -1;
    if (wp_x1) {
        for (x = wp_x0; x < wp_x1; x++) {
            memset(wp_pix[x], 0, sizeof wp_pix[x]);
            memset(wp_msk[x], 0, sizeof wp_msk[x]);
        }
        wp_x0 = wp_x1 = 0;
        mark(D_VIEW);
    }
}

/* the weapon's sprites as gfx_weapon reads them: their rectangles in the
   SNES's view pixels (x, y the top left; w, h in pixels) and first tiles */
static struct { s16 x, y; u8 w, h; u16 tile; } wsp[16];
static u8 wsp_n;                        /* how many */
static const u8 *wsp_tiles;             /* the frame's tiles (in the block) */

/* weapon_px: the weapon frame's colour (0..15, 0 none) at SNES view pixel
   (x, y): the last sprite covering it that has a colour there (a Mega
   Drive sprite's tiles go down each column first) */
static u8 weapon_px(int x, int y)
{
    int k;
    u8 v = 0;
    for (k = 0; k < wsp_n; k++) {
        int dx = x - wsp[k].x, dy = y - wsp[k].y, h8 = wsp[k].h >> 3;
        u8 c;
        if (dx < 0 || dy < 0 || dx >= wsp[k].w || dy >= wsp[k].h) continue;
        c = md_px(wsp_tiles + (u32)(wsp[k].tile + (dx >> 3) * h8 + (dy >> 3)) * 32, (u32)(dx & 7), (u32)(dy & 7));
        if (c) v = c;
    }
    return v;
}

/* gfx_weapon: weapon w's frame (2 w, + 1 firing) into wp_pix / wp_msk,
   each of its colours as a level shaded on the screen's pixels; wp_x0..,
   wp_r0.. the columns and halfwords it covers. The frame (the Mega
   Drive's sprites, placed in the SNES's 256 x 176 view, which is this
   view's size) is shown WP_NUM / WP_DEN times as big (nearest pixel), from
   the view's bottom middle, moved sideways into the view if it would go
   past an edge */
#define WP_NUM 3
#define WP_DEN 2
void gfx_weapon(u8 w, u8 firing)
{
    s8 id = (s8)(w * 2 + (firing ? 1 : 0));
    const u8 *f, *sp, *pp;
    u8 lev[16];
    u32 n, k, i;
    int bx0 = VW, bx1 = 0, by0 = VH, by1 = 0, X0, Y0, X1, Y1, X, Y, shift = 0;
    int xmin = VW, xmax = -1, ymin = VH, ymax = -1;
    if (scr_mode != 2 || w > 8) return;
    if (id == wp_frame) return;
    gfx_weapon_off();
    wp_frame = id;
    f = blk + wp_table(0) + (u32)id * 10;
    n = be16(f + 8);
    sp = blk + wp_table(1) + be16(f + 6) * 8UL;
    wsp_tiles = blk + wp_table(3) + (u32)be16(f) * 32;
    pp = blk + wp_table(2) + be16(f + 4) * 32UL;
    for (i = 0; i < 16; i++) lev[i] = md_level(be16(pp + 2 * i));
    wsp_n = 0;
    for (k = 0; k < n && k < 16; k++) {     /* the sprites, and the frame's box (bx0..bx1-1, by0..by1-1) */
        const u8 *s = sp + k * 8;
        int x = be16(s) & 0x1FF, y = (s16)be16(s + 2), sz = be16(s + 4);
        if (x >= 256) x -= 512;         /* 9-bit x */
        y += 16;                        /* (the SNES's view rows: the sprite's line y + 24, the view's first 8) */
        wsp[k].x = (s16)x;
        wsp[k].y = (s16)y;
        wsp[k].w = (u8)((((sz >> 2) & 3) + 1) * 8);    /* (Mega Drive size: width bits 2..3, height 0..1, in tiles - 1) */
        wsp[k].h = (u8)(((sz & 3) + 1) * 8);
        wsp[k].tile = be16(s + 6);
        if (x < bx0) bx0 = x;
        if (x + wsp[k].w > bx1) bx1 = x + wsp[k].w;
        if (y < by0) by0 = y;
        if (y + wsp[k].h > by1) by1 = y + wsp[k].h;
        wsp_n++;
    }
    if (!wsp_n) return;
    /* the box scaled from the view's bottom middle (VW / 2, VH) */
    X0 = VW / 2 - (VW / 2 - bx0) * WP_NUM / WP_DEN;
    X1 = X0 + (bx1 - bx0) * WP_NUM / WP_DEN;
    Y1 = VH - (VH - by1) * WP_NUM / WP_DEN;
    Y0 = Y1 - (by1 - by0) * WP_NUM / WP_DEN;
    if (X1 > VW) shift = VW - X1;       /* (into the view sideways) */
    if (X0 + shift < 0) shift = -X0;
    for (Y = Y0 < 0 ? 0 : Y0; Y < Y1 && Y < VH; Y++)
        for (X = X0; X < X1; X++) {
            int vx = X + shift;
            u8 v;
            u16 bit;
            if (vx < 0 || vx >= VW) continue;
            /* (the source pixel: nearest, counted from the box's corner) */
            v = weapon_px(bx0 + (X - X0) * WP_DEN / WP_NUM, by0 + (Y - Y0) * WP_DEN / WP_NUM);
            if (!v) continue;
            bit = (u16)(2 * (Y & 7));
            wp_msk[vx][Y >> 3] |= (u16)(3 << bit);
            wp_pix[vx][Y >> 3] = (u16)((wp_pix[vx][Y >> 3] & ~(3 << bit)) |
                                       (shade_at(lev[v], VIEW_X + vx, VIEW_Y + Y) << bit));
            if (vx < xmin) xmin = vx;
            if (vx > xmax) xmax = vx;
            if (Y < ymin) ymin = Y;
            if (Y > ymax) ymax = Y;
        }
    if (xmin <= xmax) {
        wp_x0 = (u16)xmin;
        wp_x1 = (u16)(xmax + 1);
        wp_r0 = (u16)(ymin >> 3);
        wp_r1 = (u16)((ymax >> 3) + 1);
    }
    mark(D_VIEW);
}

/* ------------------------------------------------------------ the picture */
/* gfx_flash: the damage flash for frames frames (gfx_frame counts it down) */
void gfx_flash(u8 frames) { flash_n = frames; }

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

/* gfx_backdrop: the backdrop's shading for each view row r: the ceiling's
   or floor's colour at the SNES's line 2 r (this view's screen row), as a
   level, as each parity's two pixels */
void gfx_backdrop(const u8 *floor_rgb, const u8 *ceil_rgb, u8 berserk)
{
    u16 fl = rgb15(floor_rgb, berserk), ce = rgb15(ceil_rgb, berserk);
    u32 r;
    for (r = 0; r < VIEW_H; r++) {
        u16 line = (u16)(2 * r);
        u8 l = level15(shade(line < 88 ? ce : fl, line));
        bg_nib[0][r] = NIB[0][l];
        bg_nib[1][r] = NIB[1][l];
    }
    mark(D_VIEW);
}

/* depth_of: a view column's stereo depth from the height in pixels of the
   nearest thing in it (4096 / its depth in world units, 64 a cell): 1
   (the screen's plane) at 128 or more (half a cell away), up to 8 for
   nothing at all */
static u8 depth_of(u16 h)
{
    if (h >= 128) return 1;
    return (u8)(1 + (((128 - h) * 7 + 64) >> 7));
}

/* stereo_map: stereo_src for eye e (0 left, 1 right) from col_depth: view
   column s, depth d, goes to screen columns 2 s + o and the next, o = -(d
   / 2) for the left eye, d - d / 2 for the right (the eyes d apart: d =
   1 near the screen, 8 far behind it); where two land on the same
   column the nearer wins (the later on a tie); a column none lands on
   shows its nearest neighbour's (left, or right if that is farther: the
   farther surface comes into view), or black at the picture's edges */
static void stereo_map(u32 e)
{
    u8 dep[VW], next[VW + 1], *src = stereo_src[e];
    int s, x;
    memset(src, 255, VW);
    memset(dep, 255, VW);
    for (s = 0; s < VIEW_W; s++) {
        u8 d = col_depth[s];
        int p = 2 * s + (e ? d - (d >> 1) : -(d >> 1)), k;
        for (k = 0; k < 2; k++) {
            x = p + k;
            if (x >= 0 && x < VW && d <= dep[x]) {
                src[x] = (u8)s;
                dep[x] = d;
            }
        }
    }
    next[VW] = 255;                     /* (next[x]: the first covered column from x on, 255 none) */
    for (x = VW - 1; x >= 0; x--) next[x] = src[x] != 255 ? (u8)x : next[x + 1];
    for (x = 0; x < VW; x++) {
        int l, r;
        if (src[x] != 255) continue;
        l = x - 1;
        r = next[x];
        if (l < 0 || src[l] == 255 || r == 255) continue;      /* (an edge: black) */
        if (dep[r] > dep[l]) l = r;
        src[x] = src[l];
        dep[x] = dep[l];
    }
}

/* gfx_picture: the picture view.c has just drawn in fb onto the screen:
   the slots' shading from the map's colours, each column's depth, the two
   eyes' column maps, then the view drawn and shown */
void gfx_picture(void)
{
    u32 i, t0 = time_100us();
    for (i = 0; i < VIEW_SLOTS; i++) {
        u8 l = level15(view_cgram[i]);
        slot_nib[0][VIEW_SLOT0 + i] = NIB[0][l];
        slot_nib[1][VIEW_SLOT0 + i] = NIB[1][l];
    }
    for (i = 0; i < VIEW_W; i++) col_depth[i] = depth_of(spr_h[i] > col_h[i] ? spr_h[i] : col_h[i]);
    stereo_map(0);
    stereo_map(1);
    mark(D_VIEW);
    present();
    prof_last[2] = time_100us() - t0;
}

/* a step or a quarter turn shown in VIEW_STEPS pictures (the camera in
   between: view.c's free camera, the SNES SuperFX build's way) */
#define VIEW_STEPS 4
static s16 lc_x, lc_z;                  /* the last picture's camera (world units; angle 256 a turn) */
static u8 lc_a, lc_map = 0xFF;

/* render: one picture by view.c (render_cam: from the camera set, else
   from S), its time noted */
static void render(u8 cam)
{
    u32 t0 = time_100us();
    if (cam) view_render_cam();
    else view_render();
    prof_last[1] = time_100us() - t0;
}

/* gfx_view: the view where the player now is. If that is one step or a
   quarter turn from the last picture (same map, no box), first show the
   VIEW_STEPS - 1 pictures in between. */
void gfx_view(void)
{
    s16 tx = (s16)S.px * 64 + 32, tz = (s16)S.pz * 64 + 32, dx, dz;
    u8 ta = (u8)((S.facing & 3) << 6), k, n = 1;
    s8 da;
    u32 t0 = time_100us();
    if (scr_mode != 2) return;
    if (lc_map == S.map && !box_up) {
        dx = tx - lc_x;
        dz = tz - lc_z;
        da = (s8)(u8)(ta - lc_a);
        if ((da == 0 && ((dx == 0 && (dz == 64 || dz == -64)) || (dz == 0 && (dx == 64 || dx == -64)))) ||
            (dx == 0 && dz == 0 && (da == 64 || da == -64)))
            for (k = 1; k < VIEW_STEPS; k++, n++) {     /* (drawn while the last picture waits for its frame) */
                view_camera(lc_x + dx / VIEW_STEPS * k, lc_z + dz / VIEW_STEPS * k, (u8)(lc_a + da / VIEW_STEPS * k));
                render(1);
                gfx_picture();
            }
    }
    lc_x = tx;
    lc_z = tz;
    lc_a = ta;
    lc_map = S.map;
    render(0);                          /* (view.c: from S, the player's own position) */
    gfx_picture();
    prof_last[0] = time_100us() - t0;
    prof_last[3] = n;
}
