/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* The automap (automap.h): which cells were seen, and the map as text.
 * Licence: GPL-2.0-or-later. */
#include "automap.h"
#if defined(AM_MD)
/* the Mega Drive: included by its ui.c (after game.h and level.h), which
   gives am_wall / am_door from its own map tables and marks the seen
   cells itself (its add_vis into seen[]) */
#define AM_OWN_CELLS
#define AM_NO_CAMPAIGN
#elif defined(AM_GB)                    /* (the Game Boy Color: its core API and campaign, gbc/build.sh) */
#include "plat.h"
#else
#include "rules.h"
#ifndef AM_NO_CAMPAIGN
#include "../snes/src/campaign.h"
#endif
#endif

static u8 seen[128];                    /* bit (cell & 7) of byte cell >> 3: seen */

void am_clear(void) AM_FN
{
    u8 i = 0;
    do seen[i] = 0; while (++i < 128);
}

void am_mark(u16 cell) AM_FN { seen[(cell >> 3) & 127] |= (u8)(1 << (cell & 7)); }

#ifndef AM_NO_CAMPAIGN
/* the view's list in the map's pack: vis_index gives the word offset of a
   list of cells in vis, ended by $FFFF; $FFFF itself means every cell
   (tools/md_campaign.py) */
void am_look(u8 map, u8 x, u8 z, u8 facing) AM_FN
{
    u32 vi = pack_entry(map, P_VIS_INDEX, 0), vl = pack_entry(map, P_VIS, 0);
    u16 at = blk_u16(vi + 2 * ((((u32)z << 5) + x) * 4 + (facing & 3))), c;
    u8 i = 0;
    if (at == 0xFFFF) {                 /* (every cell: a cell that can't be stood in) */
        do seen[i] = 0xFF; while (++i < 128);
        return;
    }
    for (;;) {
        c = blk_u16(vl + 2 * (u32)at++);
        if (c & 0x8000) break;
        am_mark(c);
    }
    am_mark(((u16)z << 5) + x);         /* (where the player stands) */
}
#endif

#ifndef AM_OWN_CELLS
u8 am_wall(u8 x, u8 z) AM_FN { return wall_at(x, z); }
u8 am_door(u8 x, u8 z) AM_FN { return line_at(x, z) != NONE_B; }
#endif

/* is_seen: cell (x, z) was seen (or the map was given); outside: no */
static u8 is_seen(s16 x, s16 z)
{
    u16 c;
    if (x < 0 || z < 0 || x >= 32 || z >= 32) return 0;
    if (S.map_given) return 1;
    c = ((u16)z << 5) + (u16)x;
    return (u8)((seen[c >> 3] >> (c & 7)) & 1);
}

/* cell_char: the character for cell (x, z): a floor cell when seen, a wall
   or a door when a seen floor cell is next to it (walls are never in the
   views' lists), else blank */
static char cell_char(s16 x, s16 z)
{
    u8 wall, door;
    if (x < 0 || z < 0 || x >= 32 || z >= 32) return ' ';
    wall = am_wall((u8)x, (u8)z);
    door = am_door((u8)x, (u8)z);
    if (!wall && !door) return is_seen(x, z) ? '.' : ' ';
    if (is_seen(x - 1, z) || is_seen(x + 1, z) || is_seen(x, z - 1) || is_seen(x, z + 1) || is_seen(x, z))
        return door ? '+' : '#';
    return ' ';
}

void am_row(char *out, u8 r, u8 w, u8 h, u8 px, u8 pz, u8 f) AM_FN
{
    static const char ARROW[4] = {'^', '>', 'v', '<'};
    s16 z = (s16)pz - (s16)(h >> 1) + r, x0 = (s16)px - (s16)(w >> 1), x;
    u8 k;
    for (k = 0; k < w; k++) {
        x = x0 + k;
        out[k] = (x == px && z == pz) ? ARROW[f & 3] : cell_char(x, z);
    }
    out[w] = 0;
}
