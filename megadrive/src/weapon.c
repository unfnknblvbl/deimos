/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * The Mega Drive game - the player's weapon over the bottom of the view, with
 * hardware sprites (palette line 3: its 9 colours in slots 4..12, with
 * the text's colours; the view has the other three lines). Frames and placement come from
 * tools/build_weapons.py. The status bar and dialog boxes have priority
 * over sprites, so the weapon goes behind them.
 *
 * Callers: ui.c (after every action, idle or firing frame; hidden on the
 * title and the end screens). The frames come from the campaign block
 * (pack.c sets wp_frames, wp_sprites, wp_pal, wp_tiles):
 *   wp_frames: 5 words per frame (weapon * 2 + firing): first tile in
 *     wp_tiles, number of tiles, palette number in wp_pal, first sprite in
 *     wp_sprites, number of sprites;
 *   wp_sprites: 4 words per hardware sprite: x, y (in the WP_FRAME_W x
 *     WP_FRAME_H frame the build lays the weapon out in, from its top
 *     left), size (the VDP's size bits: width and height in tiles), tile
 *     (from the frame's first);
 *   wp_pal: 16 CRAM words per palette (slots 4..12 used);
 *   wp_tiles: 32 bytes per tile.
 * The tiles of the frame shown are copied into VRAM (up to 192 at
 * TILE_WP_A, the rest at TILE_WP_B) each time the frame changes.
 *
 * Placement: tools/build_weapons.py (and the editor) lay the weapon out at
 * 2 pixels per canvas unit in a 256x160 frame, the size of the view before
 * it shrank to 208x128, with the canvas centred and its bottom row on the
 * frame's bottom. The weapon keeps that size, and the frame's bottom
 * centre is put on the view's bottom centre (WP_DX, WP_DY): the weapon
 * stands at the bottom middle of the view, as before. (Every frame fits
 * inside the 208-pixel view: the widest reaches from x 104 to 232 of the
 * frame, 80..208 in the view.)
 *
 * Licence: GPL-2.0-or-later.
 */
#include "md.h"
#include "weapon_data.h"

#define TILE_WP_A 1856          /* 192 tiles at $E800 */
#define TILE_WP_B 1664          /* 64 more at $D000 */

#define WP_FRAME_W 256          /* the frame wp_sprites' x, y are in (build_weapons.py) */
#define WP_FRAME_H 160
/* frame -> screen pixels: the frame's bottom centre on the view's */
#define WP_DX (VIEW_COL0 * 8 + VIEW_W / 2 - WP_FRAME_W / 2)     /* 0 */
#define WP_DY (VIEW_LINE0 + VIEW_H - WP_FRAME_H)                /* -8 */

static s16 cur = -1;            /* the frame on screen (weapon * 2 + firing), -1 none */

/* a frame's tile t (from its first) -> its VRAM tile number */
static u16 vram_tile(u16 t)
{
    return t < WP_SPLIT ? TILE_WP_A + t : TILE_WP_B + (t - WP_SPLIT);
}

/* no weapon: the sprite table's first entry made the only one (link 0)
   and off screen (y = 0, above the visible area, which starts at 128) */
void weapon_hide(void)
{
    u16 sr = irq_off();
    vdp_set_vram_addr(VRAM_SAT);
    VDP_DATA = 0;
    VDP_DATA = 0;
    VDP_DATA = 0;
    VDP_DATA = 0;
    irq_restore(sr);
    cur = -1;
}

/* weapon 0..8, frame 0 idle / 1 firing; another number hides it. Does
   nothing if that frame is already shown. Uploads the frame's tiles and
   its 9 colours (palette 3, slots 4..12) and writes its sprites into the
   sprite table. Waits for the DMAs. */
void weapon_show(u8 w, u8 firing)
{
    const u16 *f;
    u16 i, n, nt, sr;
    s16 id = w * 2 + (firing ? 1 : 0);
    if (w > 8) {
        weapon_hide();
        return;
    }
    if (id == cur) return;
    cur = id;
    f = wp_frames + id * 5;         /* the frame's record (see above) */
    nt = f[1];
    /* tiles: up to WP_SPLIT in the first block, the rest in the second */
    if (nt > WP_SPLIT) {
        vdp_dma_vram(wp_tiles + (u32)f[0] * 32, TILE_WP_A * 32, WP_SPLIT * 16);
        vdp_dma_vram(wp_tiles + ((u32)f[0] + WP_SPLIT) * 32, TILE_WP_B * 32, (nt - WP_SPLIT) * 16);
    } else
        vdp_dma_vram(wp_tiles + (u32)f[0] * 32, TILE_WP_A * 32, nt * 16);
    vdp_write_cram(48 + 4, wp_pal + f[2] * 16 + 4, 9);     /* palette 3, slots 4..12 */
    n = f[4];
    sr = irq_off();
    vdp_set_vram_addr(VRAM_SAT);
    /* a sprite table entry: y + 128, size and the next sprite's number
       (0 ends the list), tile entry, x + 128 (the VDP puts the screen's
       top left at 128, 128) */
    for (i = 0; i < n; i++) {
        const u16 *s = wp_sprites + (f[3] + i) * 4;
        VDP_DATA = s[1] + 128 + WP_DY;                           /* y */
        VDP_DATA = (s[2] << 8) | (i + 1 < n ? i + 1 : 0);         /* size, link */
        VDP_DATA = NT_PAL(3) | vram_tile(s[3]);                  /* low priority */
        VDP_DATA = s[0] + 128 + WP_DX;                           /* x */
    }
    irq_restore(sr);
}
