/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the SNES: getting pictures to the PPU (gfx.s).
 * Licence: GPL-2.0-or-later. */
/* The PPU's memories (VRAM, CGRAM, OAM) can only be written in the
 * vertical blank, so C queues copies (vq_add) and the NMI sends them.
 * The arguments are globals, set before the call (cc65 passes few
 * arguments cheaply). Used by ui.c; the other ports implement the same
 * interface in their machine layers. */
#ifndef GFX_H
#define GFX_H
#include "../../core/types.h"

/* vq_mode's low byte: what the copy goes to (1, VRAM high bytes only, is
   the view's picture; its high byte is then VMAIN) */
#define VQ_VRAM 0                       /* VRAM words ($2118/9) */
#define VQ_CGRAM 2
extern u32 vq_src;                      /* vq_add's arguments */
extern u16 vq_dst, vq_len, vq_mode;     /* VRAM word address or colour index; bytes; kind */
void vq_add(void);                      /* queue a copy for the next vblanks */
u8 vq_busy(void);                       /* 1 while queued copies wait */
void vq_flush(void);                    /* the queue now (forced blank only) */

extern u8 oam[544];                     /* the sprite table (sent each frame it is dirty) */
extern u8 oam_dirty;                    /* set it after changing oam */
void gfx_init(void);                    /* sprites hidden, the queue emptied */

/* Mega Drive tiles (4 bits a pixel, packed) -> SNES 4bpp in bank $7F */
extern u32 mt_src;                      /* the source (long address) */
extern u16 mt_dst, mt_n;                /* the destination in bank $7F; tiles */
void md_tiles(void);

extern char txt_chars[16][24];          /* the text layer: 0 see-through, 32..127 */
void txt_render(void);                  /* -> its sprite tiles at $7F3000 */
void fill7f(u16 at, u16 len);           /* zeros in bank $7F */

/* vdata.s: the ROM at a long address */
u8 rom_u8(u32 a);
u16 rom_u16le(u32 a);
#endif
