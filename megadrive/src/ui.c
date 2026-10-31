/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * The Mega Drive game - the player-facing side: title, input, messages, status
 * bar, dialog boxes, door-code entry, notebook, and building the list of
 * things for the renderer. The rules themselves are in rules.s.
 *
 * Controls (6-button pad; every action except Start can be moved to any
 * button on the options screen; a 3-button pad has fixed ones: A, B,
 * C + Up medkit, C + Left / Right weapons, C + Down notebook, C held
 * full detail):
 *   D-pad              step forward / back, turn
 *   A                  use / attack what is in front of you
 *   B                  pass the turn
 *   C                  use a medkit (smallest first)
 *   X / Z              previous / next weapon
 *   Y                  notebook
 *   Mode (hold)        full detail, in smooth movement
 *   Start              pause menu (resume, map, items, notebook, save, options, cheats, title)
 *
 * Screen (256x224, H32): row 0 = messages (plane B), rows 3..18 and
 * columns 3..28 = the 3D view (208x128 pixels, plane A, double-buffered;
 * md.h VIEW_*), with a black border round it in rows 1..20, rows 21..27 =
 * status bar (window plane). Dialog boxes are drawn on plane B in front of
 * the view, over its rows.
 *
 * Structure: main() runs the title screen, sets up the game screen, then
 * loops: next_edge() waits for a new button press, handle() acts on it
 * according to the current mode (ui: UI_PLAY, a dialog, a menu...), and
 * after every game action after_action() redraws the view (render.s),
 * the status bar and the weapon, and opens whatever box the rules asked
 * for. The rules (rules.s) only change S and leave flags (dialog_str,
 * pw_want, S.store...) that this file turns into boxes.
 *
 * Terms: "view" = the 3D picture; "box" = the 24x16-character text area on
 * plane B used for dialogs, menus and screens; "pager" = a box showing
 * level strings a page at a time ('|' separates lines in a string); "edge"
 * = buttons newly pressed this frame; "smooth movement" = the option that
 * animates steps and turns with in-between pictures.
 *
 * The test harness (tools/md_lockstep.py and friends) drives the ROM in an
 * emulator and reads eng_turn_seq, eng_idle, eng_mode and dbg_* through
 * the link map.
 *
 * Licence: GPL-2.0-or-later.
 */
#include "game.h"
#include "level.h"
#include "render.h"
#include "render_data.h"
#include "ui_data.h"
#include "save.h"
#include "backdrop.h"
#include "music.h"
#include "pack.h"
/* the pause menu's cheats (CHEATS): bits read by rules.s (player_pain,
   player_attack); not part of the game state, not saved */
#define CHEAT_GOD 1                     /* nothing hurts the player */
#define CHEAT_AMMO 2                    /* attacks use no ammunition */
u8 cheats;
/* every weapon (0..8, not the dogs) with each ammunition (0..4) full, or
   (keys 1) every keycard */
static void cheat_give(u8 keys)
{
    u8 i;
    if (keys) {
        S.keys |= 0x0F;
        return;
    }
    S.weapons |= 0x1FF;
    for (i = 0; i < 5; i++) S.ammo[i] = 99;
}
#define AM_MD                           /* the automap (../../core/automap.c, included below the picture's code) */
#include "../../core/automap.h"

void sound_init(void);                      /* sound.c */
void sfx_flush(void);                       /* sound.c */
void weapon_show(u8 w, u8 firing);          /* weapon.c */
void weapon_hide(void);                     /* weapon.c */

/* read by the test harness (tools/md_lockstep.py) through the link map:
   eng_turn_seq counts inputs handled (bumped after each), eng_idle is 1
   while waiting for a button, eng_mode is the ui mode (UI_*) then */
volatile u8 eng_turn_seq, eng_idle, eng_mode;
/* tests: dbg_req = 1 draws the view dbg_arg[0..2] (x, z, facing) with
   mode dbg_arg[3]; 2 draws the in-between picture from (0..2) to (4..6)
   at t = dbg_arg[7] * 2; cleared when the picture is up */
volatile u8 dbg_req, dbg_arg[8];


#define PAL_HUD 2                        /* status bar and message line palette */
#define PAL_DLG 3                        /* dialog box text palette */
#define TILE_FACE TILE_MISC_B            /* 25 tiles: the current face */
#define TILE_ICON (TILE_MISC_B + 25)     /* 9 icons x 4 tiles */
#define TILE_HUD_LINE (TILE_MISC_B + 61) /* status bar border */

/* ------------------------------------------------------------ strings */
/* string `id` of the map in play. strings_blob (pack.c) starts with a
   big-endian u16 offset per string (from the blob's start), then the
   zero-ended strings. An id out of range gives string 3 (empty). */
const char *level_string(u8 id)
{
    if (id >= n_strings) id = 3;     /* "" */
    return (const char *)strings_blob + ((strings_blob[id * 2] << 8) | strings_blob[id * 2 + 1]);
}

/* ------------------------------------------------------------ text output */
/* one character at column x, row y of name table nt, with attr (palette,
   priority) ORed into the tile entry. The font has glyphs for 32..127;
   the space and anything outside 33..127 become glyph 127, an opaque
   blank (the space glyph itself is transparent). */
static void put_char(u16 nt, u8 x, u8 y, char ch, u16 attr)
{
    u8 c = (u8)ch;
    if (c <= 32 || c > 127) c = 127;               /* opaque blank */
    vdp_put_nt(nt, x, y, attr | (TILE_FONT + (u8)(c - 32)));
}

/* a string from column x of row y of name table nt (cut at column 31),
   lower case shown as upper case; same glyph rules as put_char */
static void put_text(u16 nt, u8 x, u8 y, const char *s, u16 attr)
{
    u16 sr = irq_off();
    vdp_set_vram_addr(nt + ((u16)y * 32 + x) * 2);  /* then one entry a character */
    while (*s && x < 32) {
        u8 c = (u8)*s++;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c <= 32 || c > 127) c = 127;           /* opaque blank */
        VDP_DATA = attr | (TILE_FONT + (u8)(c - 32));
        x++;
    }
    irq_restore(sr);
}

/* ------------------------------------------------------------ messages */
/* The message line (row 0 of plane B): a ring of MSGQ messages, shown one
   at a time, each for 45 frames (msg_tick). */
#define MSGQ 8
static char msgq[MSGQ][32];             /* zero-ended, 31 characters at most */
static u8 msg_head, msg_count, msg_timer;   /* oldest, how many, frames left for the shown one */

/* queue a, b and c (any may be 0) joined into one message; when the ring
   is full the oldest is dropped */
static void msg_push(const char *a, const char *b, const char *c)
{
    u8 slot, n = 0;
    char *d;
    if (msg_count == MSGQ) {
        msg_head = (msg_head + 1) & (MSGQ - 1);
        msg_count--;
    }
    slot = (msg_head + msg_count) & (MSGQ - 1);
    d = msgq[slot];
    while (a && *a && n < 31) d[n++] = *a++;
    while (b && *b && n < 31) d[n++] = *b++;
    while (c && *c && n < 31) d[n++] = *c++;
    d[n] = 0;
    msg_count++;
}

/* the message functions of game.h (the rules call them too) */
void say(const char *s) { msg_push(s, 0, 0); }
void say2(const char *a, const char *b) { msg_push(a, b, 0); }
void say3(const char *a, const char *b, const char *c) { msg_push(a, b, c); }

static char numbuf[6];

/* n in decimal (no leading zeros); the result is in a static buffer, valid
   until the next call */
static char *utoa10(u16 n)
{
    u8 i = 5;
    numbuf[5] = 0;
    do {
        numbuf[--i] = '0' + n % 10;
        n /= 10;
    } while (n);
    return numbuf + i;
}

/* the message a, then n in decimal, then b */
void say_num(const char *a, u16 n, const char *b) { msg_push(a, utoa10(n), b); }

/* message s centred on the message line (the rest blanked) */
static void show_message(const char *s)
{
    char line[33];
    u8 n = 0, i, x;
    while (s[n] && n < 32) n++;
    x = (32 - n) >> 1;
    for (i = 0; i < 32; i++) line[i] = ' ';
    for (i = 0; i < n; i++) line[x + i] = s[i];
    line[32] = 0;
    put_text(VRAM_PLANE_B, 0, 0, line, NT_PAL(PAL_HUD));
}

/* once a frame (next_edge): queue the rules' new status message, and
   show the next queued message once the shown one has had its time */
static void msg_tick(void)
{
    if (msg_timer) msg_timer--;
    if (status_new) {                       /* (rules.s wrote status_msg) */
        status_new = 0;
        if (status_msg[0]) msg_push(status_msg, 0, 0);
    }
    if (msg_timer == 0 && msg_count) {
        show_message(msgq[msg_head]);
        msg_head = (msg_head + 1) & (MSGQ - 1);
        msg_count--;
        msg_timer = 45;                     /* 0.75 s at 60 Hz */
    }
}

/* drop the queued messages (a new input); the shown one stays until
   replaced */
static void msg_clear(void)
{
    msg_count = 0;
    msg_timer = 0;
}

/* ------------------------------------------------------------ status bar */
/* rows 22..26 (window plane), columns 7..31 of text; the face at 1..5 */
/* The text is composed in hud_new, compared with hud_shadow (what is on
   screen) and only the characters that changed are written to VRAM. */
#define HUD_ROW0 22
static char hud_shadow[5][25];
static char hud_new[5][25];
static u8 cur_face = 0xFF;              /* the face shown (0..8), 0xFF none */

/* hud_new all spaces */
static void hud_clear_rows(void)
{
    u8 r, c;
    for (r = 0; r < 5; r++)
        for (c = 0; c < 25; c++) hud_new[r][c] = ' ';
}

/* s into hud_new at status bar text row `row` (1..5) and screen column
   col (7..31) */
static void hud_text(u8 row, u8 col, const char *s)
{
    while (*s && col < 32) {
        hud_new[row - 1][col - 7] = *s++;
        col++;
    }
}

/* n in decimal at (row, col); returns the column after it */
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

/* show face f (0..3 by health, 4 hurt, 8 grin): its 25 tiles copied into
   TILE_FACE.. and a 5x5 block of the window's name table pointed at them
   (column-major: tile tx * 5 + ty) */
static void set_face(u8 f)
{
    u8 tx, ty;
    if (f == cur_face) return;
    cur_face = f;
    vdp_dma_vram(ui_faces + (u16)f * 25 * 32, TILE_FACE * 32, 25 * 16);  /* 25 tiles = 400 words */
    for (tx = 0; tx < 5; tx++)
        for (ty = 0; ty < 5; ty++)
            vdp_put_nt(VRAM_WINDOW, 1 + tx, HUD_ROW0 + ty, NT_PRIO | NT_PAL(PAL_HUD) | (TILE_FACE + tx * 5 + ty));
}

static u8 hurt_face;                    /* 1: the last action hurt the player */
extern volatile u8 grin_frames;         /* (sound.c: a pick-up's grin, counted down each frame) */

/* the face as the original picks it (b.java): the grin just after a
   pick-up, the grimace when hurt, else by health - more blood at a half,
   a third and a quarter of the maximum */
static void face_update(void)
{
    u8 hp = S.hp, mx = S.maxhp;
    if (hp && grin_frames) set_face(8);
    else if (hp && hurt_face) set_face(4);
    else set_face(hp <= mx / 4 ? 3 : hp <= mx / 3 ? 2 : hp <= mx / 2 ? 1 : 0);
}

/* the status bar from S: health, armour, weapon and ammo, level and XP,
   credits, keys (G Y B R), medkits (small + large) and soul spheres; then
   the face */
static void hud_update(void)
{
    u8 c, i, r;
    hud_clear_rows();
    hud_text(1, 7, "HEALTH");
    c = hud_num(1, 14, S.hp);
    hud_text(1, c, "/");
    hud_num(1, c + 1, S.maxhp);
    hud_text(1, 21, "AR");
    c = hud_num(1, 24, S.armor);
    hud_text(1, c, "/");
    hud_num(1, c + 1, S.maxarmor);

    hud_text(2, 7, gtext(TX_WPN_0 + S.weapon));   /* (its name: the texts table; wpn_*: build/gen/data.c) */
    if (wpn_use[S.weapon]) {                /* a weapon that uses ammo */
        hud_text(2, 21, "AMMO");
        hud_num(2, 26, S.ammo[wpn_ammo[S.weapon]]);
    }
    hud_text(3, 7, "LEVEL");
    hud_num(3, 13, S.level);
    hud_text(3, 17, "XP");
    c = hud_num(3, 20, S.xp);
    hud_text(3, c, "/");
    hud_num(3, c + 1, S.nextxp);

    hud_text(4, 7, gtext(TX_CREDITS_WORD));
    hud_num(4, 15, S.credits);
    hud_text(4, 20, "KEYS");
    if (S.keys & 1) hud_text(4, 25, "G");   /* S.keys bits 0..3: the four key colours */
    if (S.keys & 2) hud_text(4, 26, "Y");
    if (S.keys & 4) hud_text(4, 27, "B");
    if (S.keys & 8) hud_text(4, 28, "R");

    hud_text(5, 7, "MEDKITS");
    hud_num(5, 15, S.inventory[0]);
    hud_text(5, 17, "+");
    hud_num(5, 18, S.inventory[1]);
    if (S.inventory[2]) {
        hud_text(5, 21, "SOUL");
        hud_num(5, 26, S.inventory[2]);
    }
    /* write only what changed */
    for (r = 0; r < 5; r++)
        for (i = 0; i < 25; i++)
            if (hud_new[r][i] != hud_shadow[r][i]) {
                hud_shadow[r][i] = hud_new[r][i];
                put_char(VRAM_WINDOW, 7 + i, HUD_ROW0 + r, hud_new[r][i], NT_PRIO | NT_PAL(PAL_HUD));
            }
    face_update();
}

/* the status bar's background: rows 21..27 of the window filled with the
   opaque blank (glyph 127), row 21 with the border line; the shadow and
   face forgotten so hud_update writes everything */
static void hud_init(void)
{
    u8 x, y;
    for (y = 21; y < 28; y++)
        for (x = 0; x < 32; x++) vdp_put_nt(VRAM_WINDOW, x, y, NT_PRIO | NT_PAL(PAL_HUD) | (TILE_FONT + 95));
    for (x = 0; x < 32; x++) vdp_put_nt(VRAM_WINDOW, x, 21, NT_PRIO | NT_PAL(PAL_HUD) | TILE_HUD_LINE);
    for (y = 0; y < 5; y++)
        for (x = 0; x < 25; x++) hud_shadow[y][x] = ' ';
    cur_face = 0xFF;
}

/* ------------------------------------------------------------ the picture */
static struct robj objs[MAX_ROBJ];      /* the things to draw (collect_things) */

/* the cells whose things can show from view (gx, gz, facing), ORed into
   bits (bit (cell & 7) of byte cell >> 3): the map's list for the view
   (r_vis_index / r_vis: a word offset, or $FFFF for every cell, then cell
   numbers ended by a negative word), as the renderer reads it (vis_of) */
static void add_vis(u8 *bits, u8 gx, u8 gz, u8 facing)
{
    u16 o = r_vis_index[(((u16)gz << 5) + gx) * 4 + (facing & 3)], c;
    const u16 *p;
    u8 i = 0;
    if (o == 0xFFFF) {
        do bits[i] = 0xFF; while (++i < 128);
        return;
    }
    for (p = r_vis + o; !((c = *p) & 0x8000); p++) bits[c >> 3] |= (u8)(1 << (c & 7));
}

/* ---- the automap: core/automap.c, with this port's map tables ---- */
#include "../../core/automap.c"
static u8 am_map = 0xFF;                /* the map seen[] is for (0xFF: none) */

/* a solid wall at (x, z) (outside the map is): wall_bits, as rules.s's wall_at */
u8 am_wall(u8 x, u8 z)
{
    u16 c;
    if (x >= 32 || z >= 32) return 1;
    c = ((u16)z << 5) + x;
    return (u8)((wall_bits[c >> 3] >> (c & 7)) & 1);
}

/* a closed line (door) at (x, z): as rules.s's line_at */
u8 am_door(u8 x, u8 z)
{
    u16 c;
    u8 i;
    if (x >= 32 || z >= 32) return 0;
    c = ((u16)z << 5) + x;
    if (!((line_cells[c >> 3] >> (c & 7)) & 1)) return 0;
    for (i = 0; i < n_lines; i++)
        if (line_x[i] == x && line_z[i] == z && !(S.l_flags[i] & L_OPEN)) return 1;
    return 0;
}

/* the view's cells seen (after each action) */
static void am_update(void)
{
    if (am_map != S.map) {
        am_clear();
        am_map = S.map;
    }
    add_vis(seen, S.px, S.pz, S.facing);
    am_mark(((u16)S.pz << 5) + S.px);
}

/* the renderer's list of things: the entities on the map (linked into
   their cell, with a definition) in a cell whose things can show from
   view a or view b (the same view twice for a still picture; both ends
   of a step or turn), at the cell's centre, with the def's sprite slot.
   When more than MAX_ROBJ can show, the nearest to view b's cell are
   kept. Returns how many.
   (Until 0.1.1 it took the first MAX_ROBJ entities of the whole map,
   seen or not: on a map with more than that, most were never drawn.) */
static u8 robj_d[MAX_ROBJ];             /* each kept thing's distance from view b (cells, squared, capped) */
static u8 collect_things2(u8 ax, u8 az, u8 af, u8 bx, u8 bz, u8 bf)
{
    static u8 bits[128];
    u16 i;
    u8 n = 0, k;
    for (k = 0; k < 128; k++) bits[k] = 0;
    add_vis(bits, ax, az, af);
    if (ax != bx || az != bz || af != bf) add_vis(bits, bx, bz, bf);
    for (i = 0; i < n_ents; i++) {
        u8 f = S.e_flags[i], ex = S.e_x[i], ez = S.e_z[i], d, slot;
        u16 cell = ((u16)ez << 5) + ex, dd;
        s8 dx = (s8)(ex - bx), dz = (s8)(ez - bz);
        if (!(f & F_LINKED) || S.e_def[i] == NO_DEF) continue;
        if (!(bits[cell >> 3] & (1 << (cell & 7)))) continue;
        dd = (u16)(dx * dx) + (u16)(dz * dz);
        d = dd > 255 ? 255 : (u8)dd;
        if (n < MAX_ROBJ) slot = n++;
        else {                          /* full: replace the farthest, if this one is nearer */
            slot = 0;
            for (k = 1; k < MAX_ROBJ; k++)
                if (robj_d[k] > robj_d[slot]) slot = k;
            if (d >= robj_d[slot]) continue;
        }
        objs[slot].x = ex * 64 + 32;
        objs[slot].z = ez * 64 + 32;
        objs[slot].spr = r_def_spr[S.e_def[i]];
        robj_d[slot] = d;
    }
    return n;
}

/* the things for a still picture of view (gx, gz, facing) */
static u8 collect_things(u8 gx, u8 gz, u8 facing)
{
    return collect_things2(gx, gz, facing, gx, gz, facing);
}

static u8 view_low;                 /* the view on screen is low detail */
static u16 cur_pad;                 /* the buttons held (pad_read, every frame) */

/* full detail wanted: always in step movement; in smooth movement while
   the detail button is held */
static u8 want_detail(void)
{
    return !cfg.smooth || (cur_pad & action_bits(ACT_DETAIL));
}

/* draw the view from the player's cell and facing (no animation). Before
   every picture pack_portal sets the Reactor portal's wall textures (closed
   once both couplings are destroyed). */
static void refresh_picture(void)
{
    u8 n = collect_things(S.px, S.pz, S.facing);
    u8 low = !want_detail();
    pack_portal(S.couplings >= 2), render_view(S.px, S.pz, S.facing, S.l_flags, objs, n, low ? R_LOW : 0);
    view_low = low;
}

/* ---- smooth movement: the colours of the destination are chosen first
   (a quick low-detail pass), then in-between pictures of the step or turn
   are drawn as fast as the renderer allows within ANIM_FRAMES, then the
   destination itself - all in low detail with those colours, unless the
   detail button is held ---- */
#define ANIM_FRAMES 18                  /* 0.3 s at 60 Hz */
static u8 old_x, old_z, old_f;          /* the player's cell and facing before the action */

/* smooth movement after an action: returns 1 if it animated (and drew the
   final picture), 0 if the caller must draw the view itself (step mode,
   no move, a step and a turn together, a jump of more than one cell, a
   half turn). Either way old_* are set to the new position. */
static u8 animate_move(void)
{
    u8 n, turn = S.facing != old_f, step = S.px != old_x || S.pz != old_z;
    u16 start;
    u8 ox = old_x, oz = old_z, of = old_f;
    old_x = S.px;                                    /* animate each move once */
    old_z = S.pz;
    old_f = S.facing;
    if (!cfg.smooth || turn == step) return 0;       /* step mode, or nothing / both: just draw it */
    if (step && (u8)((S.px - ox) * (S.px - ox) + (S.pz - oz) * (S.pz - oz)) != 1) return 0;   /* not to a neighbour */
    if (turn && ((S.facing - of) & 3) == 2) return 0;                                          /* a half turn */
    n = collect_things2(ox, oz, of, S.px, S.pz, S.facing);   /* (seen from either end) */
    /* the destination's palettes first (no picture) */
    pack_portal(S.couplings >= 2), render_view(S.px, S.pz, S.facing, S.l_flags, objs, n, R_LOW | R_PALETTE_ONLY);
    start = frame_count;
    /* as many in-between pictures as fit in ANIM_FRAMES, each for the time
       reached so far */
    for (;;) {
        u16 el = frame_count - start, t, t2;
        if (el >= ANIM_FRAMES) break;
        t = (u16)((u32)el * 256 / ANIM_FRAMES);     /* linear time, 0..255 */
        /* smoothstep 3t^2 - 2t^3 in 1/256: t2 = t^2, then t2 (3 - 2t) */
        t2 = (t * t) >> 8;
        t = (t2 * (768 - 2 * t)) >> 8;               /* ease in and out */
        if (!t) t = 8;                  /* (the first picture moves a little) */
        render_between(ox, oz, of, S.px, S.pz, S.facing, t, S.l_flags, objs, n);
    }
    /* the destination itself: full detail if the detail button is held,
       else low detail keeping the palettes chosen above */
    if (want_detail()) {
        pack_portal(S.couplings >= 2), render_view(S.px, S.pz, S.facing, S.l_flags, objs, n, 0);
        view_low = 0;
    } else {
        pack_portal(S.couplings >= 2), render_view(S.px, S.pz, S.facing, S.l_flags, objs, n, R_LOW | R_KEEP);
        view_low = 1;
    }
    return 1;
}

/* ------------------------------------------------------------ boxes */
/* The box: DLG_W x DLG_H characters composed in box[][], shown on plane B
   at column BOX_X, row box_y with the priority bit (in front of the view).
   Box rows 1..13 hold text (BOX_LINES lines a page), row 14 the footer.
   In play it covers the view's rows (BOX_Y = VIEW_ROW0, DLG_H = VIEW_TH)
   and its middle 24 of 26 columns, centred like the view. */
#define BOX_X 4
#define BOX_Y VIEW_ROW0
#define DLG_W 24
#define DLG_H 16
#define BOX_LINES 13
/* the user interface modes (ui): what the next button press means */
#define UI_PLAY 0
#define UI_DIALOG 1
#define UI_PASSWORD 2
#define UI_DEAD 3
#define UI_DONE 4
#define UI_NOTES 5
#define UI_PAUSE 6
#define UI_OPTIONS 7
#define UI_TITLE 8
#define UI_STORE 9
#define UI_ITEMS 10
#define UI_END 11
#define UI_MAP 12
#define UI_CHEATS 13

static void next_map(void);

static u8 ui;                           /* the mode (UI_*) */
/* the pager: the string ids to show (box_n of them), the one shown
   (box_i) and its first line on this page (box_line) */
static u8 box_str[MAX_NOTES], box_n, box_i, box_line;
#define CODE_MAX 8                      /* digits (the longest code in the game has 7) */
static char code[CODE_MAX + 1];         /* the door code typed so far */
static u8 code_len, code_digit;         /* digits entered; the digit under the cursor (0..9) */
static char box[DLG_H][DLG_W];          /* the box's characters (not zero-ended) */

/* box[][] all spaces */
static void box_clear(void)
{
    u8 r, c;
    for (r = 0; r < DLG_H; r++)
        for (c = 0; c < DLG_W; c++) box[r][c] = ' ';
}

static const char *key_label(u8 tok);   /* (with the options, below) */

/* s into box row `row` from column col, up to the end of s, a '|' (a line
   break in level strings) or the box's right edge. A button token in s
   (a byte below 32: tools/key_text.py puts one in the level strings where
   the original named a phone key) is written as the name of the button
   that does that action now (key_label), also cut at the edge. The row is
   not checked: it must be below DLG_H. */
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

static u8 box_y = BOX_Y, box_h = DLG_H;     /* the title screen uses a lower, shorter box */

/* the box's box_h rows onto plane B (palette 3, in front) */
static void box_show(void)
{
    u8 r;
    char line[DLG_W + 1];
    line[DLG_W] = 0;
    for (r = 0; r < box_h; r++) {
        u8 c;
        for (c = 0; c < DLG_W; c++) line[c] = box[r][c];
        put_text(VRAM_PLANE_B, BOX_X, box_y + r, line, NT_PRIO | NT_PAL(PAL_DLG));
    }
}

/* the box's area of plane B back to what was there: the floor / ceiling
   tiles inside the view, blank outside it */
static void box_hide(void)
{
    u8 r, c;
    u16 sr = irq_off();
    for (r = 0; r < box_h; r++) {
        vdp_set_vram_addr(VRAM_PLANE_B + ((u16)(box_y + r) * 32 + BOX_X) * 2);
        for (c = 0; c < DLG_W; c++) VDP_DATA = backdrop_entry(BOX_X + c, box_y + r);
    }
    irq_restore(sr);
}

static char gen_text[48];               /* a dialog text made here (DLG_CAPTURED) */

/* a dialog's text: the level's string, or one the rules made up */
static const char *dialog_text(u8 id)
{
    if (id == DLG_CAPTURED) {
        const char *a = gtext(TX_CAPTURED), *b = def_name(captured_def);
        char *d = gen_text;
        while (*a && d < gen_text + 44) *d++ = *a++;
        while (*b && d < gen_text + 44) *d++ = *b++;
        a = gtext(TX_CAPTURED_END);         /* (the "!" after the name) */
        while (*a && d < gen_text + 47) *d++ = *a++;
        *d = 0;
        return gen_text;
    }
    return level_string(id);
}

/* the lines of dialog text `id` from line `first` into the box from row
   `row`, at column 3. Returns 1 if more lines remain (the page is full),
   0 if the text ended. */
static u8 box_string(u8 id, u8 first, u8 row)
{
    const char *s = dialog_text(id);
    u8 line = 0;
    while (*s) {
        if (line >= first) {
            if (row >= 1 + BOX_LINES) return 1;
            box_put(row++, 3, s);
        }
        while (*s && *s != '|') s++;    /* past this line ... */
        if (*s == '|') s++;             /* ... and its '|' */
        line++;
    }
    return 0;
}

/* s centred on the box's footer row (DLG_H - 2) */
static void box_footer(const char *s)
{
    u8 n = 0;
    const char *p = s;
    while (*p++) n++;
    box_put(DLG_H - 2, (DLG_W - n) >> 1, s);
}

/* the pager's page: the current string from box_line, with "A:MORE" if
   more follows (in this string or the next), else "A:OK" */
static void pager_draw(void)
{
    u8 more;
    box_clear();
    more = box_string(box_str[box_i], box_line, 1);
    box_footer(more || box_i + 1 < box_n ? "A:MORE" : "A:OK");
    box_show();
}

/* open the pager on n string ids (a dialog, or the notebook), in mode
   `mode` (UI_DIALOG or UI_NOTES) */
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

/* A in the pager: the next page, or the next string; returns 1 when there
   is nothing more (the caller closes the box) */
static u8 pager_next(void)
{
    u8 more;
    box_clear();
    more = box_string(box_str[box_i], box_line, 1);     /* (only to learn if more follows) */
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

/* the door code box: the rules' prompt string, the digits entered, the
   digit being chosen, '_' for the rest, and the keys */
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

/* start entering a door code (the rules set pw_want) */
static void open_password(void)
{
    code_len = 0;
    code_digit = 0;
    ui = UI_PASSWORD;
    password_draw();
}

/* a box with a centred title (row 5), two lines (rows 8, 9) and a footer:
   the death, sector done and end screens */
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

/* the map's monsters: returns how many are dead; *total = how many there
   are (entities placed on the map whose type is ET_ENEMY) */
static u8 count_kills(u8 *total)
{
    u16 i;
    u8 k = 0, t = 0;
    for (i = 0; i < n_map_ents; i++)
        if (def_etype[S.e_def[i]] == ET_ENEMY) {
            t++;
            if (!(S.e_flags[i] & F_ALIVE)) k++;
        }
    *total = t;
    return k;
}

static char line1[25], line2[25], title_buf[25];   /* texts for screen_box and menus */

/* copy a to d, zero-ended; returns the end (where the zero is), for
   appending */
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
    s = cat(s, gtext(TX_CREDITS_MID));
    cat(s, utoa10(S.credits));
}

/* the player died or went through an exit: the death screen, the end of
   the game, a sector's statistics, or straight on to the next map */
static void end_screens(void)
{
    if (S.dead) {
        music_stop();
        ui = UI_DEAD;
        screen_box("YOU HAVE DIED", "THE DEMONS GOT YOU.", "", "A:TRY AGAIN START:TITLE");
    } else {                                    /* through an exit */
        char *s;
        ui = UI_DONE;
        stats_lines();
        /* the exit's target map is in S.exit's low byte; 255 means a map not
           in the campaign (tools/gen_cdata.py map_target): the game's end */
        if ((S.exit & 0xFF) == 0xFF) {          /* past the Reactor: the end */
            ui = UI_END;
            screen_box("THE END", line1, line2, "START:TITLE");
            sfx(SFX_COMPLETE);
            music_play(MUS_END);
            return;
        }
        /* bit 31 of S.exit: the exit completes a sector */
        if (!(S.exit & 0x80000000UL)) {         /* not a completed sector: straight on */
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

static u16 cur_floor = 0xFFFF, cur_ceil = 0xFFFF;  /* the backdrop's colours now (CRAM) */

/* rgb888 -> the Mega Drive's colour (the console's measured DAC levels):
   each channel to the nearest of the 8 levels, packed as 0000BBB0GGG0RRR0.
   berserk: red pulled halfway to full, green and blue halved. */
static u16 md_colour(const u8 *rgb, u8 berserk)
{
    static const u8 lv[8] = {0, 52, 87, 116, 144, 172, 206, 255};
    u16 c = 0;
    u8 i, k;
    for (i = 0; i < 3; i++) {
        u16 v = rgb[i], best = 0xFFFF;
        u8 bk = 0;
        if (berserk) v = i == 0 ? (v + 255) >> 1 : v >> 1;     /* the Berserker's red */
        for (k = 0; k < 8; k++) {
            u16 d = v > lv[k] ? v - lv[k] : lv[k] - v;
            if (d < best) {
                best = d;
                bk = k;
            }
        }
        c |= (u16)bk << (1 + 4 * i);
    }
    return c;
}

/* floor and ceiling colours: the map's, changed by events, red with the Berserker.
   Remakes the backdrop (backdrop_init, a few frames) only when a colour
   changed, or always with force. */
static void update_backdrop(u8 force)
{
    u16 f = md_colour(S.floor_rgb, S.berserk != 0), c = md_colour(S.ceil_rgb, S.berserk != 0);
    if (force || f != cur_floor || c != cur_ceil) {
        cur_floor = f;
        cur_ceil = c;
        backdrop_init(f, c);
    }
}

static void store_open(void);

/* after anything happened: picture, status bar, then any box the rules opened */
static void after_action(void)
{
    if (player_fired) {
        u8 i;
        player_fired = 0;
        weapon_show(S.weapon, 1);               /* the firing frame ... */
        for (i = 0; i < 8; i++) wait_vblank();  /* ... for 8 frames */
    }
    if (player_hurt) {
        player_hurt = 0;
        flash_frames = 6;                       /* red flash (vdp.c's frame interrupt) */
        hurt_face = 1;
    } else
        hurt_face = 0;
    update_backdrop(0);                         /* if the floor / ceiling colours changed */
    am_update();                                /* the automap's seen cells */
    if (!animate_move()) refresh_picture();     /* the new view */
    hud_update();
    if (S.dead || S.exit_set) {
        weapon_hide();
        end_screens();
        return;
    }
    weapon_show(S.weapon, 0);                   /* the idle frame */
    /* what the rules left open (0xFF: nothing): a dialog, a door code, a
       vendor */
    if (dialog_str != 0xFF) open_pager(UI_DIALOG, &dialog_str, 1);
    else if (pw_want != 0xFF) open_password();
    else if (S.store != 0xFF) store_open();
    else ui = UI_PLAY;
}

/* back to play, the box removed */
static void close_box(void)
{
    ui = UI_PLAY;
    box_hide();
}

/* ------------------------------------------------------------ screens */
/* plane A's entry at screen column x, row y for the view buffer whose
   first tile is `base`: tile (x - VIEW_COL0) * VIEW_TH + y - VIEW_ROW0
   inside the view, blank in the border and the other rows */
static u16 view_entry(u16 base, u8 x, u8 y)
{
    if (y < VIEW_ROW0 || y >= VIEW_ROW0 + VIEW_TH || x < VIEW_COL0 || x >= VIEW_COL0 + VIEW_TW) return TILE_BLANK;
    return base + (u16)(x - VIEW_COL0) * VIEW_TH + y - VIEW_ROW0;
}

/* the game screen's name tables: plane A 0 and 1 show view buffers 0 and
   1 (tiles from 0 and VIEW_BUF1_TILE) in the view's place, blank
   elsewhere; plane B the backdrop tiles behind the view. (render.s
   rewrites the view's rows of plane A with every picture.) */
static void view_planes(void)
{
    u8 x, y;
    u16 sr = irq_off();
    for (y = 0; y < 32; y++) {
        vdp_set_vram_addr(VRAM_PLANE_A0 + y * 64);
        for (x = 0; x < 32; x++) VDP_DATA = view_entry(0, x, y);
        vdp_set_vram_addr(VRAM_PLANE_A1 + y * 64);
        for (x = 0; x < 32; x++) VDP_DATA = view_entry(VIEW_BUF1_TILE, x, y);
        vdp_set_vram_addr(VRAM_PLANE_B + y * 64);
        for (x = 0; x < 32; x++) VDP_DATA = backdrop_entry(x, y);
    }
    irq_restore(sr);
}

/* the font, the status bar icons and border line, and palettes 2 (status
   bar) and 3 (dialogs, text) */
static void load_ui_tiles(void)
{
    /* (the grey line two rows down: the top rows go dark while the H
       interrupt puts the status bar's palette in) */
    static const u8 line_tile[32] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x33, 0x33, 0x33, 0x33, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    u8 i;
    vdp_dma_vram(ui_font, TILE_FONT * 32, UI_FONT_TILES * 16);     /* 96 glyphs */
    /* the space glyph must be transparent (it is the blank tile) */
    vdp_fill_vram(TILE_FONT * 32, 32, 0);
    vdp_dma_vram(ui_icons, TILE_ICON * 32, 9 * 4 * 16);            /* 9 icons of 4 tiles */
    {
        u16 sr = irq_off();
        vdp_set_vram_addr(TILE_HUD_LINE * 32);
        /* (a tile row is 4 bytes: $FF = two pixels of colour 15, $33 two
           of colour 3; row 2 is the line) */
        for (i = 0; i < 32; i += 2) VDP_DATA = (line_tile[i] << 8) | line_tile[i + 1];
        irq_restore(sr);
    }
    vdp_write_cram(32, ui_hud_pal, 16);     /* palette 2 */
    vdp_write_cram(48, ui_dlg_pal, 16);     /* palette 3 */
}

static u16 prev_pad;

/* waits for the next button press; the test harness watches eng_idle and
   eng_turn_seq (bumped by the caller once the press is handled) */
/* Returns the buttons newly pressed (PAD_* bits). While waiting, once a
   frame: the message line, the tests' picture requests, the end of the
   grin, and in smooth movement the full-detail picture while the detail
   button is held. */

static u16 next_edge(void)
{
    for (;;) {
        u16 edge;
        eng_idle = 1;
        eng_mode = ui;
        wait_vblank();                  /* one pass a frame */
        msg_tick();
        cur_pad = pad_read();
        edge = cur_pad & ~prev_pad;     /* pressed now, not last frame */
        prev_pad = cur_pad;
        if (dbg_req) {                  /* (tests: draw a given view, wait for it) */
            u8 n = dbg_req == 1 ? collect_things(dbg_arg[0], dbg_arg[1], dbg_arg[2])
                               : collect_things2(dbg_arg[0], dbg_arg[1], dbg_arg[2], dbg_arg[4], dbg_arg[5], dbg_arg[6]);
            if (dbg_req == 1)
                render_view(dbg_arg[0], dbg_arg[1], dbg_arg[2], S.l_flags, objs, n, dbg_arg[3]);
            else
                render_between(dbg_arg[0], dbg_arg[1], dbg_arg[2], dbg_arg[4], dbg_arg[5], dbg_arg[6],
                               (u16)dbg_arg[7] * 2, S.l_flags, objs, n);
            while (render_busy()) {
            }
            dbg_req = 0;
            continue;
        }
        if (ui == UI_PLAY && cur_face == 8 && !grin_frames) face_update();   /* the grin is over */
        /* smooth movement: holding the detail button sharpens the view */
        if (ui == UI_PLAY && cfg.smooth && view_low && (cur_pad & action_bits(ACT_DETAIL))) {
            refresh_picture();
            weapon_show(S.weapon, 0);
        }
        if (edge) {
            eng_idle = 0;
            return edge;
        }
    }
}

/* ---- menus: a cursor over a list of lines in the box ---- */
/* title centred on row 1, the n items from row row0 at column 4 with '>'
   at column 2 before item cur, and the footer if foot is not 0 */
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

/* ---- options: movement and the button of every action ---- */
static const char * const ACT_NAMES[N_ACTIONS] = {"USE/ATTACK", "PASS TURN", "MEDKIT", "PREV WEAPON",
                                                  "NEXT WEAPON", 0, "FULL DETAIL"};

static const char * const ACT_3BUTTON[N_ACTIONS] = {"A", "B", "C+UP", "C+LEFT", "C+RIGHT", "C+DOWN", "C"};

/* the button tokens in the level strings (tools/key_text.py): KEY_ACT + an
   action (ACT_*), or KEY_MENU for the pause menu's button */
#define KEY_ACT 1
#define KEY_MENU 16

/* the name of the button token tok stands for (box_put): the button its
   action is on now (a 6-button pad's, as the options screen set it; a
   3-button pad's fixed ones), START for the pause menu, "?" for a token
   this game has no action for */
static const char *key_label(u8 tok)
{
    u8 a = tok - KEY_ACT;                       /* the action (ACT_*) */
    if (tok == KEY_MENU) return "START";
    if (a >= N_ACTIONS) return "?";             /* (also tok 0, which wraps round) */
    return pad_six ? button_names[cfg.key[a]] : ACT_3BUTTON[a];
}

/* the options box, cursor on line cur: line 0 the movement, 1..7 the
   actions' buttons, 8 DEFAULTS, 9 DONE (name at column 0, value at 13) */
static void options_draw(u8 cur)
{
    static char lines[10][21];
    const char *items[10];
    u8 i, k;
    for (i = 0; i < 10; i++) {
        const char *a, *b;
        char *d = lines[i];
        if (i == 0) {
            a = "MOVEMENT";
            b = cfg.smooth ? "SMOOTH" : "STEP";
        } else if (i <= N_ACTIONS) {
            a = ACT_NAMES[i - 1] ? ACT_NAMES[i - 1] : gtext(TX_NOTEBOOK_ITEM);   /* (0: the game's word) */
            b = pad_six ? button_names[cfg.key[i - 1]] : ACT_3BUTTON[i - 1];
        } else {
            a = i == 8 ? "DEFAULTS" : "DONE";
            b = "";
        }
        for (k = 0; k < 20; k++) d[k] = ' ';
        d[20] = 0;
        for (k = 0; a[k] && k < 20; k++) d[k] = a[k];
        for (k = 0; b[k]; k++) d[13 + k] = b[k];
        items[i] = d;
    }
    menu_draw(gtext(TX_OPTIONS_ITEM), items, 10, cur, 3,
              cur >= 1 && cur <= N_ACTIONS ? (pad_six ? "PRESS A BUTTON TO SET" : "3-BUTTON PAD: FIXED")
                                           : "A:CHANGE  START:DONE");
}

/* the options screen (from the title or the pause menu), until Start or
   DONE: Up / Down move; on MOVEMENT, Left / Right / A switch step / smooth;
   on an action, a 6-button pad's button takes that action (the action
   that had it gets this one's old button); DEFAULTS resets. Saves the
   settings on leaving and restores the mode it was called from. */
static void options_screen(void)
{
    u8 cur = 0, saved_ui = ui;
    ui = UI_OPTIONS;
    eng_turn_seq++;                             /* the button that opened it is handled */
    options_draw(cur);
    for (;;) {
        u16 e = next_edge();
        u8 b;
        if (e & PAD_START) break;
        if (e & PAD_UP) cur = cur ? cur - 1 : 9;
        else if (e & PAD_DOWN) cur = cur < 9 ? cur + 1 : 0;
        else if (cur == 0 && (e & (PAD_LEFT | PAD_RIGHT | PAD_A))) cfg.smooth ^= 1;
        else if (cur >= 1 && cur <= N_ACTIONS && pad_six) {
            for (b = 0; b < N_BUTTONS; b++)
                if (e & button_bits[b]) {
                    u8 a = cur - 1, j;
                    for (j = 0; j < N_ACTIONS; j++)          /* the action that had it gets ours */
                        if (cfg.key[j] == b) cfg.key[j] = cfg.key[a];
                    cfg.key[a] = b;
                    break;
                }
        } else if (cur == 8 && (e & PAD_A)) settings_defaults();
        else if (cur == 9 && (e & PAD_A)) break;
        else {                                  /* a button that does nothing here */
            eng_turn_seq++;
            continue;
        }
        sfx(SFX_BLIP);
        options_draw(cur);
        eng_turn_seq++;
    }
    eng_turn_seq++;
    settings_save();                            /* to battery RAM */
    sfx(SFX_SELECT);
    box_hide();
    ui = saved_ui;
}

/* ---- title ---- */
/* the title picture: raster effects off, its tiles over the view's buffer
   0 (render_reset is needed afterwards), its palette into palette 0, its
   map on plane A 0, plane B cleared, no window */
static void show_title(void)
{
    u8 x, y;
    vdp_raster(0);
    vdp_display(0);                             /* blank while loading */
    weapon_hide();
    vdp_dma_vram(title_tiles, 0, TITLE_N_TILES * 16);
    vdp_write_cram(0, title_pal, 16);
    {
        u16 sr = irq_off();
        for (y = 0; y < 28; y++) {
            vdp_set_vram_addr(VRAM_PLANE_A0 + y * 64);
            for (x = 0; x < 32; x++) VDP_DATA = title_map[y * 32 + x];
            vdp_set_vram_addr(VRAM_PLANE_B + y * 64);
            for (x = 0; x < 32; x++) VDP_DATA = TILE_BLANK;
        }
        irq_restore(sr);
    }
    vdp_set_reg(2, VRAM_PLANE_A0 >> 10);        /* plane A = $C000 */
    vdp_set_reg(18, 0x00);                      /* no window on the title */
    vdp_display(1);
}

/* the 3-button pad note on the title screen: two full-width lines */
/* show = 1 writes it on plane B rows 24-25, 0 blanks those rows */
#define PAD3_ROW 24
static void pad3_note(u8 show)
{
    u8 r, x;
    if (show) {
        put_text(VRAM_PLANE_B, 0, PAD3_ROW, "SIX BUTTON CONTROLLER PREFERRED", NT_PRIO | NT_PAL(PAL_DLG));
        put_text(VRAM_PLANE_B, 3, PAD3_ROW + 1, "PRESS A+B+C+START TO BEGIN", NT_PRIO | NT_PAL(PAL_DLG));
        return;
    }
    for (r = 0; r < 2; r++)
        for (x = 0; x < 32; x++) vdp_put_nt(VRAM_PLANE_B, x, PAD3_ROW + r, TILE_BLANK);
}

#define PAD3_BEGIN (PAD_A | PAD_B | PAD_C | PAD_START)  /* held together on a 3-button pad */

/* the title's menu box: the texts items[0..n-1] (TX_*), the cursor on
   line cur */
static void title_menu_draw(const u8 *items, u8 n, u8 cur)
{
    const char *t[3];
    u8 i;
    for (i = 0; i < n; i++) t[i] = gtext(items[i]);
    menu_draw("", t, n, cur, 1, 0);
}

/* -> 1 continue the saved game, 0 a fresh game */
/* The title, its music, then "press start"; then a small menu at the
   bottom (a 5-row box from row 23): CONTINUE (only if a save exists),
   new-game and options items. */
static u8 title_screen(void)
{
    static const u8 ITEMS_C[3] = {TX_CONTINUE_ITEM, TX_NEW_GAME, TX_OPTIONS_ITEM};    /* (the menu's texts) */
    const u8 *items;
    u8 n, cur = 0, has_save, noted = 0;
    ui = UI_TITLE;
    show_title();
    music_play(MUS_TITLE);
    box_y = 23;
    box_h = 5;
    /* "press start"; with a 3-button pad, A+B+C+Start held together */
    for (;;) {
        u16 e;
        eng_idle = 1;
        eng_mode = ui;
        wait_vblank();
        cur_pad = pad_read();
        e = cur_pad & ~prev_pad;
        prev_pad = cur_pad;
        if (!pad_six) {
            if (!noted) {
                pad3_note(1);
                noted = 1;
            }
            if (e && (cur_pad & PAD3_BEGIN) == PAD3_BEGIN) break;
            continue;
        }
        if (noted) {
            pad3_note(0);
            noted = 0;
        }
        if (e & (PAD_START | PAD_A)) break;
    }
    if (noted) pad3_note(0);
    eng_turn_seq++;
    sfx(SFX_SELECT);
    has_save = game_saved();                    /* a valid save in battery RAM? */
    items = has_save ? ITEMS_C : ITEMS_C + 1;   /* (without CONTINUE) */
    n = has_save ? 3 : 2;
    title_menu_draw(items, n, cur);
    for (;;) {
        u16 e = next_edge();
        if (e & PAD_UP) cur = cur ? cur - 1 : n - 1;
        else if (e & PAD_DOWN) cur = cur + 1 < n ? cur + 1 : 0;
        else if (e & (PAD_A | PAD_START)) {
            u8 it = items[cur];
            if (it == TX_OPTIONS_ITEM) {                 /* OPTIONS: in a full-size box */
                box_hide();
                box_y = BOX_Y;
                box_h = DLG_H;
                options_screen();
                show_title();                   /* (redraw it under the box) */
                box_y = 23;
                box_h = 5;
                ui = UI_TITLE;
            } else {
                eng_turn_seq++;
                box_hide();
                box_y = BOX_Y;
                box_h = DLG_H;
                sfx(SFX_SELECT);
                return it == TX_CONTINUE_ITEM;
            }
        }
        title_menu_draw(items, n, cur);
        eng_turn_seq++;
    }
}

/* set up the game screen (left blanked: main turns the display on once
   the first picture is up) */
static void game_screen(void)
{
    vdp_display(0);
    view_planes();                              /* name tables */
    load_ui_tiles();                            /* font, icons, palettes 2, 3 */
    update_backdrop(1);                         /* floor and ceiling tiles */
    vdp_set_reg(18, 0x95);                      /* window: rows 21.. */
    hud_init();                                 /* status bar background */
    render_reset();                             /* the title used the view's VRAM */
    vdp_raster(1);                              /* H interrupts on */
}

/* begin playing from S as it is: message msg, then the first picture and
   status bar (after_action, with no move to animate) */
static void start_play(const char *msg)
{
    am_map = 0xFF;                              /* (a game begun or loaded: the automap starts empty) */
    old_x = S.px;
    old_z = S.pz;
    old_f = S.facing;
    msg_clear();
    say(msg);
    ui = UI_PLAY;
    after_action();
}

/* a map has just started: save it (continue, try again) */
static void map_started(void)
{
    game_save();                                /* the automatic save */
    update_backdrop(1);                         /* the new map's colours */
    render_reset();
    start_play(map_title(S.map));               /* the map's title as the message */
}

/* a fresh game: the intro tune, the rules' fresh game, its first map */
static void new_game(void)
{
    music_play(MUS_INTRO);
    game_init();
    map_started();
}

/* through the exit: the next map */
static void next_map(void)
{
    box_hide();
    map_change(S.exit & 0xFF);                  /* rules.s: load the target map */
    music_play(MUS_INTRO);
    map_started();
}

/* after dying: the last save again (made when the map started, or later) */
static void try_again(void)
{
    if (!game_load()) game_init();              /* (no valid save: a fresh game) */
    update_backdrop(1);
    render_reset();
    start_play(map_title(S.map));
}

/* ---- the vendor ---- */
/* (STORE_LINES in rules.s gives each line's name tag and price; a tag's
   name is the text TX_STORE_0 + tag, "" for tags not sold) */
static u8 store_cur, store_n;           /* cursor line; lines this vendor has */
static u8 store_res;                   /* the last purchase's result (store_buy's), 0xFF none: the footer */

/* the vendor's box: "CREDITS n" as title, a line per item (name, price at
   column 17) from row 2, and the footer. (Up to 16 lines; more than 12
   would run into the footer's row, more than 14 past the box.) */
static void store_draw(void)
{
    static char lines[16][21];
    const char *items[16];
    const u8 *t = STORE_LINES + S.store * 32;  /* this vendor's 16 (tag, price) pairs */
    char *d;
    u8 i, k;
    for (i = 0; i < store_n; i++) {
        const char *nm = gtext(TX_STORE_0 + t[i * 2]);
        char *p;
        d = lines[i];
        for (k = 0; k < 20; k++) d[k] = ' ';
        d[20] = 0;
        for (k = 0; nm[k] && k < 20; k++) d[k] = nm[k];   /* (cut at the line's end) */
        p = utoa10(t[i * 2 + 1]);
        k = 17;
        while (*p) d[k++] = *p++;
        items[i] = d;
    }
    d = cat(title_buf, gtext(TX_CREDITS_SP));
    cat(d, utoa10(S.credits));
    menu_draw(title_buf, items, store_n, store_cur, 2,
              store_res == 0xFF ? "A:BUY  B:LEAVE" : store_res ? gtext(TX_NO_CREDITS + store_res - 1) : "BOUGHT");
}

/* open vendor S.store (the rules set it when the player used one) */
static void store_open(void)
{
    const u8 *t = STORE_LINES + S.store * 32;
    store_n = 0;
    while (store_n < 16 && t[store_n * 2]) store_n++;      /* tag 0 ends the list */
    store_cur = 0;
    store_res = 0xFF;
    ui = UI_STORE;
    sfx(SFX_DIALOG);
    store_draw();
}

/* a button in the vendor's box: B / Start leave (the turn goes on), Up /
   Down move, A buys the line under the cursor */
static void store_input(u16 e)
{
    store_res = 0xFF;
    if (e & (PAD_B | PAD_START)) {
        close_box();
        store_close();                  /* rules.s: the rest of the turn */
        after_action();
        return;
    }
    if (e & PAD_UP) store_cur = store_cur ? store_cur - 1 : store_n - 1;
    else if (e & PAD_DOWN) store_cur = store_cur + 1 < store_n ? store_cur + 1 : 0;
    else if (e & PAD_A) {
        u8 r = store_buy(store_cur);    /* rules.s: 0 bought, 1 too poor, 2 too many */
        store_res = r;                  /* (the footer: TX_NO_CREDITS, TX_TOO_MANY or "BOUGHT") */
        sfx(r ? SFX_DENIED : SFX_PICKUP);
        hud_update();                   /* credits, ammo, medkits changed */
        store_draw();
        return;
    }
    sfx(SFX_BLIP);
    store_draw();
}

/* ---- items: the Berserker and the collar ---- */
static u8 items_cur;                    /* 0 the Berserker, 1 the collar */

/* the items box: each item and how many (S.inventory[3], [4]) at column 15 */
static void items_draw(void)
{
    static char lines[2][21];
    const char *items[2];
    const char *nm;
    u8 i, k;
    for (i = 0; i < 2; i++) {
        char *d = lines[i], *p = utoa10(S.inventory[3 + i]);
        for (k = 0; k < 20; k++) d[k] = ' ';
        d[20] = 0;
        nm = gtext(TX_BERSERKER_ITEM + i);   /* (the Berserker, the collar) */
        for (k = 0; nm[k] && k < 20; k++) d[k] = nm[k];   /* (cut at the line's end) */
        k = 15;
        while (*p) d[k++] = *p++;
        items[i] = d;
    }
    menu_draw("ITEMS", items, 2, items_cur, 4, "A:USE  B:BACK");
}

/* ---- the automap's box: the map around the player, a character a
   cell, north up (core/automap.c) ---- */
#define MAP_W (DLG_W - 2)               /* cells across (columns 1..22) */
#define MAP_H (DLG_H - 4)               /* cells down (rows 2..13) */
static void map_draw(void)
{
    static char row[MAP_W + 1];
    u8 r;
    box_clear();
    box_put(1, (DLG_W - 3) >> 1, "MAP");
    for (r = 0; r < MAP_H; r++) {
        am_row(row, r, MAP_W, MAP_H, S.px, S.pz, S.facing);
        box_put(2 + r, 1, row);
    }
    box_footer("B:BACK");
    box_show();
}

/* ---- the cheats (the pause menu's CHEATS): god mode and infinite
   ammunition (CHEAT_* bits of cheats, checked by the rules), every weapon
   with full ammunition, every keycard. Off at power-on and after QUIT TO
   TITLE; not saved. ---- */
#define N_CHEATS 4
static const char * const CHEAT_ITEMS[N_CHEATS] = {"GOD MODE", "INFINITE AMMO", "ALL WEAPONS", "ALL KEYCARDS"};
static u8 cheat_cur;                    /* the line chosen */
static void cheats_draw(void)
{
    u8 i;
    box_clear();
    box_put(1, (DLG_W - 6) >> 1, "CHEATS");
    for (i = 0; i < N_CHEATS; i++) {
        box_put(3 + i, 2, cheat_cur == i ? ">" : " ");
        box_put(3 + i, 4, CHEAT_ITEMS[i]);
    }
    box_put(3, 19, (cheats & CHEAT_GOD) ? "ON" : "OFF");
    box_put(4, 19, (cheats & CHEAT_AMMO) ? "ON" : "OFF");
    box_footer("A:SELECT  B:BACK");
    box_show();
}

static void pause_draw(void);

/* a press in the cheats box: Up / Down move, A picks, B / Start back to
   the pause menu */
static void cheats_input(u16 e)
{
    if (e & (PAD_B | PAD_START)) {
        ui = UI_PAUSE;
        sfx(SFX_BLIP);
        pause_draw();
        return;
    }
    if (e & PAD_UP) cheat_cur = cheat_cur ? cheat_cur - 1 : N_CHEATS - 1;
    else if (e & PAD_DOWN) cheat_cur = cheat_cur < N_CHEATS - 1 ? cheat_cur + 1 : 0;
    else if (e & PAD_A) {
        switch (cheat_cur) {
        case 0:
            cheats ^= CHEAT_GOD;
            if (cheats & CHEAT_GOD) S.hp = S.maxhp;     /* (and the health topped up) */
            break;
        case 1:
            cheats ^= CHEAT_AMMO;
            break;
        case 2:
            cheat_give(0);
            break;
        default:
            cheat_give(1);
            break;
        }
        sfx(SFX_ITEM);
        hud_update();
        cheats_draw();
        return;
    }
    sfx(SFX_BLIP);
    cheats_draw();
}

/* ---- pause menu ---- */
#define N_PAUSE 8
static const char * const PAUSE_ITEMS[N_PAUSE] = {"RESUME", "MAP", "ITEMS", 0, 0, 0, "CHEATS", "QUIT TO TITLE"};
static const u8 PAUSE_TEXTS[N_PAUSE] = {0, 0, 0, TX_NOTEBOOK_ITEM, TX_SAVE_GAME, TX_OPTIONS_ITEM, 0, 0};   /* (for the 0s above: the game's texts) */
static u8 pause_cur;                    /* cursor: index in PAUSE_ITEMS */

/* the pause box: the items, the authors' line, the footer */
static void pause_draw(void)
{
    u8 i;
    box_clear();
    box_put(1, (DLG_W - 6) >> 1, "PAUSED");
    for (i = 0; i < N_PAUSE; i++) {
        box_put(3 + i, 2, pause_cur == i ? ">" : " ");
        box_put(3 + i, 4, PAUSE_ITEMS[i] ? PAUSE_ITEMS[i] : gtext(PAUSE_TEXTS[i]));
    }
    box_put(12, (DLG_W - 10) >> 1, "MADE BY AI");      /* (row 11 blank, after the 8 items) */
    box_put(13, (DLG_W - 23) >> 1, "DIRECTED BY UNFNKNBLVBL");
    box_footer("A:SELECT  START:RESUME");
    box_show();
}

/* -> 1 quit to the title */
/* a button in the pause menu: Start / B resume; Up / Down move; A picks */
static u8 pause_menu(u16 e)
{
    if (e & (PAD_START | PAD_B)) {
        close_box();
        return 0;
    }
    if (e & PAD_UP) pause_cur = pause_cur ? pause_cur - 1 : N_PAUSE - 1;
    else if (e & PAD_DOWN) pause_cur = pause_cur < N_PAUSE - 1 ? pause_cur + 1 : 0;
    else if (e & PAD_A) {
        switch (pause_cur) {
        case 0:
            close_box();
            return 0;
        case 1:                         /* MAP */
            ui = UI_MAP;
            sfx(SFX_BLIP);
            map_draw();
            return 0;
        case 2:                         /* ITEMS */
            ui = UI_ITEMS;
            items_cur = 0;
            sfx(SFX_BLIP);
            items_draw();
            return 0;
        case 3:                         /* NOTEBOOK: the notes' string ids */
            box_hide();
            if (S.n_notes) open_pager(UI_NOTES, S.notes, S.n_notes);
            else {
                ui = UI_PLAY;
                say("NO NOTES YET");
            }
            return 0;
        case 4:                         /* the save item */
            game_save();
            close_box();
            say(gtext(TX_GAME_SAVED));
            sfx(SFX_SELECT);
            return 0;
        case 5:                         /* OPTIONS */
            box_hide();
            options_screen();
            ui = UI_PLAY;
            refresh_picture();                  /* the movement mode may have changed */
            ui = UI_PAUSE;
            pause_draw();
            return 0;
        case 6:                         /* CHEATS */
            ui = UI_CHEATS;
            cheat_cur = 0;
            sfx(SFX_BLIP);
            cheats_draw();
            return 0;
        default:                        /* QUIT TO TITLE (the cheats off) */
            cheats = 0;
            box_hide();
            return 1;
        }
    }
    sfx(SFX_BLIP);
    pause_draw();
    return 0;
}

/* ------------------------------------------------------------ input */
/* nonzero if the button of action a was just pressed */
#define ACT(a) (edge & action_bits(a))

/* -> 1 to go back to the title */
/* One new button press (edge), by mode: the screens and boxes first; in
   play, Start pauses, a 3-button pad's C + direction, then the notebook,
   the D-pad and the actions. Every game action is followed by
   after_action. */
static u8 handle(u16 edge)
{
    msg_clear();
    switch (ui) {
    case UI_DEAD:
        if (edge & PAD_START) {
            box_hide();
            return 1;
        }
        if (edge & PAD_A) {
            box_hide();
            try_again();                /* the last save again */
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
            items_cur ^= 1;             /* (two items) */
            sfx(SFX_BLIP);
            items_draw();
            return 0;
        }
        if (edge & PAD_A) {
            close_box();
            old_x = S.px;               /* (nothing to animate) */
            old_z = S.pz;
            old_f = S.facing;
            if (use_item(3 + items_cur)) sfx(SFX_ITEM);     /* rules.s: inventory slot 3 or 4 */
            after_action();
        }
        return 0;
    case UI_PAUSE:
        return pause_menu(edge);
    case UI_CHEATS:
        cheats_input(edge);
        return 0;
    case UI_MAP:
        if (edge & (PAD_A | PAD_B | PAD_START)) {
            ui = UI_PAUSE;
            sfx(SFX_BLIP);
            pause_draw();
        }
        return 0;
    case UI_NOTES:
        if (((edge & PAD_A) && pager_next()) || (edge & (PAD_B | PAD_START))) close_box();
        return 0;
    case UI_DIALOG:
        if ((edge & PAD_A) && pager_next()) {
            close_box();
            resume(0);                  /* rules.s: the event goes on */
            after_action();
        }
        return 0;
    case UI_PASSWORD:
        /* Up / Down change the digit (wrapping), Right keeps it and starts
           the next (up to CODE_MAX - 1 kept, the last one is the cursor's),
           Left takes the last kept digit back */
        if (edge & PAD_UP) code_digit = (code_digit + 1) % 10;
        if (edge & PAD_DOWN) code_digit = (code_digit + 9) % 10;
        if ((edge & PAD_RIGHT) && code_len < CODE_MAX - 1) {
            code[code_len++] = '0' + code_digit;
            code_digit = 0;
        }
        if ((edge & PAD_LEFT) && code_len) code_digit = code[--code_len] - '0';
        if (edge & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)) sfx(SFX_BLIP);
        if (edge & PAD_A) {
            /* the code is the kept digits plus the one under the cursor */
            code[code_len] = '0' + code_digit;
            code[code_len + 1] = 0;
            close_box();
            resume(code);               /* rules.s: right or wrong code */
            after_action();
            return 0;
        }
        if (edge & PAD_B) {             /* cancelled: no code */
            close_box();
            resume(0);
            after_action();
            return 0;
        }
        password_draw();
        return 0;
    }
    /* UI_PLAY */
    old_x = S.px;                       /* where the move starts (animate_move) */
    old_z = S.pz;
    old_f = S.facing;
    if (edge & PAD_START) {
        ui = UI_PAUSE;
        pause_cur = 0;
        sfx(SFX_DIALOG);
        pause_draw();
        return 0;
    }
    if (!pad_six && (cur_pad & PAD_C)) {
        /* 3-button pad: C + a direction */
        if (edge & PAD_DOWN) {
            if (S.n_notes) open_pager(UI_NOTES, S.notes, S.n_notes);
            else say("NO NOTES YET");
            return 0;
        }
        if (edge & PAD_UP) act_health_item();       /* rules.s calls, one turn each */
        else if (edge & PAD_LEFT) act_cycle_weapon(-1);
        else if (edge & PAD_RIGHT) act_cycle_weapon(1);
        else if (ACT(ACT_USE)) act_use();
        else if (ACT(ACT_PASS)) act_pass();
        else return 0;
        after_action();
        return 0;
    }
    if (ACT(ACT_NOTES)) {
        if (S.n_notes) open_pager(UI_NOTES, S.notes, S.n_notes);
        else say("NO NOTES YET");
        return 0;
    }
    /* one action a press (the first that matches), each a turn in rules.s */
    if (edge & PAD_UP) act_move(0);
    else if (edge & PAD_DOWN) act_move(1);
    else if (edge & PAD_LEFT) act_turn(-1);
    else if (edge & PAD_RIGHT) act_turn(1);
    else if (ACT(ACT_USE)) act_use();
    else if (ACT(ACT_PASS)) act_pass();
    else if (ACT(ACT_MEDKIT)) act_health_item();
    else if (ACT(ACT_PREV)) act_cycle_weapon(-1);
    else if (ACT(ACT_NEXT)) act_cycle_weapon(1);
    else return 0;
    after_action();
    return 0;
}

/* the program (crt0.s jumps here after reset): set-up, then forever the
   title and a game until the player quits to the title */
int main(void)
{
    pack_init(0);                               /* map 0's tables (the title needs none, the
                                                   area colours and the weapons are found) */
    vdp_init();
    pad_init();
    sound_init();                               /* PSG quiet, YM2612 set up */
    settings_load();                            /* from battery RAM, or the defaults */
    __asm__ volatile("move #0x2000, %sr");      /* interrupts on (mask 0) */
    load_ui_tiles();
    for (;;) {
        u8 cont = title_screen();               /* 1: CONTINUE */
        game_screen();
        if (cont && game_load()) {              /* the save into S */
            music_stop();
            update_backdrop(1);
            start_play(gtext(TX_GAME_LOADED));
        }
        else
            new_game();
        while (render_busy()) {                 /* (the picture's flip pending) */
        }
        vdp_display(1);                         /* only once the first picture is up */
        for (;;) {
            u16 edge = next_edge();             /* wait for a press */
            u8 quit;
            sfx_flush();                        /* drop last turn's queued effects */
            quit = handle(edge);
            eng_turn_seq++;                     /* (tests: this input is done) */
            if (quit) break;
        }
    }
    return 0;
}

