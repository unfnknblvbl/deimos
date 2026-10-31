/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the Game Boy Color: the machine's part in ROM bank 0 (always
 * there): the cartridge's data, the cartridge RAM's banks, the level the
 * core plays, its strings, the messages and sounds it asks for, the saves'
 * bytes. (The game code: the core, split in three banks (tools/split_core.py),
 * ui.c, view.c and the drawing, all banked.)
 *
 * It implements the platform hooks the portable core (core/rules.c) and
 * the SNES port's ui.c / save.c expect (map_level, level_string, gtext, say*,
 * sfx, plat_ev_at, plat_cmd_a1/a2, the campaign.h readers), and the 3D
 * view's data access (view/vdata.h: vd_u8 ... vd_pack). It lives in bank 0
 * because it switches the ROM bank at $4000-$7FFF itself.
 *
 * The cartridge (MBC5, 4 MB, 128 KB battery RAM: 16 banks of 8 KB):
 *   ROM banks 1..63    the code (GBDK's banks)
 *   ROM bank 64..      the campaign's block (megadrive/tools/md_campaign.py),
 *                      then the Game Boy's resources (tools/gb_res.py):
 *                      block offset o is bank 64 + o / 16384
 * Its RAM (A000..BFFF, a bank at a time):
 *   bank 0   the saves (save.c's bytes)
 *   bank 1   the level the core plays: cell_head (A000, 2 KB), the level's
 *            tables (A800..) without three of them
 *   bank 2   those three (core/rules.c's PLAT_LEVEL_FAR): the events of
 *            each cell (A000), the commands' arguments (A400.., two tables)
 *   bank 3   the view's own RAM (view.c, while it draws)
 *   banks 4..7  the drawing's (vdraw.c: 4, 5 the picture, 6 its tiles;
 *            gfx.c: 7 the text's staging area)
 * Bank 1 is in place whenever the core runs.
 * GBDK's SWITCH_ROM(b) / SWITCH_RAM(b) write the MBC5's bank registers
 * ($2000 ROM bank low bits, $4000 RAM bank); _current_bank is GBDK's copy
 * of the ROM bank mapped at $4000, restored after each far read.
 * Licence: GPL-2.0-or-later.
 */
#include <gb/gb.h>
#include <string.h>
#include "plat.h"
#include "../../view/vdata.h"

/* ticks: GBDK's count of vertical blanks (view.c's VIEW_PROF timing
   points; here in bank 0, so the banked view can call it plainly) */
u16 ticks(void) { return sys_time; }

/* ------------------------------------------------------------ the ROM's data */
/* block offset -> its bank (offsets under 4 MB) and its address there:
   bank = DATA_BANK0 + off / 16384 (bits 16..21 of off times 4, plus bits
   14..15), address = $4000 + off % 16384. Written with 8-bit pieces so
   SDCC does not call its 32-bit shift helper */
#define FAR_BANK(off) (u8)(DATA_BANK0 + ((u8)((off) >> 16) << 2) + ((u8)((u16)(off) >> 8) >> 6))
#define FAR_PTR(off) ((const u8 *)(0x4000 | ((u16)(off) & 0x3FFF)))

/* the byte at block offset off. The ROM bank in place is restored */
u8 far_u8(u32 off)
{
    u8 b = _current_bank, v;
    SWITCH_ROM(FAR_BANK(off));
    v = *FAR_PTR(off);
    SWITCH_ROM(b);
    return v;
}

/* n bytes from block offset off to dst (RAM), one ROM bank's piece at a
   time: a copy may cross 16 KB bank boundaries. The ROM bank is restored */
void far_copy(u32 off, void *dst, u16 n)
{
    u8 b = _current_bank, *d = dst;
    while (n) {
        u16 room = 0x4000 - ((u16)off & 0x3FFF), k = n < room ? n : room;    /* (the bytes left in this bank) */
        SWITCH_ROM(FAR_BANK(off));
        memcpy(d, FAR_PTR(off), k);     /* (this bank's part) */
        d += k;
        off += k;
        n -= k;
    }
    SWITCH_ROM(b);
}

/* the big-endian u16 at block offset off */
u16 far_u16be(u32 off)
{
    u8 b = _current_bank;
    u16 v;
    if (((u16)off & 0x3FFF) == 0x3FFF) {        /* (its two bytes in two banks) */
        u8 t[2];
        far_copy(off, t, 2);                    /* (far_copy splits it) */
        return ((u16)t[0] << 8) | t[1];
    }
    SWITCH_ROM(FAR_BANK(off));
    {
        const u8 *p = FAR_PTR(off);
        v = ((u16)p[0] << 8) | p[1];
    }
    SWITCH_ROM(b);
    return v;
}

/* the big-endian u32 at block offset off */
u32 far_u32be(u32 off)
{
    u8 t[4];
    far_copy(off, t, 4);                        /* (it may cross a bank) */
    return ((u32)t[0] << 24) | ((u32)t[1] << 16) | ((u16)t[2] << 8) | t[3];
}

u8 data_bank0 = 64;                     /* the block's first ROM bank (plat.h's DATA_BANK0) */
static u32 res_base;                    /* the resources' block offset (their "GBRES1" header) */

/* the resources: the first bank from 64 that starts "GBRES1"; the block's
   offset in their header. Sets data_bank0 (the block's bank) and res_base
   (the resources' offset from the block). Called once by main(). The
   header (tools/gb_res.py): "GBRES1", u16 entries, u32 the block's offset
   from bank 64 (big-endian: bytes 8..11), u32 per entry */
void plat_init(void)
{
    u8 b, was = _current_bank;
    for (b = 64; b < 0xFF; b++) {       /* (each bank's first bytes, at $4000) */
        volatile const u8 *p = (volatile const u8 *)0x4000;
        SWITCH_ROM(b);
        if (p[0] == 'G' && p[1] == 'B' && p[2] == 'R' && p[3] == 'E' && p[4] == 'S' && p[5] == '1') {
            u8 blk = (u8)(64 + (((u16)p[9] << 2) | (p[10] >> 6)));   /* (offset bits 22..14) */
            data_bank0 = blk;
            res_base = (u32)(u8)(b - blk) << 14;    /* (16 KB a bank) */
            break;
        }
    }
    SWITCH_ROM(was);
}

/* resource e's block offset (RES_*): its entry in the header's table at
   +12 is an offset from the header */
u32 res_at(u8 e) { return res_base + far_u32be(res_base + 12 + 4 * (u16)e); }

/* ------------------------------------------------------------ the view's data (view/vdata.h) */

/* view.c reads its map data through these (block offsets, big-endian) */
u8 vd_u8(u32 a) { return far_u8(a); }
u16 vd_u16(u32 a) { return far_u16be(a); }
u32 vd_u32(u32 a) { return far_u32be(a); }

/* n big-endian words at block offset a into dst, byte-swapped to the
   Game Boy's little-endian order */
void vd_words(u32 a, s16 *dst, u8 n)
{
    u8 *d = (u8 *)dst, i, bk = _current_bank;
    if (((u16)a & 0x3FFF) > 0x4000 - 2 * (u16)n) {  /* (across a bank boundary: copied, then swapped) */
        u8 t;
        far_copy(a, dst, 2 * (u16)n);
        for (i = 0; i < n; i++, d += 2) {
            t = d[0];
            d[0] = d[1];
            d[1] = t;
        }
        return;
    }
    SWITCH_ROM(FAR_BANK(a));            /* (all in one bank: swapped as they are read) */
    {
        const u8 *p = FAR_PTR(a);
        for (i = 0; i < n; i++, d += 2, p += 2) {
            d[0] = p[1];
            d[1] = p[0];
        }
    }
    SWITCH_ROM(bk);
}

/* map's pack header into *p (view/vdata.h): its counts, its texture
   swaps (at most 5 pairs) and the block offsets of its tables. Called by
   view.c when it loads a map */
void vd_pack(u8 map, struct vpack *p)
{
    u32 pk = map_pack(map);             /* the pack's block offset */
    u8 i;
    p->n_walls = vd_u16(pk + 8);
    p->n_tex = vd_u16(pk + 10);
    p->n_spr = vd_u16(pk + 12);
    p->n_scenery = vd_u16(pk + 14);
    p->floor = vd_u16(pk + 16);
    p->ceiling = vd_u16(pk + 18);
    p->n_swaps = vd_u8(pk + 20);
    for (i = 0; i < 2 * p->n_swaps && i < 10; i++) p->swaps[i] = vd_u8(pk + 21 + i);
    p->walls = pack_entry(map, P_WALLS, 0);     /* (each table's offset from the pack's entry list) */
    p->tex_off = pack_entry(map, P_TEX_OFF, 0);
    p->tex_pal = pack_entry(map, P_TEX_PAL, 0);
    p->spr_cols = pack_entry(map, P_SPR_COLS, 0);
    p->spr_off = pack_entry(map, P_SPR_OFF, 0);
    p->spr_coff = pack_entry(map, P_SPR_COFF, 0);
    p->pvs_index = pack_entry(map, P_PVS_INDEX, 0);
    p->pvs = pack_entry(map, P_PVS, 0);
    p->spr_pal = pack_entry(map, P_SPR_PAL, 0);
    p->scenery = pack_entry(map, P_SCENERY, 0);
    p->def_spr = pack_entry(map, P_DEF_SPR, 0);
    p->vis_index = pack_entry(map, P_VIS_INDEX, 0);
    p->vis = pack_entry(map, P_VIS, 0);
    p->tex_bank = vd_u32(8);            /* (the block header's texture and sprite banks) */
    p->spr_bank = vd_u32(12);
}

/* ------------------------------------------------------------ the campaign (campaign.h) */
/* campaign.h's readers: the block is in ROM, so they are the far reads */
u32 blk_u32(u32 off) { return far_u32be(off); }
u16 blk_u16(u32 off) { return far_u16be(off); }
void blk_copy(u32 off, void *dst, u16 len) { far_copy(off, dst, len); }

/* the NUL-terminated string at block offset off into dst, at most max
   characters; dst[max] is always set to 0 (dst needs max + 1 bytes) */
void blk_strcpy(u32 off, char *dst, u16 max)
{
    u16 i;
    for (i = 0; i < max; i++) {
        char c = (char)far_u8(off + i);
        dst[i] = c;
        if (!c) break;
    }
    dst[max] = 0;
}

u8 n_maps(void) { return (u8)blk_u32(4); }                  /* (the block header's map count) */
u32 map_pack(u8 map) { return blk_u32(32 + 32UL * map); }   /* (map records: 32 bytes from offset 32) */

/* a pack's entry e (P_*): u32 offset and u32 size at pack + 32 + 8 e */
u32 pack_entry(u8 map, u8 e, u32 *size)
{
    u32 pk = map_pack(map);
    if (size) *size = blk_u32(pk + 36 + 8UL * e);
    return blk_u32(pk + 32 + 8UL * e);
}

/* the map's title: the 26 bytes at +6 of its record, NUL-terminated here
   (a static buffer: the next call overwrites it) */
static char title[27];
const char *map_title(u8 map)
{
    blk_copy(32 + 32UL * map + 6, title, 26);
    title[26] = 0;
    return title;
}

u8 map_orig_id(u8 map) { return (u8)(blk_u32(32 + 32UL * map + 4) >> 24); }    /* (the record's byte 4) */

/* ------------------------------------------------------------ the cartridge RAM */
/* cartridge RAM bank b at $A000-$BFFF */
void sram_bank(u8 b)
{
    SWITCH_RAM(b);
}

/* the saves' byte i (bank 0, i under 8 KB), for save.c; bank 1 (the
   level) is put back after */
u8 sram_rd(u16 i)
{
    u8 v;
    SWITCH_RAM(0);
    v = *(volatile u8 *)(0xA000 + (i & 0x1FFF));
    SWITCH_RAM(1);
    return v;
}

/* the saves' byte i set to v (battery-backed bank 0); bank 1 back after */
void sram_wr(u16 i, u8 v)
{
    SWITCH_RAM(0);
    *(volatile u8 *)(0xA000 + (i & 0x1FFF)) = v;
    SWITCH_RAM(1);
}

/* ------------------------------------------------------------ the level (glue) */
/* The "level blob" is the core's data for one map (the pack's P_LEVEL
   entry, made by the Mega Drive game's tools): a 24-byte header, then LT_N big-endian
   u16 offsets (from the blob's start) of its LT_N tables, then the tables.
   It is too big for bank 1 with all its tables, so three of them go to
   bank 2 (the core reads them through plat_ev_at and plat_cmd_a1 / a2:
   core/rules.c built with PLAT_LEVEL_FAR) and the rest is packed in bank
   1 from LEVEL_AT, its offsets rewritten. Bank 1's $A000-$A7FF is the
   core's cell_head (PLAT_CELL_HEAD, build.sh). */
#define LT_N 30                         /* the level blob's tables */
#define LT_EV_AT 20                     /* table 20: each cell's event (1024 cells) */
#define LT_CMD_A1 22                    /* tables 22, 23: the commands' u32 arguments */
#define LT_CMD_A2 23
#define LEVEL_AT 0xA800                 /* the level blob in bank 1 */
static u8 loaded_map = 0xFF;            /* the map whose blob is in place (255: none) */
static u32 strings_off;                 /* the map's strings' block offset */
static u16 a1_at, a2_at;                /* in bank 2 */

static u16 be16(const u8 *p) { return ((u16)p[0] << 8) | p[1]; }    /* (a big-endian u16 in RAM) */

/* the core's hook: the level blob of map, loaded into the cartridge RAM
   if it is not already there; returns its address in bank 1 ($A800).
   Leaves bank 1 mapped. Also notes where the map's strings are */
const u8 *map_level(u8 map)
{
    u32 n, off;
    u8 hdr[24 + 2 * LT_N];
    u16 o[LT_N + 1], at, k;
    if (map == loaded_map) return (const u8 *)LEVEL_AT;
    off = pack_entry(map, P_LEVEL, &n);         /* the blob's offset, its size n */
    strings_off = pack_entry(map, P_STRINGS, 0);
    far_copy(off, hdr, sizeof hdr);             /* its header and table offsets */
    for (k = 0; k < LT_N; k++) o[k] = be16(hdr + 24 + 2 * k);
    o[LT_N] = (u16)n;                           /* (the last table ends at the blob's end) */
    /* bank 2: the three tables kept apart */
    SWITCH_RAM(2);
    far_copy(off + o[LT_EV_AT], (void *)0xA000, o[LT_EV_AT + 1] - o[LT_EV_AT]);
    a1_at = 0xA400;
    far_copy(off + o[LT_CMD_A1], (void *)a1_at, o[LT_CMD_A1 + 1] - o[LT_CMD_A1]);
    a2_at = a1_at + (o[LT_CMD_A1 + 1] - o[LT_CMD_A1]);
    far_copy(off + o[LT_CMD_A2], (void *)a2_at, o[LT_CMD_A2 + 1] - o[LT_CMD_A2]);
    /* bank 1: the rest, packed, the header's offsets made to fit */
    SWITCH_RAM(1);
    at = o[0];
    far_copy(off, (void *)LEVEL_AT, o[0]);              /* (the header) */
    for (k = 0; k < LT_N; k++) {                /* each table: its new offset into the header, its bytes after the last */
        u16 len = o[k + 1] - o[k];
        u8 *h = (u8 *)LEVEL_AT + 24 + 2 * k;
        h[0] = (u8)(at >> 8);
        h[1] = (u8)at;
        if (k == LT_EV_AT || k == LT_CMD_A1 || k == LT_CMD_A2) continue;     /* (in bank 2: no room taken here) */
        if (LEVEL_AT + (u32)at + len > 0xC000) break;   /* (cannot happen with the game's maps) */
        far_copy(off + o[k], (void *)(LEVEL_AT + at), len);
        at += len;
    }
    loaded_map = map;
    return (const u8 *)LEVEL_AT;
}

/* the core's hook (PLAT_LEVEL_FAR): the event byte of cell (0..1023), from
   bank 2; bank 1 back after */
u8 plat_ev_at(u16 cell)
{
    u8 v;
    SWITCH_RAM(2);
    v = *(volatile u8 *)(0xA000 + (cell & 1023));
    SWITCH_RAM(1);
    return v;
}

/* the big-endian u32 at address a of cartridge RAM bank 2; bank 1 back after */
static u32 rd32_bank2(u16 a)
{
    u8 b0, b1, b2, b3;
    SWITCH_RAM(2);
    b0 = *(volatile u8 *)a;
    b1 = *(volatile u8 *)(a + 1);
    b2 = *(volatile u8 *)(a + 2);
    b3 = *(volatile u8 *)(a + 3);
    SWITCH_RAM(1);
    return ((u32)b0 << 24) | ((u32)b1 << 16) | ((u16)b2 << 8) | b3;
}

/* the core's hooks: command c's first and second u32 arguments */
u32 plat_cmd_a1(u16 c) { return rd32_bank2(a1_at + 4 * c); }
u32 plat_cmd_a2(u16 c) { return rd32_bank2(a2_at + 4 * c); }

/* the map's string id, copied into one of two buffers (the longest is 421) */
#define STR_MAX 447
static char str_buf[2][STR_MAX + 1];
static u8 str_next;

/* the core's hook: the map's string id. The two buffers alternate, so the
   last two strings returned stay valid together */
const char *level_string(u8 id)
{
    char *b = str_buf[str_next];
    u32 at = strings_off + blk_u16(strings_off + 2 * (u16)id);  /* (u16 offsets from the strings' start) */
    str_next ^= 1;
    blk_strcpy(at, b, STR_MAX);
    return b;
}

/* the game's texts (the rules' messages, the menus' words, the weapons'
   names): the campaign block's texts table, at the offset in the block
   header's u32 at byte 24 (tools/game_text.py), a big-endian word per
   text (its offset from the table) and then the texts. Each is copied
   into one of four small buffers in turn (work RAM is short: at most
   GT_MAX characters, as many as a message line holds) */
#define GT_BUFS 4
#define GT_MAX 31
static char gt_buf[GT_BUFS][GT_MAX + 1];
static u8 gt_next;

/* the core's hook: text id (TX_*), valid for the next three calls */
const char *gtext(u8 id)
{
    char *b = gt_buf[gt_next];
    u32 t = blk_u32(24);                                /* (the texts table) */
    gt_next = (gt_next + 1) & (GT_BUFS - 1);
    blk_strcpy(t + blk_u16(t + 2 * (u16)id), b, GT_MAX);
    return b;
}

/* ---- the messages: a queue the status line shows one after another
   (ui.c's msg_tick takes them from msg_head; MSGQ, a power of two, in
   snes/src/glue.h). Each message is at most 31 characters */
char msgq[MSGQ][32];
u8 msg_head, msg_count;                 /* the oldest message's index, how many wait */
u16 msg_total;                          /* messages ever queued */

/* a message made of up to three strings (0: none) queued; when the queue
   is full the oldest is dropped */
static void q_put(const char *a, const char *b, const char *c)
{
    char *d;
    u8 n = 0;
    if (msg_count == MSGQ) {
        msg_head = (msg_head + 1) & (MSGQ - 1);
        msg_count--;
    }
    d = msgq[(msg_head + msg_count) & (MSGQ - 1)];
    while (a && *a && n < 31) d[n++] = *a++;
    while (b && *b && n < 31) d[n++] = *b++;
    while (c && *c && n < 31) d[n++] = *c++;
    d[n] = 0;
    msg_count++;
    msg_total++;
}

/* the core's hooks: a message of one, two or three parts */
void say(const char *s) { q_put(s, 0, 0); }
void say2(const char *a, const char *b) { q_put(a, b, 0); }
void say3(const char *a, const char *b, const char *c) { q_put(a, b, c); }

/* n in decimal (a static buffer: the next call overwrites it) */
static char num_buf[6];
char *utoa10(u16 n)
{
    char *p = num_buf + 5;
    *p = 0;
    do {
        *--p = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    return p;
}

void say_num(const char *a, u16 n, const char *b) { q_put(a, utoa10(n), b); }   /* (a, n in decimal, b) */

/* ---- the sounds: a queue snd.c plays from (in the vertical blank
   interrupt): a ring of 8 effect ids, sfx_tail the next free place */
u8 sfx_q[8], sfx_head, sfx_tail;

extern u8 grin_frames;                  /* the status bar's grin after a pick-up (ui.c) */

/* the core's hook: sound effect id (SFX_*) queued; when the ring is full
   the oldest waiting effect is dropped. A pick-up also starts the face's
   grin (30 frames) */
void sfx(u8 id)
{
    if (id == SFX_PICKUP || id == SFX_CREDITS || id == SFX_WEAPON || id == SFX_KEY) grin_frames = 30;
    sfx_q[sfx_tail] = id;
    sfx_tail = (sfx_tail + 1) & 7;
    if (sfx_tail == sfx_head) sfx_head = (sfx_head + 1) & 7;
}
