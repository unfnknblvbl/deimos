/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the SNES: the player-facing side (megadrive/src/ui.c,
 * ported): title, input, messages, status bar, boxes of text (dialogs,
 * door codes, the notebook, the vendor, menus), the weapon, the 3D view.
 * The rules are the portable core's (core/rules.c).
 *
 * Controls (the options screen moves every action but Start to any button):
 *   D-pad              step forward / back, turn
 *   A                  use / attack what is in front of you
 *   B                  pass the turn
 *   Y                  use a medkit (smallest first)
 *   L / R              previous / next weapon
 *   X                  notebook
 *   Start              pause menu (resume, items, notebook, save, options, title)
 *
 * Screen (video.s): line 0..7 messages, 8..183 the 3D view, 184..223 the
 * status bar; boxes of text are sprites (gfx.s's text layer). (The SNES
 * builds' smaller view sits in the middle of lines 8..183 with a black
 * border round it; the weapon is raised to its bottom edge: WP_LIFT.)
 *
 * The 3DS and 32X ports build this file too (__3DS__ / __MARS__: their plat.c do
 * the machine's part).
 * (The GBC and GBA builds compile it as well, on their own machine layers.)
 *
 * How it fits: main() (at the end) starts the machine and runs the title,
 * then the game loop: read the pad, turn a button into one of the rules'
 * actions (core/rules.h: act_move, act_use, ...), then show the result:
 * the 3D view (view/view.c draws it; refresh_picture uploads it), the
 * status bar, the messages the rules queued (glue.c's msgq), and any box
 * the rules asked for (modal(): a dialog, the vendor, death, ...). The
 * variable ui says which screen or box has the input.
 *
 * Words used here:
 *   the "view"       the 3D picture in the middle of the screen
 *   a "box"          a window of text, drawn as sprites from txt_chars
 *                    (gfx.s's text layer), 24 x 16 characters at most
 *   the "HUD"        the status bar (BG1 map rows 22..26)
 *   "resources"      the SNES's own graphics made from the jar by
 *                    tools/snes_ui_res.py (font, faces, palettes, title),
 *                    in their own ROM bank (res())
 *   the "block"      the campaign block (campaign.h): the maps, and the
 *                    weapon pictures
 *
 * Licence: GPL-2.0-or-later.
 */
#include "../../core/rules.h"
#include "../../view/view.h"
#include "campaign.h"
#include "glue.h"
#include "video.h"
#include "gfx.h"
#include "save.h"
#include "banks.h"

/* a RAM buffer's address for the copy queue (vq_src): on the SNES the
   buffer's 16-bit address in bank $7E */
#if defined(__3DS__) || defined(__MARS__)
u32 plat_ramaddr(const void *p);
#define RAMADDR(p) plat_ramaddr(p)
#else
#define RAMADDR(p) ((u32)(u16)(p) | 0x7E0000UL)
#endif

void wait_vblank(void);                 /* hw.s: the next frame, then the pad */
void sound_init(void);                  /* sound.s */
void music_play(u8 id);
void music_stop(void);
#define MUS_TITLE 0                     /* the songs (tools/snes_sound.py's order) */
#define MUS_INTRO 1
#define MUS_END 2
extern u16 pad, pad_edge;               /* hw.s: buttons held; newly pressed */
extern volatile u16 frame_count;        /* hw.s: frames since start */

/* read by the tests (tools/snes_play.py) through the link map */
volatile u8 eng_turn_seq, eng_idle, eng_mode;   /* turns done; 1 while waiting for input; ui */

/* pad bits ($4218) */
#define PAD_B 0x8000
#define PAD_Y 0x4000
#define PAD_SELECT 0x2000
#define PAD_START 0x1000
#define PAD_UP 0x0800
#define PAD_DOWN 0x0400
#define PAD_LEFT 0x0200
#define PAD_RIGHT 0x0100
#define PAD_A 0x0080
#define PAD_X 0x0040
#define PAD_L 0x0020
#define PAD_R 0x0010

/* ------------------------------------------------------------ resources */
#define R_FONT 0
#define R_FACES 1
#define R_ICONS 2
#define R_HUD_PAL 3
#define R_DLG_PAL 4
#define R_TITLE_TILES 5
#define R_TITLE_MAP 6
#define R_TITLE_PAL 7
/* (entry 8 is the sound streams, read by sound.s) */

/* the resources (tools/snes_ui_res.py) start at the beginning of the bank
   named by the header's byte HEADER_AT + 7, with a directory: at
   12 + 8 i, entry i's offset (two little-endian words, low then high) and
   its size (a little-endian word). -> resource i's long ROM address */
static u32 res(u8 i)
{
    u32 base = (u32)rom_u8(HEADER_AT + 7) << 16;
    return base + rom_u16le(base + 12 + 8 * i) + ((u32)rom_u16le(base + 14 + 8 * i) << 16);
}

/* resource i's size in bytes */
static u16 res_size(u8 i)
{
    return rom_u16le(((u32)rom_u8(HEADER_AT + 7) << 16) + 16 + 8 * i);
}

/* the big-endian word at long ROM address a */
static u16 be16_rom(u32 a)
{
    return (u16)((rom_u8(a) << 8) | rom_u8(a + 1));
}

/* Mega Drive colour (9 bits in a word) -> SNES */
/* (MD: 0000 BBB0 GGG0 RRR0; each 3-bit level mapped to 5 bits by LEV5) */
static const u8 LEV5[8] = {0, 6, 10, 14, 17, 21, 25, 31};

static u16 md_to_snes(u16 c)
{
    return LEV5[(c >> 1) & 7] | ((u16)LEV5[(c >> 5) & 7] << 5) | ((u16)LEV5[(c >> 9) & 7] << 10);
}

static u16 pal_buf[16];                 /* load_pal's converted colours (queued from here) */

/* 16 Mega Drive colours (big-endian words at a) -> CGRAM from slot */
static void load_pal(u32 a, u8 slot)
{
    u8 i;
    while (vq_busy()) wait_vblank();        /* (the buffer may still be going up) */
    for (i = 0; i < 16; i++) pal_buf[i] = md_to_snes(be16_rom(a + 2 * i));
    vq_src = RAMADDR(pal_buf);
    vq_dst = slot;
    vq_len = 32;
    vq_mode = VQ_CGRAM;
    vq_add();                           /* sent at the next vblank */
}

/* n Mega Drive tiles at a -> VRAM (word address), through the buffer */
/* (256 tiles at a time: converted into TBUF by md_tiles, queued, and
   waited for before the buffer is used again; a 4bpp tile is 32 bytes,
   16 VRAM words) */
#define TBUF 0x6000                     /* 7F:6000, 8 KB */
static void load_tiles(u32 a, u16 n, u16 vram)
{
    while (n) {
        u16 k = n > 256 ? 256 : n;
        while (vq_busy()) wait_vblank();    /* the buffer is free */
        mt_src = a;
        mt_dst = TBUF;
        mt_n = k;
        md_tiles();                     /* convert (gfx.s) */
        vq_src = 0x7F0000UL + TBUF;
        vq_dst = vram;
        vq_len = k * 32;
        vq_mode = VQ_VRAM;
        vq_add();
        while (vq_busy()) wait_vblank();
        a += (u32)k * 32;
        vram += k * 16;
        n -= k;
    }
}

/* ------------------------------------------------------------ BG1's map */
/* BG1 (Mode 1, 4bpp) holds the message line (map row 31) and the status
   bar (rows 22..26); the rest is the view (Mode 7). A map entry: bits 0-9
   the tile, 10-12 the palette, 13 the priority (BG_PRIO). The map is kept
   in bgmap and sent whole when changed (bg_flush). */
#define VRAM_BG_TILES 0x4000            /* (VRAM word addresses, video.s) */
#define VRAM_BG1_MAP 0x5C00
#define VRAM_OBJ 0x6000
#define T_FONT 1                        /* tiles 1..96: characters 32..127 */
#define T_FACE 100                      /* 25: the face */
static u16 bgmap[32 * 32];              /* BG1's map (32 x 32 entries) */
static u8 bg_dirty;                     /* bgmap changed since the last bg_flush */

/* set map entry (x, y) */
static void bg_put(u8 x, u8 y, u16 e)
{
    bgmap[(u16)y * 32 + x] = e;
    bg_dirty = 1;
}

/* string s on BG1 from (x, y), with attr (palette, priority) or'ed in;
   lower case shown as capitals, anything else outside 33..127 as the
   opaque blank (font tile 95) */
static void put_text(u8 x, u8 y, const char *s, u16 attr)
{
    while (*s && x < 32) {
        u8 c = (u8)*s++;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c <= 32 || c > 127) c = 127;           /* opaque blank */
        bg_put(x++, y, attr | (T_FONT + (u8)(c - 32)));
    }
}

/* queue bgmap for VRAM if it changed */
static void bg_flush(void)
{
    if (!bg_dirty) return;
    bg_dirty = 0;
    vq_src = RAMADDR(bgmap);
    vq_dst = VRAM_BG1_MAP;
    vq_len = sizeof bgmap;
    vq_mode = VQ_VRAM;
    vq_add();
}

#define BG_PRIO 0x2000                  /* map entry: high priority (Mode 1: in front of sprites of priority 0-2) */
#if VIEW_H != 88
/* the smaller view (the SNES builds): the message line and the status bar
   kept off the top and bottom 8 lines, which many TVs hide (overscan):
   the message line at screen line 8 (map row 0, the scroll is -8), the
   status bar at lines 168..207 (rows 20..24), just under the view
   (video.s's mode_hdma switches to Mode 1 there) */
#define MSG_ROW 0                       /* the message line's map row */
#define HUD_ROW0 20                     /* the status bar's first map row */
#else
#define MSG_ROW 31                      /* the message line's map row (screen line 0, the scroll is -8) */
#define HUD_ROW0 22                     /* the status bar's first map row */
#endif

/* ------------------------------------------------------------ messages */
static u8 msg_timer;                    /* frames before the next message may replace this one */

/* s centred on the message line (at most 32 characters) */
static void show_message(const char *s)
{
    char line[33];
    u8 n = 0, i, x;
    while (s[n] && n < 32) n++;
    x = (32 - n) >> 1;
    for (i = 0; i < 32; i++) line[i] = ' ';
    for (i = 0; i < n; i++) line[x + i] = s[i];
    line[32] = 0;
    put_text(0, MSG_ROW, line, 0);
}

/* once a frame: the rules' status line becomes a message; a waiting
   message is shown when the last has had 45 frames (3/4 s) */
static void msg_tick(void)
{
    if (msg_timer) msg_timer--;
    if (status_new) {                   /* (core/rules.c set a new status line) */
        status_new = 0;
        if (status_msg[0]) say(status_msg);
    }
    if (msg_timer == 0 && msg_count) {
        show_message(msgq[msg_head]);
        msg_head = (msg_head + 1) & (MSGQ - 1);
        msg_count--;
        msg_timer = 45;
    }
}

/* drop the waiting messages */
static void msg_clear(void)
{
    msg_count = 0;
    msg_timer = 0;
}

/* ------------------------------------------------------------ status bar */
/* The status bar: the face (5 x 5 tiles at map columns 1..5) and five rows
   of text at columns 7..31. Its text is built in hud_new, then written to
   the map. */
static char hud_new[5][25];             /* the text, rows 1..5 at [row - 1], columns 7.. at [col - 7] */
static u8 cur_face = 0xFF, hurt_face;   /* the face shown (0xFF: none); frames of the hurt face */
u8 grin_frames;                         /* frames of the grin left (set by glue.c's sfx) */

/* hud_new to spaces */
static void hud_clear_rows(void)
{
    u8 r, c;
    for (r = 0; r < 5; r++)
        for (c = 0; c < 25; c++) hud_new[r][c] = ' ';
}

/* s into hud_new at row (1..5) and map column col (7..) */
static void hud_text(u8 row, u8 col, const char *s)
{
    while (*s && col < 32) {
        hud_new[row - 1][col - 7] = *s++;
        col++;
    }
}

/* the number n at (row, col) -> the column after it */
static u8 hud_num(u8 row, u8 col, u16 n)
{
    char *s = utoa10(n);
    hud_text(row, col, s);
    while (*s) {
        s++;
        col++;
    }
    return col;
}

/* show face f (the resource's faces: 25 tiles each, column by column):
   its tiles into T_FACE.., its map entries; nothing if already shown */
static void set_face(u8 f)
{
    u8 tx, ty;
    if (f == cur_face) return;
    cur_face = f;
    load_tiles(res(R_FACES) + (u32)f * 25 * 32, 25, VRAM_BG_TILES + T_FACE * 16);  /* (waits for the upload) */
    for (tx = 0; tx < 5; tx++)
        for (ty = 0; ty < 5; ty++) bg_put(1 + tx, HUD_ROW0 + ty, BG_PRIO | (T_FACE + tx * 5 + ty));
}

/* the face as the original picks it: the grin just after a pick-up, the
   grimace when hurt, else by health */
static void face_update(void)
{
    u8 hp = S.hp, mx = S.maxhp;
    if (hp && grin_frames) set_face(8);
    else if (hp && hurt_face) set_face(4);
    else set_face(hp <= mx / 4 ? 3 : hp <= mx / 3 ? 2 : hp <= mx / 2 ? 1 : 0);   /* 0 healthy .. 3 near death */
}

/* the status bar from the game state S: health, armour, weapon and ammo,
   level and XP, credits and keys, medkits and souls; then the face; the
   map queued for VRAM */
static void hud_update(void)
{
    u8 c, r;
    char line[26];
    hud_clear_rows();
    hud_text(1, 7, "HEALTH");
    c = hud_num(1, 14, S.hp);
    hud_text(1, c, "/");
    hud_num(1, c + 1, S.maxhp);
    hud_text(1, 21, "AR");
    c = hud_num(1, 24, S.armor);
    hud_text(1, c, "/");
    hud_num(1, c + 1, S.maxarmor);

    hud_text(2, 7, wpn_name[S.weapon]);
    if (wpn_use[S.weapon]) {
        hud_text(2, 21, "AMMO");
        hud_num(2, 26, S.ammo[wpn_ammo[S.weapon]]);
    }
    hud_text(3, 7, "LEVEL");
    hud_num(3, 13, S.level);
    hud_text(3, 17, "XP");
    c = hud_num(3, 20, G16(S.xp));
    hud_text(3, c, "/");
    hud_num(3, c + 1, G16(S.nextxp));

    hud_text(4, 7, "CREDITS");
    hud_num(4, 15, G16(S.credits));
    hud_text(4, 20, "KEYS");
    if (S.keys & 1) hud_text(4, 25, "G");  /* key bits: green, yellow, blue, red */
    if (S.keys & 2) hud_text(4, 26, "Y");
    if (S.keys & 4) hud_text(4, 27, "B");
    if (S.keys & 8) hud_text(4, 28, "R");

    hud_text(5, 7, "MEDKITS");          /* inventory 0, 1: the two medkit sizes; 2: souls */
    hud_num(5, 15, S.inventory[0]);
    hud_text(5, 17, "+");
    hud_num(5, 18, S.inventory[1]);
    if (S.inventory[2]) {
        hud_text(5, 21, "SOUL");
        hud_num(5, 26, S.inventory[2]);
    }
    line[25] = 0;
    for (r = 0; r < 5; r++) {
        for (c = 0; c < 25; c++) line[c] = hud_new[r][c];
        put_text(7, HUD_ROW0 + r, line, BG_PRIO);
    }
    face_update();
    bg_flush();                         /* queue the map */
}

/* BG1's map cleared; the status bar's rows filled with the opaque blank
   (so the bar is solid) */
static void hud_init(void)
{
    u8 x, y;
    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x++) bgmap[(u16)y * 32 + x] = 0;
    for (y = HUD_ROW0; y < HUD_ROW0 + 5; y++)
        for (x = 0; x < 32; x++) bgmap[(u16)y * 32 + x] = BG_PRIO | (T_FONT + 95);
    bg_dirty = 1;
    cur_face = 0xFF;
}

/* ------------------------------------------------------------ the weapon */
/* the Mega Drive's frames (the block's weapons: frames, sprites, palettes,
   tiles), each Mega Drive sprite (up to 32x32) shown as one 32x32 sprite */
#define WP_OAM 96                       /* the weapon's first sprite (up to 16: 96..111) */
#define WP_BUF 0x6000                   /* 7F:6000: 16 x 16 tiles */
/* WP_LIFT: lines the weapon is raised by, so that it stands on the view's
   bottom edge: the SNES shows the view at twice its size centred in the
   176 lines below the message line (video.s), so a view shorter than the
   full 88 rows (view.h's VIEW_H: the SNES builds' 72) has 88 - VIEW_H
   lines of border under it. 0 at the full size (every other port) */
#define WP_LIFT (88 - VIEW_H)
static s8 wp_cur = -1;                  /* the frame shown: weapon * 2 + firing; -1 none */
static u8 wp_n;                         /* its sprite count */

/* the block offset of weapon table k (0 frames, 1 sprites, 2 palettes,
   3 tiles): the block's long at 16 points at their directory of (u32
   offset, u32 size) pairs */
static u32 wp_table(u8 k)
{
    return blk_u32(blk_u32(16) + 8UL * k);
}

/* the weapon's sprites off screen (y 224; sprites 96..127) */
static void weapon_hide(void)
{
    u8 i;
    for (i = 0; i < 32; i++) oam[(WP_OAM + i) * 4 + 1] = 224;
    oam_dirty = 1;
}

/* the sprites' high table: sprite k's size bit (large) */
static void oam_large(u8 k, u8 large)
{
    u8 *h = &oam[512 + (k >> 2)], b = 2 << ((k & 3) * 2);
    if (large) *h |= b;
    else *h &= ~b;
}

/* show weapon w (0..8; more hides it), its firing frame if firing. The
   frame record (10 bytes in the frames table, at id * 10): u16 first
   tile, -, u16 palette, u16 first sprite, u16 sprites. A sprite record (8
   bytes): u16 x, y, size (MD: bits 2-3 width - 1, bits 0-1 height - 1,
   in 8-pixel cells), first tile (its tiles column by column). Each MD
   sprite becomes one 32x32 SNES sprite: its tiles are converted into a
   4 x 4 tile cell of WP_BUF (16 tiles wide), which then goes to the
   sprite tiles in VRAM. Nothing to do if that frame is already shown.
   Waits for the uploads. */
static void weapon_show(u8 w, u8 firing)
{
    s8 id = (s8)(w * 2 + (firing ? 1 : 0));
    u32 f, sp, tiles, at;
    u16 t0, n, k;
    if (w > 8) {
        weapon_hide();
        return;
    }
    if (id == wp_cur) return;
    wp_cur = id;
    f = wp_table(0) + (u16)id * 10;     /* the frame's record */
    t0 = blk_u16(f);
    n = blk_u16(f + 8);
    sp = wp_table(1) + blk_u16(f + 6) * 8UL;
    tiles = wp_table(3) + (u32)t0 * 32;
    load_pal(BLOCK_ADDR + wp_table(2) + blk_u16(f + 4) * 32UL, 240);    /* sprite palette 7 */
    while (vq_busy()) wait_vblank();    /* WP_BUF (= TBUF) is free */
    fill7f(WP_BUF, 8192);               /* empty tiles: see-through */
    for (k = 0; k < n && k < 16; k++) { /* per MD sprite: */
        u16 x = blk_u16(sp + k * 8), y = blk_u16(sp + k * 8 + 2), sz = blk_u16(sp + k * 8 + 4),
            tile = blk_u16(sp + k * 8 + 6);
        u8 w8 = ((sz >> 2) & 3) + 1, h8 = (sz & 3) + 1, cx, cy;
        u8 bx = (k & 3) * 4, by = (k >> 2) * 4;     /* its 4 x 4 cell in WP_BUF */
        for (cx = 0; cx < w8; cx++)
            for (cy = 0; cy < h8; cy++) {
                at = tiles + (u32)(tile + cx * h8 + cy) * 32;
                mt_src = BLOCK_ADDR + at;
                mt_dst = WP_BUF + ((u16)(by + cy) * 16 + bx + cx) * 32;
                mt_n = 1;
                md_tiles();             /* one tile into place */
            }
        {
            u8 *o = &oam[(WP_OAM + k) * 4];
#if WP_LIFT
            u16 sy = y + 8 + 16 - 1 - WP_LIFT;  /* (raised onto the smaller view's bottom edge) */
#else
            u16 sy = y + 8 + 16 - 1;    /* (the screen offset; SNES sprites show a line below their y) */
#endif
            o[0] = (u8)x;
            o[1] = sy > 223 ? 224 : (u8)sy;     /* below the screen: hidden */
            o[2] = (u8)((by * 16 + bx));        /* the tile (VRAM_OBJ's tile 0 = WP_BUF's start) */
            o[3] = 0x20 | (7 << 1);             /* priority 2, palette 7 */
            oam_large(WP_OAM + k, 1);           /* 32x32 */
            /* the high table's bit 0 of the pair: x's bit 8 */
            if (x & 0x100) oam[512 + ((WP_OAM + k) >> 2)] |= 1 << (((WP_OAM + k) & 3) * 2);
            else oam[512 + ((WP_OAM + k) >> 2)] &= ~(1 << (((WP_OAM + k) & 3) * 2));
        }
    }
    for (; k < 32; k++) oam[(WP_OAM + k) * 4 + 1] = 224;  /* the rest hidden */
    wp_n = (u8)n;
    vq_src = 0x7F0000UL + WP_BUF;
    vq_dst = VRAM_OBJ;
    vq_len = 8192;
    vq_mode = VQ_VRAM;
    vq_add();                           /* the tiles up */
    while (vq_busy()) wait_vblank();
    oam_dirty = 1;                      /* then the sprites */
}

/* ------------------------------------------------------------ the picture */
void gsu_init(void);
static u8 shown_map = 0xFF;             /* the map whose colours are in CGRAM (0xFF: none) */
static u16 cur_floor = 0xFFFF, cur_ceil = 0xFFFF;   /* the floor and ceiling in grad_hdma (BGR555) */

/* rgb888 (the berserker's red) -> BGR555 */
/* (with berserk: halfway to full red, green and blue halved) */
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

/* the floor and ceiling colours (from S, tinted while berserk) into the
   HDMA table if they changed, or always with force */
static void update_backdrop(u8 force)
{
    u16 f = rgb15(S.floor_rgb, S.berserk != 0), c = rgb15(S.ceil_rgb, S.berserk != 0);
    if (force || f != cur_floor || c != cur_ceil) {
        cur_floor = f;
        cur_ceil = c;
        view_gradient(f, c);
    }
}

#ifdef SUPERFX
/* the SuperFX build: a step or a quarter turn is shown in FX_STEPS
   pictures (the camera in between), and a picture goes up while the GSU
   draws the next (no wait) */
#define FX_STEPS 4
static s16 lc_x, lc_z;                  /* the last picture's camera (world units: 64 a cell) */
static u8 lc_a, lc_map = 0xFF;          /* its angle (256 a turn), its map */

/* draw and show the view where the player now is. If the move since the
   last picture was one step (64 units) or one quarter turn (64), the
   FX_STEPS - 1 cameras in between are drawn and shown first */
static void refresh_picture(void)
{
    s16 tx = (s16)S.px * 64 + 32, tz = (s16)S.pz * 64 + 32, dx, dz;
    u8 ta = (u8)((S.facing & 3) << 6), k;
    s8 da;
    if (lc_map == S.map) {
        dx = tx - lc_x;
        dz = tz - lc_z;
        da = (s8)(u8)(ta - lc_a);       /* the turn, -128..127 */
        if ((da == 0 && ((dx == 0 && (dz == 64 || dz == -64)) || (dz == 0 && (dx == 64 || dx == -64)))) ||
            (dx == 0 && dz == 0 && (da == 64 || da == -64)))
            for (k = 1; k < FX_STEPS; k++) {
                view_camera(lc_x + dx / FX_STEPS * k, lc_z + dz / FX_STEPS * k, (u8)(lc_a + da / FX_STEPS * k));
                view_render_cam();      /* the GSU draws it */
                view_upload();          /* (waits for the last picture's upload first) */
            }
    }
    lc_x = tx;
    lc_z = tz;
    lc_a = ta;
    lc_map = S.map;
    view_render();                      /* the final picture */
    if (shown_map != S.map) {           /* a new map: its colours */
        shown_map = S.map;
        while (view_busy()) wait_vblank();
        cgram_load();
    }
    view_upload();
}
#elif defined(__3DS__) || defined(__MARS__)
/* the 3DS: the GPU draws the view every frame; the camera glides to the
   player (r3d.c) */
static void refresh_picture(void)
{
    view_render();
    shown_map = S.map;
}
#else
/* the plain SNES build: draw the view into the frame buffer, queue it (and
   a new map's colours) and wait until it is in VRAM, since the next
   picture is drawn into the same buffer */
static void refresh_picture(void)
{
    view_render();                      /* view/view.c (draw.s for the pixels) */
    if (shown_map != S.map) {
        shown_map = S.map;
        cgram_load();
    }
    view_upload();
    while (view_busy()) wait_vblank();
}
#endif

/* ------------------------------------------------------------ boxes */
/* A box is the text layer (txt_chars, here called box): 16 rows of 24
   characters drawn into sprite tiles by gfx.s's txt_render and shown as
   96 16x16 sprites (8 rows of 12) from screen tile (BOX_X, box_y). Its
   text lines are split at '|' (the original's line break). */
#define BOX_X 4                         /* the box's left, in 8-pixel tiles */
#define BOX_Y 2                         /* its usual top, in tiles */
#define DLG_W 24                        /* characters a row */
#define DLG_H 16                        /* rows */
#define BOX_LINES 13                    /* text rows a page (rows 1..13) */
/* ui: who has the input */
#define UI_PLAY 0
#define UI_DIALOG 1
#define UI_PASSWORD 2
#define UI_DEAD 3
#define UI_DONE 4
#define UI_END 5
#define UI_STORE 6
#define UI_PAUSE 7
#define UI_NOTES 8
#define UI_ITEMS 9
#define UI_OPTIONS 10
#define UI_TITLE 11
#define MAX_NOTES 16                    /* strings a pager can hold */

static void next_map(void);

static u8 ui;                           /* UI_*: the screen or box in charge */
/* the pager (a box showing strings a page at a time): the string ids, their
   count, the one shown, the first line shown */
static u8 box_str[MAX_NOTES], box_n, box_i, box_line;
#define CODE_MAX 8                      /* a door code's digits at most */
static char code[CODE_MAX + 1];         /* the digits entered */
static u8 code_len, code_digit;         /* how many; the digit being chosen (0-9) */
#define box txt_chars
static u8 box_y = BOX_Y, box_h = DLG_H, box_up;     /* top (tiles), rows shown, 1 while shown */

/* the box's first box_h rows to spaces (opaque), the rest to 0 (see-through) */
static void box_clear(void)
{
    u8 r, c;
    for (r = 0; r < DLG_H; r++)
        for (c = 0; c < DLG_W; c++) box[r][c] = r < box_h ? ' ' : 0;
}

static const char *key_label(u8 tok);   /* (with the options, below) */

/* s at (row, col) up to its end, a '|' or the box's edge. A button token
   in s (a byte below 32: megadrive/tools/key_text.py puts one in the
   maps' strings where the original named a phone key) is written as the
   name of the button that does that action now (key_label), also cut at
   the edge. */
static void box_put(u8 row, u8 col, const char *s)
{
    while (*s && *s != '|' && col < DLG_W) {
        u8 c = (u8)*s++;
        if (c < 32) {                               /* a token: the button's name */
            const char *n = key_label(c);
            while (*n && col < DLG_W) box[row][col++] = *n++;
        } else
            box[row][col++] = c;
    }
}

/* show the box as it now is: its characters into tiles (txt_render), the
   tiles to VRAM's sprite area, sprites 0..95 placed (rows past box_h
   hidden). Waits until the tiles are up. */
static void box_show(void)
{
    u8 i, j;
    if (!box_up) {
        weapon_hide();                  /* (the box's tiles go where the weapon's were) */
        wp_cur = -1;
    }
    while (vq_busy()) wait_vblank();
    txt_render();                       /* the characters into tiles at $7F3000 (gfx.s) */
    vq_src = 0x7F3000UL;
    vq_dst = VRAM_OBJ;
    vq_len = 12288;                     /* 384 tiles */
    vq_mode = VQ_VRAM;
    vq_add();
    for (j = 0; j < 8; j++)             /* 8 rows of 12 sprites */
        for (i = 0; i < 12; i++) {
            u8 k = j * 12 + i, *o = &oam[k * 4];
            u16 t = (k & 7) * 2 + (k >> 3) * 32;    /* its first tile (as txt_render lays them) */
            o[0] = BOX_X * 8 + i * 16;
            o[1] = j * 2 < box_h ? box_y * 8 + j * 16 - 1 : 224;
            o[2] = (u8)t;
            o[3] = 0x30 | (6 << 1) | (u8)(t >> 8);      /* priority 3, palette 6 */
                                                        /* (bit 0: the tile number's bit 8) */
            oam_large(k, 0);            /* 16x16 */
            oam[512 + (k >> 2)] &= ~(1 << ((k & 3) * 2));   /* x bit 8 clear */
        }
    while (vq_busy()) wait_vblank();
    oam_dirty = 1;
    box_up = 1;
}

/* the box's sprites off screen */
static void box_hide(void)
{
    u8 k;
    for (k = 0; k < 96; k++) oam[k * 4 + 1] = 224;
    oam_dirty = 1;
    box_up = 0;
}

static char gen_text[48];               /* dialog_text's made-up string */

/* the text of dialog id: the map's string id (glue.c's level_string), or
   for DLG_CAPTURED "YOU CAPTURED|THE <name>!" */
static const char *dialog_text(u8 id)
{
    if (id == DLG_CAPTURED) {
        const char *a = "YOU CAPTURED|THE ", *b = def_name(captured_def);
        char *d = gen_text;
        while (*a) *d++ = *a++;
        while (*b && d < gen_text + 44) *d++ = *b++;
        *d++ = '!';
        *d = 0;
        return gen_text;
    }
    return level_string(id);
}

/* the lines of dialog id from line first into the box from row on, as
   many as fit (to row BOX_LINES) -> 1 if lines are left over */
static u8 box_string(u8 id, u8 first, u8 row)
{
    const char *s = dialog_text(id);
    u8 line = 0;
    while (*s) {
        if (line >= first) {
            if (row >= 1 + BOX_LINES) return 1;
            box_put(row++, 3, s);
        }
        while (*s && *s != '|') s++;    /* to the next line */
        if (*s == '|') s++;
        line++;
    }
    return 0;
}

/* s centred on the box's second-last row */
static void box_footer(const char *s)
{
    u8 n = 0;
    const char *p = s;
    while (*p++) n++;
    box_put(box_h - 2, (DLG_W - n) >> 1, s);
}

/* the pager's current page, with "A:MORE" if anything follows */
static void pager_draw(void)
{
    u8 more;
    box_clear();
    more = box_string(box_str[box_i], box_line, 1);
    box_footer(more || box_i + 1 < box_n ? "A:MORE" : "A:OK");
    box_show();
}

/* open a pager of the n strings ids, with ui = mode (UI_DIALOG, UI_NOTES) */
static void open_pager(u8 mode, const u8 *ids, u8 n)
{
    u8 k;
    for (k = 0; k < n; k++) box_str[k] = ids[k];
    box_n = n;
    box_i = 0;
    box_line = 0;
    ui = mode;
    sfx(SFX_DIALOG);
    pager_draw();
}

/* A in a pager: the next page, or the next string -> 1 when all were shown */
static u8 pager_next(void)
{
    u8 more;
    box_clear();
    more = box_string(box_str[box_i], box_line, 1);
    if (more) box_line += BOX_LINES;
    else {
        box_i++;
        box_line = 0;
        if (box_i >= box_n) return 1;
    }
    sfx(SFX_BLIP);
    pager_draw();
    return 0;
}

/* the door code box: the rules' prompt (pw_prompt), the digits so far,
   the digit being chosen, '_' for the rest */
static void password_draw(void)
{
    u8 k;
    char shown[CODE_MAX + 1];
    box_clear();
    box_string(pw_prompt, 0, 1);
    for (k = 0; k < CODE_MAX; k++) shown[k] = k < code_len ? code[k] : (k == code_len ? '0' + code_digit : '_');
    shown[CODE_MAX] = 0;
    box_put(10, 8, "> ");
    box_put(10, 10, shown);
    box_put(12, 2, "UP/DOWN DIGIT  RIGHT ADD");
    box_put(13, 1, "LEFT DEL  A:OK  B:CANCEL");
    box_show();
}

/* the rules want a door code (pw_want): open the code box */
static void open_password(void)
{
    code_len = 0;
    code_digit = 0;
    ui = UI_PASSWORD;
    password_draw();
}

/* a box with a centred title (row 5), two lines (rows 8, 9) and a footer */
static void screen_box(const char *title, const char *l1, const char *l2, const char *foot)
{
    u8 n = 0;
    while (title[n]) n++;
    box_clear();
    box_put(5, (DLG_W - n) >> 1, title);
    box_put(8, 2, l1);
    box_put(9, 2, l2);
    box_footer(foot);
    box_show();
}

/* the map's enemies killed -> the count; *total = the map's enemies */
static u8 count_kills(u8 *total)
{
    u16 i;
    u8 k = 0, t = 0;
    for (i = 0; i < n_map_ents; i++)
        if (S.e_def[i] != NONE_B && def_etype(S.e_def[i]) == ET_ENEMY) {
            t++;
            if (!(S.e_flags[i] & F_ALIVE)) k++;
        }
    *total = t;
    return k;
}

static char line1[25], line2[25], title_buf[32];   /* the end screens' lines */

/* append a at d -> the new end (NUL-terminated) */
static char *cat(char *d, const char *a)
{
    while (*a) *d++ = *a++;
    *d = 0;
    return d;
}

/* line1 = "KILLS k/n", line2 = "LEVEL l  CREDITS c" */
static void stats_lines(void)
{
    u8 tot, k = count_kills(&tot);
    char *s = cat(line1, "KILLS ");
    s = cat(s, utoa10(k));
    s = cat(s, "/");
    cat(s, utoa10(tot));
    s = cat(line2, "LEVEL ");
    s = cat(s, utoa10(S.level));
    s = cat(s, "  CREDITS ");
    cat(s, utoa10(G16(S.credits)));
}

/* after a turn that ended in death or through an exit: the death box, the
   end of the game, a map's completion box, or on to the next map */
static void end_screens(void)
{
    if (S.dead) {
        music_stop();
        ui = UI_DEAD;
        screen_box("YOU HAVE DIED", "THE DEMONS GOT YOU.", "", "A:TRY AGAIN START:TITLE");
    } else {                                    /* through an exit door */
        char *s;
        ui = UI_DONE;
        stats_lines();
        if (S.exit[3] == 0xFF) {                /* the end (no next map) */
            ui = UI_END;
            screen_box("THE END", line1, line2, "START:TITLE");
            sfx(SFX_COMPLETE);
            music_play(MUS_END);
            return;
        }
        if (!(S.exit[0] & 0x80)) {              /* not a completed sector: straight on */
            next_map();
            return;
        }
        s = cat(title_buf, map_title(S.map));
        cat(s, " DONE");
        screen_box(title_buf, line1, line2, "A:CONTINUE");
        sfx(SFX_COMPLETE);
        music_play(MUS_END);
    }
}

static void store_open(void);

/* show what an action of the rules did: the firing frame (8 frames), the
   damage flash, the floor and ceiling, the new view, the status bar; then
   the death or exit screens, or the box the rules want (a dialog, a door
   code, the vendor), else back to play */
static void after_action(void)
{
    if (player_fired) {                 /* (flags set by core/rules.c) */
        u8 i;
        player_fired = 0;
        weapon_show(S.weapon, 1);
        for (i = 0; i < 8; i++) wait_vblank();
    }
    if (player_hurt) {
        player_hurt = 0;
        flash(6);                       /* red for 6 frames */
        hurt_face = 1;
    } else
        hurt_face = 0;
    update_backdrop(0);
    refresh_picture();
    hud_update();
    if (S.dead || S.exit_set) {
        weapon_hide();
        end_screens();
        return;
    }
    weapon_show(S.weapon, 0);
    if (dialog_str != 0xFF) open_pager(UI_DIALOG, &dialog_str, 1);
    else if (pw_want != 0xFF) open_password();
    else if (S.store != 0xFF) store_open();
    else ui = UI_PLAY;
}

/* close the box: back to play, the weapon shown again */
static void close_box(void)
{
    ui = UI_PLAY;
    box_hide();
    weapon_show(S.weapon, 0);
}

/* ------------------------------------------------------------ input */
static u16 cur_pad, prev_pad;           /* the buttons now and at the last frame */

/* wait for a button to go down, a frame at a time, doing each frame's
   work meanwhile (the grin's count, the messages, BG1's map) -> the
   buttons newly pressed. eng_idle is 1 while it waits (for the tests). */
static u16 next_edge(void)
{
    for (;;) {
        u16 edge;
        eng_idle = 1;
        eng_mode = ui;
        wait_vblank();
        if (grin_frames) grin_frames--;
        msg_tick();
        bg_flush();
        cur_pad = pad;
        edge = cur_pad & ~prev_pad;
        prev_pad = cur_pad;
        if (ui == UI_PLAY && cur_face == 8 && !grin_frames) {   /* the grin is over */
            face_update();
            bg_flush();
        }
        if (edge) {
            eng_idle = 0;
            return edge;
        }
    }
}

/* ---- menus ---- */
/* a menu box: the title, n items from row row0 with '>' at cur, a footer */
static void menu_draw(const char *title, const char * const *items, u8 n, u8 cur, u8 row0, const char *foot)
{
    u8 i, len = 0;
    while (title[len]) len++;
    box_clear();
    box_put(1, (DLG_W - len) >> 1, title);
    for (i = 0; i < n; i++) {
        box_put(row0 + i, 2, cur == i ? ">" : " ");
        box_put(row0 + i, 4, items[i]);
    }
    if (foot) box_footer(foot);
    box_show();
}

/* ---- options: the button of every action ---- */
#define N_BUTTONS 7                     /* (as save.c's) */
/* button index (cfg.key's values) -> its pad bit, its name */
static const u16 BUTTON_BITS[N_BUTTONS] = {PAD_A, PAD_B, PAD_X, PAD_Y, PAD_L, PAD_R, PAD_SELECT};
#ifdef __MARS__
/* (the 32X: the Mega Drive pad's buttons that m68k/main.c reads as these:
   A, B, Y as X, C as Y, X as L, Z as R, Mode as SELECT) */
static const char * const BUTTON_NAMES[N_BUTTONS] = {"A", "B", "Y", "C", "X", "Z", "MODE"};
#else
static const char * const BUTTON_NAMES[N_BUTTONS] = {"A", "B", "X", "Y", "L", "R", "SELECT"};
#endif
static const char * const ACT_NAMES[N_ACTIONS] = {"USE/ATTACK", "PASS TURN", "MEDKIT", "PREV WEAPON",
                                                  "NEXT WEAPON", "NOTEBOOK"};

/* the button tokens in the maps' strings (megadrive/tools/key_text.py):
   KEY_ACT + an action (ACT_*), or KEY_MENU for the pause menu's button */
#define KEY_ACT 1
#define KEY_MENU 16

/* the name of the button token tok stands for (box_put): the button its
   action is on now (cfg.key, as the options screen set it), START for the
   pause menu, "?" for a token this game has no action for (the Mega
   Drive's full detail) */
static const char *key_label(u8 tok)
{
    u8 a = tok - KEY_ACT;                       /* the action (ACT_*) */
    if (tok == KEY_MENU) return "START";
    if (a >= N_ACTIONS) return "?";             /* (also tok 0, which wraps round) */
    return BUTTON_NAMES[cfg.key[a]];
}

/* the pad bit of the button action a (ACT_*) is on */
static u16 action_bits(u8 a)
{
    return BUTTON_BITS[cfg.key[a]];
}

/* the options box: each action and its button, then DEFAULTS and DONE */
static void options_draw(u8 cur)
{
    static char lines[8][21];
    const char *items[8];
    u8 i, k;
    for (i = 0; i < 8; i++) {
        const char *a, *b;
        char *d = lines[i];
        if (i < N_ACTIONS) {
            a = ACT_NAMES[i];
            b = BUTTON_NAMES[cfg.key[i]];
        } else {
            a = i == N_ACTIONS ? "DEFAULTS" : "DONE";
            b = "";
        }
        for (k = 0; k < 20; k++) d[k] = ' ';
        d[20] = 0;
        for (k = 0; a[k]; k++) d[k] = a[k];
        for (k = 0; b[k]; k++) d[13 + k] = b[k];
        items[i] = d;
    }
    menu_draw("OPTIONS", items, 8, cur, 3, cur < N_ACTIONS ? "PRESS A BUTTON TO SET" : "A:CHANGE  START:DONE");
}

/* the options screen (from the title or the pause menu): Up/Down choose a
   line; on an action's line any of the seven buttons takes it (the action
   that had that button gets this one's old button, so each stays on its
   own button); DEFAULTS; DONE or Start saves the settings and returns.
   eng_turn_seq counts each input, for the tests. */
static void options_screen(void)
{
    u8 cur = 0, saved_ui = ui, b;
    ui = UI_OPTIONS;
    eng_turn_seq++;
    options_draw(cur);
    for (;;) {
        u16 e = next_edge();
        if (e & PAD_START) break;
        if (e & PAD_UP) cur = cur ? cur - 1 : 7;
        else if (e & PAD_DOWN) cur = cur < 7 ? cur + 1 : 0;
        else if (cur < N_ACTIONS) {
            for (b = 0; b < N_BUTTONS; b++)
                if (e & BUTTON_BITS[b]) {
                    u8 j;
                    for (j = 0; j < N_ACTIONS; j++)          /* the action that had it gets ours */
                        if (cfg.key[j] == b) cfg.key[j] = cfg.key[cur];
                    cfg.key[cur] = b;
                    break;
                }
            if (b == N_BUTTONS) {       /* not one of the seven (the d-pad's left / right) */
                eng_turn_seq++;
                continue;
            }
        } else if (cur == N_ACTIONS && (e & PAD_A)) settings_defaults();
        else if (cur == N_ACTIONS + 1 && (e & PAD_A)) break;
        else {
            eng_turn_seq++;
            continue;
        }
        sfx(SFX_BLIP);
        options_draw(cur);
        eng_turn_seq++;
    }
    eng_turn_seq++;
    settings_save();                    /* into the battery RAM (save.c) */
    sfx(SFX_SELECT);
    box_hide();
    ui = saved_ui;
}

/* the font for the text layer (bank $7F) */
/* (96 tiles, characters 32..127, at $7FD000: gfx.s's FONT) */
static void text_font(void)
{
    mt_src = res(R_FONT);
    mt_dst = 0xD000;
    mt_n = 96;
    md_tiles();                         /* convert from the MD format */
    fill7f(0xD000, 32);                     /* the space: see through (txt_render's 0) */
}

/* ---- title ---- */
/* the title picture (Mode 1, BG1 tiles from VRAM 0), made from the MD
   title's tiles, palette and map. The screen is off meanwhile, and the
   copies are made at once (vq_flush), not in vblanks. */
static void show_title(void)
{
    u8 x, y;
    u32 m = res(R_TITLE_MAP);
    screen_off();
    box_hide();
    weapon_hide();
    wp_cur = -1;
    video_init();
    video_full(0);                      /* Mode 1, BG1's tiles at VRAM 0 */
    vq_flush();
    load_tiles(res(R_TITLE_TILES), res_size(R_TITLE_TILES) / 32, 0x0000);
    load_pal(res(R_TITLE_PAL), 0);
    /* the resource's map (28 rows of 32 big-endian MD entries) to SNES
       entries. MD: bit 15 priority, 13-14
       palette, 12 vertical flip, 11 horizontal flip, 0-10 tile. SNES: 15
       vertical flip, 14 horizontal flip, 13 priority, 10-12 palette, 0-9
       tile. Rows 28..31 are empty. */
    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x++) {
            u16 e = 0;
            if (y < 28) {
                u16 d = be16_rom(m + ((u16)y * 32 + x) * 2);
                e = (d & 0x7FF) | ((d >> 13 & 3) << 10) | (d & 0x8000 ? BG_PRIO : 0) | (d & 0x800 ? 0x4000 : 0) |
                    (d & 0x1000 ? 0x8000 : 0);
            }
            bgmap[(u16)y * 32 + x] = e;
        }
    bg_dirty = 1;
    bg_flush();
    load_pal(res(R_DLG_PAL), 224);      /* the boxes' palette (sprite palette 6) */
    vq_flush();
    screen_on();
}

/* the title menu's box top (tiles): at the bottom of the screen; on the
   SNES builds (the smaller view) ending at line 214, above the 8 lines a
   TV's overscan may hide */
#if VIEW_H != 88
#define TITLE_BOX_Y 21
#else
#define TITLE_BOX_Y 23
#endif

/* the title: wait for A or Start, then a menu (CONTINUE if a game is
   saved, NEW GAME, OPTIONS) in a short box at the bottom -> 1 continue,
   0 a new game */
static u8 title_screen(void)
{
    static const char * const ITEMS_C[3] = {"CONTINUE", "NEW GAME", "OPTIONS"};
    const char * const *items;
    u8 n, cur = 0, has_save;
    ui = UI_TITLE;
    show_title();
    music_play(MUS_TITLE);
    box_y = TITLE_BOX_Y;                /* the menu's box: 5 rows at the bottom */
    box_h = 5;
    for (;;) {                          /* (next_edge would also run the message line) */
        u16 e;
        eng_idle = 1;
        eng_mode = ui;
        wait_vblank();
        cur_pad = pad;
        e = cur_pad & ~prev_pad;
        prev_pad = cur_pad;
        if (e & (PAD_START | PAD_A)) break;
    }
    eng_idle = 0;
    eng_turn_seq++;
    sfx(SFX_SELECT);
    has_save = game_saved();
    items = has_save ? ITEMS_C : ITEMS_C + 1;
    n = has_save ? 3 : 2;
    menu_draw("", items, n, cur, 1, 0);
    for (;;) {
        u16 e = next_edge();
        if (e & PAD_UP) cur = cur ? cur - 1 : n - 1;
        else if (e & PAD_DOWN) cur = cur + 1 < n ? cur + 1 : 0;
        else if (e & (PAD_A | PAD_START)) {
            const char *it = items[cur];
            if (it[0] == 'O') {         /* OPTIONS: the full-size box, then back */
                box_hide();
                box_y = BOX_Y;
                box_h = DLG_H;
                options_screen();
                box_y = TITLE_BOX_Y;
                box_h = 5;
                ui = UI_TITLE;
            } else {
                eng_turn_seq++;
                box_hide();
                box_y = BOX_Y;
                box_h = DLG_H;
                sfx(SFX_SELECT);
                return it[0] == 'C';
            }
        }
        menu_draw("", items, n, cur, 1, 0);
        eng_turn_seq++;
    }
}

/* the game screen: the view, the status bar, the font */
/* (the screen off; BG1's font tiles, the status bar's and the boxes'
   palettes, BG1's map, the floor and ceiling; all sent at once) */
static void game_screen(void)
{
    screen_off();
    video_init();
    load_tiles(res(R_FONT), 96, VRAM_BG_TILES + T_FONT * 16);
    load_pal(res(R_HUD_PAL), 0);
    load_pal(res(R_DLG_PAL), 224);
    hud_init();
    bg_flush();
    shown_map = 0xFF;
    update_backdrop(1);
    vq_flush();
    video_game();                       /* (after the uploads: its colour HDMA must not
                                           write CGRAM's address while the palettes' DMA runs) */
}

/* into play with message msg: the view and the status bar shown */
static void start_play(const char *msg)
{
    msg_clear();
    say(msg);
    ui = UI_PLAY;
    after_action();
}

/* a map has begun: the game is saved (each map's start is a save point)
   and its title is the first message */
static void map_started(void)
{
    game_save();
    update_backdrop(1);
    start_play(map_title(S.map));
}

/* a new game from the first map */
static void new_game(void)
{
    music_play(MUS_INTRO);
    game_init();
    map_started();
}

/* through the exit to the next map (S.exit[3]: its number) */
static void next_map(void)
{
    box_hide();
    map_change(S.exit[3]);              /* the rules load it */
    music_play(MUS_INTRO);
    map_started();
}

/* after death: the saved game (the map's start), or a new game */
static void try_again(void)
{
    if (!game_load()) game_init();
    update_backdrop(1);
    start_play(map_title(S.map));
}

/* ---- the vendor ---- */
/* STORE_LINES (core): per vendor, 16 lines of (name tag, price); a tag 0
   ends the list. STORE_NAMES: the tag's name ("" for tags not sold). */
static const char * const STORE_NAMES[17] = {
    "", "", "SM MEDKIT", "LG MEDKIT", "SOUL SPHERE", "BERSERKER", "", "10 HAL. CANS", "10 BULLETS",
    "10 SHELLS", "10 CELLS", "3 ROCKETS", "", "+1 ACCURACY", "+1 AGILITY", "+1 STRENGTH", "+1 DEFENSE"};
static u8 store_cur, store_n;           /* the line chosen; the vendor's lines */
static const char *store_note;          /* the last purchase's result (the footer), or 0 */

/* the vendor's box: the credits, each line's name and price */
static void store_draw(void)
{
    static char lines[16][21];
    const char *items[16];
    const u8 *t = STORE_LINES + S.store * 32;
    char *d;
    u8 i, k;
    for (i = 0; i < store_n; i++) {
        const char *nm = STORE_NAMES[t[i * 2]];
        char *p;
        d = lines[i];
        for (k = 0; k < 20; k++) d[k] = ' ';
        d[20] = 0;
        for (k = 0; nm[k]; k++) d[k] = nm[k];
        p = utoa10(t[i * 2 + 1]);
        k = 17;
        while (*p) d[k++] = *p++;
        items[i] = d;
    }
    d = cat(title_buf, "CREDITS ");
    cat(d, utoa10(G16(S.credits)));
    menu_draw(title_buf, items, store_n, store_cur, 2, store_note ? store_note : "A:BUY  B:LEAVE");
}

/* the rules opened a vendor (S.store): its box */
static void store_open(void)
{
    const u8 *t = STORE_LINES + S.store * 32;
    store_n = 0;
    while (store_n < 16 && t[store_n * 2]) store_n++;
    store_cur = 0;
    store_note = 0;
    ui = UI_STORE;
    sfx(SFX_DIALOG);
    store_draw();
}

/* a button in the vendor's box: B / Start leaves (the rules are told,
   then the turn goes on), Up / Down choose, A buys */
static void store_input(u16 e)
{
    static const char * const RESULT[3] = {"BOUGHT", "NOT ENOUGH CREDITS", "YOU HAVE TOO MANY"};
    store_note = 0;
    if (e & (PAD_B | PAD_START)) {
        close_box();
        store_close();
        after_action();
        return;
    }
    if (e & PAD_UP) store_cur = store_cur ? store_cur - 1 : store_n - 1;
    else if (e & PAD_DOWN) store_cur = store_cur + 1 < store_n ? store_cur + 1 : 0;
    else if (e & PAD_A) {
        u8 r = store_buy(store_cur);    /* 0 bought, 1 too few credits, 2 too many */
        store_note = RESULT[r];
        sfx(r ? SFX_DENIED : SFX_PICKUP);
        hud_update();
        store_draw();
        return;
    }
    sfx(SFX_BLIP);
    store_draw();
}

/* ---- items: the Berserker and the Dog Collar ---- */
static u8 items_cur;                    /* 0 the Berserker, 1 the Dog Collar */

/* the items box: each item and how many (S.inventory[3], [4]) */
static void items_draw(void)
{
    static char lines[2][21];
    const char *items[2];
    static const char * const NM[2] = {"BERSERKER", "DOG COLLAR"};
    u8 i, k;
    for (i = 0; i < 2; i++) {
        char *d = lines[i], *p = utoa10(S.inventory[3 + i]);
        for (k = 0; k < 20; k++) d[k] = ' ';
        d[20] = 0;
        for (k = 0; NM[i][k]; k++) d[k] = NM[i][k];
        k = 15;
        while (*p) d[k++] = *p++;
        items[i] = d;
    }
    menu_draw("ITEMS", items, 2, items_cur, 4, "A:USE  B:BACK");
}

/* ---- pause menu ---- */
static const char * const PAUSE_ITEMS[6] = {"RESUME", "ITEMS", "NOTEBOOK", "SAVE GAME", "OPTIONS",
                                            "QUIT TO TITLE"};
static u8 pause_cur;                    /* the line chosen */

/* the pause menu's box */
static void pause_draw(void)
{
    u8 i;
    box_clear();
    box_put(1, (DLG_W - 6) >> 1, "PAUSED");
    for (i = 0; i < 6; i++) {
        box_put(3 + i, 2, pause_cur == i ? ">" : " ");
        box_put(3 + i, 4, PAUSE_ITEMS[i]);
    }
    box_put(11, (DLG_W - 10) >> 1, "MADE BY AI");
    box_put(12, (DLG_W - 23) >> 1, "DIRECTED BY UNFNKNBLVBL");
    box_footer("A:SELECT  START:RESUME");
    box_show();
}

/* a button in the pause menu -> 1 to quit to the title */
static u8 pause_menu(u16 e)
{
    if (e & (PAD_START | PAD_B)) {
        close_box();
        return 0;
    }
    if (e & PAD_UP) pause_cur = pause_cur ? pause_cur - 1 : 5;
    else if (e & PAD_DOWN) pause_cur = pause_cur < 5 ? pause_cur + 1 : 0;
    else if (e & PAD_A) {
        switch (pause_cur) {
        case 0:
            close_box();
            return 0;
        case 1:
            ui = UI_ITEMS;
            items_cur = 0;
            sfx(SFX_BLIP);
            items_draw();
            return 0;
        case 2:
            if (S.n_notes) open_pager(UI_NOTES, S.notes, S.n_notes);
            else {
                close_box();
                say("NO NOTES YET");
            }
            return 0;
        case 3:
            game_save();
            close_box();
            say("GAME SAVED");
            sfx(SFX_SELECT);
            return 0;
        case 4:
            box_hide();
            options_screen();
            ui = UI_PAUSE;
            pause_draw();
            return 0;
        default:
            box_hide();
            return 1;
        }
    }
    sfx(SFX_BLIP);
    pause_draw();
    return 0;
}

/* the newly pressed buttons include action a's button */
#define ACT(a) (edge & action_bits(a))

/* one input: given to the box or screen in charge (ui), or in play turned
   into a rules action (then after_action shows the result) -> 1 to go
   back to the title */
static u8 handle(u16 edge)
{
    msg_clear();                        /* a new input: the waiting messages go */
    switch (ui) {
    case UI_DEAD:
        if (edge & PAD_START) {
            box_hide();
            return 1;
        }
        if (edge & PAD_A) {
            box_hide();
            try_again();
        }
        return 0;
    case UI_DONE:
        if (edge & PAD_A) next_map();
        return 0;
    case UI_END:
        if (edge & PAD_START) {
            box_hide();
            return 1;
        }
        return 0;
    case UI_STORE:
        store_input(edge);
        return 0;
    case UI_ITEMS:
        if (edge & (PAD_B | PAD_START)) {
            close_box();
            return 0;
        }
        if (edge & (PAD_UP | PAD_DOWN)) {
            items_cur ^= 1;
            sfx(SFX_BLIP);
            items_draw();
            return 0;
        }
        if (edge & PAD_A) {
            box_hide();
            if (use_item(3 + items_cur)) sfx(SFX_ITEM);     /* inventory 3, 4 */
            after_action();
        }
        return 0;
    case UI_PAUSE:
        return pause_menu(edge);
    case UI_NOTES:
        if (((edge & PAD_A) && pager_next()) || (edge & (PAD_B | PAD_START))) close_box();
        return 0;
    case UI_DIALOG:
        if ((edge & PAD_A) && pager_next()) {   /* the dialog's last page read */
            box_hide();
            resume(0);                  /* the rules carry on */
            after_action();
        }
        return 0;
    case UI_PASSWORD:                   /* Up / Down the digit, Right adds it, Left takes one back */
        if (edge & PAD_UP) code_digit = (code_digit + 1) % 10;
        if (edge & PAD_DOWN) code_digit = (code_digit + 9) % 10;
        if ((edge & PAD_RIGHT) && code_len < CODE_MAX - 1) {
            code[code_len++] = '0' + code_digit;
            code_digit = 0;
        }
        if ((edge & PAD_LEFT) && code_len) code_digit = code[--code_len] - '0';
        if (edge & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)) sfx(SFX_BLIP);
        if (edge & PAD_A) {             /* the digit being chosen is the last */
            code[code_len] = '0' + code_digit;
            code[code_len + 1] = 0;
            box_hide();
            resume(code);               /* the rules check the code */
            after_action();
            return 0;
        }
        if (edge & PAD_B) {
            box_hide();
            resume(0);                  /* cancelled */
            after_action();
            return 0;
        }
        password_draw();
        return 0;
    }
    /* UI_PLAY */
    if (edge & PAD_START) {
        ui = UI_PAUSE;
        pause_cur = 0;
        sfx(SFX_DIALOG);
        pause_draw();
        return 0;
    }
    if (ACT(ACT_NOTES)) {
        if (S.n_notes) open_pager(UI_NOTES, S.notes, S.n_notes);
        else say("NO NOTES YET");
        return 0;
    }
    /* the rules' actions (core/rules.c): one turn each */
    if (edge & PAD_UP) act_move(0);
    else if (edge & PAD_DOWN) act_move(1);
    else if (edge & PAD_LEFT) act_turn(-1);
    else if (edge & PAD_RIGHT) act_turn(1);
    else if (ACT(ACT_USE)) act_use();
    else if (ACT(ACT_PASS)) act_pass();
    else if (ACT(ACT_MEDKIT)) act_health_item();
    else if (ACT(ACT_PREV)) act_cycle_weapon(-1);
    else if (ACT(ACT_NEXT)) act_cycle_weapon(1);
    else return 0;                      /* no button that does anything */
    after_action();
    return 0;
}

/* the program (crt0.s calls it): the machine set up once, then for ever:
   the title, the game screen, a loaded or new game, and the input loop
   until the player quits to the title */
int main(void)
{
    gfx_init();                         /* sprites hidden, the upload queue */
#ifdef SUPERFX
    gsu_init();                         /* the GSU and its work RAM routines */
#endif
    campaign_init();                    /* find the campaign block */
    sound_init();                       /* the SPC700's driver and streams */
    text_font();                        /* the boxes' font into bank $7F */
    settings_load();                    /* the buttons (save.c) */
    screen_on();                        /* NMI on (the screen is set up below) */
    for (;;) {
        u8 cont = title_screen();
        game_screen();
        screen_on();
        if (cont && game_load()) {      /* CONTINUE, and the save is good */
            music_stop();
            update_backdrop(1);
            start_play("GAME LOADED");
        } else
            new_game();
        for (;;) {
            u16 edge = next_edge();
            u8 quit;
            sfx_head = sfx_tail;            /* a new input: the last turn's waiting effects go */
            quit = handle(edge);
            eng_turn_seq++;
            if (quit) break;
        }
    }
    return 0;
}
