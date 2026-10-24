; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

;=======================================================================
; Doom RPG -> Sega Master System: display engine
;
; This file owns the hardware: reset, interrupts, the VDP. The game itself
; (rules, monsters, events, menus) is assembler in game/, included below at
; $1000 (bank 0) and $4000 (bank 1); it calls the services (svc_*, also in
; the jump table at $0100) and shares the RAM block at $C000 (game/game.asm).
;
; Pictures: drawn live by rom/render.asm (its own bank, paged into slot 1
; while it runs), at one of two sizes (large 160x96 or small 128x80) inside
; a Doom-style border. Monsters/items straight ahead are hardware sprites
; drawn over the view.
;
; Colour: bank 0 = view palette, bank 1 = sprite palette while the view is
; drawn; line interrupts swap both to the 32-colour status-bar palette for
; the status bar and back again. Rows 1 and 18 are black so the swaps
; never show.
;
; Licence: GPL-2.0-or-later.
;
; ---- How the pieces fit ------------------------------------------------
; Start-up: the reset vector jumps to init, which sets the mapper, clears
; RAM, silences the sound chip, programs the VDP and jumps to game_main
; (game/ui.asm, at C_ENTRY = $1000). From then on the game logic is in
; charge; it calls the engine's services:
;   svc_wait_frame      wait for the next frame interrupt
;   svc_read_pad        read the joypad (and the Game Gear's Start)
;   svc_render          draw the view eng_view_x/z/f (rom/render.asm)
;   svc_refresh_sprite  show the hardware sprite eng_ent_def/eng_ent_dist
;   svc_dialog_show     draw dlg_text as a box over the view
;   svc_title           the title screen, until button 1
;   svc_game_screen     set up the play screen (status bar, palettes)
; The game code calls them by name; the same entry points are also a jump
; table at $0100 for anything that needs fixed addresses.
;
; Interrupts: the frame interrupt (vblank) counts frames, runs the damage
; flash timer and the sound effects, and writes one dirty status-bar row.
; Line interrupts (VDP register 10) swap the palettes between the view and
; the status bar part-way down the screen (see irq_handler).
;
; ---- Memory map and banking --------------------------------------------
; The Sega mapper has three 16 KB ROM slots. Writing a bank number to:
;   $FFFD pages it into slot 0 ($0000-$3FFF; always bank 0 here),
;   $FFFE pages it into slot 1 ($4000-$7FFF; bank 1, or RENDER_BANK while
;         the renderer runs),
;   $FFFF pages it into slot 2 ($8000-$BFFF; HOME_BANK = the level blob
;         while the game logic runs, or a data bank while the engine reads
;         it).
; $FFFC is the mapper's control register (0 = ROM, no cartridge RAM).
; These registers are in the RAM mirror ($E000-$FFFF mirrors $C000-$DFFF).
; ROM: bank 0 $0000-$0FFF this engine, $1000-$3FFF game logic; bank 1
; $4000-$7FBF game logic, $7FC0 ROM directory, $7FF0 Sega header; bank 2
; the level blob at $A000; bank 3 the level header; bank 4 strings, status
; bar, fonts, sprite directories; bank 5 title, dialog font, chunky-to-
; planar tables; bank 6 the renderer; banks 7-8 height tables; then the
; level's big tables and the hardware sprites (docs/rom_format.md).
; RAM ($C000-$DFFF): $C000 engine variables shared with the game, $C010
; engine-only variables, $C0C0 sound queue and player, $C100 status text,
; $C200 dialog text, $C400.. game state and game variables
; (game/game.asm), $D400-$DDFF the renderer's buffers and variables
; (rom/render.asm), the stack grows down from $DFF0.
;
; ---- VRAM layout (16 KB) ------------------------------------------------
;   tiles 0..         hardware sprite patterns (the sprite in view)
;   tiles 56..127     status bar tiles (56 = solid black)
;   tiles 128..       the view's tiles (and the border); the dialog font
;                     and the title screen also go here when shown
;   $3800             name table (32 x 24 entries of 2 bytes = 1536 bytes,
;                     so the view's tiles must stay below tile 448)
;   tiles 496..       ($3E00-$3EFF, the gap after the name table) the rest
;                     of the status bar tiles
;   $3F00             sprite attribute table (SAT): 64 Y bytes, then at
;                     $3F80 64 (X, tile) pairs
; A name-table entry is 2 bytes: tile number low 8 bits, then a byte whose
; bit 0 is tile bit 8 and bit 3 selects the second palette (CRAM 16-31,
; the "sprite palette"). Tile n's pattern is at VRAM n * 32.
;
; Jargon: "view" = the 3D picture of the corridor; "status bar" or "HUD"
; = the text rows at the bottom (and the message row at the top on the
; SMS); "level header" = the 64-byte table in bank 3 that says where the
; level's renderer data is (rom/live_layout.inc); "level blob" = the
; rules' copy of the level in bank 2.
;=======================================================================

; WLA-DX memory map: three 16 KB ROM slots (the mapper's) and the 8 KB of
; RAM at $C000. The ROM bank map declares 256 banks; wlalink writes all of
; them and tools/rom_size.py trims the file to the banks actually used.
.MEMORYMAP
DEFAULTSLOT 0
SLOTSIZE $4000
SLOT 0 $0000
SLOT 1 $4000
SLOT 2 $8000
SLOTSIZE $2000
SLOT 3 $C000
.ENDME

.ROMBANKMAP
BANKSTOTAL 256
BANKSIZE $4000
BANKS 256
.ENDRO

; I/O ports
.define VDPData    $BE          ; VDP data port: VRAM/CRAM bytes, auto-incrementing address
.define VDPAddress $BF          ; VDP control port (write): address / register commands
.define VDPStatus  $BF          ; VDP status (read): bit 7 = frame interrupt pending; reading acknowledges interrupts
.define IOPort1    $DC          ; joypad port A: bits 0-5 = up, down, left, right, button 1, button 2 (0 = pressed)
.define PSGPort    $7F          ; SN76489 sound chip (write only)

.define HOME_BANK        2         ; the level blob: always in slot 2 while the game logic runs
.define BANK_MISC        4         ; strings, fonts, status bar, sprite directory
.define BANK_TITLE       5         ; title screen, dialog font (and the chunky-to-planar tables)
.define C_ENTRY          $1000     ; the game logic's entry (game_code_0: jp game_main); name kept from when it was C

.define NAMETABLE        $3800     ; VRAM address of the name table (screen map)
.define SAT              $3F00     ; VRAM address of the sprite attribute table
.define BLACK_TILE       56        ; status-bar tile slot 0 = solid black
; Line interrupts (SMS): the VDP's line counter is loaded from register 10
; and counts down once per line; when it underflows, it raises a line
; interrupt and reloads from R10. A new R10 value is only picked up at the
; next reload (and on every line of the vertical blank).
.define FRAMES_LINE_SWAP_VIEW 7    ; R10: interrupt at the end of line 7
.define FRAMES_LINE_SWAP_HUD  127  ; set during the line-7 int: lands the HUD swap on line 143
.define R1_DISPLAY_ON   $E2        ; display on, frame int, 8x16 sprites
.IFDEF GAMEGEAR
; Game Gear (assembled with -DGAMEGEAR): the 160x144 screen is the name
; table's columns 6..25, rows 3..20. The view fills its width at rows 3..14,
; row 15 is black, row 16 holds the messages, rows 17..20 the status bar.
; The view's palettes go in at every frame interrupt (vblank); one line
; interrupt at the end of line 119 swaps in the status bar's. Colours are
; kept as Master System bytes (--BBGGRR) and widened to the Game Gear's
; 12-bit CRAM words as they are written (gg_col).
.define GG_LINE_HUD      119       ; R10: interrupt at the end of line 119 (the view's last)
.define GG_HUD_ROW       16        ; screen row of the message line
.define GG_VIEW_SHIFT    8         ; sprites: the view is a row higher than on the SMS
.ENDIF
.define R1_DISPLAY_OFF  $A2       ; display off, frame int, 8x16 sprites (VRAM may be written at full speed)

.include "rom/live_layout.inc"          ; level header offsets, RENDER_BANK and the other fixed banks
.include "build/gfx/live/state.inc"     ; generated: game-state addresses the renderer reads (L_FLAGS)

;=======================================================================
; RAM
;=======================================================================
; init clears $C000-$DDFF to 0 at reset, so every variable starts at 0.
.ENUM $C000
; ---- shared with the game code (game/game.asm) ----
; game/game.asm defines the same addresses as ENG_*; keep the two in step.
eng_frame       db      ; $C000 incremented by the frame interrupt (wraps at 256)
eng_ent_def     db      ; $C001 sprite to show (entity def id), $FF = none
eng_ent_dist    db      ; $C002 1..4 cells ahead
eng_hud_dirty   db      ; $C003 bit0 = row 0, bits 1-5 = rows 19-23
eng_flash       db      ; $C004 frames of red damage flash left
eng_solid       db      ; $C005 (game) 1 = solid-colour walls
eng_turn_seq    db      ; $C006 (game) bumped after every handled input
eng_idle        db      ; $C007 (game) 1 while waiting for input
eng_mode        db      ; $C008 (game) ui state
eng_view_x      db      ; $C009 (game) the view to draw: cell x, z, facing
eng_view_z      db      ; $C00A
eng_view_f      db      ; $C00B
eng_vsize       db      ; $C00C (game) 0 = large view, 1 = small
eng_vmode       db      ; $C00D (game) drawing the view: 0 = outward from the centre, in place; 1 = at once (large: black meanwhile, small: drawn out of sight)
eng_pause       db      ; $C00E set by the pause button (NMI), cleared by C
eng_detail      db      ; $C00F 1 = low detail (two-pixel columns), set by C
.ENDE
; ---- the engine's own variables (the game code does not use these) ----
.ENUM $C010
var_line_state  db      ; 0 = next line int loads view palettes, 1 = ignored (line 15),
                        ; 2 = loads HUD palette (line 143), 3 = title (ignore all)
                        ; (Game Gear: 0 = next line int loads the HUD palette,
                        ; 1 = already done this frame, 3 = ignore all)
var_cur_view    dw      ; (not used by the current code)
var_tile_set    db      ; set (0/1) holding the displayed view
var_next_set    db      ; (not used by the current code)
var_dlg_font    db      ; 1 = dialog font is in tiles 128.. (over the view)
var_need_layout db      ; 1 = border + view area need rewriting
var_vdp_touched db      ; set by every interrupt (it moves the VDP address)
var_dlg_base    dw      ; its first tile (the dialog font's first tile: 128)
var_spr_shown   db      ; 1 = a hardware sprite is prepared and should be shown
spr_cols        db      ; the prepared sprite: width in 8-pixel columns
spr_rows        db      ;   height in 16-pixel rows (8x16 hardware sprites)
spr_x0          db      ;   screen x of its left edge
spr_y0          db      ;   screen y of its top edge (SMS coordinates)
tmp_a           db      ; dialog_show: chunks left in the current row
tmp_b           db      ; (not used by the current code)
tmp_w           dw      ; (not used by the current code)
tmp_a2          db      ; (not used by the current code)
tmp_base        dw      ; (not used by the current code)
tmp_count       db      ; dialog_show: rows left
view_pal        dsb 16  ; bank-0 CRAM bytes for the current view
ent_pal         dsb 16  ; bank-1 CRAM bytes (sprite, or text while a box is open)
new_pal         dsb 16  ; (not used by the current code)
ent_new_pal     dsb 16  ; the next sprite's palette, staged by sprite_prepare until it is shown
var_views       db      ; counts finished pictures
gg_start_prev   db      ; (Game Gear) Start held last time
.ENDE
; Palette bytes above are Master System CRAM bytes, --BBGGRR (2 bits per
; component); the Game Gear build widens them as it writes them.
.ENUM $C0C0
; ---- sound (the game code adds ids to the queue) ----
; The queue is a ring of 8: the game writes an id at sfx_q + sfx_qt and
; advances sfx_qt (mod 8); sfx_update takes ids from sfx_qh. Empty when
; sfx_qh = sfx_qt.
sfx_q           dsb 8   ; $C0C0 ring of effect ids (tools/build_sfx.py)
sfx_qh          db      ; $C0C8 next to play (engine)
sfx_qt          db      ; $C0C9 next free (game)
sfx_ptr         dw      ; next segment of the playing effect, 0 = idle
sfx_left        db      ; frames left in the segment
sfx_per         dw      ; tone period, 12.4 fixed point (the PSG's 10-bit period = sfx_per >> 4)
sfx_dlt         dw      ; added to sfx_per every frame (a pitch sweep; negative = rising)
sfx_tatt        db      ; tone attenuation 0 (loudest) .. 15 (silent)
sfx_natt        db      ; noise attenuation 0 .. 15
sfx_noise       db      ; $FF = no noise
                        ; else the noise control value (bits 0-1 rate, 3 = clocked by channel 2; bit 2 white noise)
sfx_tdr         db      ; decay reload / counters
sfx_tdc         db      ;   tone: every sfx_tdr frames (0 = never) sfx_tatt goes up by 1
sfx_ndr         db      ;   noise: the same for sfx_natt
sfx_ndc         db      ;   (sfx_tdc / sfx_ndc count the frames down)
.ENDE
.ENUM $C100
hud_text        dsb 192 ; 6 rows x 32 ASCII: row 0, rows 19..23 (cols 7..31 shown)
                        ; (Game Gear: row 0 is the message line, rows 1..4 the
                        ; status bar beside the face; see hud_flush_row)
.ENDE
.ENUM $C200
dlg_text        dsb 384 ; 16 x 24 ASCII, drawn over the view by svc_dialog_show
                        ; (Game Gear: 16 x 20, packed 20 bytes per row)
.ENDE

;=======================================================================
.BANK 0 SLOT 0
; Reset vector: the Z80 starts here at power-on and on reset.
.ORG $0000
    di                       ; no interrupts until the VDP is set up
    im 1                     ; interrupt mode 1: every maskable interrupt calls $0038
    jp init                     ; start-up

; Maskable interrupt (IM 1): the VDP's frame and line interrupts.
.ORG $0038
    jp irq_handler              ; frame and line interrupts

; Non-maskable interrupt: the Master System's pause button. It only sets
; eng_pause; the game code polls and clears it. Keeps every register.
.ORG $0066
    push af
    ld a, 1
    ld (eng_pause), a        ; tell the game the pause button was pressed
    pop af
    retn

;-----------------------------------------------------------------------
; Services for the game logic (also as a jump table at $0100).
; Every service keeps IX/IY and pages the home bank back into slot 2.
; Apart from IX and IY, each may change any register (AF, BC, DE, HL)
; unless its own comment says what it returns.
;-----------------------------------------------------------------------
.ORG $0100
    jp svc_wait_frame       ; $0100
    jp svc_read_pad         ; $0103 -> hl = pad bits
    jp svc_render           ; $0106 draws eng_view_x/z/f
    jp svc_refresh_sprite   ; $0109
    jp svc_dialog_show      ; $010C
    jp svc_title            ; $010F
    jp svc_game_screen      ; $0112

; svc_wait_frame: waits until the next frame interrupt has run.
; In: interrupts enabled. Out: nothing. Clobbers AF, B; slot 2 = HOME_BANK.
svc_wait_frame:
    push ix
    push iy
    call wait_frame          ; halt until eng_frame changes
    jr svc_exit

; svc_read_pad: reads joypad 1.
; Out: HL = pad bits, 1 = pressed: bit 0 up, 1 down, 2 left, 3 right,
;      4 button 1, 5 button 2 (PAD_* in game/game.asm); H = 0.
; Game Gear: also watches Start (port $00, bit 7, 0 = pressed) and sets
; eng_pause when it goes from released to pressed, so Start acts as the
; Master System's pause button. Clobbers AF, B (Game Gear).
; Does not touch the mapper, IX or IY, so it needs no svc_exit.
svc_read_pad:
.IFDEF GAMEGEAR
    in a, ($00)              ; Start (bit 7, 0 = pressed): the pause button
    cpl
    and $80                  ; a = $80 if Start is held now, else 0 (Z set)
    ld hl, gg_start_prev
    ld b, (hl)               ; b = last time's value
    ld (hl), a               ; remember this time's
    jr z, +                  ; not held: nothing to do
    xor b
    jr z, +                  ; held before too: not a new press
    ld a, 1
    ld (eng_pause), a        ; a new press of Start: pause
+:
.ENDIF
    in a, (IOPort1)          ; joypad port A (active low)
    cpl                      ; 1 = pressed
    and $3F                  ; keep the 6 bits of pad 1 (bits 6-7 belong to pad 2)
    ld l, a
    ld h, 0
    ret

; svc_render: draws the view at cell eng_view_x, eng_view_z facing
; eng_view_f, at the size and in the mode set by eng_vsize, eng_vmode,
; eng_detail and eng_solid, plus the hardware sprite eng_ent_def /
; eng_ent_dist. The renderer's code lives in RENDER_BANK and is paged into
; slot 1 for the call; that hides bank 1 (half the game logic), which is
; paged back afterwards. Out: var_views + 1. Clobbers everything but IX, IY.
svc_render:
    push ix
    push iy
    ld a, RENDER_BANK
    ld ($FFFE), a            ; mapper: the renderer's bank into slot 1 ($4000)
    call render_main         ; rom/render.asm: draw the picture
    ld a, 1
    ld ($FFFE), a            ; mapper: bank 1 (game logic) back into slot 1
    ld hl, var_views
    inc (hl)                ; (for tools/time_steps.py: a picture was finished)
    jr svc_exit

; svc_refresh_sprite: replaces the hardware sprite shown over the view
; with eng_ent_def / eng_ent_dist (or hides it) without redrawing the view.
svc_refresh_sprite:
    push ix
    push iy
    call refresh_sprite         ; swap the sprite
    jr svc_exit

; svc_dialog_show: draws dlg_text as a text box over the view (dialog_show).
svc_dialog_show:
    push ix
    push iy
    call dialog_show            ; draw the box
    jr svc_exit

; svc_title: shows the title screen and returns once button 1 has been
; pressed and both buttons released (title_screen).
svc_title:
    push ix
    push iy
    call title_screen           ; show it and wait for button 1
    jr svc_exit

; svc_game_screen: sets up the play screen: status bar, black view area,
; palettes, line interrupts (game_screen). The view itself is drawn by the
; next svc_render.
svc_game_screen:
    push ix
    push iy
    call game_screen            ; status bar, palettes, line interrupts
; Common exit of the services: pages the level blob (HOME_BANK) back into
; slot 2, since the engine pages other data banks in, and restores the
; caller's IX and IY (pushed by each service on entry).
svc_exit:
    ld a, HOME_BANK
    ld ($FFFF), a            ; mapper: the level blob back into slot 2
    pop iy
    pop ix
    ret

;-----------------------------------------------------------------------
; init: power-on start-up (reached from the reset vector, never returns).
; Pages banks 0, 1 and HOME_BANK into slots 0-2, clears RAM, silences the
; PSG, programs the VDP (display off), clears VRAM, then jumps to the
; game logic's game_main (via C_ENTRY), with interrupts still disabled.
;-----------------------------------------------------------------------
init:
    di
    ld sp, $DFF0             ; stack grows down from $DFF0 (top of RAM; $DFFC-$DFFF mirror the mapper registers)
    xor a
    ld ($FFFC), a            ; mapper control: ROM in slot 2, no cartridge RAM
    ld ($FFFD), a            ; slot 0 = bank 0
    ld a, 1
    ld ($FFFE), a            ; slot 1 = bank 1
    ld a, HOME_BANK
    ld ($FFFF), a            ; slot 2 = the level blob's bank
    ; clear RAM below the stack
    ld hl, $C000
    ld de, $C001
    ld bc, $1DFF             ; $C000..$DDFF: everything up to the stack page
    ld (hl), 0
    ldir                     ; copy the 0 forward through the block
    call psg_silence         ; all four sound channels off
    ld a, 3
    ld (var_line_state), a   ; interrupts do nothing with palettes until game_screen
    call load_vdp_registers  ; mode 4, display off, line counter off
    call clear_vram          ; 16 KB of zeros
    jp C_ENTRY               ; -> game_main (game/ui.asm)

;-----------------------------------------------------------------------
; Title: shows the title screen and waits for button 1 (press + release)
;
; In: nothing. Out: returns with interrupts enabled, the display on and
; slot 2 = BANK_TITLE (svc_exit restores it). Overwrites tiles 128.. (the
; view's), the whole name table and CRAM. Clobbers AF, BC, DE, HL.
; Called by svc_title.
;-----------------------------------------------------------------------
title_screen:
    di
    ld a, 3
    ld (var_line_state), a   ; the interrupt handler leaves the palettes alone
    ld a, $FF
    ld b, 10
    call vdp_set_register    ; R10 = $FF: no line interrupt within the frame
    ld a, R1_DISPLAY_OFF
    ld b, 1
    call vdp_set_register    ; R1: blank the screen so VRAM can be written fast
    ld a, BANK_TITLE
    ld ($FFFF), a            ; mapper: bank 5 (title) into slot 2
    ld hl, 128*32
    call vdp_set_vram_write_hl   ; VRAM: tile 128
    ld hl, title_tiles_data
    ld bc, title_tiles_data_end-title_tiles_data
    call copy_hl_to_vdp_bc   ; the title's tile patterns
    ld hl, $0000
    call vdp_set_cram_write_hl   ; CRAM: colour 0
    ld hl, hud_palette_data  ; the title uses the status bar's 32 colours
.IFDEF GAMEGEAR
    ld bc, 32 << 8           ; b = 32 colours, c = 0 (nothing ORed in)
    call gg_cram_b           ; widened to Game Gear words
.ELSE
    ld bc, 32
    call copy_hl_to_vdp_bc   ; 32 CRAM bytes
.ENDIF
    ld hl, NAMETABLE
    call vdp_set_vram_write_hl  ; VRAM: the name table
    ld hl, title_map_data
    ld bc, 1536              ; the whole name table: 32 x 24 entries x 2 bytes
    call copy_hl_to_vdp_bc      ; the title's screen map
    call hide_sprites        ; no sprites on the title
    ld a, R1_DISPLAY_ON
    ld b, 1
    call vdp_set_register    ; R1: display on
    ei
_title_wait:
    ; wait for button 1 to be pressed (checked once a frame)
    call wait_frame
    in a, (IOPort1)
    cpl
    and $10                  ; bit 4: button 1
    jr z, _title_wait
_title_release:
    ; then for both buttons to be released, so the press is not seen again
    call wait_frame
    in a, (IOPort1)
    cpl
    and $30                  ; bits 4-5: buttons 1 and 2
    jr nz, _title_release
    ret

;-----------------------------------------------------------------------
; Game screen: status bar graphics, black view area, line interrupts on
;
; In: nothing. Uploads the status bar's tiles and its fixed layout
; (hud_static_data), blacks out the rest of the name table, fills hud_text
; with spaces and marks every status row dirty (the frame interrupt then
; writes them, one per frame), loads the level's palette into view_pal and
; the status bar's text colours into ent_pal, and starts the line
; interrupts. Sets var_need_layout so the next svc_render draws the
; border. Out: interrupts enabled, display on, slot 2 left at the
; palette's bank (svc_exit restores it). Clobbers AF, BC, DE, HL.
; Called by svc_game_screen.
;-----------------------------------------------------------------------
game_screen:
    di
    ld a, 3
    ld (var_line_state), a   ; no palette swaps while this runs
    ld a, R1_DISPLAY_OFF
    ld b, 1
    call vdp_set_register    ; R1: display off
    ld a, BANK_MISC
    ld ($FFFF), a            ; mapper: bank 4 (status bar tiles) into slot 2
    ; status bar tiles: slots 0..71 -> tiles 56..127, 72.. -> 496..
    ld hl, BLACK_TILE*32
    call vdp_set_vram_write_hl   ; VRAM: tile 56
    ld hl, hud_tiles_data
    ld bc, 72*32
    call copy_hl_to_vdp_bc   ; the first 72 status-bar tiles
    ld hl, 496*32
    call vdp_set_vram_write_hl   ; VRAM: tile 496
    ld hl, hud_tiles_data + 72*32
    ld bc, hud_tiles_data_end - hud_tiles_data - 72*32
    call copy_hl_to_vdp_bc   ; the rest of them
    call fill_nametable_black    ; every screen cell = the black tile
.IFDEF GAMEGEAR
    ld hl, NAMETABLE + GG_HUD_ROW*64
.ELSE
    ld hl, NAMETABLE + 19*64
.ENDIF
    call vdp_set_vram_write_hl   ; the status bar's first screen row
    ld hl, hud_static_data
    ld bc, 320               ; 5 rows x 32 entries x 2 bytes: the status bar's fixed tiles
    call copy_hl_to_vdp_bc      ; the status bar's fixed rows
    call hide_sprites           ; no sprite yet
    ; status text: all spaces, everything dirty
    ld hl, hud_text
    ld de, hud_text+1
    ld bc, 191
    ld (hl), ' '
    ldir
    ld a, $3F
    ld (eng_hud_dirty), a    ; bits 0-5: all six status rows need writing
    xor a
    ld (var_dlg_font), a     ; tiles 128.. do not hold the dialog font (the title may be there)
    ld (var_spr_shown), a
    ld (var_line_state), a   ; palette swaps on from the next line interrupt
    ld a, 1
    ld (var_tile_set), a     ; small view: the first picture goes into set 0
    ld (var_need_layout), a
    ; the level's palette: the level header says where it is
    ld a, LEVEL_BANK
    ld ($FFFF), a            ; mapper: bank 3 (level header) into slot 2
    ld a, ($8000 + H_PALETTE_BANK)
    ld hl, ($8000 + H_PALETTE_ADDR)
    ld ($FFFF), a            ; mapper: the palette's bank into slot 2
    ld de, view_pal
    ld bc, 16
    ldir                     ; 16 colours -> view_pal (written to CRAM by the interrupts)
    ld hl, hud_palette_data + 16
    ld de, ent_pal
    ld bc, 16
    ldir                     ; bank 1 starts as the status bar's text colours
.IFDEF GAMEGEAR
    ld a, GG_LINE_HUD
.ELSE
    ld a, FRAMES_LINE_SWAP_VIEW
.ENDIF
    ld b, 10
    call vdp_set_register    ; R10: the first line interrupt of each frame
    ld a, R1_DISPLAY_ON
    ld b, 1
    call vdp_set_register    ; R1: display on
    ei
    ret

;-----------------------------------------------------------------------
; Interrupt handler: frame + line interrupts (raster palette swaps)
;
; Entered through $0038 for every VDP interrupt. Reading the VDP status
; port acknowledges the interrupt; its bit 7 says whether this was the
; frame interrupt (vblank, after the last visible line) or a line
; interrupt. Both kinds:
; - set var_vdp_touched, because writing CRAM/registers here moves the
;   VDP's address: code that streams to VRAM with interrupts on (the
;   renderer) checks it and sets the address again;
; - save and restore AF, BC, DE, HL (IX, IY and the shadow registers are
;   not used).
; Frame interrupt: eng_frame + 1, eng_flash - 1 (if not 0), one step of
; the sound effects, then (unless var_line_state = 3: the title or a
; screen being set up) re-arms the line interrupts and writes one dirty
; status-bar row.
;
; Master System screen, by 8-line tile rows: row 0 messages, row 1 black,
; rows 2..17 border + view, row 18 black, rows 19..23 status bar. The
; status bar uses all 32 colours (both palettes); the view uses bank 0
; for the picture and bank 1 for the sprite. Line interrupts:
;   end of line 7   (state 0): view palettes in, R10 = 127   -> state 1
;   end of line 15  (state 1): nothing (the counter reloads to 127) -> 2
;   end of line 143 (state 2): status bar palette in, R10 = 7  -> state 0
; Each palette write happens during a black row, so it never shows.
;
; Game Gear screen (the 160x144 window = lines 24..167 of the 192): rows
; 3..14 the view, row 15 black, rows 16..20 the messages and status bar.
; The view's palettes go in at the frame interrupt; one line interrupt at
; the end of line 119 (state 0 -> 1) swaps in the status bar's.
;-----------------------------------------------------------------------
irq_handler:
.IFDEF GAMEGEAR
    push af
    push bc
    push de
    push hl
    ld a, 1
    ld (var_vdp_touched), a  ; the VDP address is about to move
    in a, (VDPStatus)        ; acknowledge; bit 7 = frame interrupt
    rlca                     ; bit 7 -> carry
    jr c, _irq_frame
    ld a, (var_line_state)
    or a
    jr nz, _irq_exit         ; already swapped this frame (or title): ignore
    ; end of line 119 (row 15 is black): the status bar's palette, its
    ; text colours (bank 1) first: the message row needs those soonest
    ld a, 1
    ld (var_line_state), a
    ld hl, $0020
    call vdp_set_cram_write_hl   ; CRAM byte $20 = colour 16 (2 bytes per colour on the GG)
    ld hl, hud_palette_data + 16
    ld bc, 16 << 8
    call gg_cram_b           ; status bar colours 16-31
    ld hl, $0000
    call vdp_set_cram_write_hl   ; CRAM colour 0
    ld hl, hud_palette_data
    ld bc, 16 << 8
    call gg_cram_b           ; status bar colours 0-15
    jr _irq_exit
_irq_frame:
    ld hl, eng_frame
    inc (hl)                 ; count frames (wait_frame watches this)
    ld a, (eng_flash)
    or a
    jr z, +
    dec a
    ld (eng_flash), a        ; one frame less of the damage flash
+:  call sfx_update          ; one frame of the sound effects
    ld a, (var_line_state)
    cp 3
    jr z, _irq_exit          ; title / screen set-up: leave the palettes alone
    ; vblank: the view's palettes (bank 0, red while the damage flash
    ; lasts) and the sprite's (bank 1)
    ld hl, $0000
    call vdp_set_cram_write_hl   ; CRAM colour 0
    ld hl, view_pal
    ld c, 0
    ld a, (eng_flash)
    or a
    jr z, +
    ld c, $0F                ; damage flash: full red in every view colour
+:  ld b, 16
    call gg_cram_b           ; colours 0-15: the view
    ld hl, ent_pal
    ld bc, 16 << 8
    call gg_cram_b           ; colours 16-31: the sprite (or the text colours)
    xor a
    ld (var_line_state), a   ; the next line interrupt swaps to the status bar
    ld a, GG_LINE_HUD
    ld b, 10
    call vdp_set_register    ; R10: line interrupt at the end of line 119
    call hud_flush_row       ; write one changed status row
_irq_exit:
    pop hl
    pop de
    pop bc
    pop af
    ei
    reti
.ELSE
    push af
    push bc
    push de
    push hl
    ld a, 1
    ld (var_vdp_touched), a  ; the VDP address is about to move
    in a, (VDPStatus)        ; acknowledge; bit 7 = frame interrupt
    rlca                     ; bit 7 -> carry
    jp c, _irq_frame
    ; a line interrupt: what to do depends on which one of the frame it is
    ld a, (var_line_state)
    cp 3
    jr z, _irq_exit          ; title / set-up: ignore
    cp 1
    jr z, _irq_skip          ; line 15
    cp 2
    jr z, _irq_hud_swap      ; line 143
    ; end of line 7: view (bank 0) + sprite/text (bank 1) palettes.
    ; R10 first: it must be written before the line-15 counter reload.
    ld a, FRAMES_LINE_SWAP_HUD
    ld b, 10
    call vdp_set_register    ; R10 = 127: after line 15, the next is line 143
    ld hl, $0000
    call vdp_set_cram_write_hl   ; CRAM colour 0
    ld hl, view_pal
    ld c, 0
    ld a, (eng_flash)
    or a
    jr z, +
    ld c, $03                ; damage flash: full red in every view colour
+:  ld b, 16
    ; colours 0-15: the view (ORed with c); this loop's writes are 41
    ; cycles apart, slow enough for the VDP while the screen is drawn
-:  ld a, (hl)
    or c
    out (VDPData), a
    inc hl
    djnz -
    ld b, 16
    call cram_write_b_paced  ; colours 16-31 from ent_pal (it follows view_pal in RAM)
    ld a, 1
    ld (var_line_state), a
    jr _irq_exit
_irq_skip:
    ; end of line 15: the counter has just reloaded with 127; nothing to do
    ld a, 2
    ld (var_line_state), a
    jr _irq_exit
_irq_hud_swap:
    ; end of line 143: the 32-colour status-bar palette
    ld hl, $0000
    call vdp_set_cram_write_hl   ; CRAM colour 0
    ld hl, hud_palette_data
    ld b, 32
    call cram_write_b_paced  ; all 32 colours
    ld a, FRAMES_LINE_SWAP_VIEW
    ld b, 10
    call vdp_set_register    ; R10 = 7 again (picked up in the vertical blank)
    xor a
    ld (var_line_state), a
    jr _irq_exit
_irq_frame:
    ld hl, eng_frame
    inc (hl)                 ; count frames (wait_frame watches this)
    ld a, (eng_flash)
    or a
    jr z, +
    dec a
    ld (eng_flash), a        ; one frame less of the damage flash
+:  call sfx_update          ; one frame of the sound effects
    ld a, (var_line_state)
    cp 3
    jr z, _irq_exit          ; title / screen set-up: no line interrupts wanted
    ; start the next frame's sequence at line 7 (also recovers if a line
    ; interrupt was missed while interrupts were disabled)
    xor a
    ld (var_line_state), a
    ld a, FRAMES_LINE_SWAP_VIEW
    ld b, 10
    call vdp_set_register    ; R10 = 7
    call hud_flush_row       ; write one changed status row (in vblank)
_irq_exit:
    pop hl
    pop de
    pop bc
    pop af
    ei
    reti

.ENDIF

.IFDEF GAMEGEAR
; b colours from (hl) (Master System bytes --BBGGRR) to the CRAM, as Game
; Gear words; c is ORed into each low byte (GGGGRRRR). Writes >= 26 cycles
; apart, so it may run while the screen is drawn.
; In: HL = source, B = count (1..255; 0 means 256), C = OR mask, the VDP
; address already set to a CRAM byte address (2 bytes per colour).
; Out: HL = source + B. Clobbers AF, B (= 0). Keeps C, DE.
; Needs gg_col (the 64-entry lookup table) at a 256-byte boundary: its
; high byte alone is put in H.
gg_cram_b:
    ld a, (hl)
    inc hl
    push hl
    and $3F                  ; the 6 colour bits
    add a, a                 ; 2 bytes per table entry
    ld l, a
    ld h, >gg_col            ; hl = gg_col + colour * 2
    ld a, (hl)
    or c                     ; e.g. $0F: red at full for the damage flash
    out (VDPData), a         ; low byte: green and red
    inc l                   ; 4
    ld a, (hl)              ; 7
    nop                     ; 4
    nop                     ; 4
    out (VDPData), a        ; 30 cycles after the last
    pop hl
    djnz gg_cram_b
    ret
.ENDIF

;-----------------------------------------------------------------------
; Sound effects on the PSG, one frame step per frame interrupt.
; The game code appends effect ids to sfx_q; they play one after another.
; Uses tone channel 0 and the noise channel; channel 2 stays silent but
; clocks the noise in noise modes 3/7. Data format: tools/build_sfx.py.
;
; PSG (SN76489) writes, one byte each to port $7F: a "latch" byte has bit
; 7 set, bits 6-5 = channel (0-2 tone, 3 noise), bit 4 = 1 for volume /
; 0 for tone or noise control, bits 3-0 = data. For a tone, a second byte
; (bit 7 clear) carries period bits 4-9. Volume data is an attenuation:
; 0 = loudest, 15 = silent.
;
; Effect data (build/gfx/sfx.inc): sfx_table holds one pointer per id;
; each effect is a list of 9-byte segments ended by a 0 byte:
;   +0 frames (1..255; 0 = end of the effect)
;   +1 u16 start period, 12.4 fixed point
;   +3 s16 period change per frame, 12.4 fixed point
;   +5 tone attenuation (15 = tone off)
;   +6 noise control ($FF = no noise; else bits 0-2 for the noise register)
;   +7 noise attenuation
;   +8 decay: high nibble = frames per tone attenuation step, low nibble
;      = frames per noise attenuation step (0 = no decay)
;-----------------------------------------------------------------------

; psg_silence: sets all four channels' attenuation to 15 (silent).
; Clobbers A. Called at reset and when an effect ends.
psg_silence:
    ld a, $9F                ; channel 0 volume = off
    out (PSGPort), a
    ld a, $BF                ; channel 1 volume = off
    out (PSGPort), a
    ld a, $DF                ; channel 2 volume = off
    out (PSGPort), a
    ld a, $FF                ; noise volume = off
    out (PSGPort), a
    ret

; sfx_update: one frame of the sound effects (called by the frame
; interrupt). If a segment is playing, writes its current period and
; volumes and advances it; when a segment runs out, loads the next one;
; when an effect ends, silences the PSG; when idle, starts the next
; queued effect. Ids outside sfx_table are dropped. Reads/writes the sfx_*
; variables. Clobbers AF, BC, DE, HL.
sfx_update:
    ld a, (sfx_left)
    or a
    jr nz, _sfx_run          ; a segment is still playing
    ld hl, (sfx_ptr)
    ld a, h
    or l
    jr nz, _sfx_seg          ; an effect is playing: its next segment
    ; idle: start the next queued effect, if any
    ld a, (sfx_qt)
    ld b, a
    ld a, (sfx_qh)
    cp b
    ret z                    ; head = tail: the queue is empty
    ld e, a
    inc a
    and 7                    ; ring of 8
    ld (sfx_qh), a           ; this entry is taken
    ld d, 0
    ld hl, sfx_q
    add hl, de
    ld a, (hl)               ; a = effect id
    cp (sfx_table_end - sfx_table) / 2
    ret nc                   ; unknown id: ignore it
    add a, a
    ld e, a
    ld hl, sfx_table
    add hl, de
    ld a, (hl)
    inc hl
    ld h, (hl)
    ld l, a                  ; hl = the effect's first segment
_sfx_seg:
    ; load one segment from (hl)
    ld a, (hl)
    or a
    jp z, _sfx_end           ; 0 frames: the end of the effect
    inc hl
    ld (sfx_left), a
    ld e, (hl)
    inc hl
    ld d, (hl)
    inc hl
    ld (sfx_per), de
    ld e, (hl)
    inc hl
    ld d, (hl)
    inc hl
    ld (sfx_dlt), de
    ld a, (hl)
    inc hl
    ld (sfx_tatt), a
    ld a, (hl)
    inc hl
    ld (sfx_noise), a
    cp $FF
    jr z, +
    or $E0                   ; noise control (resets the noise generator)
    out (PSGPort), a
+:  ld a, (hl)
    inc hl
    ld (sfx_natt), a
    ld a, (hl)
    inc hl
    ld (sfx_ptr), hl         ; the next segment
    ld b, a
    rrca
    rrca
    rrca
    rrca
    and $0F                  ; high nibble: the tone's decay period
    ld (sfx_tdr), a
    ld (sfx_tdc), a
    ld a, b
    and $0F                  ; low nibble: the noise's
    ld (sfx_ndr), a
    ld (sfx_ndc), a
_sfx_run:
    ; period = sfx_per >> 4 -> c = low 4 bits, b = high 6 bits
    ld hl, (sfx_per)
    ld a, l
    rrca
    rrca
    rrca
    rrca
    and $0F
    ld c, a                  ; bits 0-3 of the period
    ld a, h
    and $3F
    ld b, a                  ; bits 4-9
    ld a, c
    or $80                   ; channel 0 tone
    out (PSGPort), a         ; latch: channel 0 period bits 0-3
    ld a, b
    out (PSGPort), a         ; data: period bits 4-9
    ld a, (sfx_noise)
    cp $FF
    jr z, +                  ; no noise in this segment
    and 3
    cp 3
    jr nz, +                 ; noise at a fixed rate: channel 2 not needed
    ld a, c
    or $C0                   ; channel 2 tone (clocks the noise)
    out (PSGPort), a
    ld a, b
    out (PSGPort), a         ; same period: the noise follows the sweep
+:  ld a, (sfx_tatt)
    or $90
    out (PSGPort), a         ; channel 0 volume
    ld a, (sfx_noise)
    cp $FF
    ld a, $FF                ; (no noise: noise volume = off)
    jr z, +
    ld a, (sfx_natt)
    or $F0
+:  out (PSGPort), a         ; noise channel volume
    ; next frame
    ld hl, (sfx_per)
    ld de, (sfx_dlt)
    add hl, de
    ld (sfx_per), hl         ; the pitch sweep
    ; tone decay: every sfx_tdr frames, one step quieter (until silent)
    ld a, (sfx_tdr)
    or a
    jr z, +
    ld hl, sfx_tdc
    dec (hl)
    jr nz, +
    ld (hl), a               ; reload the counter
    ld hl, sfx_tatt
    ld a, (hl)
    cp 15
    jr z, +
    inc (hl)
    ; noise decay: the same with sfx_ndr / sfx_ndc / sfx_natt
+:  ld a, (sfx_ndr)
    or a
    jr z, +
    ld hl, sfx_ndc
    dec (hl)
    jr nz, +
    ld (hl), a
    ld hl, sfx_natt
    ld a, (hl)
    cp 15
    jr z, +
    inc (hl)
+:  ld hl, sfx_left
    dec (hl)                 ; one frame of the segment done
    ret
_sfx_end:
    ; the effect is over: idle (the next frame looks at the queue)
    ld hl, 0
    ld (sfx_ptr), hl
    jp psg_silence           ; (tail call) silence every channel

; generated by tools/build_sfx.py: sfx_table, sfx_table_end and the
; effects' segment data (format above). In bank 0 so the frame interrupt
; can always read it.
.include "build/gfx/sfx.inc"

; writes b bytes from (hl) to the data port, >= 26 cycles apart
; (the VDP's minimum while it draws the screen; here 41 cycles per byte).
; In: HL = source, B = count, the VDP address already set.
; Out: HL = source + count, B = 0. Clobbers A.
cram_write_b_paced:
    ld a, (hl)          ; 7
    out (VDPData), a    ; 11
    inc hl              ; 6
    nop                 ; 4
    djnz cram_write_b_paced
    ret

; Writes one dirty status-bar row (from the frame interrupt, in vblank).
; row 0 = all 32 columns of screen row 0; rows 1..5 = screen rows 19..23,
; columns 7..31 (the face is in columns 1..5).
; In: eng_hud_dirty (bit n = hud_text row n changed; the game sets bits),
; hud_text. Out: the lowest dirty row is written to the name table and
; its bit cleared; nothing happens if no bit is set. One row per frame
; keeps the interrupt short. Clobbers AF, BC, DE, HL.
; Each character c (ASCII) becomes the 2-byte name-table entry at
; font_index_data + (c - 32) * 2, with palette bit 3 set; codes outside
; 32..95 show as a space (entry 0).
; Game Gear layout: row 0 = message line (screen row 16, columns 6..25,
; text columns 6..25); rows 1..3 = screen rows 17..19, columns 9..25
; beside the face (text columns 7..23); row 4 = screen row 20, columns
; 6..25 (text columns 7..26); row 5 is not shown.
hud_flush_row:
.IFDEF GAMEGEAR
    ld a, (var_dlg_font)     ; a box covers the status bar: later
    or a
    ret nz
.ENDIF
    ld a, (eng_hud_dirty)
    or a
    ret z                    ; nothing changed
    ; find the lowest set bit: c = its number
    ld c, 0
-:  rrca
    jr c, +
    inc c
    jr -
+:  ; c = row index; clear its bit
    ld b, c
    ld a, 1
    inc b
    dec b
    jr z, ++                 ; c = 0: the mask is 1
-:  add a, a                 ; a = 1 << c
    djnz -
++: cpl
    ld hl, eng_hud_dirty
    and (hl)
    ld (hl), a               ; eng_hud_dirty &= ~(1 << c)
.IFDEF GAMEGEAR
    ld a, c
    or a
    jr nz, +
    ; the message line: screen row 16, columns 6..25
    ld hl, NAMETABLE + (GG_HUD_ROW*32 + 6)*2
    ld de, hud_text + 6
    ld b, 20
    jr ++
+:  cp 5
    ret z                    ; (row 5: not shown on the Game Gear)
    ; rows 1..3: screen rows 17..19, columns 9..25 (beside the face);
    ; row 4: screen row 20, columns 6..25. Text from column 7.
    ld h, 0
    ld l, c
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl               ; c * 32: the row's offset in hud_text
    ld de, hud_text+7
    add hl, de
    ex de, hl                ; de = the text
    ld a, c
    add a, GG_HUD_ROW
    ld l, a
    ld h, 0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl              ; *32
    ld a, c
    cp 4
    ld a, 9                  ; rows 1..3: from column 9, 17 characters
    ld b, 17
    jr nz, +
    ld a, 6                  ; row 4: from column 6, 20 characters
    ld b, 20
+:  push de
    ld e, a
    ld d, 0
    add hl, de
    add hl, hl              ; *2
    ld de, NAMETABLE
    add hl, de               ; hl = VRAM address of the first cell
    pop de
.ELSE
    ld a, c
    or a
    jr nz, +
    ; row 0: the message line, screen row 0, all 32 columns
    ld hl, NAMETABLE
    ld de, hud_text
    ld b, 32
    jr ++
+:  ; screen row 18+c, column 7
    ld h, 0
    ld l, c
    ld de, 18
    add hl, de
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl              ; *32
    ld de, 7
    add hl, de
    add hl, hl              ; *2
    ld de, NAMETABLE
    add hl, de
    push hl                  ; the VRAM address
    ld h, 0
    ld l, c
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl               ; c * 32
    ld de, hud_text+7
    add hl, de
    ex de, hl                ; de = the text, from column 7
    pop hl
    ld b, 25                 ; columns 7..31
.ENDIF
    ; hl = VRAM address, de = text, b = number of characters
++: call vdp_set_vram_write_hl
-:  ld a, (de)
    inc de
    sub 32
    cp 64
    jr c, +
    xor a                    ; not in the font: a space
+:  push de
    ld l, a
    ld h, 0
    add hl, hl
    ld de, font_index_data
    add hl, de               ; hl = the character's name-table entry
    ld a, (hl)
    out (VDPData), a         ; tile number, low 8 bits
    inc hl
    ld a, (hl)
    or 8                    ; sprite palette (text colours)
    nop
    nop                      ; (spacing the VDP writes)
    out (VDPData), a         ; tile bit 8 and flags
    pop de
    djnz -
    ret

; wait_frame: waits for the next frame interrupt (until eng_frame
; changes). HALT sleeps until any interrupt; line interrupts wake it too,
; hence the loop. Needs interrupts enabled, or it hangs.
; Clobbers AF, B.
wait_frame:
    ld a, (eng_frame)
_wait_frame_w:
    halt
    ld b, a
    ld a, (eng_frame)
    cp b
    jr z, _wait_frame_w
    ret

;-----------------------------------------------------------------------
; VDP helpers
;
; The VDP (video chip) is driven through two ports:
; - control ($BF): two bytes per command. Second byte bits 7-6 say what:
;   01 = set a VRAM write address, 11 = set a CRAM (palette) write address,
;   10 = write a register (first byte = value, second = $80 | register).
;   The other 14 bits are the address (low byte first).
; - data ($BE): reads/writes VRAM or CRAM at the address, which then goes
;   up by 1.
; While the screen is being drawn the VDP needs at least 26 cycles between
; data writes, or bytes are lost; with the display off or in the vertical
; blank, writes may go at full speed.
;-----------------------------------------------------------------------
; The VDP's 11 registers as set at start-up (loaded by load_vdp_registers):
vdp_init_data:
    .db $36         ; r0: mode 4, line interrupts, mask column 0
    .db $A2         ; r1: display off, frame int on, 8x16 sprites
    .db $FF         ; r2: nametable $3800
    .db $FF         ; r3 (unused in mode 4; all bits set as usual)
    .db $FF         ; r4 (unused in mode 4; all bits set as usual)
    .db $FF         ; r5: SAT $3F00
    .db $FB         ; r6: sprite patterns at $0000
    .db $00         ; r7: border = bank1 colour 0 (black)
    .db $00         ; r8: hscroll
    .db $00         ; r9: vscroll
    .db $FF         ; r10: line counter (off until the game starts)

; load_vdp_registers: writes vdp_init_data into VDP registers 0..10.
; Clobbers AF, B, HL. Called once by init.
load_vdp_registers:
    ld hl, vdp_init_data
    ld b, 0                  ; register number
-:  ld a, (hl)
    out (VDPAddress), a      ; the value
    ld a, b
    or $80
    out (VDPAddress), a      ; $80 | register: write it
    inc hl
    inc b
    ld a, b
    cp 11
    jr nz, -
    ret

; a = value, b = register
; Writes one VDP register. Clobbers A.
vdp_set_register:
    out (VDPAddress), a
    ld a, b
    or $80
    out (VDPAddress), a
    ret

; Sets the VDP to write VRAM from address HL ($0000-$3FFF); later bytes
; go to the data port. Clobbers A.
vdp_set_vram_write_hl:
    ld a, l
    out (VDPAddress), a
    ld a, h
    and $3F
    or $40                   ; code 01: VRAM write
    out (VDPAddress), a
    ret

; Sets the VDP to write CRAM (the palette) from byte HL. SMS: one byte per
; colour, 32 colours; Game Gear: two bytes per colour (64 bytes).
; Clobbers A.
vdp_set_cram_write_hl:
    ld a, l
    out (VDPAddress), a
    ld a, h
    and $3F
    or $C0                   ; code 11: CRAM write
    out (VDPAddress), a
    ret

; clear_vram: zeroes all 16 KB of VRAM (display off only: unpaced).
; Clobbers AF, BC, HL.
clear_vram:
    ld hl, $0000
    call vdp_set_vram_write_hl  ; VRAM address 0
    ld bc, $4000             ; 16 KB
-:  xor a
    out (VDPData), a
    dec bc
    ld a, b
    or c
    jr nz, -
    ret

; bc bytes from (hl) to the data port (display off only)
; The VDP address must already be set. Out: HL = source + count, BC = 0.
; Clobbers A.
copy_hl_to_vdp_bc:
    ld a, (hl)
    out (VDPData), a
    inc hl
    dec bc
    ld a, b
    or c
    jr nz, copy_hl_to_vdp_bc
    ret

; fill_nametable_black: sets all 768 name-table entries (32 x 24) to the
; black tile (56) with the second palette. Display off only (unpaced).
; Clobbers AF, BC, HL.
fill_nametable_black:
    ld hl, NAMETABLE
    call vdp_set_vram_write_hl  ; VRAM: the name table
    ld bc, 768
-:  ld a, BLACK_TILE
    out (VDPData), a         ; tile number
    ld a, 8                  ; sprite-palette colour 0 is always black
    out (VDPData), a         ; bit 3: second palette
    dec bc
    ld a, b
    or c
    jr nz, -
    ret

; hide_sprites: writes Y = $D0 into the first SAT entry. On the VDP, a Y
; of $D0 ends the sprite list (in the 192-line mode), so no sprite shows.
; Changes the VDP address, so interrupts should be off (or the caller
; must accept that an interrupt may move it). Clobbers A, HL.
hide_sprites:
    ld hl, SAT
    call vdp_set_vram_write_hl  ; VRAM: the SAT's first Y byte
    ld a, $D0
    out (VDPData), a
    ret

; hide_sprites_safe: hide_sprites with interrupts disabled around it
; (so an interrupt cannot change the VDP address between the two
; writes). Returns with interrupts enabled. Clobbers A, HL.
hide_sprites_safe:
    di
    call hide_sprites
    ei
    ret

; Waits until the beam is just below the view (V counter 150..154) and
; returns with interrupts disabled.
; Port $7E reads the VDP's V counter: the line being drawn. Being just
; below the view gives the caller a window to change the name table or
; the sprite table without tearing. Clobbers AF.
; Called by rom/render.asm.
wait_below_view:
.IFDEF GAMEGEAR
    in a, ($7E)              ; (below the Game Gear's screen: 168..172)
    cp 168
    jr c, wait_below_view
    cp 173
    jr nc, wait_below_view
.ELSE
    in a, ($7E)
    cp 150
    jr c, wait_below_view    ; still above line 150
    cp 155
    jr nc, wait_below_view   ; past line 154: wait for the next frame's
.ENDIF
    di
    ret

; Paced VRAM upload with the display ON: hl = src, de = VRAM address,
; bc = count in 16-byte units. 8 bytes per short DI window, writes 37
; cycles apart (the VDP needs >= 26 during active display).
; Out: HL and DE advanced past the data, BC = 0. Clobbers AF.
; Returns with interrupts enabled (even if they were off before). Short
; DI windows keep the line interrupts' palette swaps on time.
; Called by sprite_prepare, dialog_show and rom/render.asm.
vram_upload_paced:
    ld a, b
    or c
    ret z                    ; nothing to copy
-:  push bc
    call _vram_upload_paced_8    ; 8 bytes
    call _vram_upload_paced_8    ; 8 more: one 16-byte unit
    pop bc
    dec bc
    ld a, b
    or c
    jr nz, -
    ret
; one DI window: 8 bytes from HL to VRAM address DE; HL += 8, DE += 8.
; Clobbers AF, B.
_vram_upload_paced_8:
    di
    ex de, hl
    call vdp_set_vram_write_hl   ; the address must be set again: an interrupt may have moved it
    ex de, hl
    ld b, 8
-:  ld a, (hl)              ; 7
    out (VDPData), a        ; 11
    inc hl                  ; 6
    djnz -                  ; 13 -> 37 cycles per byte
    ei
    ld a, e
    add a, 8
    ld e, a
    ret nc
    inc d                    ; de += 8
    ret

;-----------------------------------------------------------------------
; Sprites
;-----------------------------------------------------------------------
; Reads eng_ent_def/eng_ent_dist, uploads that record's patterns to VRAM
; tiles 0.. (paced), stages its palette in ent_new_pal and its layout in
; spr_*. The sprite must already be hidden.
; In: eng_ent_def (entity type 0..63, $FF = none), eng_ent_dist (1..4
; cells ahead, 0 = none), eng_vsize (which size's art).
; Out: var_spr_shown = 1 if a sprite was prepared, else 0 (and nothing
; else changes). Leaves slot 2 at the sprite's bank. Interrupts enabled
; on return (vram_upload_paced). Clobbers AF, BC, DE, HL.
; Data (bank 4): sprite_dir_small ($B000) and sprite_dir_data ($B800)
; each hold 64 types x 4 distances x {u8 bank, u16 address, u8 0}; after
; the large one, at $BC00, 64 x 16 palette bytes (one palette per type,
; for both sizes). A record: u8 columns, rows, x, y, 4 bytes pad, then
; the 8x16 patterns (64 bytes each), column by column.
sprite_prepare:
    xor a
    ld (var_spr_shown), a
    ld a, (eng_ent_def)
    cp $FF
    ret z                    ; nothing to show
    ld a, (eng_ent_dist)
    or a
    ret z                    ; nothing to show
    ld a, BANK_MISC
    ld ($FFFF), a            ; mapper: bank 4 (sprite directories) into slot 2
    ; palette: 16 bytes per type, after the large view's 1 KB directory
    ld a, (eng_ent_def)
    ld l, a
    ld h, 0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl               ; def*16
    ld de, sprite_dir_data + 64*4*4      ; palettes (shared by both sizes)
    add hl, de
    ld de, ent_new_pal
    ld bc, 16
    ldir                     ; stage the palette (shown by the caller)
    ; directory entry (def*4 + dist-1)*4
    ld a, (eng_ent_def)
    ld l, a
    ld h, 0
    add hl, hl
    add hl, hl
    ld a, (eng_ent_dist)
    dec a
    ld e, a
    ld d, 0
    add hl, de
    add hl, hl
    add hl, hl
    ld de, sprite_dir_data
    ld a, (eng_vsize)
    or a
    jr z, +
    ld de, sprite_dir_small  ; small view: its own directory
+:  add hl, de
    ld a, (hl)               ; the record's bank
    inc hl
    ld e, (hl)
    inc hl
    ld d, (hl)               ; de = its address in slot 2
    ld ($FFFF), a            ; mapper: the record's bank into slot 2
    ex de, hl                ; hl = record
    ld a, (hl)
    ld (spr_cols), a
    inc hl
    ld a, (hl)
    ld (spr_rows), a
    inc hl
    ld a, (hl)
    ld (spr_x0), a
    inc hl
    ld a, (hl)
    ld (spr_y0), a
    ld de, 5
    add hl, de               ; patterns
    ; bytes = cols*rows*64 -> 16-byte units = cols*rows*4
    ; (computed in A: assumes cols * rows * 4 < 256, i.e. under 64 sprites)
    push hl
    ld a, (spr_cols)
    ld b, a
    ld a, (spr_rows)
    ld c, a
    xor a
-:  add a, c                 ; a = cols * rows
    djnz -
    add a, a
    add a, a                 ; * 4
    ld c, a
    ld b, 0
    pop hl
    ld de, $0000             ; VRAM: tile 0
    call vram_upload_paced   ; the patterns, while the display is on
    ld a, 1
    ld (var_spr_shown), a
    ret

; writes the sprite attribute table (interrupts off)
; In: var_spr_shown, spr_cols, spr_rows, spr_x0, spr_y0. If no sprite is
; prepared, just hides the sprites. Otherwise one hardware sprite (8x16)
; per column and row, in column order to match the patterns: sprite k
; uses tiles 2k and 2k+1. Writes the Y bytes at SAT ($D0 after the last
; ends the list) and the (X, tile) pairs at SAT + $80. Clobbers AF, BC,
; E, HL. Called by refresh_sprite and rom/render.asm.
entity_write_sat:
    ld a, (var_spr_shown)
    or a
    jp z, hide_sprites       ; (tail call) none: hide them all
    ld hl, SAT
    call vdp_set_vram_write_hl   ; VRAM: the SAT's Y table
    ld a, (spr_cols)
    ld b, a
--: ld a, (spr_rows)
    ld c, a
    ld a, (spr_y0)
.IFDEF GAMEGEAR
    sub GG_VIEW_SHIFT
.ENDIF
-:  out (VDPData), a         ; this sprite's Y
    add a, 16                ; the next row is 16 lines lower
    dec c
    jr nz, -
    djnz --
    ld a, $D0
    out (VDPData), a         ; end of the sprite list
    ld hl, SAT + $80
    call vdp_set_vram_write_hl   ; VRAM: the SAT's (X, tile) table
    ld a, (spr_x0)
    ld e, a                  ; e = this column's X
    ld a, (spr_cols)
    ld b, a
    ld h, 0                  ; tile number (even)
--: ld a, (spr_rows)
    ld c, a
-:  ld a, e
    out (VDPData), a         ; X
    ld a, h
    out (VDPData), a         ; tile (8x16 sprites use tiles n and n+1)
    inc h
    inc h
    dec c
    jr nz, -
    ld a, e
    add a, 8                 ; the next column is 8 pixels to the right
    ld e, a
    djnz --
    ret

; refresh_sprite: changes the sprite over the view without drawing the
; view: hides the old sprite, uploads the new one's patterns, then at the
; next frame (in vblank) puts in its palette and its SAT entries.
; In: eng_ent_def, eng_ent_dist, eng_vsize. Interrupts enabled on return.
; Clobbers AF, BC, DE, HL; slot 2 left at the sprite's bank.
; Called by svc_refresh_sprite.
refresh_sprite:
    call hide_sprites_safe   ; the old sprite off before its tiles change
    call sprite_prepare      ; new patterns into tiles 0..
    call wait_frame          ; just after the frame interrupt: in vblank
    di
    ld hl, ent_new_pal
    ld de, ent_pal
    ld bc, 16
    ldir                     ; the sprite's palette (written to CRAM by the interrupts)
    call entity_write_sat    ; and its positions
    ei
    ret

;-----------------------------------------------------------------------
; Dialog box: dlg_text (16 x 24 ASCII) drawn over the view with the
; dialog font, which is loaded into the idle tile set (the view on screen
; is untouched; showing any view afterwards restores the picture).
;
; (What the code does: the font always goes into tiles 128.. - the large
; view's tiles and the small view's first set - so the picture's tiles
; there are overwritten. The box's name-table entries cover the whole
; view, so this does not show; var_need_layout = 1 makes the next
; svc_render rewrite the border and view area, which brings the picture
; back.)
; In: dlg_text, 16 rows of 24 characters (Game Gear: 20), ASCII 32..126;
; other codes show as a space. Character c uses tile 128 + (c - 32).
; Out: the box on screen (SMS: screen rows 2..17, columns 4..27; Game
; Gear: rows 3..18, columns 6..25), the sprite hidden, bank 1 of the
; palette = the status bar's text colours (via ent_pal), var_dlg_font = 1
; (the font stays loaded for the next box until a view is drawn).
; Writes the name table with the display on: a few entries per DI window.
; Clobbers AF, BC, DE, HL; slot 2 may be left at BANK_TITLE.
; Called by svc_dialog_show.
;-----------------------------------------------------------------------
dialog_show:
    ld a, 1
    ld (var_need_layout), a  ; the next view must redraw the border too
    ld hl, 128
    ld (var_dlg_base), hl    ; the font's first tile
    ld a, (var_dlg_font)
    or a
    jr nz, +                 ; font already in VRAM
    ld de, 128*32            ; VRAM: tile 128
    ld a, BANK_TITLE
    ld ($FFFF), a            ; mapper: bank 5 (dialog font) into slot 2
    ld hl, dlg_font_data
    ld bc, (dlg_font_data_end - dlg_font_data) / 16
    call vram_upload_paced   ; the font's patterns, display on
    ld a, 1
    ld (var_dlg_font), a
+:  call hide_sprites_safe   ; no sprite over the box
    xor a
    ld (var_spr_shown), a
    ld hl, hud_palette_data + 16
    ld de, ent_pal
    ld bc, 16
    ldir                     ; bank 1 = text colours (from the next frame)
.IFDEF GAMEGEAR
    ; rows 3..18, columns 6..25; 10 entries per DI window
    ld hl, dlg_text
    ld de, NAMETABLE + (3*32+6)*2
    ld a, 16
    ld (tmp_count), a
_dlg_row:
    ld a, 2
    ld (tmp_a), a
.ELSE
    ; rows 2..17, columns 4..27; 8 entries per DI window
    ld hl, dlg_text
    ld de, NAMETABLE + (2*32+4)*2
    ld a, 16
    ld (tmp_count), a
_dlg_row:
    ld a, 3
    ld (tmp_a), a
.ENDIF
_dlg_chunk:
    ; hl = next character, de = VRAM address of the next entry
    di
    ex de, hl
    call vdp_set_vram_write_hl   ; set the address (an interrupt may have moved it)
    ex de, hl
.IFDEF GAMEGEAR
    ld b, 10
.ELSE
    ld b, 8
.ENDIF
-:  ld a, (hl)
    inc hl
    sub 32
    cp 95
    jr c, +
    xor a                    ; outside 32..126: a space
+:  push hl
    ld hl, (var_dlg_base)
    add a, l
    ld l, a
    jr nc, +
    inc h                    ; hl = tile number (128 + c - 32)
+:  ld a, l
    out (VDPData), a         ; tile low
    ld a, h
    and 1                    ; tile bit 8
    or 8                     ; sprite palette = text colours
    nop
    out (VDPData), a
    pop hl
    djnz -
    ei                       ; let a pending line interrupt in between chunks
    ld a, e
.IFDEF GAMEGEAR
    add a, 20
.ELSE
    add a, 16
.ENDIF
    ld e, a
    jr nc, +
    inc d                    ; de += 2 bytes per entry written
+:  ld a, (tmp_a)
    dec a
    ld (tmp_a), a
    jr nz, _dlg_chunk
    ; next row: +64 bytes - 48 (Game Gear: 40) already advanced
    ex de, hl
.IFDEF GAMEGEAR
    ld bc, 24
.ELSE
    ld bc, 16
.ENDIF
    add hl, bc
    ex de, hl
    ld a, (tmp_count)
    dec a
    ld (tmp_count), a
    jr nz, _dlg_row
    ret

;-----------------------------------------------------------------------
; Bank 0 data the interrupt handler needs (must never be paged out)
;-----------------------------------------------------------------------
; The status bar's 32 colours (SMS CRAM bytes): 0-15 for its graphics,
; 16-31 for its text (also used as bank 1 while a dialog box is open).
hud_palette_data:
.incbin "build/gfx/hud_palette.bin"
; Status-bar font: for each character 32..95, its 2-byte name-table entry
; (tile number low byte, then tile bit 8 and flags). The tiles are among
; the status-bar tiles uploaded by game_screen.
font_index_data:
.IFDEF GAMEGEAR
.incbin "build/gfx/font_index_gg.bin"
.ELSE
.incbin "build/gfx/font_index.bin"
.ENDIF
engine_end:                  ; (end of the engine's code and data; $0E00 must not be reached on the GG)
.IFDEF GAMEGEAR
; The colour table must start on a 256-byte boundary (gg_cram_b loads only
; its high byte), so it is placed at $0E00, below the game code at $1000.
.ORGA $0E00
gg_col:                      ; Master System colour (--BBGGRR) -> Game Gear CRAM word
.INCLUDE "rom/gg_col.inc"
.ENDIF

;-----------------------------------------------------------------------
; The game logic (the game/ .asm files): $1000.. in bank 0, $4000.. in bank 1
; game.asm only defines constants and RAM; ui, core, math32 and events
; fill bank 0 from $1000; combat and actions fill bank 1 from $4000.
;-----------------------------------------------------------------------
.include "game/game.asm"
.ORGA $1000
game_code_0:
    jp game_main                ; C_ENTRY (init jumps here)
.include "game/ui.asm"
.include "game/core.asm"
.include "game/math32.asm"
.include "game/events.asm"
.include "game/gen/tables.inc"
game_code_0_end:

; Bank 1 sits in slot 1 ($4000) for the whole game, except while
; svc_render has the renderer's bank there.
.BANK 1 SLOT 1
.ORGA $4000
game_code_1:
.include "game/combat.asm"
.include "game/actions.asm"
game_code_1_end:

.ORGA $7FC0
rom_directory:               ; where the generated parts are (tools/rom_dir.py, the editor)
.include "build/gfx/rom_dir.inc"

; The Sega ROM header, which the console's BIOS checks before booting.
; tools/rom_size.py fills in the checksum and the size code after linking.
.ORGA $7FF0
.db "TMR SEGA"                   ; signature
.db $00, $00                     ; reserved
.db $00, $00                     ; checksum (little-endian), filled in by rom_size.py
.db $00, $00, $00                ; product code and version (unused)
.db $40                          ; high nibble region (4 = SMS export; 7 for the Game Gear), low nibble size code (both set by rom_size.py)

; Bank 2 (HOME_BANK) stays in slot 2 while the game logic runs. Its first
; 8 KB ($8000-$9FFF) are left empty here.
.BANK 2 SLOT 2
.ORGA $A000                  ; LEVEL_ADDR: the level blob (tools/gen_cdata.py, or the editor)
.incbin "build/gfx/level.bin"

;=======================================================================
; Bank 3 and banks 9..: the level's data, starting with the level header
; (tools/live_data.py, or the editor; rom/live_layout.inc)
;=======================================================================
.include "build/gfx/live/data_banks.inc"

;=======================================================================
; Bank 4: strings (C reads them at $8000), fonts, title, sprite directory
;=======================================================================
.BANK 4 SLOT 2
.ORGA $8000
strings_data:                ; the level's strings, up to $9FFF (8 KB; docs/rom_format.md allows 6 KB)
.IFDEF GAMEGEAR
.incbin "build/gfx/strings_gg.bin"   ; (the Game Gear's: its buttons' names, its narrower box)
.ELSE
.incbin "build/gfx/strings.bin"
.ENDIF
.ORGA $A000
hud_tiles_data:              ; status bar tile patterns (32 bytes per tile; see game_screen)
.IFDEF GAMEGEAR
.incbin "build/gfx/hud_tiles_gg.bin"
.ELSE
.incbin "build/gfx/hud_tiles.bin"
.ENDIF
hud_tiles_data_end:
hud_static_data:             ; the status bar's fixed name-table rows (5 x 32 entries)
.IFDEF GAMEGEAR
.incbin "build/gfx/hud_static_gg.bin"
.ELSE
.incbin "build/gfx/hud_static.bin"
.ENDIF
.ORGA $B000
sprite_dir_small:            ; small view: 64 defs x 4 distances x 4 bytes
.incbin "build/gfx/sprite_dir_small.bin" READ 1024
.ORGA $B800
sprite_dir_data:             ; large view: 64 defs x 4 distances x 4 bytes, then 64 x 16 palette bytes
.incbin "build/gfx/sprite_dir_large.bin"

;=======================================================================
; Bank 5: title screen
;=======================================================================
.BANK 5 SLOT 2
.ORGA $8000
title_tiles_data:            ; patterns, loaded at tile 128
.IFDEF GAMEGEAR
.incbin "build/gfx/title_tiles_gg.bin"
.ELSE
.incbin "build/gfx/title_tiles.bin"
.ENDIF
title_tiles_data_end:
title_map_data:              ; the whole name table (32 x 24 entries, 1536 bytes)
.IFDEF GAMEGEAR
.incbin "build/gfx/title_map_gg.bin"
.ELSE
.incbin "build/gfx/title_map.bin"
.ENDIF
.ORGA $9000
dlg_font_data:               ; the dialog boxes' font (copied over the view while a box is open)
.incbin "build/gfx/dlg_font.bin"
dlg_font_data_end:
.ORGA $A000                  ; chunky-to-planar tables of the renderer (pages $A0..$BF)
.incbin "build/gfx/live/c2p.bin"

;=======================================================================
; The renderer (bank RENDER_BANK, run in slot 1) and its data
;=======================================================================
.include "rom/render.asm"

; Height tables: for each projected height, the rows it covers on screen,
; the texture start and step, and its depth (1792 x 5 bytes; see
; tools/live_data.py and rom/render.asm). One table per view size.
.BANK TABLE_BANK_LARGE SLOT 2
.ORGA $8000
.incbin "build/gfx/live/table_large.bin"
.BANK TABLE_BANK_SMALL SLOT 2
.ORGA $8000
.incbin "build/gfx/live/table_small.bin"
; generated: the hardware sprites' records for each view size (banks
; after the level's data)
.include "build/gfx/sprite_banks_large.inc"
.include "build/gfx/sprite_banks_small.inc"
