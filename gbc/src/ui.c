/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the Game Boy Color: the player-facing side - the SNES port's
 * snes/src/ui.c with the Game Boy's screen (gfx.c): the same states, menus,
 * pages of text and button handling (so the SNES game's recorded runs play
 * here move for move: the tests), drawn on the Game Boy's 160 x 144.
 *
 * The pad is the SNES's bits (pad.c turns the Game Boy's buttons into them):
 *   D-pad                    step forward / back, turn
 *   A                        use / attack (SNES A)
 *   B                        pass the turn (SNES B)
 *   SELECT + A               medkit (SNES Y)
 *   SELECT + B               notebook (SNES X)
 *   SELECT + LEFT / RIGHT    previous / next weapon (SNES L / R)
 *   START                    pause menu
 *   SELECT + START           the SNES's SELECT (an action may be set to it)
 * (The options screen can move the actions to other buttons: cfg.key.)
 *
 * Screen: row 0 messages, rows 1..12 the 3D view (128 x 88, in the middle,
 * in 12 rows of tiles), rows 13..17 the face and the status bar; boxes of
 * text on the window.
 *
 * Structure: ui_main loops over the title (title_screen) and a game; in a
 * game, next_edge waits for a new button press and handle() acts on it
 * according to the screen state ui (UI_*): the core's act_* functions
 * (core/rules.c, through core_api.h) play a turn, after_action shows the
 * result (picture, status bar, end screens, dialogs). The core reports to
 * ui.c through globals (S the game state, dialog_str, pw_want, status_msg,
 * player_fired, player_hurt ...). save.c (the SNES port's, banked) does the
 * saves and the settings (cfg).
 * Licence: GPL-2.0-or-later.
 */
#pragma bank 255
#include "plat.h"
#include "gfx.h"
#include "save_api.h"
#include "sound.h"

/* the songs (snd.c: the sound blob's three) */
#define MUS_TITLE 0
#define MUS_INTRO 1
#define MUS_END 2

/* read by the tests through the link map (the SNES's tools/snes_play.py;
   on the Game Boy, pad.c's script mode): eng_turn_seq counts handled
   presses, eng_idle is 1 while the game waits for a button, eng_mode is
   ui (UI_*) */
volatile u8 eng_turn_seq, eng_idle, eng_mode;

/* pad bits: the SNES's ($4218) */
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

/* ------------------------------------------------------------ messages */
static u8 msg_timer;                  /* frames the current message stays before the next */
static char menu_lines[16][21];       /* (the options, the vendor, the items: one at a time) */

/* a frame of the message line: the core's status message queued (say,
   plat.c), and when the shown one's time is up the next shown for 45
   frames */
static void msg_tick(void)
{
    if (msg_timer) msg_timer--;
    if (status_new) {
        status_new = 0;
        if (status_msg[0]) say(status_msg);
    }
    if (msg_timer == 0 && msg_count) {
        gfx_message(msgq[msg_head]);    /* (onto the message line) */
        msg_head = (msg_head + 1) & (MSGQ - 1);
        msg_count--;
        msg_timer = 45;
    }
}

/* the waiting messages dropped (a new press) */
static void msg_clear(void)
{
    msg_count = 0;
    msg_timer = 0;
}

/* ------------------------------------------------------------ status bar: 5 lines of 20 */
static char hud_new[5][21];             /* the status bar as it should be (gfx.c draws what changed) */
static u8 cur_face = 0xFF, hurt_face;   /* the face shown (255: none); 1 after a hit this turn */
u8 grin_frames;                         /* frames of the grin left (plat.c's sfx starts it) */

/* s into the status bar's line row from column col (clipped at 20) */
static void hud_text(u8 row, u8 col, const char *s)
{
    while (*s && col < 20) hud_new[row][col++] = *s++;
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

/* face f (0..3 by health, 4 hurt, 8 grin) shown if it is not already */
static void set_face(u8 f)
{
    if (f == cur_face) return;
    cur_face = f;
    gfx_face(f);                        /* (its tiles to VRAM) */
}

/* the face as the original picks it: the grin just after a pick-up, the
   grimace when hurt, else by health */
static void face_update(void)
{
    u8 hp = S.hp, mx = S.maxhp;
    if (hp && grin_frames) set_face(8);
    else if (hp && hurt_face) set_face(4);
    else set_face(hp <= mx / 4 ? 3 : hp <= mx / 3 ? 2 : hp <= mx / 2 ? 1 : 0);
}

/* the status bar rebuilt from S (health, armour, weapon and ammo, level
   and XP, credits and keys, medkits and soul spheres) and drawn; the face
   too */
static void hud_update(void)
{
    u8 c, r;
    for (r = 0; r < 5; r++) {
        for (c = 0; c < 20; c++) hud_new[r][c] = ' ';
        hud_new[r][20] = 0;
    }
    hud_text(0, 0, "HEALTH");
    c = hud_num(0, 7, S.hp);
    hud_text(0, c, "/");
    hud_num(0, c + 1, S.maxhp);
    hud_text(0, 15, "AR");             /* (from 15: the health may be 999/999) */
    hud_num(0, 17, S.armor);

    hud_text(1, 0, wpn_name[S.weapon]);
    if (wpn_use[S.weapon]) {            /* (a weapon that uses ammunition) */
        hud_text(1, 13, "AMMO");
        hud_num(1, 18, S.ammo[wpn_ammo[S.weapon]]);
    }
    hud_text(2, 0, "LEVEL");
    hud_num(2, 6, S.level);
    hud_text(2, 9, "XP");
    c = hud_num(2, 12, G16(S.xp));      /* (G16: a big-endian u16 field of S) */
    hud_text(2, c, "/");
    hud_num(2, c + 1, G16(S.nextxp));

    hud_text(3, 0, "CREDITS");
    hud_num(3, 8, G16(S.credits));
    hud_text(3, 13, "KEY");
    if (S.keys & 1) hud_text(3, 16, "G");  /* (key bits: green, yellow, blue, red) */
    if (S.keys & 2) hud_text(3, 17, "Y");
    if (S.keys & 4) hud_text(3, 18, "B");
    if (S.keys & 8) hud_text(3, 19, "R");

    hud_text(4, 0, "MEDKITS");
    hud_num(4, 8, S.inventory[0]);      /* (small medkits + large ones) */
    hud_text(4, 10, "+");
    hud_num(4, 11, S.inventory[1]);
    if (S.inventory[2]) {               /* (soul spheres) */
        hud_text(4, 14, "SOUL");
        hud_num(4, 19, S.inventory[2]);
    }
    gfx_hud(hud_new);                   /* (the changed lines drawn) */
    face_update();
}

/* ------------------------------------------------------------ the weapon */
static s8 wp_cur = -1;                  /* the weapon frame shown (2 w + firing; -1 none) */

/* the weapon's sprites hidden */
static void weapon_hide(void)
{
    gfx_weapon_off();
}

/* weapon w shown (its firing frame if firing); w past 8 hides it */
static void weapon_show(u8 w, u8 firing)
{
    s8 id = (s8)(w * 2 + (firing ? 1 : 0));
    if (w > 8) {
        weapon_hide();
        return;
    }
    if (id == wp_cur) return;
    wp_cur = id;
    gfx_weapon(w, firing);
}

/* ------------------------------------------------------------ the picture */
static u8 shown_map = 0xFF;             /* the map last drawn */
static u8 cur_floor[3] = {0xFF, 0, 0}, cur_ceil[3], cur_berserk = 0xFF;    /* the backdrop's colours as set */

/* the floor and ceiling colours (S.floor_rgb, S.ceil_rgb, 8-bit RGB) and
   the berserk tint sent to gfx.c when they changed (or force) */
static void update_backdrop(u8 force)
{
    u8 b = S.berserk != 0;
    if (force || b != cur_berserk || cur_floor[0] != S.floor_rgb[0] || cur_floor[1] != S.floor_rgb[1] ||
        cur_floor[2] != S.floor_rgb[2] || cur_ceil[0] != S.ceil_rgb[0] || cur_ceil[1] != S.ceil_rgb[1] ||
        cur_ceil[2] != S.ceil_rgb[2]) {
        u8 k;
        for (k = 0; k < 3; k++) {
            cur_floor[k] = S.floor_rgb[k];
            cur_ceil[k] = S.ceil_rgb[k];
        }
        cur_berserk = b;
        gfx_backdrop(S.floor_rgb, S.ceil_rgb, b);
    }
}

/* the 3D view drawn again */
static void refresh_picture(void)
{
    if (shown_map != S.map) shown_map = S.map;
    gfx_view();                         /* (view.c draws it, gfx.c shows it) */
}

/* ------------------------------------------------------------ boxes */
#define BOX_Y 2                         /* (the SNES's box position: unused by gfx.c) */
#define DLG_W 24                        /* a box line's characters */
#define DLG_H 16                        /* a box's lines at most */
#define BOX_LINES 13                    /* the text lines a page of a dialog shows */
/* the screen states (ui): what a press does (handle) */
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
#define MAX_NOTES 16                    /* the pager's strings at most */

static void next_map(void);

static u8 ui;                           /* the screen state (UI_*) */
static u8 box_str[MAX_NOTES], box_n, box_i, box_line;  /* the pager: its strings, how many, the one shown, its first line */
#define CODE_MAX 8                      /* a door code's digits at most */
static char code[CODE_MAX + 1];         /* the code entered so far */
static u8 code_len, code_digit;         /* its digits, the digit being chosen */
static char box[DLG_H][DLG_W];          /* the box's text (0: past its lines) */
static u8 box_y = BOX_Y, box_h = DLG_H, box_up;     /* the box's place (unused), its lines, whether it is up */

/* the box emptied: box_h lines of spaces, the rest 0 */
static void box_clear(void)
{
    u8 r, c;
    for (r = 0; r < DLG_H; r++)
        for (c = 0; c < DLG_W; c++) box[r][c] = r < box_h ? ' ' : 0;
}

static const char *key_label(u8 tok);   /* (with the options, below) */

/* s into the box at (row, col), up to its end or a '|' (the strings'
   line break), cut at the box's edge. A button token in s (a byte below
   32: megadrive/tools/key_text.py puts one in the maps' strings where the
   original named a phone key) is written as the name of the button that
   does that action now (key_label). */
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

/* the box shown (the weapon hidden the first time) */
static void box_show(void)
{
    if (!box_up) {
        weapon_hide();
        wp_cur = -1;
    }
    gfx_box(box, box_y, box_h);         /* (drawn on the window) */
    box_up = 1;
}

/* the box gone */
static void box_hide(void)
{
    gfx_box_off();
    box_up = 0;
}

static char gen_text[48];               /* a dialog made here ("YOU CAPTURED ...") */

/* dialog id's text: the map's string id, or for DLG_CAPTURED one made
   with the captured monster's name */
static const char *dialog_text(u8 id)
{
    if (id == DLG_CAPTURED) {
        const char *a = "YOU CAPTURED|THE ", *b = def_name(captured_def);    /* (the core's name of the monster) */
        char *d = gen_text;
        while (*a) *d++ = *a++;
        while (*b && d < gen_text + 44) *d++ = *b++;
        *d++ = '!';
        *d = 0;
        return gen_text;
    }
    return level_string(id);            /* (plat.c: from the map's strings) */
}

/* string id's lines from line first into the box from row, at column 3;
   returns 1 when lines are left over (more than BOX_LINES) */
static u8 box_string(u8 id, u8 first, u8 row)
{
    const char *s = dialog_text(id);
    u8 line = 0;
    while (*s) {
        if (line >= first) {
            if (row >= 1 + BOX_LINES) return 1;
            box_put(row++, 3, s);
        }
        while (*s && *s != '|') s++;
        if (*s == '|') s++;
        line++;
    }
    return 0;
}

/* s centred on the box's second-last line */
static void box_footer(const char *s)
{
    u8 n = 0;
    const char *p = s;
    while (*p++) n++;
    box_put(box_h - 2, (DLG_W - n) >> 1, s);
}

/* the pager's page: box_str[box_i] from line box_line, A:MORE or A:OK */
static void pager_draw(void)
{
    u8 more;
    box_clear();
    more = box_string(box_str[box_i], box_line, 1);
    box_footer(more || box_i + 1 < box_n ? "A:MORE" : "A:OK");
    box_show();
}

/* the pager opened on n strings (a dialog, the notes), in state mode */
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
   is none left (the caller closes it) */
static u8 pager_next(void)
{
    u8 more;
    box_clear();
    more = box_string(box_str[box_i], box_line, 1);     /* (does this page leave lines over?) */
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

/* the door code's box: the prompt (the map's string pw_prompt), the
   digits so far, the digit being chosen, '_' for the rest */
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

/* the door code's box opened (the core asked: pw_want) */
static void open_password(void)
{
    code_len = 0;
    code_digit = 0;
    ui = UI_PASSWORD;
    password_draw();
}

/* a full box with a title, two lines and a footer (death, a sector done,
   the end) */
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

/* the map's enemies killed; *total gets how many there are (entities
   whose definition is an enemy) */
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

static char line1[25], line2[25], title_buf[32];    /* screen_box's and the menus' texts */

/* a appended at d; returns the end (its NUL) */
static char *cat(char *d, const char *a)
{
    while (*a) *d++ = *a++;
    *d = 0;
    return d;
}

/* line1 "KILLS k/t", line2 "LEVEL l  CREDITS c" */
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

/* after a turn that killed the player or used an exit: the death box, the
   end, a sector's "DONE" box, or straight on to the next map (S.exit:
   byte 0 bit 7 a completed sector, byte 3 the next map, 255 the end) */
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
        if (S.exit[3] == 0xFF) {                /* the end */
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

/* what a turn's result shows: the firing frame for 8 frames, the red flash
   and the hurt face, the backdrop, the picture, the status bar; then the
   end screens, or whatever the core asks for next (a dialog, a door code,
   the vendor), else play */
static void after_action(void)
{
    if (player_fired) {
        u8 i;
        player_fired = 0;
        weapon_show(S.weapon, 1);       /* (the firing frame) */
        for (i = 0; i < 8; i++) wait_vblank();  /* (held 8 frames) */
    }
    if (player_hurt) {
        player_hurt = 0;
        gfx_flash(6);                   /* (6 frames of red) */
        hurt_face = 1;
    } else
        hurt_face = 0;
    update_backdrop(0);                 /* (berserk or the map's colours may have changed) */
    refresh_picture();
    hud_update();
    if (S.dead || S.exit_set) {
        weapon_hide();
        end_screens();
        return;
    }
    weapon_show(S.weapon, 0);
    if (dialog_str != 0xFF) open_pager(UI_DIALOG, &dialog_str, 1);    /* (255: none, for each of these) */
    else if (pw_want != 0xFF) open_password();
    else if (S.store != 0xFF) store_open();
    else ui = UI_PLAY;
}

/* back to play: the box gone, the weapon back */
static void close_box(void)
{
    ui = UI_PLAY;
    box_hide();
    weapon_show(S.weapon, 0);
}

/* ------------------------------------------------------------ input */
static u16 cur_pad, prev_pad;           /* the pad this frame and the last */

/* frames pass (messages, the grin's end) until a button goes down; returns
   the buttons newly pressed. eng_idle is 1 meanwhile (the tests wait for it) */
static u16 next_edge(void)
{
    for (;;) {
        u16 edge;
        eng_idle = 1;
        eng_mode = ui;
        wait_vblank();                  /* (the frame: VRAM jobs, the pad) */
        if (grin_frames) grin_frames--;
        msg_tick();
        cur_pad = pad;
        edge = cur_pad & ~prev_pad;     /* (pressed now, not before) */
        prev_pad = cur_pad;
        if (ui == UI_PLAY && cur_face == 8 && !grin_frames) face_update();     /* the grin is over */
        if (edge) {
            eng_idle = 0;
            return edge;
        }
    }
}

/* ---- menus ---- */
/* a menu: the title centred on line 1, n items from line row0 with '>'
   at cur, an optional footer */
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

/* ---- options: the button of every action (the Game Boy's names for the SNES's buttons) ---- */
#define N_BUTTONS 7                     /* the buttons an action can have (cfg.key: an index here) */
static const u16 BUTTON_BITS[N_BUTTONS] = {PAD_A, PAD_B, PAD_X, PAD_Y, PAD_L, PAD_R, PAD_SELECT};
static const char * const BUTTON_NAMES[N_BUTTONS] = {"A", "B", "SEL+B", "SEL+A", "SEL+LEFT", "SEL+RIGHT",
                                                     "SEL+START"};
static const char * const ACT_NAMES[N_ACTIONS] = {"USE/ATTACK", "PASS TURN", "MEDKIT", "PREV WEAPON",
                                                  "NEXT WEAPON", "NOTEBOOK"};

/* the button tokens in the maps' strings (megadrive/tools/key_text.py):
   KEY_ACT + an action (ACT_*), or KEY_MENU for the pause menu's button */
#define KEY_ACT 1
#define KEY_MENU 16

/* the name of the button token tok stands for (box_put): the button its
   action is on now (cfg.key, as the options screen set it; on the GBA the
   build's names, with L and R), START for the pause menu, "?" for a token
   this game has no action for (the Mega Drive's full detail) */
static const char *key_label(u8 tok)
{
    u8 a = tok - KEY_ACT;                       /* the action (ACT_*) */
    if (tok == KEY_MENU) return "START";
    if (a >= N_ACTIONS) return "?";             /* (also tok 0, which wraps round) */
    return BUTTON_NAMES[cfg.key[a]];
}

/* the pad bit of action a (ACT_*) as the settings have it */
static u16 action_bits(u8 a)
{
    return BUTTON_BITS[cfg.key[a]];
}

/* the options' menu: each action and its button, DEFAULTS, DONE */
static void options_draw(u8 cur)
{
    char (*lines)[21] = menu_lines;     /* (one buffer for the menus: the work RAM is short) */
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
        for (k = 0; b[k] && 11 + k < 20; k++) d[11 + k] = b[k];
        items[i] = d;
    }
    menu_draw("OPTIONS", items, 8, cur, 3, cur < N_ACTIONS ? "PRESS A BUTTON TO SET" : "A:CHANGE  START:DONE");
}

/* the options: up / down choose, on an action any of the 7 buttons sets
   it (the action that had that button takes the old one), DEFAULTS, DONE
   or START leave (the settings saved). Returns to the caller's state */
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
            if (b == N_BUTTONS) {       /* (not a button an action can have) */
                eng_turn_seq++;         /* (the press counts as handled for the tests) */
                continue;
            }
        } else if (cur == N_ACTIONS && (e & PAD_A)) settings_defaults();   /* (save.c: the default buttons) */
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
    settings_save();                    /* (save.c: into the cartridge RAM) */
    sfx(SFX_SELECT);
    box_hide();
    ui = saved_ui;
}

/* ---- title ---- */
/* the title screen up (no box, no weapon) */
static void show_title(void)
{
    box_hide();
    weapon_hide();
    wp_cur = -1;
    gfx_title();
}

/* the title: START or A brings the menu (CONTINUE when there is a saved
   game, NEW GAME, OPTIONS); returns 1 for CONTINUE, 0 for NEW GAME */
static u8 title_screen(void)
{
    static const char * const ITEMS_C[3] = {"CONTINUE", "NEW GAME", "OPTIONS"};
    const char * const *items;
    u8 n, cur = 0, has_save;
    ui = UI_TITLE;
    show_title();
    music_play(MUS_TITLE);
    box_y = 13;                         /* (a small box: 5 lines) */
    box_h = 5;
    for (;;) {                          /* (until START or A) */
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
    has_save = game_saved();            /* (save.c: a game in the cartridge RAM?) */
    items = has_save ? ITEMS_C : ITEMS_C + 1;
    n = has_save ? 3 : 2;
    menu_draw("", items, n, cur, 1, 0);
    for (;;) {
        u16 e = next_edge();
        if (e & PAD_UP) cur = cur ? cur - 1 : n - 1;
        else if (e & PAD_DOWN) cur = cur + 1 < n ? cur + 1 : 0;
        else if (e & (PAD_A | PAD_START)) {
            const char *it = items[cur];
            if (it[0] == 'O') {         /* (OPTIONS: in a full-size box, then back) */
                box_hide();
                box_y = BOX_Y;
                box_h = DLG_H;
                options_screen();
                box_y = 13;
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

/* the game screen: the view, the status bar */
static void game_screen(void)
{
    gfx_game();
    cur_face = 0xFF;
    shown_map = 0xFF;
    update_backdrop(1);
}

/* play starts (or goes on) with message msg and the screen brought up to
   date */
static void start_play(const char *msg)
{
    msg_clear();
    say(msg);
    ui = UI_PLAY;
    after_action();
}

/* a map begins: the game saved (save.c), the map's title shown */
static void map_started(void)
{
    game_save();
    update_backdrop(1);
    start_play(map_title(S.map));
}

/* a new game: the core's game_init (the first map) */
static void new_game(void)
{
    music_play(MUS_INTRO);
    game_init();
    map_started();
}

/* through the exit: the core's map_change to S.exit[3] */
static void next_map(void)
{
    box_hide();
    map_change(S.exit[3]);
    music_play(MUS_INTRO);
    map_started();
}

/* after death: the saved game (made at the map's start) loaded, else a
   new game */
static void try_again(void)
{
    if (!game_load()) game_init();
    update_backdrop(1);
    start_play(map_title(S.map));
}

/* ---- the vendor ---- */
/* the vendor's item names, by the core's store item id (STORE_LINES) */
static const char * const STORE_NAMES[17] = {
    "", "", "SM MEDKIT", "LG MEDKIT", "SOUL SPHERE", "BERSERKER", "", "10 HAL. CANS", "10 BULLETS",
    "10 SHELLS", "10 CELLS", "3 ROCKETS", "", "+1 ACCURACY", "+1 AGILITY", "+1 STRENGTH", "+1 DEFENSE"};
static u8 store_cur, store_n;           /* the item chosen, the items */
static const char *store_note;          /* the last purchase's result (0: none) */

/* the vendor's menu: S.store's line in STORE_LINES (16 pairs: item id,
   price; 0 ends), the credits in its title */
static void store_draw(void)
{
    char (*lines)[21] = menu_lines;
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

/* the vendor opened (the core set S.store) */
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

/* a press at the vendor: B / START leave (the core's store_close), up /
   down choose, A buys (the core's store_buy: 0 bought, 1 too few credits,
   2 too many) */
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
        u8 r = store_buy(store_cur);
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

/* the items menu: each with its count (S.inventory[3], [4]) */
static void items_draw(void)
{
    char (*lines)[21] = menu_lines;
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
static u8 pause_cur;                    /* the item chosen */

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

/* a press in the pause menu; returns 1 for QUIT TO TITLE */
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
            game_save();                /* (save.c) */
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

/* the press holds action a's button (handle's edge) */
#define ACT(a) (edge & action_bits(a))

/* a press (edge: the buttons newly down) in screen state ui; in play, a
   turn of the core (act_*) then after_action. Returns 1 to go back to the
   title */
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
            if (use_item(3 + items_cur)) sfx(SFX_ITEM);
            after_action();
        }
        return 0;
    case UI_PAUSE:
        return pause_menu(edge);
    case UI_NOTES:
        if (((edge & PAD_A) && pager_next()) || (edge & (PAD_B | PAD_START))) close_box();
        return 0;
    case UI_DIALOG:
        if ((edge & PAD_A) && pager_next()) {
            box_hide();
            resume(0);                  /* (the core goes on after the dialog) */
            after_action();
        }
        return 0;
    case UI_PASSWORD:
        if (edge & PAD_UP) code_digit = (code_digit + 1) % 10;
        if (edge & PAD_DOWN) code_digit = (code_digit + 9) % 10;
        if ((edge & PAD_RIGHT) && code_len < CODE_MAX - 1) {
            code[code_len++] = '0' + code_digit;
            code_digit = 0;
        }
        if ((edge & PAD_LEFT) && code_len) code_digit = code[--code_len] - '0';
        if (edge & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)) sfx(SFX_BLIP);
        if (edge & PAD_A) {
            code[code_len] = '0' + code_digit;
            code[code_len + 1] = 0;
            box_hide();
            resume(code);               /* (the core checks the code) */
            after_action();
            return 0;
        }
        if (edge & PAD_B) {
            box_hide();
            resume(0);
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
    /* the core's actions: a step forward / back, a turn left / right, then
       the actions on their set buttons */
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

/* the game: the sound and settings started, then forever the title and a
   game (a loaded one or a new one) until QUIT TO TITLE. Called by main() */
void ui_main(void) BANKED
{
    sound_init();                       /* (sound.c) */
    settings_load();                    /* (save.c: the buttons, from the cartridge RAM) */
    for (;;) {
        u8 cont = title_screen();
        game_screen();
        if (cont && game_load()) {      /* (save.c: the saved game into S) */
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
}
