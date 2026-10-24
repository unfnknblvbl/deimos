; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

; Doom RPG for the SNES: the 3D view's size for the assembler, as
; view/view.h gives it to the C. The defaults are view.h's (128 x 88);
; snes/build.sh and build_fx.sh pass the same -D VIEW_W=... -D VIEW_H=...
; to ca65 as to cc65 (the SNES game: 104 x 72, 80% of the size), so the C
; and the assembler always agree. Included by draw.s, vcore.s, video.s
; and gsu_cpu.s (snes/gsu/view.s has the same sums for the GSU).
; ca65. Licence: GPL-2.0-or-later.
;
; The screen (video.s): the view is shown by Mode 7 at twice its size,
; centred in the 256 x 176 area below the message line (lines 8..183)
; that the default view fills; a smaller one leaves a black border of
; VIEW_PAD_X pixels left and right and VIEW_PAD_Y lines above and below.

.ifndef VIEW_W
VIEW_W = 128                    ; the view's width in pixels (a multiple of 8)
.endif
.ifndef VIEW_H
VIEW_H = 88                     ; its height
.endif
; the frame buffer (view.h): at 128 x 88 the original layout (strips of 3
; blocks of 32 rows, uploaded through VMAIN's 8-bit rotation); at any
; other size tiled (FB_TILED: 8 x 8 Mode 7 tiles, each column's 8 pixels
; in turn, VIEW_H / 8 tiles down a strip; uploaded as it is and shown
; transposed by Mode 7's matrix)
.if VIEW_W = 128 && VIEW_H = 88
FB_TILED    = 0
FB_ROWS     = 96                ; its rows: 3 blocks of 32 (88..95 not shown)
FB_GROUP    = 32                ; the rows a column runs down in a row (a block's)
.else
FB_TILED    = 1
FB_ROWS     = VIEW_H
FB_GROUP    = 8                 ; (a tile's)
.endif
FB_STRIPS   = VIEW_W / 8        ; its 8-pixel-wide strips
FB_BYTES    = VIEW_W * FB_ROWS  ; its size (12288 at 128 x 88, 7488 at 104 x 72)
TILE_ROWS   = FB_ROWS / 8       ; the Mode 7 tiles down a strip
VIEW_HALF_H = VIEW_H / 2        ; the horizon's row
VIEW_FOCAL  = VIEW_W / 2        ; the focal length in pixels (a 90 degree field of view)
VIEW_PAD_X  = 128 - VIEW_W      ; screen pixels of border left and right of the view
VIEW_PAD_Y  = 88 - VIEW_H       ; lines of border above and below it
VIEW_LINES  = 2 * VIEW_H        ; the screen lines the view takes (176 at 88 rows)
VIEW_TOP    = 8 + VIEW_PAD_Y    ; the first of them (below the 8-line message line)

.if VIEW_W .mod 8 <> 0 || VIEW_W > 128 || VIEW_H > 88 || (FB_TILED && VIEW_H .mod 8 <> 0)
.error "view.inc: VIEW_W must be a multiple of 8 up to 128, VIEW_H 88 or a multiple of 8 below"
.endif
