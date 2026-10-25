/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* the Mega Drive game - the campaign's maps (src/pack.c). Licence: GPL-2.0-or-later.
 *
 * The campaign block (tools/md_campaign.py) holds every map's data at the
 * end of the ROM. pack_init chooses one map: it sets the pointers the
 * renderer (render_data.h), the rules (level_blob) and the weapon sprites
 * use, and the area colours (src/area.c). Maps are numbered 0..n_maps-1 in
 * campaign order (0 the UAC Base). */
#ifndef PACK_H
#define PACK_H
#include "md.h"
extern u8 n_maps;                  /* how many maps the block holds (set by pack_init) */
void pack_init(u8 map);            /* point every table at map `map` */
const char *map_title(u8 map);     /* the map's title, in capitals (from the jar) */
u8 map_orig_id(u8 map);            /* the original game's id (2 = the Junction) */
extern const unsigned char *r_tex_addr[];  /* each texture slot's 4096 texels (in the texture bank) */
extern u8 r_tex_map[];                     /* texture slot -> the slot drawn (the portal's swaps) */
void pack_portal(u8 closed);       /* the Reactor's portal closed: its walls' textures change */
#endif
