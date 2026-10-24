; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

;=======================================================================
; Doom RPG -> Sega Master System: the live renderer (bank RENDER_BANK,
; paged into slot 1 at $4000 while it runs; svc_render in main.asm is the
; trampoline).
;
; Draws the view of (eng_view_x, eng_view_z, eng_view_f) at the size in
; eng_vsize (0 = large 160x96, 1 = small 128x80). tools/live_data.py has
; the geometry, the data it reads and a bit-exact Python model of this
; code (the tests compare the two).
;
;  1. walls: every wall on this view's visibility list is culled (open
;     door, back face), clipped and projected; each screen column keeps
;     the nearest wall (largest height h8), its slot and texture column u
;  2. billboards (lamps, chairs...): projected, sorted far to near
;  3. strips of 8 columns: each column is filled top to bottom into a
;     chunky buffer (ceiling, wall texels, floor; one "colour pair" byte
;     per pixel), billboards drawn over it, then rows of 8 pixels are
;     turned into planar tile rows (the pair's nibble is chosen by screen
;     parity: 2x2 dither for free) and written to VRAM. Tile rows that are
;     all ceiling or all floor use the two solid tiles instead.
;  4. the name table: each strip's 8-pixel column of tiles is shown by
;     writing its name-table entries (solid tiles where a strip has none of
;     its own), and the hardware sprite is put back.
;
; Licence: GPL-2.0-or-later.
;
; ---- Where it fits -------------------------------------------------------
; Only entry point: render_main, called by svc_render (rom/main.asm) with
; this bank in slot 1. It calls back into bank 0 for the VDP helpers
; (hide_sprites_safe, wait_below_view, wait_frame, vram_upload_paced,
; vdp_set_vram_write_hl) and the sprite code (sprite_prepare,
; entity_write_sat). It reads the engine variables eng_* / var_* (main.asm)
; and the doors' state at L_FLAGS (the game state, build/gfx/live/state.inc).
; It pages data banks into slot 2 ($8000-$BFFF) through the mapper
; register $FFFF as it goes; the bank numbers and addresses all come from
; the level header (bank 3), copied into r_hdr at the start.
;
; ---- Terms ---------------------------------------------------------------
; world units  64 per map cell; x grows east, z grows south. Facing
;              0 north, 1 east, 2 south, 3 west.
; view space   d = depth (distance ahead of the camera), l = lateral
;              (distance to the right). The camera sits 16 units behind the
;              cell centre, with a 90-degree field of view, so a point is
;              on screen when |l| < d.
; F            half the view's width in pixels (80 or 64). Screen column of
;              a point = F + F * l / d.
; p            a projected position: 1/256 pixel from the centre column.
; h8           a wall's projected height in 1/256 pixels (8.8 fixed point):
;              h8 = 8192 * W / d, i.e. a 64-unit wall seen at depth d is
;              32 * W / d pixels high. Bigger h8 = nearer.
; height table one per view size (banks 7 and 8, paged as r_tbank): 1792
;              entries of 8 bytes, found from h8 by table_entry:
;                +0 y0   first screen row of the wall (0..H)
;                +1 y1   row after its last
;                +2 v0   texture row at y0, 8.8 fixed point (texels * 256)
;                +4 dv   texture rows per screen row, 8.8
;                +6 d    the depth this height stands for (u16)
;              (tools/live_data.py size_table builds it.)
; colour pair  a byte holding two palette indices; the pixel at (x, y)
;              shows the high nibble when x + y is even, else the low one.
;              Textures, billboards and the floor are stored as pairs.
; strip        8 screen columns, i.e. one column of tiles. Strips are drawn
;              one at a time.
; chunky buffer one byte per pixel of one strip ($D800-$DBFF), filled
;              column by column, then converted to the VDP's planar tiles.
; slot         a numbered wall that won at least one column this picture
;              (r_slot_tex holds its texture); COLW holds slot numbers.
; billboard    scenery drawn as a scaled picture facing the camera.
; OUTWARD / AT ONCE  the two update modes, eng_vmode 0 / 1 (README).
; low detail   eng_detail = 1: only even columns are worked out, each
;              drawn two pixels wide.
;
; ---- Registers -----------------------------------------------------------
; Routines here may change any register, including IX and IY (svc_render
; saves the game's IX and IY), unless their comment says otherwise.
;=======================================================================

.define R_NEAR      20          ; near clipping depth in world units: walls are cut there, billboards nearer are skipped
; Per-column arrays: one 256-byte page each, indexed by screen column x
; (0..W-1, at most 159); the bytes from $A0 on are used for other lists.
.define COLH_LO     $D4         ; pages of per-column arrays (index = x, 0..159)
.define COLH_HI     $D5         ;   COLH_LO/HI: h8 of the nearest wall so far (0 = none)
.define COLW        $D6         ;   that wall's slot ($FF = no wall)
.define COLU        $D7         ;   its texture column u (0..63)
.define CHUNKY      $D8         ; $D800-$DBFF: column c at $D800 + c*128, rows 0..95
                                ; (columns 2k and 2k+1 share page $D8+k: the even one
                                ; at offset 0, the odd one at offset 128)
.define MAX_SLOTS   64          ; walls that may win columns in one picture
.define MAX_BILLS   6           ; billboards per picture (more are ignored)
.define BILL_SIZE   16          ; bytes per r_bills entry (B_* offsets below)

; RAM: the tails of the column pages ($xxA0-$xxFF) and $DC00-$DDFF
.ENUM $D4A0
r_pvs       dsb 96          ; this view's list: n, ids (u16)..., nb, scenery...
                            ; (n = wall count, then n wall ids; nb = scenery count, then nb u8 scenery ids)
.ENDE
.ENUM $D5A0
r_bills     dsb MAX_BILLS*BILL_SIZE     ; the projected billboards (B_* layout)
.ENDE
.ENUM $D6A0
r_slot_tex  dsb MAX_SLOTS   ; texture number of each wall slot
r_order     dsb 8           ; billboard indices, sorted far to near
s_dvhi      dsb 8           ; this strip: dv high byte of each column (s_cols has no room for it)
.ENDE
.ENUM $D7A0
s_cols      dsb 8*8         ; per column: y0, y1, v0 (2), dv lo, addr lo, bank, addr hi
                            ; (this strip's 8 columns, 8 bytes each; "addr" = the texture
                            ; column's 64 bytes in slot 2; bank 0 = no wall in this column)
r_tilebuf   dsb 32          ; (not used by the current code)
.ENDE
.ENUM $DC00
r_w         db      ; view width
r_h         db      ; view height
r_f         db      ; W / 2
r_strips    db      ; strips across the view (W / 8)
r_rows      db      ; tile rows down the view (H / 8)
r_tbank     db      ; this size's table bank
r_vrow      db      ; view position on screen (tile row, column)
r_vcol      db
r_base      dw      ; first tile of the target set
r_ntorg     dw      ; name table address of the view's top-left
r_solid     dw      ; tile number of the solid ceiling tile (+1 = floor, +2.. border)
r_floor     dw      ; this size's floor rows (floor_rows_*: even, odd, whole; 96 each)
r_ftile     dw      ; this size's all-floor tile per tile row (offsets from r_solid)
r_vx        dw      ; the camera, in world units (16 behind the cell centre)
r_vz        dw
r_face      db      ; facing 0..3 (N, E, S, W)
r_nslot     db      ; wall slots used so far
r_nbill     db      ; billboards in r_bills
r_masks     dsb 2*20        ; per strip: rc, rf (for the name table)
; one wall being set up
; (w_ax .. w_pad: a copy of its 16-byte record; docs/rom_format.md)
w_ax        dw      ; end A, world units (signed)
w_az        dw
w_bx        dw      ; end B
w_bz        dw
w_tex       db      ; texture number (for a billboard: its sprite number)
w_line      db      ; door slot, $FF = a plain wall
w_ua        db      ; texture column at A and at B (one texel per unit)
w_ub        db
w_two       db      ; 1 = two-sided (drawn from behind too)
w_pad       dsb 3
w_da        dw      ; A in view space: depth, lateral
w_la        dw
w_db        dw      ; B in view space
w_lb        dw
w_m         db      ; slanted wall: dl/dd = +1, -1 (diagonal) or 0 (along the view)
w_l0        dw      ; slanted wall: its line is l = l0 + m * d
w_pa        dw      ; projected A and B (p: 1/256 px from the centre)
w_pb        dw
w_x0        db      ; first screen column covered
w_x1        db      ; column after the last
w_h8        dw      ; front wall: its height (constant)
w_hacc      dsb 4   ; slanted wall: h8 << 8 at the current column (32-bit)
w_dacc      dsb 4   ;   and its step per column
w_su        db      ; texture direction: +1, -1 or 0
w_cu        db      ; texture column offset (u = cu + su * position, mod 64)
w_l8        dw      ; front wall: lateral position * 256 at the current column
w_dl8       dw      ;   and its step per column
w_slot      db      ; this wall's slot number
w_neg       db      ; project: 1 if l was negative
; maths
m_n         dsb 4           ; dividend / quotient
m_d         dw              ; divisor
m_a         dw              ; mul16 operands
m_b         dw
m_p         dsb 4           ; product
; strip pass
s_s         db              ; strip index
s_x         db              ; first column of the strip
s_miny0     db              ; smallest y0 of the strip's walls and billboards
s_maxy1     db              ; largest y1
s_rc        db              ; first tile row that is not solid ceiling
s_rf        db              ; first tile row of solid floor
s_c         db              ; current column (0..7 in the strip; screen column in the billboard code)
s_tile      dw              ; a tile number, or the billboard record's address
s_tmp_dvhi  db              ; scratch: a column's dv high byte
s_tmp_c     db              ; scratch: billboard canvas column, or a strip number
s_btop      db              ; billboard: first canvas row with pixels
s_blen      db              ; billboard: canvas row after the last
s_bdata     dw              ; (not used by the current code)
s_bbank     db              ; billboard: bank of its sprite record
s_v         dw              ; billboard: texture row at its first screen row (8.8)
s_dv        dw              ; texture rows per screen row (8.8)
s_colbase   dw              ; address of chunky column s_c, row 0
s_n         db              ; a row count (or a tile count in strip_tiles)
s_y         db              ; (not used by the current code)
s_mask      db              ; (not used by the current code)
s_jump      dw              ; fill_column: entry into the unrolled copies
s_by0       db              ; billboard: first screen row
s_bn        db              ; billboard: number of screen rows
s_whole     db              ; fill_column: whole pair bytes
s_hasbill   db              ; a billboard covers part of this strip
s_ft        dw              ; write_all_names: this row's all-floor tile
s_left      db              ; outward order: strips left of this one still to do
s_right     db              ; and the next strip on the right
r_hdr       dsb H_SIZE      ; copy of the level header (rom/live_layout.inc)
r_ram_end   db              ; (marks the end, for the check below)
.ENDE
; build-time check: the game keeps its message queue at $DCE8
.IF r_ram_end > $DCE8
.FAIL "the renderer's variables run into the messages at $DCE8 (game/game.asm)"
.ENDIF
.ENUM $DDF0                 ; frame counts per phase (read by the tests)
r_prof      dsb 8           ; eng_frame at: +0 start, +1 walls done, +2 billboards
                            ; done, +3 strips done; +6 frames spent filling
                            ; columns (summed over strips), +7 scratch; +4 is
                            ; cleared, +5 unused. (No tool in tools/ reads
                            ; these at present.)
r_dbg       db              ; tests: bit 0 skips the column fill, bit 1 the tile conversion
                            ; (and bit 2 the per-strip name-table writes; 0 at reset)
.ENDE

; The renderer's code: ROM bank RENDER_BANK, assembled for slot 1 ($4000).
.BANK RENDER_BANK SLOT 1
.ORGA $4000

;-----------------------------------------------------------------------
; render_main: the whole view. Called with interrupts on, slot 1 = this
; bank. Returns with slot 2 paged anywhere (svc_exit restores it).
;
; In: eng_view_x/z/f, eng_vsize, eng_vmode, eng_detail, eng_solid,
; eng_ent_def/eng_ent_dist (the hardware sprite), var_need_layout,
; var_tile_set. Out: the picture on screen with its sprite;
; var_need_layout = 0, var_dlg_font = 0, var_tile_set flipped after a
; small AT ONCE picture. Interrupts enabled on return. Clobbers all
; registers (IX and IY too).
; Order: set-up; the border if needed; hide or blank what must not show
; while drawing; walls -> columns; billboards; strips (fill, convert,
; and in OUTWARD mode each strip's names); then show the picture and
; the sprite.
;-----------------------------------------------------------------------
render_main:
    call r_setup                ; size, camera, target tiles, level header
    ld a, (var_need_layout)
    or a
    jr z, +
    ; first picture, a size change, or a dialog box was shown: redo the
    ; border and black out the view area
    xor a
    ld (var_need_layout), a
    call hide_sprites_safe      ; the old sprite off while the border changes
    call r_layout               ; border tiles and name table around the view
    call blank_view             ; view area: black tiles
+:  ld a, (eng_vmode)
    or a
    jr nz, +
    ; OUTWARD (either size): the view is redrawn in place, strip by strip,
    ; so the sprite goes now
    call hide_sprites_safe      ; sprite off: its picture is being redrawn
    jr ++
+:  ld a, (eng_vsize)
    or a
    jr nz, ++
    ; large, AT ONCE: the sprite goes and the view turns black until the
    ; whole picture is ready (small, AT ONCE: drawn out of sight instead)
    call hide_sprites_safe      ; sprite off
    call blank_view             ; view area black until the picture is done
++: xor a
    ld (r_prof+4), a
    ld (r_prof+6), a
    ld a, (eng_frame)
    ld (r_prof), a              ; profile: start
    call r_clear_columns        ; no wall in any column yet
    call r_load_pvs             ; this cell and facing's visibility list
    call r_walls                ; every listed wall -> the nearest per column
    ld a, (eng_frame)
    ld (r_prof+1), a
    call r_billboards           ; project and sort the scenery
    ld a, (eng_frame)
    ld (r_prof+2), a
    call r_strips_all           ; fill, convert and upload every strip
    ld a, (eng_frame)
    ld (r_prof+3), a
    xor a
    ld (var_dlg_font), a        ; the view's tiles have replaced the dialog font
    ; ---- show it
    ld a, (eng_vmode)
    or a
    jr z, +                     ; OUTWARD: every strip's names are already out
    ld a, (eng_vsize)
    or a
    jr nz, _rm_small
    call wait_below_view        ; large, AT ONCE: all names at once
    call write_all_names        ; show the whole picture at once
    ei
+:  call sprite_prepare         ; the sprite's patterns into tiles 0..
    call wait_frame             ; then, in the vertical blank:
    di
    ld hl, ent_new_pal
    ld de, ent_pal
    ld bc, 16
    ldir                        ; its palette (the interrupts write it to CRAM)
    call entity_write_sat       ; and its positions
    ei
    ret
_rm_small:                      ; small, AT ONCE: show the other set
    call hide_sprites_safe      ; the old sprite off before its tiles change
    call sprite_prepare         ; the sprite's patterns into tiles 0..
    call wait_below_view        ; returns with interrupts off, below the view
    call write_all_names        ; point the view at the set just drawn
    ld hl, ent_new_pal
    ld de, ent_pal
    ld bc, 16
    ldir
    ld a, (var_tile_set)
    xor 1
    ld (var_tile_set), a        ; that set is now the one on screen
    ei
    call wait_frame             ; then put it in at the next vertical blank
    di
    call entity_write_sat       ; the sprite at the next vertical blank
    ei
    ret

;-----------------------------------------------------------------------
; size-dependent set-up
;-----------------------------------------------------------------------
; 8 bytes per size, copied to r_w .. r_vcol by r_setup: the view's width
; and height in pixels, F = W / 2, strips (W / 8), tile rows (H / 8), the
; height table's bank, and the view's top-left on screen (name-table row
; and column).
r_size_data:          ; w, h, f, strips, rows, table bank, view row, view column
.IFDEF GAMEGEAR
    .db 160, 96, 80, 20, 12, TABLE_BANK_LARGE, 3, 6     ; (the Game Gear: the top of its screen)
    .db 128, 80, 64, 16, 10, TABLE_BANK_SMALL, 4, 8
.ELSE
    .db 160, 96, 80, 20, 12, TABLE_BANK_LARGE, 4, 6
    .db 128, 80, 64, 16, 10, TABLE_BANK_SMALL, 5, 8
.ENDIF

; r_setup: everything that depends on the size, the mode and the cell:
; copies the level header into r_hdr, this size's parameters into r_w ..
; r_vcol, works out r_ntorg, the target tiles (r_base, r_solid), the
; floor tables (r_floor, r_ftile) and the camera (r_vx, r_vz, r_face).
; In: eng_vsize, eng_vmode, var_tile_set, eng_view_x/z/f.
; Leaves slot 2 at LEVEL_BANK. Clobbers AF, BC, DE, HL.
r_setup:
    ; the level header: where every part of the level's data is
    ld a, LEVEL_BANK
    ld ($FFFF), a               ; mapper: bank 3 into slot 2
    ld hl, $8000
    ld de, r_hdr
    ld bc, H_SIZE
    ldir
    ld a, (eng_vsize)
    and 1
    add a, a
    add a, a
    add a, a                    ; 8 bytes per size
    ld e, a
    ld d, 0
    ld hl, r_size_data
    add hl, de
    ld de, r_w
    ld bc, 8
    ldir                        ; r_w, r_h, r_f, r_strips, r_rows, r_tbank, r_vrow, r_vcol
    ; name table origin = NAMETABLE + (row*32 + col)*2
    ld a, (r_vrow)
    ld hl, r_vcol
    ld e, (hl)
    ld l, a
    ld h, 0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl          ; row*32
    ld d, 0
    add hl, de          ; + col
    add hl, hl          ; 2 bytes per entry
    ld de, NAMETABLE
    add hl, de
    ld (r_ntorg), hl
    ; target tiles
    ld a, (eng_vsize)
    or a
    jr nz, +
    ; tile numbering: strip s, tile row r uses tile r_base + s * rows + r,
    ; so each strip's tiles are consecutive in VRAM
    ld hl, 128                  ; large: one set, tiles 128..367, border at 368
    ld (r_base), hl
    ld hl, 368
    ld (r_solid), hl
    ld hl, floor_rows_large
    ld (r_floor), hl
    ld hl, floor_tiles_large
    ld (r_ftile), hl
    jr ++
+:  ld hl, 40                   ; small: border at 40, sets at 128 / 288
                                ; (tiles 40..55, just below the status bar's)
    ld (r_solid), hl
    ld hl, floor_rows_small
    ld (r_floor), hl
    ld hl, floor_tiles_small
    ld (r_ftile), hl
    ; AT ONCE draws into the hidden set (var_tile_set = 0: 288, 1: 128)
    ; and swaps; OUTWARD draws over the set on screen (0: 128, 1: 288)
    ld a, (eng_vmode)
    or a
    ld a, (var_tile_set)        ; (ld keeps the flags of the "or a")
    jr nz, +
    xor 1                       ; OUTWARD: the set on screen
+:  ld hl, 288                  ; a = 0: set 1, a = 1: set 0
    or a
    jr z, +
    ld hl, 128
+:  ld (r_base), hl
++: ; camera: cell centre minus 16 along the facing
    ld a, (eng_view_f)
    and 3
    ld (r_face), a
    ld a, (eng_view_x)
    call cell_centre            ; x * 64 + 32
    ld (r_vx), hl
    ld a, (eng_view_z)
    call cell_centre            ; z * 64 + 32
    ld (r_vz), hl
    ld a, (r_face)
    or a
    jr nz, +
    ld hl, (r_vz)               ; north: forward = -z, camera at z + 16
    ld de, 16
    add hl, de
    ld (r_vz), hl
    ret
+:  dec a
    jr nz, +
    ld hl, (r_vx)               ; east: forward = +x
    ld de, -16
    add hl, de
    ld (r_vx), hl
    ret
+:  dec a
    jr nz, +
    ld hl, (r_vz)               ; south
    ld de, -16
    add hl, de
    ld (r_vz), hl
    ret
+:  ld hl, (r_vx)               ; west
    ld de, 16
    add hl, de
    ld (r_vx), hl
    ret

; a = cell -> hl = a*64 + 32
; (the centre of map cell a in world units). Clobbers DE.
cell_centre:
    ld l, a
    ld h, 0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    ld de, 32
    add hl, de
    ret

; r_clear_columns: no wall in any column yet: COLH_LO/HI = 0 (height 0
; loses to any wall), COLW = $FF; r_nslot = r_nbill = 0. Clears the whole
; pages, including their tails (r_pvs, r_bills, r_slot_tex...), which are
; refilled afterwards. Clobbers AF, BC, DE, HL.
r_clear_columns:
    ld hl, COLH_LO*256
    ld de, COLH_LO*256+1
    ld bc, 255
    ld (hl), 0
    ldir
    ld hl, COLH_HI*256
    ld de, COLH_HI*256+1
    ld bc, 255
    ld (hl), 0
    ldir
    ld hl, COLW*256
    ld de, COLW*256+1
    ld bc, 255
    ld (hl), $FF
    ldir
    xor a
    ld (r_nslot), a
    ld (r_nbill), a
    ret

; copies this view's visibility record into r_pvs (empty if none)
; The visibility index (pvs_index) has one u16 per cell and facing, at
; ((z * 32 + x) * 4 + facing) * 2 (the map is 32 cells wide): 0 = nothing
; visible, else 1 + the record's offset from the first visibility bank
; (bank = H_PVS_BANK + offset / 16 KB). A record: n, n wall ids (u16),
; nb, nb scenery ids (u8); built so it never crosses a bank.
; In: eng_view_x/z, r_face. Out: r_pvs (n = 0 and nb = 0 if none).
; Leaves slot 2 at the record's bank. Clobbers AF, BC, DE, HL.
r_load_pvs:
    xor a
    ld (r_pvs), a               ; n = 0
    ld (r_pvs+1), a             ; nb = 0 (right after an empty wall list)
    ld a, (r_hdr+H_PVS_INDEX_BANK)
    ld ($FFFF), a               ; mapper: the index's bank into slot 2
    ld a, (eng_view_z)
    ld l, a
    ld h, 0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    ld a, (eng_view_x)
    ld e, a
    ld d, 0
    add hl, de
    add hl, hl
    add hl, hl
    ld a, (r_face)
    ld e, a
    add hl, de          ; (z*32 + x)*4 + f
    add hl, hl          ; u16 entries
    ld de, (r_hdr+H_PVS_INDEX_ADDR)
    add hl, de
    ld e, (hl)
    inc hl
    ld d, (hl)
    ld a, d
    or e
    ret z               ; 0: nothing visible from here
    dec de              ; offset into the pvs blob
    ld a, d
    rlca
    rlca
    and 3               ; offset bits 14-15: which bank
    ld hl, r_hdr+H_PVS_BANK
    add a, (hl)
    ld ($FFFF), a       ; mapper: the record's bank into slot 2
    ld a, d
    and $3F
    or $80
    ld h, a
    ld l, e             ; hl = $8000 + (offset & $3FFF)
    ; n walls * 2 + 1, then nb + 1
    ld a, (hl)
    add a, a
    inc a
    ld c, a
    ld b, 0
    push hl
    add hl, bc
    ld a, (hl)          ; nb
    pop hl
    inc a
    add a, c
    ld c, a
    ld b, 0
    jr nc, +
    inc b
+:  ld de, r_pvs
    ldir                ; the whole record (at most 96 bytes)
    ret

;-----------------------------------------------------------------------
; walls
;-----------------------------------------------------------------------
; r_walls: runs r_wall on every wall id in r_pvs. Out: COLH/COLW/COLU
; hold the nearest wall per column, r_slot_tex the slots' textures.
; Clobbers everything.
r_walls:
    ld a, (r_pvs)
    or a
    ret z               ; no walls
    ld b, a
    ld hl, r_pvs+1
-:  push bc
    ld e, (hl)
    inc hl
    ld d, (hl)
    inc hl
    push hl
    call r_wall         ; cull, clip, project and draw into the columns
    pop hl
    pop bc
    djnz -
    ret

; de = wall index
; One wall: skips it if it is an open door, edge-on, or seen from behind
; (unless two-sided: then its ends are swapped so it faces the camera);
; turns its ends into view space; takes the next slot (gives up if all
; MAX_SLOTS are used) and hands over to r_wall_front (parallel to the
; screen: da = db) or r_wall_side (anything else). Both write every
; column where the wall is nearer than what is there already.
; Clobbers everything; slot 2 is left at a data bank.
r_wall:
    ; fetch the record (16 bytes) from the walls bank
    ld a, (r_hdr+H_WALLS_BANK)
    ld ($FFFF), a       ; mapper: the walls' bank into slot 2
    ex de, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl          ; index * 16
    ld de, (r_hdr+H_WALLS_ADDR)
    add hl, de
    ld de, w_ax
    ld bc, 16
    ldir                ; the record -> w_ax .. w_pad
    ; open door?
    ld a, (w_line)
    cp $FF
    jr z, +             ; a plain wall
    ld e, a
    ld d, 0
    ld hl, L_FLAGS      ; the game state's door flags (one byte per door)
    add hl, de
    bit 0, (hl)         ; L_OPEN (game/game.asm)
    ret nz              ; open: not drawn
+:  ; back face: cross = (ax - vx)*sz - (az - vz)*sx, sx/sz = signs of b - a
    ; (walls run along an axis or a diagonal, so the signs alone give the
    ; cross product's sign: > 0 means the camera sees the front)
    ld hl, (w_ax)
    ld de, (r_vx)
    or a
    sbc hl, de
    ld (m_a), hl                ; ax - vx
    ld hl, (w_az)
    ld de, (r_vz)
    or a
    sbc hl, de
    ld (m_b), hl                ; az - vz
    ld hl, 0
    ; + (ax - vx) * sign(bz - az)
    push hl
    ld hl, (w_bz)
    ld de, (w_az)
    call cmp_signed             ; a = sign(hl - de)
    pop hl
    or a
    jr z, ++                    ; sz = 0: no term
    ld de, (m_a)
    jp m, +
    add hl, de                  ; sz = +1
    jr ++
+:  or a
    sbc hl, de                  ; sz = -1
++: ; - (az - vz) * sign(bx - ax)
    push hl
    ld hl, (w_bx)
    ld de, (w_ax)
    call cmp_signed             ; a = sign(bx - ax)
    pop hl
    or a
    jr z, ++
    ld de, (m_b)
    jp m, +
    or a
    sbc hl, de
    jr ++
+:  add hl, de
++: ld a, h
    or l
    ret z                       ; edge on
    bit 7, h
    jr z, +                     ; front side
    ld a, (w_two)
    or a
    ret z                       ; back of a one-sided wall: not drawn
    call swap_ab                ; two-sided: turn it round
+:  ; view space
    ld hl, (w_ax)
    ld de, (w_az)
    call to_view                ; A -> (depth, lateral)
    ld (w_da), hl
    ld (w_la), de
    ld hl, (w_bx)
    ld de, (w_bz)
    call to_view                ; B -> (depth, lateral)
    ld (w_db), hl
    ld (w_lb), de
    ; slot
    ld a, (r_nslot)
    cp MAX_SLOTS
    ret nc                      ; out of slots: the wall is dropped
    ld (w_slot), a              ; (add_slot claims it once the wall is on screen)
    ld hl, (w_da)
    ld de, (w_db)
    or a
    sbc hl, de
    jp z, r_wall_front          ; same depth at both ends: parallel to the screen
    jp r_wall_side              ; otherwise: slanted or along the view

; swaps the wall's ends (a two-sided wall seen from behind)
; Swaps A and B and their texture columns ua and ub. Clobbers AF, B, DE, HL.
swap_ab:
    ld hl, (w_ax)
    ld de, (w_bx)
    ld (w_ax), de
    ld (w_bx), hl
    ld hl, (w_az)
    ld de, (w_bz)
    ld (w_az), de
    ld (w_bz), hl
    ld a, (w_ua)
    ld b, a
    ld a, (w_ub)
    ld (w_ua), a
    ld a, b
    ld (w_ub), a
    ret

; hl = x, de = z (world) -> hl = depth, de = lateral
; Rotates (x - r_vx, z - r_vz) into the camera's frame for r_face.
; Clobbers AF.
to_view:
    push de
    ld de, (r_vx)
    or a
    sbc hl, de                  ; rx
    ex de, hl
    pop hl
    push de
    ld de, (r_vz)
    or a
    sbc hl, de                  ; rz
    ex de, hl                   ; de = rz
    pop hl                      ; hl = rx
    ld a, (r_face)
    or a
    jr nz, +
    ; north: d = -rz, l = rx
    ex de, hl
    call neg_hl                 ; d = -rz
    ret
+:  dec a
    ret z                       ; east: d = rx, l = rz
    dec a
    jr nz, +
    ; south: d = rz, l = -rx
    call neg_hl                 ; l = -rx
    ex de, hl
    ret
+:  ; west: d = -rx, l = -rz
    call neg_hl                 ; d = -rx
    ex de, hl
    call neg_hl                 ; l = -rz
    ex de, hl
    ret

; hl = -hl (two's complement). Clobbers AF.
neg_hl:
    xor a
    sub l
    ld l, a
    sbc a, a
    sub h
    ld h, a
    ret

; a = sign(hl - de): 1, 0 or $FF (flags of a set)
; (Z for 0, M for $FF.) Assumes hl - de does not overflow 16 bits.
; Clobbers HL.
cmp_signed:
    or a
    sbc hl, de
    ld a, h
    or l
    ret z
    bit 7, h
    ld a, 1
    jr z, +
    ld a, $FF
+:  or a
    ret

; hl -> |hl|, a = 1 if it was negative
; (else a = 0). Flags not meaningful.
abs_hl:
    xor a
    bit 7, h
    ret z
    call neg_hl
    ld a, 1
    ret

;-----------------------------------------------------------------------
; projection: hl = d (> 0), de = l -> hl = p (1/256 px from the centre)
;   |l| >= d: +-(F+1)*256;  else p = ((|l|*65536 / d) * F) >> 8
; (i.e. p = 256 * F * l / d; a point outside the 90-degree view lands
; just off the edge, so col_range clamps it to the border.)
; Clobbers everything but IX, IY; uses m_n, m_d, w_neg.
;-----------------------------------------------------------------------
project:
    ld (m_d), hl
    ex de, hl
    call abs_hl                 ; hl = |l|, a = 1 if l < 0
    ld (w_neg), a
    ld de, (m_d)
    push hl
    or a
    sbc hl, de
    pop hl
    jr c, +                     ; |l| < d: on screen
    ld a, (r_f)
    inc a
    ld h, a
    ld l, 0                     ; (F + 1) * 256
    jr _proj_sign
+:  ld (m_n+2), hl
    ld hl, 0
    ld (m_n), hl                ; m_n = |l| << 16
    call div32_16               ; |l| << 16 / d  (< 65536)
    ld hl, (m_n)
    ld a, (r_f)
    call mul_hl_a_hi            ; (hl * a) >> 8
_proj_sign:
    ld a, (w_neg)
    or a
    ret z
    jp neg_hl                   ; (tail call) l < 0: p = -p

; hl = (hl * a) >> 8 (unsigned, a 8-bit, result 16-bit)
; Shift-and-add over a's 8 bits, most significant first.
; Clobbers AF, BC, DE.
mul_hl_a_hi:
    ex de, hl
    ld hl, 0
    ld c, 0                     ; c:hl = 24-bit accumulator (c = top)
    ld b, 8
-:  add hl, hl
    rl c
    rla
    jr nc, +
    add hl, de
    jr nc, +
    inc c
+:  djnz -
    ld l, h
    ld h, c
    ret

; columns covered by p in [w_pa, w_pb] (either order) -> w_x0, w_x1; cf = empty
; (x0 = first column, x1 = the column after the last, both clamped to
; 0..W; in low detail x0 is rounded up to even.) Clobbers everything but
; IX, IY.
col_range:
    ld hl, (w_pa)
    ld de, (w_pb)
    push hl
    call cmp_signed             ; which p is smaller?
    pop hl
    jp m, +
    ex de, hl                   ; hl = smaller
+:  push de
    call p_to_col               ; smaller p -> x0
    ld (w_x0), a
    pop hl
    call p_to_col               ; larger p -> x1
    ld (w_x1), a
    ld a, (eng_detail)          ; low detail: only even columns are worked out
    or a
    jr z, +
    ld a, (w_x0)
    inc a
    and $FE
    ld (w_x0), a
+:  ld a, (w_x1)
    ld b, a
    ld a, (w_x0)
    cp b
    ccf                         ; carry if x0 >= x1
    ret

; hl = p -> a = clamp(((p + 127) >> 8) + F, 0, W)
; (the column boundary nearest to p, numbered like the columns).
; Clobbers DE, HL.
p_to_col:
    ld de, 127
    add hl, de
    ld l, h                     ; arithmetic >> 8
    ld h, 0
    bit 7, l
    jr z, +
    dec h
+:  ld a, (r_f)
    ld e, a
    ld d, 0
    add hl, de
    bit 7, h
    jr z, +
    xor a
    ret
+:  ld a, h
    or a
    jr nz, ++
    ld a, (r_w)
    cp l
    jr c, ++
    ld a, l
    ret
++: ld a, (r_w)
    ret

;-----------------------------------------------------------------------
; a wall parallel to the screen (da == db)
;
; Its height is the same in every column (h8 from the depth). Along the
; wall, the lateral position l at column x's centre is
;   l = (x + 0.5 - F) * d / F,
; kept as l8 = l * 256 and stepped by dl8 = 256 * d / F per column; the
; texture column is u = cu + su * (l8 >> 8) (mod 64), with su the
; direction the texture runs and cu chosen so that u = ua at l = la.
; l8 is kept mod 65536: only l8 >> 8 mod 64 matters.
; Each column x0 .. x1-1 (every other one in low detail) where h8 beats
; COLH gets COLH = h8, COLW = this slot, COLU = u.
;-----------------------------------------------------------------------
r_wall_front:
    ld hl, (w_da)
    bit 7, h
    ret nz                      ; behind the camera
    ld de, 8
    or a
    sbc hl, de
    ret c                       ; nearer than 8: nothing sensible to draw
    ld hl, (w_da)
    ld de, (w_la)
    call project                ; end A on screen
    ld (w_pa), hl
    ld hl, (w_db)
    ld de, (w_lb)
    call project                ; end B on screen
    ld (w_pb), hl
    call col_range              ; -> w_x0, w_x1
    ret c                       ; no column: off screen or too thin
    ; h8 = min(65535, 8192*W / d)
    ld hl, (w_da)
    call height_of_depth        ; h8 = 8192 * W / d
    ld (w_h8), hl
    ; su = sign(ub - ua) * sign(lb - la); cu = (ua - su*la) & 63
    ld hl, (w_lb)
    ld de, (w_la)
    call cmp_signed             ; a = sign(lb - la)
    ld b, a                     ; b = sign(lb - la)
    ld a, (w_ub)
    ld hl, w_ua
    sub (hl)
    jr z, +                     ; ub = ua: su = 0
    jr nc, ++                   ; ub > ua: su = sign(lb - la)
    ld a, b
    neg
    ld b, a                     ; ub < ua: su = -sign(lb - la)
    jr ++
+:  ld b, 0
++: ld a, b
    ld (w_su), a
    ld a, (w_la)
    ld c, a                     ; (only la's low byte matters mod 64)
    ld a, (w_ua)
    bit 7, b
    jr nz, +
    sub c                       ; su >= 0: ua - la
    jr ++
+:  add a, c                    ; su < 0: ua + la
++: and 63
    ld (w_cu), a
    ; l8 at x0 and the step
    ld a, (w_x0)
    ld hl, (w_da)
    call l8_start               ; l * 256 at column x0's centre
    ld (w_l8), hl
    ld hl, (w_da)
    call dl8_of                 ; its step per column
    call ld_double              ; (x2 in low detail)
    ld (w_dl8), hl
    call add_slot               ; claim the slot, remember the texture
    ; ---- the columns
    ld a, (w_x1)
    ld b, a
    ld a, (w_x0)
    ld c, a                     ; c = column
_rwf_col:
    ; nearer (strictly higher) than the column's wall so far?
    ld l, c
    ld h, COLH_HI
    ld a, (w_h8+1)
    cp (hl)
    jr c, _rwf_next             ; high byte lower: behind
    jr nz, _rwf_win             ; high byte higher: in front
    dec h                       ; COLH_LO
    ld a, (w_h8)
    cp (hl)
    jr c, _rwf_next
    jr z, _rwf_next             ; equal: the earlier wall keeps it
_rwf_win:
    ld h, COLH_LO
    ld a, (w_h8)
    ld (hl), a
    inc h                       ; COLH_HI
    ld a, (w_h8+1)
    ld (hl), a
    inc h                       ; COLW
    ld a, (w_slot)
    ld (hl), a
    inc h                       ; COLU
    ; u = (cu + su * (l8 >> 8)) & 63
    ld a, (w_su)
    or a
    ld a, (w_l8+1)              ; (ld keeps the flags of the "or a")
    jp p, +
    neg                         ; su < 0: -(l8 >> 8)
+:  ld e, a
    ld a, (w_su)
    or a
    ld a, e
    jr nz, +
    xor a                       ; su = 0: 0
+:  ld e, a
    ld a, (w_cu)
    add a, e
    and 63
    ld (hl), a                  ; COLU = u
_rwf_next:
    ld hl, (w_l8)
    ld de, (w_dl8)
    add hl, de
    ld (w_l8), hl
    inc c
    ld a, (eng_detail)
    add a, c
    ld c, a                     ; next column (+2 in low detail)
    cp b
    jr c, _rwf_col              ; while c < x1
    ret

; low detail: hl = 2 * hl (the step over two columns)
; (unchanged at high detail). Clobbers AF.
ld_double:
    ld a, (eng_detail)
    or a
    ret z
    add hl, hl
    ret

; add_slot: r_slot_tex[w_slot] = w_tex and r_nslot = w_slot + 1 (the slot
; is used). Clobbers AF, DE, HL.
add_slot:
    ld a, (w_slot)
    ld e, a
    ld d, 0
    ld hl, r_slot_tex
    add hl, de
    ld a, (w_tex)
    ld (hl), a
    ld a, (w_slot)
    inc a
    ld (r_nslot), a
    ret

; hl = d -> hl = min(65535, 8192*W / d)    (d >= 8)
; (h8: the height in 1/256 pixels of a 64-unit wall at depth d.)
; Clobbers everything but IX, IY; uses m_n, m_d.
height_of_depth:
    ld (m_d), hl
    ld a, (r_w)
    ld l, a
    ld h, 0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl                  ; W << 5
    ld (m_n+1), hl              ; m_n = W << 13
    xor a
    ld (m_n), a
    ld (m_n+3), a
    call div32_16
    ld a, (m_n+2)
    ld hl, m_n+3
    or (hl)                     ; quotient above 16 bits?
    ld hl, (m_n)
    ret z
    ld hl, $FFFF                ; yes: clamp
    ret

; a = x0, hl = d -> hl = ((x0*256 + 128) * d / F - 256*d) & $FFFF
; (= 256 * the lateral position at column x0's centre, at depth d.)
; Clobbers everything but IY, IX included (mul16 uses it): callers that
; need IX push it.
l8_start:
    push hl
    ld (m_b), hl                ; d
    ld h, a
    ld l, 128
    ld (m_a), hl
    call mul16                  ; m_p = (x0*256+128) * d
    ld hl, (m_p)
    ld (m_n), hl
    ld hl, (m_p+2)
    ld (m_n+2), hl
    ld a, (r_f)
    ld l, a
    ld h, 0
    ld (m_d), hl
    call div32_16
    pop de                      ; d
    ld hl, (m_n)
    ld a, h
    sub e                       ; - 256*d (mod 2^16): high byte minus d's low byte
    ld h, a
    ret

; hl = d -> hl = 256*d / F
; (how far l8 moves per screen column at depth d). Clobbers everything
; but IX, IY.
dl8_of:
    ld a, h
    ld (m_n+2), a
    ld a, l
    ld (m_n+1), a
    xor a
    ld (m_n), a
    ld (m_n+3), a
    ld a, (r_f)
    ld l, a
    ld h, 0
    ld (m_d), hl
    call div32_16
    ld hl, (m_n)
    ret

;-----------------------------------------------------------------------
; any other wall: l = l0 + m*d with m in {0, 1, -1}
;
; (m = 0: the wall runs straight away from the camera; m = +-1: a
; diagonal.) The wall is clipped at depth R_NEAR. Column x's ray has
; l / d = (x + 0.5 - F) / F; where it meets the wall,
;   h8 = 8192 * W / d = N(x) / l0,  N(x) = 16384*(x - F) + 8192 - 8192*W*m,
; which is linear in x: so h8 << 8 starts at N(x0) * 256 / l0 (w_hacc)
; and steps by 16384 * 256 / l0 per column (w_dacc), with no division
; per column. The texture column follows the depth (one texel per unit
; along d): u = cu + su * d (mod 64), d taken from the height table's
; entry for h8.
; Each column x0 .. x1-1 (every other one in low detail) where h8 beats
; COLH gets COLH = h8, COLW = this slot, COLU = u. Leaves slot 2 at the
; height table's bank.
;-----------------------------------------------------------------------
r_wall_side:
    ; both nearer than NEAR?
    ld hl, (w_da)
    ld de, R_NEAR
    call cmp_signed             ; a = sign(da - NEAR)
    push af
    ld hl, (w_db)
    ld de, R_NEAR
    call cmp_signed             ; a = sign(db - NEAR)
    pop bc                      ; b = sign(da - NEAR)
    and b
    ret m                       ; both negative
    ; m = (lb - la) / (db - da)
    ld hl, (w_lb)
    ld de, (w_la)
    call cmp_signed             ; a = sign(lb - la)
    ld b, a                     ; sign(lb - la)
    ld hl, (w_db)
    ld de, (w_da)
    call cmp_signed
    ld c, a                     ; sign(db - da)
    ld a, b
    or a
    jr z, +                     ; la = lb: m = 0
    xor c                       ; same sign -> 0 (m = 1), else $FE -> m = -1
    jr z, ++
    ld a, $FF
    jr +
++: ld a, 1
+:  ld (w_m), a
    ; clip at NEAR
    ld hl, (w_da)
    ld de, R_NEAR
    call cmp_signed             ; is A nearer than NEAR?
    jp p, +                     ; A not nearer than NEAR: try B
    ; A too near: move it along the wall to depth NEAR
    ld hl, R_NEAR
    ld de, (w_da)
    or a
    sbc hl, de                  ; NEAR - da
    call m_times_hl             ; m * (NEAR - da): the lateral change
    ld de, (w_la)
    add hl, de
    ld (w_la), hl
    ld hl, R_NEAR
    ld (w_da), hl
    jr ++
+:  ld hl, (w_db)
    ld de, R_NEAR
    call cmp_signed             ; is B nearer than NEAR?
    jp p, ++
    ld hl, R_NEAR
    ld de, (w_db)
    or a
    sbc hl, de
    call m_times_hl             ; m * (NEAR - db)
    ld de, (w_lb)
    add hl, de
    ld (w_lb), hl
    ld hl, R_NEAR
    ld (w_db), hl
++: ; l0 = la - m*da
    ld hl, (w_da)
    call m_times_hl             ; m * da
    ex de, hl
    ld hl, (w_la)
    or a
    sbc hl, de
    ld (w_l0), hl
    ld a, h
    or l
    ret z                       ; l0 = 0: the wall's line passes through the eye
    ; columns
    ld hl, (w_da)
    ld de, (w_la)
    call project                ; end A on screen
    ld (w_pa), hl
    ld hl, (w_db)
    ld de, (w_lb)
    call project                ; end B on screen
    ld (w_pb), hl
    call col_range              ; -> w_x0, w_x1
    ret c                       ; nothing on screen
    ; N0 = 16384*(x0 - F) + 8192 - 8192*W*m  -> |N0| << 8 in m_n (32-bit)
    call side_n0
    ; hacc = (|N0| << 8) / |l0|
    ld hl, (w_l0)
    call abs_hl                 ; |l0|
    ld (m_d), hl
    call div32_16
    ld hl, (m_n)
    ld (w_hacc), hl
    ld hl, (m_n+2)
    ld (w_hacc+2), hl
    ; dacc = (16384 << 8) / |l0|, negated when l0 < 0
    ld hl, 0
    ld (m_n), hl
    ld hl, $0040                ; 16384 << 8 = $00400000
    ld (m_n+2), hl
    call div32_16
    ld hl, (w_l0+1)
    bit 7, l                    ; l0's sign bit (l = high byte of l0)
    jr z, +
    ; negate the 32-bit quotient
    ; (N0 and l0 have the same sign, as h8 > 0, so |N0| / |l0| needs no
    ; sign; the step 16384 / l0 does)
    ld hl, m_n
    ld b, 4
    xor a
    scf
    ccf
-:  ld a, 0
    sbc a, (hl)
    ld (hl), a
    inc hl
    djnz -
+:  ld a, (eng_detail)         ; low detail: two columns per step
    or a
    jr z, +
    ld hl, m_n
    sla (hl)
    inc hl
    rl (hl)
    inc hl
    rl (hl)
    inc hl
    rl (hl)
+:  ld hl, (m_n)
    ld (w_dacc), hl
    ld hl, (m_n+2)
    ld (w_dacc+2), hl
    ; su = sign(ub - ua) * sign(db - da); cz = (ua - su*da) & 63
    ld hl, (w_db)
    ld de, (w_da)
    call cmp_signed             ; a = sign(db - da)
    ld b, a
    ld a, (w_ub)
    ld hl, w_ua
    sub (hl)
    jr z, +
    jr nc, ++
    ld a, b
    neg
    ld b, a
    jr ++
+:  ld b, 0
++: ld a, b
    ld (w_su), a
    ld a, (w_da)
    ld c, a
    ld a, (w_ua)
    bit 7, b
    jr nz, +
    sub c
    jr ++
+:  add a, c
++: and 63
    ld (w_cu), a
    call add_slot               ; claim the slot, remember the texture
    ; ---- the columns (table bank paged for the depth lookups)
    ld a, (r_tbank)
    ld ($FFFF), a               ; mapper: this size's height table into slot 2
    ld a, (w_x0)
    ld c, a                     ; c = column
_rws_col:
    ; h8 = clamp(hacc >> 8, 1, 65535)
    ld a, (w_hacc+3)
    or a
    jr z, +
    ld de, $FFFF                ; too big: clamp
    bit 7, a
    jr z, ++
    ld de, 1                    ; negative: clamp to 1
    jr ++
+:  ld de, (w_hacc+1)
    ld a, d
    or e
    jr nz, ++
    inc e                       ; 0 -> 1
++: ; de = h8: nearer than this column's wall?
    ld l, c
    ld h, COLH_HI
    ld a, d
    cp (hl)
    jr c, _rws_next
    jr nz, _rws_win
    dec h
    ld a, e
    cp (hl)
    jr c, _rws_next
    jr z, _rws_next
_rws_win:
    ld h, COLH_LO
    ld (hl), e
    inc h                       ; COLH_HI
    ld (hl), d
    inc h                       ; COLW
    ld a, (w_slot)
    ld (hl), a
    push hl
    ex de, hl
    call table_entry            ; hl = entry
    ld de, 6
    add hl, de
    ld a, (hl)                  ; depth (low byte is all u needs)
    ld e, a
    ld a, (w_su)
    or a
    jr z, +
    ld a, e
    jp p, ++
    neg
    jr ++
+:  xor a
++: ld e, a
    ld a, (w_cu)
    add a, e
    and 63
    pop hl
    inc h                       ; COLU
    ld (hl), a                  ; u = cu + su * d (mod 64)
_rws_next:
    ; hacc += dacc
    ld hl, (w_hacc)
    ld de, (w_dacc)
    add hl, de
    ld (w_hacc), hl
    ld hl, (w_hacc+2)
    ld de, (w_dacc+2)
    adc hl, de
    ld (w_hacc+2), hl
    inc c
    ld a, (eng_detail)
    add a, c
    ld c, a
    ld a, (w_x1)
    dec a
    cp c
    jr nc, _rws_col             ; while c < x1
    ret

; hl = m * hl (m in w_m)
; (m = 0: 0; +1: hl; -1: -hl). Clobbers AF.
m_times_hl:
    ld a, (w_m)
    or a
    jr nz, +
    ld hl, 0
    ret
+:  ret p
    jp neg_hl                   ; (tail call) m = -1: -hl

; |N0| << 8 into m_n, N0 = 16384*(x0 - F) + 8192 - 8192*W*m
; m_p holds N0 as a 32-bit signed number while it is built. The result
; keeps m_p's low 3 bytes (|N0| < 2^24 for any view here).
; In: w_x0, r_f, r_w, w_m. Clobbers AF, B, DE, HL, m_p.
side_n0:
    ; 32-bit signed sum in m_p
    ld a, (w_x0)
    ld hl, r_f
    sub (hl)                    ; x0 - F (signed byte)
    ld l, a
    rla
    sbc a, a
    ld h, a                     ; hl = x0 - F
    ld e, a                     ; sign byte
    ; m_p = (x0 - F) << 14
    ld (m_p), hl
    ld a, e
    ld (m_p+2), a
    ld (m_p+3), a
    ld b, 14
-:  ld hl, m_p
    sla (hl)
    inc hl
    rl (hl)
    inc hl
    rl (hl)
    inc hl
    rl (hl)
    djnz -
    ; + 8192
    ld hl, (m_p)
    ld de, 8192
    add hl, de
    ld (m_p), hl
    ld hl, (m_p+2)
    ld de, 0
    adc hl, de
    ld (m_p+2), hl
    ; - 8192*W*m  (8192*W = W << 13)
    ld a, (w_m)
    or a
    jr z, ++
    push af
    ld a, (r_w)
    ld l, a
    ld h, 0
    ld b, 5
-:  add hl, hl
    djnz -                      ; W << 5  -> << 8 more by bytes
    ; value = (W << 5) << 8 : bytes 1..2 = hl
    pop af
    jp p, +
    ; m = -1: add
    ld a, (m_p+1)
    add a, l
    ld (m_p+1), a
    ld a, (m_p+2)
    adc a, h
    ld (m_p+2), a
    ld a, (m_p+3)
    adc a, 0
    ld (m_p+3), a
    jr ++
+:  ld a, (m_p+1)
    sub l
    ld (m_p+1), a
    ld a, (m_p+2)
    sbc a, h
    ld (m_p+2), a
    ld a, (m_p+3)
    sbc a, 0
    ld (m_p+3), a
++: ; absolute value
    ld a, (m_p+3)
    bit 7, a
    jr z, +
    ld hl, m_p
    ld b, 4
    or a
-:  ld a, 0
    sbc a, (hl)
    ld (hl), a
    inc hl
    djnz -
+:  ; m_n = |m_p| << 8
    xor a
    ld (m_n), a
    ld hl, (m_p)
    ld (m_n+1), hl
    ld a, (m_p+2)
    ld (m_n+3), a
    ret

; hl = h8 -> hl = address of its table entry (table bank paged)
; Entries are 8 bytes (y0, y1, v0, dv, d) from $8000. Heights below
; $4000 (64 px) get one entry per 16 units of h8 (entries 0..1023);
; taller ones one per 64 (entries 1024..1791), finer where it matters.
; tools/live_data.py hidx() is the same mapping. Clobbers AF.
table_entry:
    ld a, h
    cp $40
    jr nc, +
    ; idx = h8 >> 4; entry = idx * 8 = h8 >> 1 (with low 3 bits clear)
    srl h
    rr l
    ld a, l
    and $F8
    ld l, a
    ld a, h
    or $80
    ld h, a
    ret
+:  ; idx = 1024 + ((h8 - $4000) >> 6); entry = idx*8 = 8192 + ((h8 - $4000) >> 3)
    sub $40
    ld h, a
    srl h
    rr l
    srl h
    rr l
    srl h
    rr l
    ld a, l
    and $F8
    ld l, a
    ld a, h
    add a, $A0                  ; $8000 + 8192
    ld h, a
    ret

;-----------------------------------------------------------------------
; maths
;-----------------------------------------------------------------------
; m_n (32-bit) / m_d (16-bit) -> m_n quotient (32-bit), hl = remainder.
; Done as two 16-bit steps (the high word first, skipped when it is
; smaller than the divisor), each a register-only long division.
; m_d must not be 0. Clobbers AF, BC, DE, HL.
div32_16:
    ld de, (m_d)
    ld hl, (m_n+2)              ; high word
    or a
    sbc hl, de
    add hl, de                  ; hl back; carry set if hl < de
    jr c, +                     ; high < d: quotient high = 0, remainder = high
    ld bc, (m_n+2)
    ld hl, 0
    call div_hlbc_de            ; 0:high / d -> high quotient, hl = remainder
    ld (m_n+2), bc
    jr ++
+:  ld bc, 0
    ld (m_n+2), bc
++: ld bc, (m_n)
    call div_hlbc_de            ; remainder:low / d -> low quotient
    ld (m_n), bc
    ret

; (hl:bc) / de with hl < de -> bc = quotient, hl = remainder.
; Unrolled; a divisor below 256 (the common case: F, and nearby depths)
; keeps the remainder in a and takes half the time. Same results.
; Restoring division: 16 times, shift hl:bc left one bit, and if the
; remainder (hl) is now >= de, subtract de and set the new quotient bit
; (bit 0 of c). Clobbers AF.
div_hlbc_de:
    ld a, d
    or a
    jp z, _dq_small             ; divisor < 256
.rept 16
    sla c
    rl b
    adc hl, hl
    jr c, +                     ; past 16 bits: certainly >= de
    sbc hl, de                  ; (carry is clear here)
    jr nc, ++
    add hl, de
    jr +++
+:  or a
    sbc hl, de
++: inc c
+++:
.endr
    ret
_dq_small:                      ; d = 0: remainder < e fits in a
    ld a, l
.rept 16
    sla c
    rl b
    rla
    jr c, +                     ; past 8 bits: certainly >= e
    cp e
    jr c, ++
+:  sub e
    inc c
++:
.endr
    ld l, a
    ld h, 0
    ret

; m_p = m_a * m_b (16 x 16 -> 32, unsigned)
; Shift-and-add, most significant bit of m_a first, with the product in
; hl:ix. Clobbers AF, BC, DE, HL and IX.
mul16:
    ld bc, (m_a)
    ld de, (m_b)
    ld hl, 0                    ; high word, ix = low word
    ld ix, 0
    ld a, 16
_mul_loop:
    add ix, ix
    adc hl, hl
    sla c
    rl b
    jr nc, +
    add ix, de
    jr nc, +
    inc hl
+:  dec a
    jr nz, _mul_loop
    ld (m_p), ix
    ld (m_p+2), hl
    ret

;-----------------------------------------------------------------------
; billboards: projected, kept in r_bills, sorted far to near (r_order)
; entry: d (2), spr, x0, x1, h8 (2), c8 (2), dc8 (2), y0, y1, pad (3)
;-----------------------------------------------------------------------
; (the bytes the line above calls "pad" now hold B_CX and B_V0)
.define B_D     0               ; u16 depth (d - 16; used for sorting)
.define B_SPR   2               ; billboard sprite number
.define B_X0    3               ; first screen column, and the column after the last
.define B_X1    4
.define B_H8    5               ; u16 h8 of its depth (for the depth test and the table)
.define B_C8    7               ; u16 canvas column * 256 at column B_CX
.define B_DC8   9               ; u16 canvas columns * 256 per screen column
.define B_Y0    11              ; first and after-last screen rows that show canvas rows
.define B_Y1    12
.define B_CX    13              ; the screen column B_C8 belongs to (moves as strips are drawn)
.define B_V0    14              ; v at B_Y0 (rows trimmed to the sprite's canvas rows)

; r_billboards: projects every scenery id in r_pvs (r_bill) and sorts
; r_order far to near, so nearer billboards are drawn over farther ones.
; Clobbers everything.
r_billboards:
    ld a, (r_pvs)
    add a, a
    ld e, a
    ld d, 0
    ld hl, r_pvs+1
    add hl, de                  ; past the n wall ids
    ld a, (hl)                  ; nb
    or a
    ret z
    ld b, a
    inc hl
-:  push bc
    ld a, (hl)
    inc hl
    push hl
    call r_bill                 ; project one (if it is on screen)
    pop hl
    pop bc
    djnz -
    ; sort: insertion sort of r_order by d, descending (stable)
    ld a, (r_nbill)
    cp 2
    ret c                       ; 0 or 1: nothing to sort
    ld c, 1                     ; i = 1
_bs_outer:
    ld a, (r_nbill)
    cp c
    ret z                       ; i = n: done
    ld b, c                     ; j = i
    ; move entry j down while the one before it is nearer
_bs_inner:
    ld a, b
    or a
    jr z, _bs_next
    ; compare d[order[j-1]] < d[order[j]] -> swap
    ld hl, r_order
    ld e, b
    ld d, 0
    add hl, de
    ld a, (hl)
    push hl
    call bill_ptr               ; ix = entry order[j]
    ld e, (ix+B_D)
    ld d, (ix+B_D+1)
    pop hl
    dec hl
    ld a, (hl)
    push hl
    push de
    call bill_ptr               ; ix = entry order[j-1]
    ld l, (ix+B_D)
    ld h, (ix+B_D+1)
    pop de
    or a
    sbc hl, de                  ; d[j-1] - d[j]
    pop hl
    jr nc, _bs_next             ; d[j-1] >= d[j]: in order
    ld a, (hl)
    inc hl
    ld e, (hl)
    ld (hl), a
    dec hl
    ld (hl), e
    dec b
    jr _bs_inner
_bs_next:
    inc c
    jr _bs_outer

; a = bill slot -> ix
; (ix = r_bills + a * 16). Clobbers AF, DE.
bill_ptr:
    add a, a
    add a, a
    add a, a
    add a, a
    ld e, a
    ld d, 0
    ld ix, r_bills
    add ix, de
    ret

; a = scenery index
; Projects one prop and, if any of it is on screen, adds it to r_bills
; (and r_order): depth, columns, rows trimmed to the sprite's canvas,
; the canvas column at x0 and its step. Props nearer than R_NEAR (after
; the 16 below) are skipped, and so is everything once MAX_BILLS are in.
; The prop is a 64-unit-wide picture centred on (x, z), facing the
; camera. Clobbers everything, IX included.
r_bill:
    ld b, a
    ld a, (r_nbill)
    cp MAX_BILLS
    ret nc                      ; list full
    ; scenery record: x, z, spr, pad (in this bank)
    ld l, b
    ld h, 0
    add hl, hl
    ld e, l
    ld d, h
    add hl, hl
    add hl, de                  ; *6
    ld a, (r_hdr+H_SCENERY_BANK)
    ld ($FFFF), a               ; mapper: the props' bank into slot 2
    ld de, (r_hdr+H_SCENERY_ADDR)
    add hl, de
    push hl
    ld e, (hl)
    inc hl
    ld d, (hl)
    inc hl
    push de
    ld e, (hl)
    inc hl
    ld d, (hl)
    pop hl                      ; hl = x, de = z
    call to_view                ; the prop's centre -> (depth, lateral)
    ; d -= 16
    ; (the props' depth is taken 16 units nearer, as in tools/live_data.py)
    ld bc, -16
    add hl, bc
    ld (w_da), hl
    ld (w_la), de
    pop hl
    inc hl
    inc hl
    inc hl
    inc hl
    ld a, (hl)
    ld (w_tex), a               ; sprite slot
    ld hl, (w_da)
    ld de, R_NEAR
    call cmp_signed             ; a = sign(d - NEAR)
    ret m                       ; too near (or behind)
    ; columns of l - 32 .. l + 32
    ld hl, (w_la)
    ld de, -32
    add hl, de
    ex de, hl
    ld hl, (w_da)
    call project                ; left edge (l - 32)
    ld (w_pa), hl
    ld hl, (w_la)
    ld de, 32
    add hl, de
    ex de, hl
    ld hl, (w_da)
    call project                ; right edge (l + 32)
    ld (w_pb), hl
    call col_range              ; -> w_x0, w_x1
    ret c                       ; off screen
    ld a, (r_nbill)
    ld hl, r_order
    ld e, a
    ld d, 0
    add hl, de
    ld (hl), a                  ; r_order[n] = n (sorted later)
    call bill_ptr               ; ix = the new entry
    ld hl, (w_da)
    ld (ix+B_D), l
    ld (ix+B_D+1), h
    ld a, (w_tex)
    ld (ix+B_SPR), a
    ld a, (w_x0)
    ld (ix+B_X0), a
    ld a, (w_x1)
    ld (ix+B_X1), a
    ld a, (w_x0)
    ld (ix+B_CX), a
    push ix
    call height_of_depth        ; h8 of the prop's depth
    pop ix
    ld (ix+B_H8), l
    ld (ix+B_H8+1), h
    ; rows of this height, trimmed to the rows where the sprite's canvas
    ; rows [first, end) show (v from the table entry, stepped by dv)
    ld a, (r_tbank)
    ld ($FFFF), a               ; mapper: the height table into slot 2
    call table_entry            ; hl = the height table entry for h8
    ld c, (hl)                  ; y0
    inc hl
    ld b, (hl)                  ; y1
    inc hl
    ld e, (hl)
    inc hl
    ld d, (hl)                  ; v0
    inc hl
    ld a, (hl)
    ld (s_dv), a
    inc hl
    ld a, (hl)
    ld (s_dv+1), a
    ex de, hl                   ; hl = v
    push hl
    ld a, (w_tex)               ; sprite: its canvas rows from bsprite_dir
    add a, a
    add a, a
    add a, a                    ; 8 bytes per directory entry
    ld e, a
    ld d, 0
    ld a, (r_hdr+H_BSPRITE_DIR_BANK)
    ld ($FFFF), a               ; mapper: the billboard directory into slot 2
    ld hl, (r_hdr+H_BSPRITE_DIR_ADDR)
    add hl, de
    inc hl
    inc hl
    inc hl
    inc hl                      ; skip the u32 offset
    ld a, (hl)
    ld (s_btop), a              ; first row
    inc hl
    ld a, (hl)
    ld (s_blen), a              ; end row
    pop hl
    ; skip rows above the first canvas row
    ; (c = screen row, b = y1, hl = v; h = canvas row)
-:  ld a, c
    cp b
    jr nc, +                    ; out of rows
    ld a, (s_btop)
    cp h
    jr z, +
    jr c, +                     ; v int >= first
    ld de, (s_dv)
    add hl, de
    inc c
    jr -
+:  ld (ix+B_Y0), c
    ld (ix+B_V0), l
    ld (ix+B_V0+1), h
    ; and stop at the end row
-:  ld a, c
    cp b
    jr nc, +
    ld a, (s_blen)
    dec a
    cp h
    jr c, +                     ; v int >= end
    ld de, (s_dv)
    add hl, de
    inc c
    jr -
+:  ld (ix+B_Y1), c
    ; c8 = l8_start(x0, d) - (l - 32)*256
    ; (the canvas column, * 256, at column x0: one canvas column per
    ; world unit across the 64-unit picture; only the high byte changes)
    push ix
    ld a, (w_x0)
    ld hl, (w_da)
    call l8_start
    pop ix
    ld a, (w_la)
    sub 32
    ld e, a
    ld a, h
    sub e
    ld (ix+B_C8+1), a
    ld (ix+B_C8), l
    push ix
    ld hl, (w_da)
    call dl8_of                 ; step per column
    call ld_double              ; (x2 in low detail)
    pop ix
    ld (ix+B_DC8), l
    ld (ix+B_DC8+1), h
    ld hl, r_nbill
    inc (hl)                    ; the entry is in
    ret

;-----------------------------------------------------------------------
; strips
;-----------------------------------------------------------------------
; strips in order from the middle outwards (large: 9, 10, 8, 11 ... 0,
; 19; small: 7, 8, 6, 9 ... 0, 15), so that in OUTWARD mode the picture
; grows from the centre
; Out: every strip drawn (r_strip), r_masks filled. Clobbers everything.
r_strips_all:
    ld a, (r_strips)
    srl a
    ld (s_left), a
    ld (s_right), a             ; both start at the middle
-:  ld a, (s_left)
    or a
    ret z
    dec a
    ld (s_left), a
    call _strip_at              ; the next strip to the left
    ld a, (s_right)
    call _strip_at              ; and the next to the right
    ld hl, s_right
    inc (hl)
    jr -
; a = strip
; Sets s_s and s_x (= a * 8) and draws it (r_strip, tail call).
_strip_at:
    ld (s_s), a
    add a, a
    add a, a
    add a, a
    ld (s_x), a
    jp r_strip                  ; (tail call) draw it

; r_strip: draws strip s_s (screen columns s_x .. s_x + 7).
; Pass 1 gathers each column's rows and texture into s_cols / s_dvhi and
; the strip's extent (s_miny0 .. s_maxy1, widened by billboards); that
; gives the tile rows that need drawing, s_rc .. s_rf - 1 (above them
; all ceiling, below all floor). Pass 2 fills those rows of the chunky
; buffer, column by column; then the billboards go over them
; (strip_bills), the rows are converted and uploaded (strip_tiles), and
; strip_finish records s_rc / s_rf and (OUTWARD) writes the names.
; Clobbers everything; leaves slot 2 at some data bank.
r_strip:
    ; ---- pass 1: per column rows and texture column
    ld a, (r_tbank)
    ld ($FFFF), a               ; mapper: the height table into slot 2
    ld a, (r_h)
    ld (s_miny0), a
    xor a
    ld (s_maxy1), a
    ld (s_c), a
    ld ix, s_cols               ; ix = this column's 8-byte entry
_sp1:
    ld a, (s_x)
    ld hl, s_c
    add a, (hl)
    ld l, a                     ; screen column
    ld h, COLW
    ld a, (hl)                  ; its wall's slot
    cp $FF
    jr nz, +
    ; no wall: y0 = y1 = H/2, no texture
    ld a, (r_h)
    srl a
    ld (ix+0), a
    ld (ix+1), a
    ld (ix+6), 0
    jp _sp1_minmax
+:  ; texture column: ci = texcols[tex*64 + u]
    ; (texcols: per texture, 64 u16 column ids; column id ci's 64 pair
    ; bytes are at bank H_TEX_BANK + ci / 256, address $8000 + (ci & 255) * 64)
    push hl
    ld e, a
    ld d, 0
    ld hl, r_slot_tex
    add hl, de
    ld a, (hl)                  ; tex
    pop hl
    inc h
    ld e, (hl)                  ; u
    ld l, a
    ld h, 0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl                  ; tex*64
    ld d, 0
    add hl, de
    add hl, hl
    ld a, (r_hdr+H_TEXCOLS_BANK)
    ld ($FFFF), a               ; mapper: the texcols table into slot 2
    ld de, (r_hdr+H_TEXCOLS_ADDR)
    add hl, de
    ld e, (hl)
    inc hl
    ld d, (hl)                  ; ci
    ld a, (r_tbank)
    ld ($FFFF), a               ; back to the height table
    ld a, (r_hdr+H_TEX_BANK)
    add a, d
    ld (ix+6), a                ; bank
    ; addr = $8000 + (ci & 255) * 64 -> hi = $80 + (ci & 255) >> 2, lo = (ci & 3) << 6
    ld a, e
    rrca
    rrca
    ld b, a
    and $3F
    or $80
    ld (ix+7), a                ; addr hi
    ld a, b
    and $C0
    ld (ix+5), a                ; addr lo (5 = spare byte of dv? no: use 5)
                                ; (byte 5 holds the address's low byte;
                                ; dv's high byte goes to s_dvhi instead)
    ; rows from the table
    ld a, (s_x)
    ld hl, s_c
    add a, (hl)
    ld l, a
    ld h, COLH_HI
    ld d, (hl)
    dec h
    ld e, (hl)
    ex de, hl
    call table_entry            ; hl = the entry for this column's h8
    ld a, (hl)
    ld (ix+0), a
    inc hl
    ld a, (hl)
    ld (ix+1), a
    inc hl
    ld a, (hl)
    ld (ix+2), a
    inc hl
    ld a, (hl)
    ld (ix+3), a
    inc hl
    ld a, (hl)
    ld (ix+4), a                ; dv lo
    inc hl
    ld a, (hl)
    ld (s_tmp_dvhi), a
    ; pack: entry 8 bytes = y0, y1, v0lo, v0hi, dvlo, addrlo, bank, addrhi; dv hi kept in v0hi's top? no:
    ; dv hi needs a byte: store it in the column's 9th... use the s_dvhi array
    ; (that is: the 8-byte entry has no room for dv's high byte, so it
    ; goes into s_dvhi[s_c])
    ld a, (s_c)
    ld e, a
    ld d, 0
    ld hl, s_dvhi
    add hl, de
    ld a, (s_tmp_dvhi)
    ld (hl), a
_sp1_minmax:
    ; widen the strip's extent: s_miny0 = min(y0), s_maxy1 = max(y1)
    ld a, (ix+0)
    ld hl, s_miny0
    cp (hl)
    jr nc, +
    ld (hl), a
+:  ld a, (ix+1)
    ld hl, s_maxy1
    cp (hl)
    jr c, +
    ld (hl), a
+:
    ld de, 8
    ld a, (eng_detail)          ; low detail: even columns only
    or a
    jr z, +
    ld e, 16
+:  add ix, de
    ld hl, s_c
    inc (hl)
    add a, (hl)
    ld (hl), a                  ; s_c += 1 (+ 1 more in low detail)
    cp 8
    jp nz, _sp1
    ; ---- billboards over this strip widen the rows
    call strip_bill_rows        ; (also sets s_hasbill)
    ; tile rows: rc = miny0 >> 3, rf = (maxy1 + 7) >> 3
    ld a, (s_miny0)
    rrca
    rrca
    rrca
    and $1F
    ld (s_rc), a
    ld a, (s_maxy1)
    add a, 7
    rrca
    rrca
    rrca
    and $1F
    ld (s_rf), a
    ld b, a
    ld a, (s_rc)
    cp b
    jr c, +
    ; nothing but solid tiles
    ld (s_rf), a                ; (rf = rc: no tile rows of its own)
    jp strip_finish             ; (tail call) record rc = rf
+:  ld a, (r_dbg)
    bit 0, a
    jp nz, _sp2_skip            ; (test switch: no fill)
    ; ---- pass 2: fill rows rc*8 .. rf*8 of every column
    ld a, (eng_frame)
    ld (r_prof+7), a            ; profile: fill start
    xor a
    ld (s_c), a
    ld ix, s_cols
_sp2:
    ; A column pair whose two columns show the same thing (a near wall:
    ; each texel spans several columns) is filled once with whole pair
    ; bytes in its even column, and its odd column is zeroed: the tile
    ; conversion ORs the two, so the picture is the same. Not in low detail
    ; (whole bytes anyway), nor where billboards (nibble by nibble) are.
    ld a, (eng_detail)
    ld hl, s_hasbill
    or (hl)
    ld hl, s_c
    or (hl)                     ; (and only at the even column of a pair)
    rra                         ; (all three are 0/1 or a column: bit 0 says)
    jr c, _sp2_one
    call pair_same              ; do columns s_c and s_c + 1 match?
    jr nz, _sp2_one
    ld a, 1
    ld (s_whole), a
    call fill_column            ; the even column, whole bytes
    call zero_odd               ; and its partner cleared
    ld de, 16
    add ix, de
    ld hl, s_c
    inc (hl)
    inc (hl)
    jr _sp2_next
_sp2_one:
    ld a, (eng_detail)          ; low detail: whole bytes, even columns only
    ld (s_whole), a
    call fill_column            ; one column, nibbles by parity
    ld de, 8
    ld a, (eng_detail)
    or a
    jr z, +
    ld e, 16
+:  add ix, de
    ld hl, s_c
    inc (hl)
    add a, (hl)
    ld (hl), a
_sp2_next:
    ld a, (s_c)
    cp 8
    jr nz, _sp2
    ld a, (r_prof+7)
    ld b, a
    ld a, (eng_frame)
    sub b
    ld hl, r_prof+6
    add a, (hl)
    ld (hl), a                  ; profile: frames spent filling
_sp2_skip:
    call strip_bills            ; billboards over the chunky buffer
    call strip_tiles            ; chunky -> planar tiles in VRAM
    jp strip_finish             ; (tail call) r_masks, and names if OUTWARD

; z if the columns at ix and ix+8 (s_c and s_c + 1) have the same entry
; and texture step (then they show the same pixels, column for column)
; Clobbers AF, B, DE, HL.
pair_same:
    push ix
    pop de
    ld hl, 8
    add hl, de
    ld b, 8
-:  ld a, (de)
    cp (hl)
    ret nz
    inc de
    inc hl
    djnz -
    ld a, (s_c)
    ld e, a
    ld d, 0
    ld hl, s_dvhi
    add hl, de
    ld a, (hl)
    inc hl
    cp (hl)
    ret

; zeroes rows rc*8 .. rf*8 of the odd column s_c + 1 (s_c even)
; Jumps into zfill (96 unrolled "ld (hl), a / inc l" pairs, in
; build/gfx/live/unroll.inc) at copy 96 - n so that exactly n rows are
; written; zfill's ret returns to zero_odd's caller. Clobbers AF, B,
; DE, HL.
zero_odd:
    ld a, (s_c)
    srl a
    add a, CHUNKY
    ld h, a
    ld a, (s_rc)
    add a, a
    add a, a
    add a, a
    ld b, a
    or 128
    ld l, a                     ; the odd column's half of the page
    ld a, (s_rf)
    add a, a
    add a, a
    add a, a
    sub b
    ld b, a                     ; rows
    ld a, 96
    sub b
    add a, a                    ; entry = zfill + (96 - n) * 2
    ld e, a
    ld d, 0
    push hl
    ld hl, zfill
    add hl, de
    ex (sp), hl                 ; (sp) = entry, hl = dest
    xor a
    ret                         ; -> zfill copies; their ret returns to our caller

; fills chunky column s_c (rows rc*8 .. rf*8) from the entry at ix.
; s_whole = 1: whole pair bytes (low detail, or both columns of a pair the same).
; Each pixel keeps only the nibble it shows: high when x + y is even.
; Three runs, each done by jumping into unrolled code (unroll.inc) at the
; copy that leaves exactly the right number of rows:
;   ceiling rows rc*8 .. y0-1: zeros (zfill);
;   wall rows y0 .. y1-1: texels stepped down the texture column (tfill_e
;     / tfill_o / tfill_f), or one colour for solid walls (afill);
;   floor rows y1 .. rf*8-1: copied from this size's floor rows (ffill).
; In: ix = the column's s_cols entry, s_c, s_rc, s_rf, s_whole,
; eng_solid. Clobbers everything but IX (IY is used); leaves slot 2 at
; the texture's bank.
fill_column:
    ld a, (s_c)
    ld b, a
    srl a
    add a, CHUNKY
    ld h, a
    ld a, b
    and 1
    rrca
    ld l, a
    ld (s_colbase), hl          ; = CHUNKY*256 + s_c * 128
    ld a, (s_rc)
    add a, a
    add a, a
    add a, a
    ld c, a                     ; first row
    ; ceiling rows c .. y0 (colour 0: zero in either nibble)
    ld a, (ix+0)
    sub c
    jr z, ++
    jr c, ++
    ; entry = zfill + (96 - n) * 2
    ld b, a
    ld a, l
    add a, c
    ld l, a
    ld a, 96
    sub b
    add a, a
    ld e, a
    ld d, 0
    push hl
    ld hl, zfill
    add hl, de
    ex (sp), hl                 ; (sp) = entry
    ld de, ++
    ex de, hl
    ex (sp), hl                 ; (sp) = return, hl = entry
    push hl
    ex de, hl                   ; hl = dest
    xor a
    ret                         ; -> zfill copies, back to ++
++: ; wall rows y0 .. y1
    ld a, (ix+1)
    sub (ix+0)
    jp z, _fc_floor
    jp c, _fc_floor             ; no wall rows
    ld (s_n), a
    ld a, (ix+6)
    or a
    jp z, _fc_floor             ; bank 0: no wall in this column
    ld ($FFFF), a               ; mapper: the texture column's bank into slot 2
    ld a, (eng_solid)
    or a
    jp nz, _fc_solid
    ; iy = column base + y0 - (96 - n)
    ld a, (s_n)
    sub 96
    add a, (ix+0)               ; y0 + n - 96 (may be negative)
    ld e, a
    rla
    sbc a, a
    ld d, a
    ld hl, (s_colbase)
    add hl, de
    push hl
    pop iy                      ; copy k writes (iy + k): copy 96 - n writes row y0
    ; entry = (variant) + (96 - n) * 8; variant: copies with even k keep the
    ; high nibble when (c + y0 + n) is even
    ; Each copy is "ld c, h / ld a, (bc) / and mask / ld (iy+k), a /
    ; add hl, de" (8 bytes): b:c = the texel's address (b = the column's
    ; page, h = its low byte + v's integer part), hl = v in 8.8, de = dv.
    ; The table keeps v below 64, so the texel stays in its 64-byte column.
    ld a, (s_c)
    add a, (ix+0)
    ld hl, s_n
    add a, (hl)
    ld de, tfill_e
    rra
    jr nc, +
    ld de, tfill_o
+:  ld a, 96
    sub (hl)
    ld l, a
    ld h, 0
    add hl, hl
    ld a, (s_whole)
    or a
    jr nz, +
    add hl, hl
    add hl, hl                  ; 8 bytes a copy
    add hl, de
    jr ++
+:  ld e, l
    ld d, h
    add hl, hl
    add hl, de                  ; low detail: 6 bytes a copy, whole bytes
    ld de, tfill_f
    add hl, de
++: ld (s_jump), hl             ; where to enter
    ; dv hi of this column
    ld a, (s_c)
    ld e, a
    ld d, 0
    ld hl, s_dvhi
    add hl, de
    ld d, (hl)
    ld e, (ix+4)                ; de = dv
    ld b, (ix+7)                ; column page
    ld a, (ix+5)
    add a, (ix+3)
    ld h, a                     ; column low byte + v int
    ld l, (ix+2)                ; v frac
    push ix                     ; (the unrolled code needs no IX, but keeps it safe)
    push hl
    ld hl, _fc_back
    ex (sp), hl                 ; return address for the unrolled code's ret; hl = v
    push hl
    ld hl, (s_jump)
    ex (sp), hl                 ; jump target on the stack, hl = v
    ret                         ; -> the unrolled copies
_fc_back:
    pop ix
    jr _fc_floor
_fc_solid:
    ; solid-colour walls: rows y0 .. y1 all get the texel in the middle of
    ; the texture column (v = 32), drawn like the floor
    ld h, (ix+7)
    ld a, (ix+5)
    add a, 32
    ld l, a
    ld d, (hl)                  ; its pair byte
    ld hl, (s_colbase)
    ld a, l
    add a, (ix+0)
    ld l, a                     ; dest = row y0
    ld a, (s_c)
    add a, (ix+0)
    ld c, a
    ld a, (s_n)
    ld b, a
    ld a, 96
    sub b
    ld e, a                     ; entry copy 96 - n
    ; afill stores a on even copies and c on odd ones; copy 96 - n writes
    ; row y0, which shows the high nibble when s_c + y0 is even. So a gets
    ; the high nibble when (96 - n) and (s_c + y0) have the same parity.
    ld a, (s_whole)
    or a
    jr nz, ++                   ; whole bytes: a = c = the pair byte
    ld a, e
    xor c
    rra                         ; carry = parities differ
    ld a, d
    jr c, +
    and $F0
    ld c, d
    ld d, a
    ld a, c
    and $0F
    ld c, a
    ld a, d
    jr +++
+:  and $0F
    ld c, d
    ld d, a
    ld a, c
    and $F0
    ld c, a
    ld a, d
    jr +++
++: ld a, d
    ld c, d
+++:
    ld d, 0
    sla e                       ; 2 bytes per afill copy
    push hl
    ld hl, _fc_floor
    ex (sp), hl                 ; return to the floor
    push hl
    ld hl, afill
    add hl, de
    ex (sp), hl                 ; (sp) = entry, hl = dest
    ret                         ; -> afill
_fc_floor:
    ; floor rows y1 .. rf*8: copied from this size's floor rows (the floor
    ; fades into the ceiling colour towards the horizon, tools/live_data.py
    ; floor_tables): the even or odd column's nibbles, or whole bytes in
    ; low detail
    ld a, (s_rf)
    add a, a
    add a, a
    add a, a
    sub (ix+1)
    ret z
    ret c                       ; no floor rows to draw
    ld b, a                     ; rows
    ld a, (s_whole)
    or a
    ld a, 192                   ; low detail: the whole-byte table
    jr nz, +
    ld a, (s_c)
    rrca
    sbc a, a
    and 96                      ; odd column: the second table
+:  add a, (ix+1)               ; + y1
    ld e, a
    ld a, 0
    adc a, a
    ld d, a                     ; de = table offset (may pass 255)
    ld hl, (r_floor)
    add hl, de
    push hl                     ; source
    ld hl, (s_colbase)
    ld a, l
    add a, (ix+1)
    ld e, a
    ld d, h                     ; destination: row y1
    ld a, 96                    ; entry = ffill + (96 - n) * 2 (ldi each)
    sub b
    ld l, a
    ld h, 0
    add hl, hl
    ld bc, ffill
    add hl, bc
    ex (sp), hl                 ; (sp) = entry, hl = source
    ret                         ; -> ldi copies; their ret returns to our caller

; billboards that cover this strip: widen s_miny0 / s_maxy1 by their rows
; Out: s_hasbill = 1 if any billboard covers part of the strip.
; Clobbers everything but IY (IX points at the last billboard checked).
strip_bill_rows:
    xor a
    ld (s_hasbill), a
    ld a, (r_nbill)
    or a
    ret z
    ld b, a
    ld c, 0
-:  push bc
    ld a, c
    call bill_ptr               ; ix = billboard c
    call bill_in_strip          ; carry if it covers this strip
    jr nc, +
    ld a, 1
    ld (s_hasbill), a
    ld a, (ix+B_Y0)
    ld hl, s_miny0
    cp (hl)
    jr nc, ++
    ld (hl), a
++: ld a, (ix+B_Y1)
    ld hl, s_maxy1
    cp (hl)
    jr c, +
    ld (hl), a
+:  pop bc
    inc c
    djnz -
    ret

; ix = bill: carry if it covers part of the strip s_x .. s_x+7
; (x0 < s_x + 8 and s_x < x1). Clobbers AF.
bill_in_strip:
    ld a, (s_x)
    add a, 8
    cp (ix+B_X0)                ; s_x + 8 > x0 ?
    jr c, ++
    jr z, ++
    ld a, (s_x)
    cp (ix+B_X1)                ; s_x < x1 ?
    ret                         ; carry = covers
++: or a
    ret

; draws the billboards (far to near) over this strip
; Clobbers everything; leaves slot 2 at a sprite bank.
strip_bills:
    ld a, (r_nbill)
    or a
    ret z
    ld b, a
    ld c, 0
-:  push bc
    ld hl, r_order
    ld e, c
    ld d, 0
    add hl, de
    ld a, (hl)
    call bill_ptr               ; ix = the next one, far to near
    call bill_in_strip          ; carry if it covers this strip
    call c, draw_bill_strip     ; draw its columns in this strip
    pop bc
    inc c
    djnz -
    ret

; ix = bill: its columns in this strip
; Finds the sprite's record (bank and address), its texture step, then
; for each column of the bill inside this strip: steps B_C8 to that
; column, and if the canvas column is 0..63 and the bill is at least as
; near as the column's wall (h8 >= COLH), draws it (draw_bill_column).
; Writes B_CX / B_C8 back, so the next strip continues from there.
; Clobbers everything but IX, IY.
draw_bill_strip:
    ; sprite record: bsprite_dir[spr] = 32-bit offset into the blob
    ld a, (ix+B_SPR)
    add a, a
    add a, a
    add a, a
    ld e, a
    ld d, 0
    ld a, (r_hdr+H_BSPRITE_DIR_BANK)
    ld ($FFFF), a               ; mapper: the billboard directory into slot 2
    ld hl, (r_hdr+H_BSPRITE_DIR_ADDR)
    add hl, de
    ld e, (hl)
    inc hl
    ld d, (hl)
    inc hl
    ld a, (hl)                  ; bits 16..23
    ; bank = BSPR_BANK + (offset >> 14), addr = $8000 + (offset & $3FFF)
    ld l, d
    sla l
    rla
    sla l
    rla
    ld hl, r_hdr+H_BSPR_BANK
    add a, (hl)
    ld (s_bbank), a
    ld a, d
    and $3F
    or $80
    ld d, a
    ld (s_tile), de             ; sprite record address
    ; rows and texture stepping of its height (table bank)
    ld a, (r_tbank)
    ld ($FFFF), a               ; mapper: the height table into slot 2
    ld l, (ix+B_H8)
    ld h, (ix+B_H8+1)
    call table_entry            ; the entry for the bill's h8
    ld de, 4
    add hl, de                  ; the entry's dv
    ld e, (hl)
    inc hl
    ld d, (hl)
    ld (s_dv), de
    ld a, (ix+B_Y0)
    ld (s_by0), a
    ld c, a
    ld a, (ix+B_Y1)
    sub c
    ret z
    ret c                       ; no rows show
    ld (s_bn), a
    ld l, (ix+B_V0)
    ld h, (ix+B_V0+1)
    ld (s_v), hl
    ld a, (s_bbank)
    ld ($FFFF), a               ; mapper: the sprite's bank into slot 2
    ; columns max(x0, s_x) .. min(x1, s_x + 8)
    ld a, (s_x)
    cp (ix+B_X0)
    jr nc, +
    ld a, (ix+B_X0)
+:  ld (s_c), a
    ; strips can come right to left: step back to this strip's first column
    ; (subtracting exactly undoes the steps forward)
    ld b, a
-:  ld a, b
    cp (ix+B_CX)
    jr nc, +                    ; CX <= first: the loop below steps forward
    ld a, (eng_detail)
    inc a
    ld c, a
    ld a, (ix+B_CX)
    sub c
    ld (ix+B_CX), a
    ld l, (ix+B_C8)
    ld h, (ix+B_C8+1)
    ld e, (ix+B_DC8)
    ld d, (ix+B_DC8+1)
    or a
    sbc hl, de
    ld (ix+B_C8), l
    ld (ix+B_C8+1), h
    jr -
+:
_dbs_col:
    ld a, (s_c)
    cp (ix+B_X1)
    ret nc                      ; past the bill's last column
    ld b, a
    ld a, (s_x)
    add a, 8
    cp b
    ret z
    ret c                       ; past the strip
    ; advance c8 to this column
-:  ld a, (ix+B_CX)
    cp b
    jr nc, +
    inc (ix+B_CX)
    ld a, (eng_detail)
    add a, (ix+B_CX)
    ld (ix+B_CX), a
    ld l, (ix+B_C8)
    ld h, (ix+B_C8+1)
    ld e, (ix+B_DC8)
    ld d, (ix+B_DC8+1)
    add hl, de
    ld (ix+B_C8), l
    ld (ix+B_C8+1), h
    jr -
+:  ld a, (ix+B_C8+1)           ; canvas column
    cp 64
    jr nc, _dbs_next
    ld (s_tmp_c), a
    ; depth test: h8 >= colh[x]
    ld l, b
    ld h, COLH_HI
    ld a, (ix+B_H8+1)
    cp (hl)
    jr c, _dbs_next
    jr nz, +
    dec h
    ld a, (ix+B_H8)
    cp (hl)
    jr c, _dbs_next
+:  call draw_bill_column
_dbs_next:
    ld hl, s_c
    inc (hl)
    ld a, (eng_detail)
    add a, (hl)
    ld (hl), a
    jr _dbs_col

; draws canvas column s_tmp_c of the bill at ix into chunky column (s_c - s_x)
; A billboard sprite record: 64 u16 offsets (from the record) of each
; canvas column's 64-byte block (one pair byte per canvas row; 0 = an
; empty column), then the blocks, each 64-byte aligned. Bytes equal to
; H_BILL_KEY are transparent. Rows s_by0 .. s_by0 + s_bn - 1 are drawn,
; the texture row starting at s_v and stepping by s_dv. At high detail
; each pixel keeps only the nibble it shows (alternating down the
; column); at low detail whole bytes. Register use in the loops: b:c =
; the texel's address (c = ixh), ix = v (8.8), de = dv, iyl = rows left,
; iyh = the transparent byte, hl = destination.
; Clobbers everything but IX (saved), IY is changed.
draw_bill_column:
    ; the column's block: header[c] (u16 offset from the record), 0 = empty
    ld a, (s_tmp_c)
    add a, a
    ld e, a
    ld d, 0
    ld hl, (s_tile)
    add hl, de
    ld e, (hl)
    inc hl
    ld d, (hl)
    ld a, d
    or e
    ret z
    ld hl, (s_tile)
    add hl, de                  ; block (64-aligned)
    push ix
    ld b, h                     ; block page
    ld a, (s_v+1)
    add a, l
    ld ixh, a                   ; block low byte + v int
    ld a, (s_v)
    ld ixl, a
    ld de, (s_dv)
    ld a, (s_bn)
    ld iyl, a
    ld a, (r_hdr+H_BILL_KEY)
    ld iyh, a                   ; the transparent byte
    ; dest: chunky column (s_c - s_x), row y0
    ld a, (s_c)
    push af
    ld hl, s_x
    sub (hl)
    ld c, a
    srl a
    add a, CHUNKY
    ld h, a
    ld a, c
    and 1
    rrca
    ld l, a
    ld a, (s_by0)
    add a, l
    ld l, a
    ; first row keeps the high nibble when (x + y0) is even
    pop af
    ld c, a
    ld a, (eng_detail)
    or a
    jr nz, _dbc_full
    ld a, (s_by0)
    add a, c
    rra
    jr c, _dbc_lo
_dbc_hi:
    ld a, ixh
    ld c, a
    ld a, (bc)
    cp iyh
    jr z, +
    and $F0
    ld (hl), a
+:  inc l
    add ix, de
    dec iyl
    jr z, _dbc_done
_dbc_lo:
    ld a, ixh
    ld c, a
    ld a, (bc)
    cp iyh
    jr z, +
    and $0F
    ld (hl), a
+:  inc l
    add ix, de
    dec iyl
    jr nz, _dbc_hi
_dbc_done:
    pop ix
    ret
_dbc_full:                      ; low detail: the whole pair byte
    ld a, ixh
    ld c, a
    ld a, (bc)
    cp iyh
    jr z, +
    ld (hl), a
+:  inc l
    add ix, de
    dec iyl
    jr nz, _dbc_full
    pop ix
    ret

; converts pixel rows rc*8 .. rf*8 of this strip into planar tiles,
; straight into VRAM (tiles of a strip are consecutive)
;
; A tile row is 4 bytes, one per bit plane (bit 7 = leftmost pixel). For
; each pixel row, the 8 chunky bytes are first combined into 4 pair
; bytes b, c, d, e (columns 0+1, 2+3, 4+5, 6+7: the even column's byte
; ORed with the odd one's, as each keeps only the nibble it shows). Then
; plane p = T[p][0][b] | T[p][1][c] | T[p][2][d] | T[p][3][e], where the
; chunky-to-planar tables (bank C2P_BANK, slot 2) have one 256-byte page
; per (row parity, plane, pair): $A0 + plane*4 + pair for even rows,
; $B0 + ... for odd rows (whose pair bytes hold the two pixels the other
; way round). Each page puts the plane's bits of a pair at its two bit
; positions. The rows are written as one run from iy, with interrupts
; off for each row only (4 writes, well over 26 cycles apart); if an
; interrupt moved the VDP address (var_vdp_touched), c2p_set_addr sets
; it again. Two rows per .rept copy (even, then odd), 4 copies = 1 tile.
; In: s_s, s_rc, s_rf, r_rows, r_base, eng_detail. Clobbers everything
; (IX, IY included); leaves slot 2 at C2P_BANK.
strip_tiles:
    ld a, (r_dbg)
    bit 1, a
    ret nz                      ; (test switch: no conversion)
    ; iy = VRAM address of tile (base + s*rows + rc)
    ld a, (r_rows)
    ld e, a
    ld a, (s_s)
    ld hl, 0
    ld d, 0
    ld b, 8
-:  add hl, hl
    rla
    jr nc, +
    add hl, de
+:  djnz -                      ; hl = s * rows
    ld a, (s_rc)
    ld e, a
    add hl, de
    ld de, (r_base)
    add hl, de
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl                  ; * 32 bytes per tile
    push hl
    pop iy
    ld a, C2P_BANK
    ld ($FFFF), a               ; mapper: the conversion tables into slot 2
    ld a, (s_rc)
    ld b, a
    add a, a
    add a, a
    add a, a
    ld ixl, a                   ; pixel row
    ld a, (s_rf)
    sub b
    ld (s_n), a                 ; tiles
    ld a, 1
    ld (var_vdp_touched), a     ; first row: set the address
    ld a, (eng_detail)
    or a
    jp nz, _stl_tile
_st_tile:
.rept 4
    ; even pixel row: gather the 4 pair bytes
    ld h, CHUNKY
    ld a, ixl
    ld l, a
    ld a, (hl)
    set 7, l                    ; the odd column of the pair
    or (hl)
    ld b, a
    inc h
    ld a, (hl)
    res 7, l
    or (hl)
    ld c, a
    inc h
    ld a, (hl)
    set 7, l
    or (hl)
    ld d, a
    inc h
    ld a, (hl)
    res 7, l
    or (hl)
    ld e, a
    ld h, $A0                   ; the even-row tables
    di
    ld a, (var_vdp_touched)
    or a
    call nz, c2p_set_addr       ; an interrupt moved the VDP address: set it again
    ; plane 0 (pages $A0-$A3), then planes 1, 2, 3
    ld l, b
    ld a, (hl)
    inc h
    ld l, c
    or (hl)
    inc h
    ld l, d
    or (hl)
    inc h
    ld l, e
    or (hl)
    inc h
    out (VDPData), a
    ld l, b
    ld a, (hl)
    inc h
    ld l, c
    or (hl)
    inc h
    ld l, d
    or (hl)
    inc h
    ld l, e
    or (hl)
    inc h
    out (VDPData), a
    ld l, b
    ld a, (hl)
    inc h
    ld l, c
    or (hl)
    inc h
    ld l, d
    or (hl)
    inc h
    ld l, e
    or (hl)
    inc h
    out (VDPData), a
    ld l, b
    ld a, (hl)
    inc h
    ld l, c
    or (hl)
    inc h
    ld l, d
    or (hl)
    inc h
    ld l, e
    or (hl)
    inc h
    out (VDPData), a
    ei
    inc ixl
    ; odd pixel row: the same with the odd-row tables
    ld h, CHUNKY
    ld a, ixl
    ld l, a
    ld a, (hl)
    set 7, l
    or (hl)
    ld b, a
    inc h
    ld a, (hl)
    res 7, l
    or (hl)
    ld c, a
    inc h
    ld a, (hl)
    set 7, l
    or (hl)
    ld d, a
    inc h
    ld a, (hl)
    res 7, l
    or (hl)
    ld e, a
    ld h, $B0                   ; the odd-row tables
    di
    ld a, (var_vdp_touched)
    or a
    call nz, c2p_set_addr
    ld l, b
    ld a, (hl)
    inc h
    ld l, c
    or (hl)
    inc h
    ld l, d
    or (hl)
    inc h
    ld l, e
    or (hl)
    inc h
    out (VDPData), a
    ld l, b
    ld a, (hl)
    inc h
    ld l, c
    or (hl)
    inc h
    ld l, d
    or (hl)
    inc h
    ld l, e
    or (hl)
    inc h
    out (VDPData), a
    ld l, b
    ld a, (hl)
    inc h
    ld l, c
    or (hl)
    inc h
    ld l, d
    or (hl)
    inc h
    ld l, e
    or (hl)
    inc h
    out (VDPData), a
    ld l, b
    ld a, (hl)
    inc h
    ld l, c
    or (hl)
    inc h
    ld l, d
    or (hl)
    inc h
    ld l, e
    or (hl)
    inc h
    out (VDPData), a
    ei
    inc ixl
.endr
    ld hl, s_n
    dec (hl)
    jp nz, _st_tile             ; next tile down
    ret

; low detail: each column pair holds one whole pair byte in its even
; column, so the gather needs no OR with the odd column
_stl_tile:
.rept 4
    ld h, CHUNKY
    ld a, ixl
    ld l, a
    ld b, (hl)
    inc h
    ld c, (hl)
    inc h
    ld d, (hl)
    inc h
    ld e, (hl)
    ld h, $A0
    di
    ld a, (var_vdp_touched)
    or a
    call nz, c2p_set_addr
    ld l, b
    ld a, (hl)
    inc h
    ld l, c
    or (hl)
    inc h
    ld l, d
    or (hl)
    inc h
    ld l, e
    or (hl)
    inc h
    out (VDPData), a
    ld l, b
    ld a, (hl)
    inc h
    ld l, c
    or (hl)
    inc h
    ld l, d
    or (hl)
    inc h
    ld l, e
    or (hl)
    inc h
    out (VDPData), a
    ld l, b
    ld a, (hl)
    inc h
    ld l, c
    or (hl)
    inc h
    ld l, d
    or (hl)
    inc h
    ld l, e
    or (hl)
    inc h
    out (VDPData), a
    ld l, b
    ld a, (hl)
    inc h
    ld l, c
    or (hl)
    inc h
    ld l, d
    or (hl)
    inc h
    ld l, e
    or (hl)
    inc h
    out (VDPData), a
    ei
    inc ixl
    ld h, CHUNKY
    ld a, ixl
    ld l, a
    ld b, (hl)
    inc h
    ld c, (hl)
    inc h
    ld d, (hl)
    inc h
    ld e, (hl)
    ld h, $B0
    di
    ld a, (var_vdp_touched)
    or a
    call nz, c2p_set_addr
    ld l, b
    ld a, (hl)
    inc h
    ld l, c
    or (hl)
    inc h
    ld l, d
    or (hl)
    inc h
    ld l, e
    or (hl)
    inc h
    out (VDPData), a
    ld l, b
    ld a, (hl)
    inc h
    ld l, c
    or (hl)
    inc h
    ld l, d
    or (hl)
    inc h
    ld l, e
    or (hl)
    inc h
    out (VDPData), a
    ld l, b
    ld a, (hl)
    inc h
    ld l, c
    or (hl)
    inc h
    ld l, d
    or (hl)
    inc h
    ld l, e
    or (hl)
    inc h
    out (VDPData), a
    ld l, b
    ld a, (hl)
    inc h
    ld l, c
    or (hl)
    inc h
    ld l, d
    or (hl)
    inc h
    ld l, e
    or (hl)
    inc h
    out (VDPData), a
    ei
    inc ixl
.endr
    ld hl, s_n
    dec (hl)
    jp nz, _stl_tile
    ret

; (interrupts off) points the VDP at pixel row ixl of this strip:
; iy + (ixl - rc*8) * 4
; Clears var_vdp_touched. Keeps BC, DE, HL; clobbers AF.
c2p_set_addr:
    xor a
    ld (var_vdp_touched), a
    push hl
    ld a, (s_rc)
    add a, a
    add a, a
    add a, a
    ld l, a
    ld a, ixl
    sub l
    ld l, a
    ld h, 0
    add hl, hl
    add hl, hl
    push de
    push iy
    pop de
    add hl, de
    pop de
    ld a, l
    out (VDPAddress), a
    ld a, h
    or $40                      ; VRAM write
    out (VDPAddress), a
    pop hl
    ret

; a * e -> hl
; (8 x 8 -> 16 bits, unsigned). Clobbers AF, B, D.
mul_a_e:
    ld hl, 0
    ld d, 0
    ld b, 8
-:  add hl, hl
    rla
    jr nc, +
    add hl, de
+:  djnz -
    ret

; Generated by tools/live_data.py (unrolled_fill): the unrolled fills
; tfill_e, tfill_o, tfill_f (texture), zfill (zeros), ffill (ldi copies),
; afill (two alternating bytes), each 96 copies then ret, entered at copy
; 96 - n to do n rows; and the floor tables floor_rows_large/small (3 x
; 96 pair bytes: as kept by an even column, an odd column, and whole) and
; floor_tiles_large/small (per tile row, the all-floor tile as an offset
; from r_solid: 0 = the ceiling tile, 1 = the full floor tile, 14.. the
; in-between ones, as the floor fades towards the horizon).
.include "build/gfx/live/unroll.inc"

; remembers this strip's solid rows, and (OUTWARD) writes its column of
; the name table now
; r_masks[s_s] = (s_rc, s_rf). Clobbers everything but IX, IY.
strip_finish:
    ld a, (s_s)
    add a, a
    ld e, a
    ld d, 0
    ld hl, r_masks
    add hl, de
    ld a, (s_rc)
    ld (hl), a
    inc hl
    ld a, (s_rf)
    ld (hl), a
    ld a, (eng_vmode)
    or a
    ret nz                      ; AT ONCE: all names are written at the end
    ld a, (r_dbg)
    bit 2, a
    ret nz                      ; (test switch)
    ld a, (s_s)
    jp write_strip_names        ; (tail call)

; a = strip: writes its column of name table entries
; Rows above s_rc show the solid ceiling tile, rows from s_rf on the
; row's all-floor tile, the rows between the strip's own tiles. One entry
; per DI window, with the display on. Clobbers AF, BC, DE, HL.
write_strip_names:
    ld (s_tmp_c), a
    add a, a
    ld e, a
    ld d, 0
    ld hl, r_masks
    add hl, de
    ld a, (hl)
    ld (s_rc), a
    inc hl
    ld a, (hl)
    ld (s_rf), a
    ; first tile of the strip
    ld a, (r_rows)
    ld e, a
    ld a, (s_tmp_c)
    call mul_a_e                ; strip * rows
    ld de, (r_base)
    add hl, de
    ld (s_tile), hl
    ; name table address of (row 0, strip)
    ld a, (s_tmp_c)
    add a, a
    ld e, a
    ld d, 0
    ld hl, (r_ntorg)
    add hl, de
    ld c, 0                     ; row
-:  push hl
    ; entry
    ld a, c
    ld de, (s_tile)
    ld hl, s_rc
    cp (hl)
    jr nc, +
    ld de, (r_solid)            ; ceiling
    jr ++
+:  ld hl, s_rf
    cp (hl)
    jr c, +
    call floor_tile             ; floor
    jr ++
+:  ld l, c
    ld h, 0
    add hl, de
    ex de, hl                   ; the strip's own tile for this row
++: pop hl
    push hl
    di
    call vdp_set_vram_write_hl  ; this row's entry in the name table
    ld a, e
    out (VDPData), a            ; tile low byte
    ld a, d
    and 1                       ; tile bit 8; palette 0 (the view's)
    nop
    nop
    out (VDPData), a
    ei
    pop hl
    ld de, 64                   ; one name-table row down
    add hl, de
    inc c
    ld a, (r_rows)
    cp c
    jr nz, -
    ret

; c = tile row -> de = its all-floor tile (the floor fades by rows)
; Clobbers HL.
floor_tile:
    ld hl, (r_ftile)
    ld e, c
    ld d, 0
    add hl, de
    ld e, (hl)
    ld hl, (r_solid)
    add hl, de
    ex de, hl
    ret

; writes the whole view's name table from r_masks, row by row (call it
; with interrupts off, below the view: ~40 cycles between writes)
; Leaves interrupts off. Clobbers everything but IY (IX walks r_masks).
write_all_names:
    ld hl, (r_ntorg)
    ld c, 0                     ; row
_wan_row:
    push hl
    call floor_tile             ; (kept out of the timed loop below)
    ld (s_ft), de
    pop hl
    push hl
    call vdp_set_vram_write_hl  ; start of this row in the name table
    ; tile of (row, strip 0) = base + row; + rows per strip
    ld l, c
    ld h, 0
    ld de, (r_base)
    add hl, de
    ld (s_tile), hl
    ld ix, r_masks
    ld a, (r_strips)
    ld b, a
_wan_col:
    ld a, c
    cp (ix+0)
    jr nc, +
    ld de, (r_solid)            ; ceiling
    jr ++
+:  cp (ix+1)
    jr c, +
    ld de, (s_ft)               ; floor (this row's tile)
    jr ++
+:  ld de, (s_tile)
++: ld a, e
    out (VDPData), a
    ld a, d
    and 1
    out (VDPData), a
    ld hl, (s_tile)
    ld a, (r_rows)
    add a, l
    ld l, a
    jr nc, +
    inc h
+:  ld (s_tile), hl
    inc ix
    inc ix
    djnz _wan_col
    pop hl
    ld de, 64
    add hl, de
    inc c
    ld a, (r_rows)
    cp c
    jr nz, _wan_row
    ret

; writes the black view (view area only), row by row, short DI windows
; (every entry = BLACK_TILE with the second palette, whose colour 0 is
; black). Clobbers AF, BC, DE, HL; interrupts enabled on return.
blank_view:
    ld hl, (r_ntorg)
    ld a, (r_rows)
    ld c, a
--: push hl
    di
    call vdp_set_vram_write_hl  ; start of this row of the view
    ld a, (r_strips)
    ld b, a
-:  ld a, BLACK_TILE
    out (VDPData), a
    ld a, 8
    nop
    nop
    out (VDPData), a
    djnz -
    ei
    pop hl
    ld de, 64
    add hl, de
    dec c
    jr nz, --
    ret

; screen layout of this size: border tiles + rows 2..17 of the name table
; (the view's own area left alone)
; The border's tiles start with the solid ceiling and floor tiles (r_solid
; + 0, + 1), then the rock and frame, then the in-between floor tiles.
; Game Gear: no border; see below. Clobbers everything, IX included;
; leaves slot 2 at a data bank.
r_layout:
    ; tiles: (r_solid) .. from the border tiles of this size
    ld a, (r_hdr+H_BORDER_BANK)
    ld ($FFFF), a               ; mapper: the border's bank into slot 2
    ld hl, (r_hdr+H_BTILES_LARGE_ADDR)
    ld a, (r_hdr+H_N_BTILES_LARGE)
    ld c, a
    ld a, (eng_vsize)
    or a
    jr z, +
    ld hl, (r_hdr+H_BTILES_SMALL_ADDR)
    ld a, (r_hdr+H_N_BTILES_SMALL)
    ld c, a
+:  ld b, 0
    sla c
    rl b                        ; 16-byte units = tiles * 2
    push hl
    ld hl, (r_solid)
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    ex de, hl                   ; de = VRAM address of tile r_solid
    pop hl
    call vram_upload_paced      ; the border's tiles, display on
.IFDEF GAMEGEAR
    ; Game Gear: no border. Rows 2..15 outside the view are black (the
    ; small view sits in the large one's place), rows 16..20 the status
    ; bar again (a box may have covered them): its pictures, and all of
    ; its text redrawn
    ld de, NAMETABLE + 2*64
    ld c, 2
_ggl_row:
    ld b, 0
_ggl_col:
    ld a, c
    ld ix, r_vrow
    sub (ix+0)
    jr c, _ggl_put
    ld ix, r_rows
    cp (ix+0)
    jr nc, _ggl_put
    ld a, b
    ld ix, r_vcol
    sub (ix+0)
    jr c, _ggl_put
    ld ix, r_strips
    cp (ix+0)
    jr c, _ggl_next
_ggl_put:
    ; outside the view: a black entry (one per DI window)
    di
    ex de, hl
    push hl
    call vdp_set_vram_write_hl  ; this black entry's address
    pop hl
    ex de, hl
    ld a, BLACK_TILE
    out (VDPData), a
    ld a, 8
    nop
    nop
    out (VDPData), a
    ei
_ggl_next:
    inc de
    inc de
    inc b
    ld a, b
    cp 32
    jr nz, _ggl_col
    inc c
    ld a, c
    cp GG_HUD_ROW
    jr nz, _ggl_row
    ld a, BANK_MISC
    ld ($FFFF), a               ; mapper: bank 4 (status bar layout) into slot 2
    ld hl, hud_static_data
    ld de, NAMETABLE + GG_HUD_ROW*64
    ld bc, 320 / 16
    call vram_upload_paced      ; the status bar's fixed rows
    ld a, $3F
    ld (eng_hud_dirty), a       ; and all its text rewritten by the interrupts
    ret
.ENDIF
    ; names: 16 rows x 32 entries (u16), view area skipped
    ld a, (r_hdr+H_BORDER_BANK)
    ld ($FFFF), a               ; mapper: the border's bank into slot 2
    ld hl, (r_hdr+H_BMAP_LARGE_ADDR)
    ld a, (eng_vsize)
    or a
    jr z, +
    ld hl, (r_hdr+H_BMAP_SMALL_ADDR)
+:  ld de, NAMETABLE + 2*64
    ld c, 2                     ; screen row
_lay_row:
    push bc
    ld b, 0                     ; column
_lay_col:
    ; inside the view?
    ld a, c
    ld ix, r_vrow
    sub (ix+0)
    jr c, _lay_put
    ld ix, r_rows
    cp (ix+0)
    jr nc, _lay_put
    ld a, b
    ld ix, r_vcol
    sub (ix+0)
    jr c, _lay_put
    ld ix, r_strips
    cp (ix+0)
    jr nc, _lay_put
    inc hl
    inc hl                      ; inside the view: skip this entry
    jr _lay_next
_lay_put:
    ; one border entry per DI window (the display is on)
    di
    ex de, hl
    push hl
    call vdp_set_vram_write_hl  ; this border entry's address
    pop hl
    ex de, hl
    ld a, (hl)
    out (VDPData), a
    inc hl
    ld a, (hl)
    nop
    nop
    out (VDPData), a
    inc hl
    ei
_lay_next:
    inc de
    inc de
    inc b
    ld a, b
    cp 32
    jr nz, _lay_col
    pop bc
    inc c
    ld a, c
    cp 18
    jr nz, _lay_row
    ret

render_bank_end:                ; (end of the renderer's code; it must fit in 16 KB)
