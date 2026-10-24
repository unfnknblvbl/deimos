/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* the Mega Drive game - the colours of each enclosed area (md_areas.c).
 *
 * A host-side tool (not 68000 code): build.sh compiles md_areas.c as a
 * program (with MD_AREAS_MAIN) to make build/gen/areas.bin; the Doom RPG
 * editor compiles it in to remake the table when it writes a ROM. The
 * format of the table is described at the top of md_areas.c.
 *
 * Licence: GPL-2.0-or-later. */
#ifndef MD_AREAS_H
#define MD_AREAS_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* the areas table for the campaign's block blk[0, n): its size (0: not a
   block); *out is malloc'd (free it). blk is the campaign block as
   tools/md_campaign.py writes it (build/gen/campaign.bin). */
size_t md_areas(const unsigned char *blk, size_t n, unsigned char **out);
#ifdef __cplusplus
}
#endif
#endif
