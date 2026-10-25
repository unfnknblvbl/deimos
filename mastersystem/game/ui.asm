; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

;=======================================================================
; The player-facing side: input, messages, the status bar, dialog boxes,
; door-code entry, the notebook, the pause menu, and choosing what the
; engine draws (the view and the sprite of what is straight ahead).
;
; Controls:
;   Up / Down          step forward / back
;   Left / Right       turn
;   Button 1           use / attack what is in front of you
;   Button 2 + L / R   previous / next weapon
;   Button 2 + Up      use a medkit (smallest first)
;   Button 2 + Down    pass the turn
;   Button 2 + 1       notebook (also in the pause menu)
;   Pause              pause menu: resume, notebook, view settings
;
; Bank 0. game_main (the entry point: rom/main.asm jumps to it at $1000)
; runs the whole game: the title, then one frame at a time, reading the
; pad and handing each new press to `handle`, which dispatches on `ui`
; (UI_*: what the screen is showing). In play, the actions (actions.asm)
; change the game state; after_action then redraws the view and the
; status bar and opens any box the rules asked for.
;
; The engine services this file calls (rom/main.asm, through the same
; names as the jump table at $0100):
;   svc_wait_frame       wait for the next frame interrupt
;   svc_read_pad         -> l = the buttons held (PAD_* bits)
;   svc_render           draw the view at ENG_VIEW_X/Z/F
;   svc_refresh_sprite   redraw only the sprite (ENG_ENT_DEF/DIST)
;   svc_dialog_show      show the box in DLG_TEXT over the view
;   svc_title            the title screen (waits for a button)
;   svc_game_screen      set up the play screen (border, status bar)
;
; Text: messages queue in msgq and show one at a time on the top row
; (HUD_TEXT row 0). The status bar (rows 1..5) is built in HUD_STAGE and
; copied to HUD_TEXT only where it changed; the frame interrupt draws the
; rows marked in ENG_HUD_DIRTY. Boxes are written into DLG_TEXT (DLG_H
; rows of DLG_W characters) and shown with svc_dialog_show. A '|' in a
; level string starts a new line.
;=======================================================================

.ENUM $D370
ui_row          db                  ; hud_row_text / box_put: row, column (unused)
ui_col          db                  ; (unused)
ui_n            db                  ; (unused)
hu_c            db                  ; hud_update: the column after a number (unused)
ss_d            db                  ; select_sprite: distance being looked at (1..4)
ss_x            db                  ; the cell being looked at
ss_z            db
ss_best         db                  ; the entity chosen on it, $FF none
rp_doors        db                  ; refresh_picture: 1 if a door changed
bx_line         db                  ; box_string: the line of the string being read
bx_first        db                  ; the first line to show
bx_row          db                  ; the next box row to write
ui_edge         db                  ; this input: buttons newly pressed this frame
ui_pad          db                  ; buttons held
ui_ram_end      db
.ENDE

;-----------------------------------------------------------------------
; strings and sounds
;-----------------------------------------------------------------------
; a = level string -> hl = a copy of it (strbuf; a missing one is "")
; Pages BANK_MISC (the strings) into slot 2 while copying (at most 399
; characters, then a 0), then pages BANK_HOME back. The copy lives until
; the next level_string (and strbuf is shared with the monsters' turn).
; Clobbers a, bc, de.
level_string:
    ld hl, n_strings
    cp (hl)
    jr c, +
    ld a, 3                     ; (string 3 is "")
+:  ld l, a
    ld h, 0
    add hl, hl
    ld de, STRINGS_ADDR
    add hl, de                  ; hl = &offset[string]
    ld a, BANK_MISC
    ld ($FFFF), a               ; mapper: the strings' bank into slot 2
    ld e, (hl)
    inc hl
    ld d, (hl)
    ld hl, STRINGS_ADDR
    add hl, de                  ; hl = the text
    ld de, strbuf
    ld bc, 399                  ; room in strbuf (400) less the 0
-:  ld a, (hl)
    or a
    jr z, +
    ld (de), a
    inc hl
    inc de
    dec bc
    ld a, b
    or c
    jr nz, -
+:  xor a
    ld (de), a
    ld a, BANK_HOME
    ld ($FFFF), a               ; mapper: the level blob back into slot 2
    ld hl, strbuf
    ret

; a = sound effect: queued (dropped if the queue is full); keeps bc, de, hl
; The engine plays the queue (SFX_QUEUE, a ring of 8) one effect after
; another; SFX_TAIL is the game's end of it. One slot is always left
; empty, so at most 7 wait.
sfx:
    push hl
    push bc
    ld c, a
    ld a, (SFX_TAIL)
    ld b, a
    inc a
    and 7                       ; the slot after the tail
    ld hl, SFX_HEAD
    cp (hl)
    jr z, +                     ; it would reach the head: full
    ld (SFX_TAIL), a
    ld a, b
    ld hl, SFX_QUEUE
    call add_hl_a
    ld (hl), c                  ; store the id in the old tail slot
+:  pop bc
    pop hl
    ret

; a new input drops the effects still waiting from the last turn, so the
; sound keeps up with a fast player (the one playing finishes)
; Interrupts are off while the tail is moved, because the frame
; interrupt reads both ends. Clobbers a.
sfx_flush:
    di
    ld a, (SFX_HEAD)
    ld (SFX_TAIL), a            ; tail = head: empty
    ei
    ret

;-----------------------------------------------------------------------
; messages: a queue of lines for the top row, one shown every 45 frames
;-----------------------------------------------------------------------
; hl, de, bc = up to three strings (0 = none): one message of them (31 at most)
; Joined into the next free slot of msgq; if all MSGQ slots are taken,
; the oldest message is dropped. Clobbers every register except IX, IY.
msg_push:
    push bc
    push de
    push hl
    ld a, (msg_count)
    cp MSGQ
    jr nz, +
    ld hl, msg_head             ; full: the oldest goes
    ld a, (hl)
    inc a
    and MSGQ-1
    ld (hl), a
    ld hl, msg_count
    dec (hl)
+:  ld a, (msg_head)
    ld hl, msg_count
    add a, (hl)
    and MSGQ-1                  ; the slot after the newest
    ld l, a
    ld h, 0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl                  ; * 32 bytes a slot
    ld de, msgq
    add hl, de
    ex de, hl                   ; de = the slot
    ld c, 0                     ; length
    pop hl
    call mp_append              ; the first string
    pop hl
    call mp_append              ; the second
    pop hl
    call mp_append              ; the third
    xor a
    ld (de), a                  ; the terminating 0
    ld hl, msg_count
    inc (hl)
    ret

; hl = a string or 0, de = where, c = the length so far (31 at most)
; -> de, c advanced past what was copied; clobbers a, hl
mp_append:
    ld a, h
    or l
    ret z                       ; no string
-:  ld a, c
    cp 31
    ret nc                      ; the message is full
    ld a, (hl)
    or a
    ret z
    ld (de), a
    inc hl
    inc de
    inc c
    jr -

; hl = a message (queued for the top row); clobbers every register
say:
    ld de, 0
    ld bc, 0
    jr msg_push

; hl, de = two strings, one message; clobbers every register
say2:
    ld bc, 0
    jr msg_push

; hl = text, de = a number, bc = more text: "<text><number><more>";
; clobbers every register
say_num:
    push hl
    push bc
    ex de, hl
    call utoa10                 ; hl = the number's digits
    ex de, hl                   ; de = them (the middle string)
    pop bc
    pop hl
    jr msg_push

; hl = n -> hl = its decimal digits (numbuf), unsigned, no leading zeros;
; clobbers a, bc, de. Digits are produced from the last, backwards.
utoa10:
    ld de, numbuf+5
    xor a
    ld (de), a                  ; the terminating 0
-:  push de
    ld bc, 10
    call div16                  ; hl = n / 10, de = n % 10
    ld a, e
    add a, '0'
    pop de
    dec de
    ld (de), a
    ld a, h
    or l
    jr nz, -                    ; until the quotient is 0
    ex de, hl
    ret

; once a frame (game_main): counts the message timer down, queues a new
; status message from a script, and when the timer has run out shows the
; next waiting message for 45 frames. Clobbers every register.
msg_tick:
    ld hl, msg_timer
    ld a, (hl)
    or a
    jr z, +
    dec (hl)
+:  ld a, (status_new)          ; a status message from an event
    or a
    jr z, +
    xor a
    ld (status_new), a
    ld a, (status_msg)
    or a
    jr z, +                     ; (empty: nothing)
    ld hl, status_msg
    call say
+:  ld a, (msg_timer)
    or a
    ret nz                      ; the one shown still has time
    ld a, (msg_count)
    or a
    ret z
    ld a, (msg_head)            ; the next one
    ld l, a
    ld h, 0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl                  ; * 32
    ld de, msgq
    add hl, de
    call show_message           ; onto the top row
    ld hl, msg_head
    ld a, (hl)
    inc a
    and MSGQ-1
    ld (hl), a
    ld hl, msg_count
    dec (hl)
    ld a, 45                    ; frames before the next
    ld (msg_timer), a
    ret

; drops every waiting message (the one on screen stays); clobbers a
msg_clear:
    xor a
    ld (msg_count), a
    ld (msg_timer), a
    ret

;-----------------------------------------------------------------------
; the status bar. Row 0 is written straight into HUD_TEXT. Rows 1..5
; (screen rows 19..23, columns 7..31) are built in HUD_STAGE and only
; copied - and marked for the interrupt to redraw - when they changed,
; so the bar never shows a half-built row.
;-----------------------------------------------------------------------
; b = row, c = column -> hl = where that cell's text is; keeps bc;
; clobbers a, de. Row 0 is HUD_TEXT itself (32 columns); rows 1..5 are
; the staging area HUD_STAGE, 25 bytes a row holding columns 7..31.
hud_cell:
    ld a, b
    or a
    jr nz, +
    ld hl, HUD_TEXT             ; row 0
    ld a, c
    jp add_hl_a
+:  dec a
    ld h, a
    ld e, 25
    push bc
    call mul8                   ; (row - 1) * 25
    pop bc
    ld de, HUD_STAGE-7          ; (column 7 is the first byte of a staged row)
    add hl, de
    ld a, c
    jp add_hl_a

; b = row, c = column, hl = text: written (upper case) up to column 31
; -> c = the column after it; keeps b; clobbers a, de, hl
hud_row_text:
    push hl
    call hud_cell               ; hl = where to write
    pop de                      ; de = the text
-:  ld a, c
    cp 32
    ret nc                      ; past the last column
    ld a, (de)
    or a
    ret z
    cp 'a'
    jr c, +
    cp 'z'+1
    jr nc, +
    sub 32                      ; lower case -> upper case (the font has no lower case)
+:  ld (hl), a
    inc hl
    inc de
    inc c
    jr -

; b = row: cleared (row 0: all 32 columns, rows 1..5: columns 7..31)
; Clobbers a, bc, de, hl.
hud_row_clear:
    ld c, 0
    ld a, b
    or a
    jr z, +
    ld c, 7                     ; status rows: text from column 7 (columns 0..6 are not text)
+:  call hud_cell
    ld a, 32
    sub c
    ld b, a                     ; b = columns to clear
-:  ld (hl), ' '
    inc hl
    djnz -
    ret

; the staged rows that changed go to HUD_TEXT, marked for redrawing
; Compares each of rows 1..5 (25 bytes, columns 7..31) of HUD_STAGE with
; HUD_TEXT; a row that differs is copied and its bit (1 << row) set in
; ENG_HUD_DIRTY, so the frame interrupt redraws it. Clobbers every
; register except IX, IY.
hud_commit:
    ld b, 1                     ; b = row 1..5
-:  push bc
    ld h, b
    dec h
    ld e, 25
    call mul8                   ; (row - 1) * 25
    ld de, HUD_STAGE
    add hl, de                  ; staged row
    pop bc
    push bc
    push hl
    ld l, b
    ld h, 0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl                  ; row * 32
    ld de, HUD_TEXT+7
    add hl, de
    ex de, hl                   ; de = the shown row
    pop hl
    push hl
    push de
    ld b, 25
--: ld a, (de)                  ; compare the 25 bytes
    cp (hl)
    jr nz, +
    inc hl
    inc de
    djnz --
    pop de                      ; the same
    pop hl
    jr ++
+:  pop de
    pop hl
    ld bc, 25
    ldir                        ; copy the staged row over the shown one
    pop bc
    push bc
    ld a, b
    ld hl, bit_mask
    call add_hl_a_read          ; a = 1 << row
    ld hl, ENG_HUD_DIRTY
    or (hl)
    ld (hl), a                  ; mark the row for the interrupt
++: pop bc
    inc b
    ld a, b
    cp 6
    jr nz, -
    ret

; hl = a message: centred on the top row
; (A message longer than the row starts at its first column and is cut
; at column 31.) Marks row 0 dirty (ENG_HUD_DIRTY bit 0). Clobbers every
; register except IX, IY.
show_message:
    push hl
    ld b, 0
    call hud_row_clear          ; clear the top row
    pop hl
    push hl
    ld c, 0                     ; its length
-:  ld a, (hl)
    or a
    jr z, +
    inc hl
    inc c
    jr -
+:  ld a, c
.IFDEF GAMEGEAR
    ld c, 6                     ; (the Game Gear shows columns 6..25)
    cp 20
    jr nc, +
    ld b, a
    ld a, 20
    sub b
    srl a
    add a, 6                    ; column = 6 + (20 - length) / 2
    ld c, a
.ELSE
    ld c, 0
    cp 32
    jr nc, +
    ld b, a
    ld a, 32
    sub b
    srl a                       ; column = (32 - length) / 2
    ld c, a
.ENDIF
+:  pop hl
    ld b, 0
    call hud_row_text           ; write it
    ld hl, ENG_HUD_DIRTY
    set 0, (hl)                 ; the interrupt redraws row 0
    ret

; b = row, c = column, hl = n -> c = the column after it; keeps b
hud_put_num:
    push bc
    call utoa10                 ; hl = the digits
    pop bc
    jp hud_row_text

; what the status bar shows; rebuilding it is skipped when none of this
; changed and the bar still holds it (the game screen clears it)
; -> Z if it is the same
; The 17 values: hp, maxhp, armour, maxarmour, level, xp (2), nextxp (2),
; credits (2), weapon, its ammo, the 3 inventory counts, keys. They are
; compared with hud_snap and saved there. HUD_TEXT row 1 column 7 holding
; 'H' (of HEALTH / HP) shows that the bar has been drawn; otherwise it
; counts as changed. Clobbers every register except IX, IY.
hud_same:
    ld de, line1                ; (the 17 values, built in line1)
    ld a, (S_HP)
    ld (de), a
    inc de
    ld a, (S_MAXHP)
    ld (de), a
    inc de
    ld a, (S_ARMOR)
    ld (de), a
    inc de
    ld a, (S_MAXARMOR)
    ld (de), a
    inc de
    ld a, (S_LEVEL)
    ld (de), a
    inc de
    ld hl, S_XP                 ; xp, nextxp, credits (6 bytes)
    ld bc, 6
    ldir
    ld a, (S_WEAPON)
    ld (de), a
    inc de
    push de
    ld hl, wpn_ammo
    call add_hl_a_read          ; the weapon's ammo type
    ld hl, S_AMMO
    call add_hl_a_read          ; how much of it
    pop de
    ld (de), a
    inc de
    ld hl, S_INVENTORY
    ld bc, 3
    ldir
    ld a, (S_KEYS)
    ld (de), a
    ; compare with the last time, and keep it
    ld c, 0                     ; differences
    ld a, (HUD_TEXT+32+7)       ; row 1, column 7
    cp 'H'
    jr z, +
    inc c                       ; the bar was cleared: draw it
+:  ld hl, line1
    ld de, hud_snap
    ld b, 17
-:  ld a, (de)
    cp (hl)
    jr z, +
    inc c
+:  ld a, (hl)
    ld (de), a                  ; keep the new value
    inc hl
    inc de
    djnz -
    ld a, c
    or a
    ret

; rebuilds the status bar (rows 1..5) if anything on it changed, then
; commits the rows that differ (hud_commit). The layout differs between
; the Master System (5 rows of 25 columns) and the Game Gear (narrower).
; The ld bc, $RRCC values below are (row, column). Clobbers every
; register except IX, IY. Called by after_action.
hud_update:
    call hud_same
    ret z                       ; nothing changed
    ld b, 1
-:  push bc
    call hud_row_clear          ; clear staged rows 1..5
    pop bc
    inc b
    ld a, b
    cp 6
    jr nz, -
.IFDEF GAMEGEAR
    ; Game Gear: rows 1..3 show columns 7..23 (beside the face), row 4
    ; columns 7..26. HP n/n  AR n
    ; (Each label is written with hud_row_text, each number with
    ; hud_num_a / hud_put_num; "_here" continues where the last ended.)
    ld bc, $0107
    ld hl, str_hud_hp
    call hud_row_text
    ld a, (S_HP)
    ld bc, $010A
    call hud_num_a
    ld hl, str_slash
    call hud_row_text
    ld a, (S_MAXHP)
    call hud_num_a_here
    ld bc, $0112
    ld hl, str_hud_ar
    call hud_row_text
    ld a, (S_ARMOR)
    ld bc, $0115
    call hud_num_a
    ; the weapon, its ammo
    ld a, (S_WEAPON)
    add a, a
    ld hl, TX_WPN_0             ; (the weapons' names: the game's texts)
    call add_hl_a               ; hl = &the address of its name (a word)
    ld a, (hl)
    inc hl
    ld h, (hl)
    ld l, a                     ; hl = the weapon's name
    ld bc, $0207
    call hud_row_text
    ld a, (S_WEAPON)
    ld hl, wpn_use
    call add_hl_a_read          ; ammo used per shot
    or a
    jr z, +                     ; none (axe...): no ammo count
    ld a, (S_WEAPON)
    ld hl, wpn_ammo
    call add_hl_a_read          ; its ammo type
    ld hl, S_AMMO
    call add_hl_a_read          ; how much is left
    ld bc, $0215
    call hud_num_a
+:  ; LV n  XP n/n
    ld bc, $0307
    ld hl, str_hud_lv
    call hud_row_text
    ld a, (S_LEVEL)
    ld bc, $0309
    call hud_num_a
    ld bc, $030C
    ld hl, str_hud_xp
    call hud_row_text
    ld hl, (S_XP)
    ld bc, $030F
    call hud_put_num
    ld hl, str_slash
    call hud_row_text
    ld hl, (S_NEXTXP)
    call hud_put_num_here
    ; CR n  MK n+n  GYBR
    ld bc, $0407
    ld hl, str_hud_cr
    call hud_row_text
    ld hl, (S_CREDITS)
    ld bc, $040A
    call hud_put_num
    ld bc, $040F
    ld hl, str_hud_mk
    call hud_row_text
    ld a, (S_INVENTORY)
    ld bc, $0412
    call hud_num_a
    ld hl, str_plus
    call hud_row_text
    ld a, (S_INVENTORY+1)
    call hud_num_a_here
    ld a, (S_KEYS)              ; G Y B R at columns 23..26
    ld e, a                     ; (a letter for each key held: bits 0..3)
    ld bc, $0417
    ld d, 'G'
    bit 0, e
    call nz, hud_char
    inc c
    ld d, 'Y'
    bit 1, e
    call nz, hud_char
    inc c
    ld d, 'B'
    bit 2, e
    call nz, hud_char
    inc c
    ld d, 'R'
    bit 3, e
    call nz, hud_char
    jp hud_commit               ; show the rows that changed
.ELSE
    ; Master System:
    ; HEALTH n/n  AR n/n
    ; (Each label is written with hud_row_text, each number with
    ; hud_num_a / hud_put_num; "_here" continues where the last ended.)
    ld bc, $0107
    ld hl, str_hud_health
    call hud_row_text
    ld a, (S_HP)
    ld bc, $010E
    call hud_num_a
    ld hl, str_slash
    call hud_row_text
    ld a, (S_MAXHP)
    call hud_num_a_here
    ld bc, $0115
    ld hl, str_hud_ar
    call hud_row_text
    ld a, (S_ARMOR)
    ld bc, $0118
    call hud_num_a
    ld hl, str_slash
    call hud_row_text
    ld a, (S_MAXARMOR)
    call hud_num_a_here
    ; the weapon, its ammo
    ld a, (S_WEAPON)
    add a, a
    ld hl, TX_WPN_0             ; (the weapons' names: the game's texts)
    call add_hl_a               ; hl = &the address of its name (a word)
    ld a, (hl)
    inc hl
    ld h, (hl)
    ld l, a                     ; hl = the weapon's name
    ld bc, $0207
    call hud_row_text
    ld a, (S_WEAPON)
    ld hl, wpn_use
    call add_hl_a_read          ; ammo used per shot
    or a
    jr z, +                     ; none (axe...): no ammo count
    ld bc, $0215
    ld hl, str_hud_ammo
    call hud_row_text
    ld a, (S_WEAPON)
    ld hl, wpn_ammo
    call add_hl_a_read          ; its ammo type
    ld hl, S_AMMO
    call add_hl_a_read          ; how much is left
    ld bc, $021A
    call hud_num_a
+:  ; LEVEL n  XP n/n
    ld bc, $0307
    ld hl, str_hud_level
    call hud_row_text
    ld a, (S_LEVEL)
    ld bc, $030D
    call hud_num_a
    ld bc, $0311
    ld hl, str_hud_xp
    call hud_row_text
    ld hl, (S_XP)
    ld bc, $0314
    call hud_put_num
    ld hl, str_slash
    call hud_row_text
    ld hl, (S_NEXTXP)
    call hud_put_num_here
    ; CREDITS n  KEYS GYBR
    ld bc, $0407
    ld hl, (TX_CREDITS_WORD)
    call hud_row_text
    ld hl, (S_CREDITS)
    ld bc, $040F
    call hud_put_num
    ld bc, $0414
    ld hl, str_hud_keys
    call hud_row_text
    ld a, (S_KEYS)              ; G Y B R at columns 25..28
    ld e, a                     ; (a letter for each key held: bits 0..3)
    ld bc, $0419
    ld d, 'G'
    bit 0, e
    call nz, hud_char
    inc c
    ld d, 'Y'
    bit 1, e
    call nz, hud_char
    inc c
    ld d, 'B'
    bit 2, e
    call nz, hud_char
    inc c
    ld d, 'R'
    bit 3, e
    call nz, hud_char
    ; MEDKITS n+n  SOUL n
    ld bc, $0507
    ld hl, str_hud_medkits
    call hud_row_text
    ld a, (S_INVENTORY)
    ld bc, $050F
    call hud_num_a
    ld bc, $0511
    ld hl, str_plus
    call hud_row_text
    ld a, (S_INVENTORY+1)
    ld bc, $0512
    call hud_num_a
    ld a, (S_INVENTORY+2)       ; soul spheres: shown only when there are some
    or a
    jr z, +
    ld bc, $0515
    ld hl, str_hud_soul
    call hud_row_text
    ld a, (S_INVENTORY+2)
    ld bc, $051A
    call hud_num_a
+:  jp hud_commit               ; show the rows that changed
.ENDIF

; b = row, c = column, d = a letter: written there; keeps bc, de; clobbers a, hl
hud_char:
    push de
    call hud_cell
    pop de
    ld (hl), d
    ret

; a = n, b = row, c = column: written -> c = the column after
hud_num_a:
    ld l, a
    ld h, 0
    jp hud_put_num
; a = n written at (b, c): straight after what was written last (a "/"
; or a "+"; hud_row_text left c after it). Same as hud_num_a.
hud_num_a_here:
    ld l, a
    ld h, 0
; hl = n written at (b, c): the same as hud_put_num
hud_put_num_here:
    jp hud_put_num

;-----------------------------------------------------------------------
; the picture
;-----------------------------------------------------------------------
; the nearest thing straight ahead (1..4 cells) drawn as a sprite: a
; monster first, else the first thing on that cell
; Walks from the player's cell in the facing direction; a wall or a
; closed door ends the search. Skips F_HIDDEN entities. Out:
; ENG_ENT_DEF = the chosen entity's def and ENG_ENT_DIST = its distance,
; or $FF and 0 for nothing. Clobbers every register except IX, IY.
select_sprite:
    ld a, (S_PX)
    ld (ss_x), a
    ld a, (S_PZ)
    ld (ss_z), a
    ld a, 1
    ld (ss_d), a
_ss_cell:                       ; one cell further: ss_x/z += the facing's step
    ld a, (S_FACING)
    and 3
    ld c, a
    ld hl, dir_dx
    call add_hl_a_read          ; the facing's x step
    ld hl, ss_x
    add a, (hl)
    ld (hl), a
    ld b, a
    ld a, c
    ld hl, dir_dz
    call add_hl_a_read          ; the facing's z step
    ld hl, ss_z
    add a, (hl)
    ld (hl), a
    ld c, a
    call wall_at
    jr nz, _ss_none             ; a wall hides everything further
    call line_at
    cp $FF
    jr nz, _ss_none             ; so does a closed door
    ld a, $FF
    ld (ss_best), a
    ld e, 0
-:  call ent_at                 ; each linked entity on the cell
    cp $FF
    jr z, ++
    ld d, a
    ld hl, S_E_FLAGS
    call add_hl_a_read
    and F_HIDDEN
    jr nz, +                    ; hidden: skip
    ld a, d
    push bc
    push de
    call etype
    pop de
    pop bc
    cp ET_ENEMY
    jr nz, +++
    ld a, d                     ; a monster: this one
    ld (ss_best), a
    jr ++
+++:
    ld a, (ss_best)             ; another kind: only if nothing was found yet
    inc a
    jr nz, +
    ld a, d
    ld (ss_best), a
+:  ld a, d
    inc a
    ld e, a
    jr -
++: ld a, (ss_best)
    cp $FF
    jr z, +                     ; nothing here: look further
    ld hl, S_E_DEF
    call add_hl_a_read          ; its def picks the sprite
    ld (ENG_ENT_DEF), a
    ld a, (ss_d)
    ld (ENG_ENT_DIST), a
    ret
+:  ld hl, ss_d
    inc (hl)
    ld a, (hl)
    cp 5
    jp c, _ss_cell              ; up to 4 cells
_ss_none:
    ld a, $FF
    ld (ENG_ENT_DEF), a
    xor a
    ld (ENG_ENT_DIST), a
    ret

; a = 1 to force: draws the view again if anything in it changed (where
; you stand, where you look, or a door); otherwise only the sprite, if
; that changed
; Keeps cur_x/z/f, cur_doors, cur_def and cur_dist as what is on screen.
; Clobbers every register except IX, IY.
refresh_picture:
    push af
    call select_sprite          ; ENG_ENT_DEF / DIST for what is ahead now
    xor a
    ld (rp_doors), a
    ld c, 0
-:  ld a, c                     ; has any door opened or closed?
    ld hl, n_lines
    cp (hl)
    jr nc, ++
    ld hl, S_L_FLAGS
    call add_hl_a_read
    and L_OPEN
    ld b, a
    ld a, c
    ld hl, cur_doors
    call add_hl_a
    ld a, (hl)
    cp b
    jr z, +
    ld (hl), b                  ; remember it as drawn
    ld a, 1
    ld (rp_doors), a
+:  inc c
    jr -
++: pop af
    ld hl, rp_doors
    or (hl)
    jr nz, +                    ; forced, or a door changed: draw
    ld a, (S_PX)
    ld hl, cur_x
    cp (hl)
    jr nz, +
    ld a, (S_PZ)
    ld hl, cur_z
    cp (hl)
    jr nz, +
    ld a, (S_FACING)
    ld hl, cur_f
    cp (hl)
    jr nz, +
    ld a, (ENG_ENT_DEF)         ; only the sprite?
    ld hl, cur_def
    cp (hl)
    jr nz, ++
    ld a, (ENG_ENT_DIST)
    ld hl, cur_dist
    cp (hl)
    jr z, +++                   ; nothing changed
++: call svc_refresh_sprite     ; the engine redraws only the sprite
    jr +++
+:  ld a, (S_PX)
    ld (cur_x), a
    ld (ENG_VIEW_X), a
    ld a, (S_PZ)
    ld (cur_z), a
    ld (ENG_VIEW_Z), a
    ld a, (S_FACING)
    ld (cur_f), a
    ld (ENG_VIEW_F), a
    call svc_render             ; the engine draws the whole view (and the sprite)
+++:
    ld a, (ENG_ENT_DEF)
    ld (cur_def), a
    ld a, (ENG_ENT_DIST)
    ld (cur_dist), a
    ret

;-----------------------------------------------------------------------
; boxes over the view: dialogs, the notebook, door codes, the end screens
; The text goes into DLG_TEXT (row-major, DLG_W characters a row);
; svc_dialog_show puts the box on screen. The pager shows a list of
; level strings (box_str, box_n) a box at a time: rows 1..BOX_LINES for
; the text, row DLG_H-2 for "1:MORE" or "1:OK".
;-----------------------------------------------------------------------
; fills DLG_TEXT with spaces; clobbers bc, de, hl
box_clear:
    ld hl, DLG_TEXT
    ld de, DLG_TEXT+1
    ld bc, DLG_W*DLG_H-1
    ld (hl), ' '
    ldir                        ; (copies the space forward through the block)
    ret

; b = row, c = column, hl = text: written up to a '|' (new line) or the
; end of the row -> hl = where it stopped
; Keeps b; c ends after the last character written; clobbers a, de.
box_put:
    push hl
    ld h, b
    ld e, DLG_W
    push bc
    call mul8                   ; hl = row * DLG_W
    pop bc
    ld de, DLG_TEXT
    add hl, de
    ld a, c
    call add_hl_a
    ex de, hl                   ; de = where to write
    pop hl
-:  ld a, c
    cp DLG_W
    ret nc                      ; the end of the row
    ld a, (hl)
    or a
    ret z
    cp '|'
    ret z
    ld (de), a
    inc hl
    inc de
    inc c
    jr -

; a = level string, b = first line, c = first row: up to BOX_LINES lines
; of it -> a = 1 (NZ) if more lines follow
; Lines are separated by '|'; those before line b are skipped; each line
; goes at column BOX_COL of the next row; rows up to BOX_LINES are used.
; (A line longer than the box is cut by box_put.) a = 0 (Z) when the
; string ended. Clobbers every register except IX, IY.
box_string:
    push bc
    call level_string           ; hl = the string (in strbuf)
    pop bc
    ld a, b
    ld (bx_first), a
    ld a, c
    ld (bx_row), a
    xor a
    ld (bx_line), a
_bs_line:
    ld a, (hl)
    or a
    ret z                       ; the end (a = 0)
    ld a, (bx_first)
    ld b, a
    ld a, (bx_line)
    cp b
    jr c, _bs_skip              ; before the first line wanted
    ld a, (bx_row)
    cp 1 + BOX_LINES
    jr c, +
    ld a, 1                     ; no room: more follows
    or a
    ret
+:  ld b, a
    inc a
    ld (bx_row), a
    ld c, BOX_COL
    push hl
    call box_put                ; this line onto row b
    pop hl
_bs_skip:                       ; past this line's '|'
    ld a, (hl)
    or a
    jr z, +
    inc hl
    cp '|'
    jr nz, _bs_skip
+:  push hl
    ld hl, bx_line
    inc (hl)
    pop hl
    jr _bs_line

; hl = text: centred on the box's second-last row; clobbers every register
box_footer:
    push hl
    ld c, 0
-:  ld a, (hl)                  ; c = its length
    or a
    jr z, +
    inc hl
    inc c
    jr -
+:  ld a, DLG_W
    sub c
    srl a                       ; column = (DLG_W - length) / 2
    ld c, a
    ld b, DLG_H-2
    pop hl
    jp box_put

; the pager's current page: string box_str[box_i] from line box_line,
; with "1:MORE" if more follows (of this string or another) or "1:OK" on
; the last page; then shown. Clobbers every register except IX, IY.
pager_draw:
    call box_clear
    ld a, (box_i)
    ld hl, box_str
    call add_hl_a_read          ; a = the string
    ld hl, box_line
    ld b, (hl)
    ld c, 1
    call box_string             ; its lines from row 1
    jr nz, +                    ; more of this string
    ld a, (box_i)
    inc a
    ld hl, box_n
    cp (hl)
    ld hl, str_ok
    jr nc, ++                   ; the last string: OK
+:  ld hl, str_more
++: call box_footer
    jp svc_dialog_show          ; the engine shows the box

; a = UI_DIALOG / UI_NOTES, hl = string ids, b = how many
; (b > 0.) Copies the ids into box_str, plays the dialog sound and shows
; the first page. Clobbers every register except IX, IY.
open_pager:
    ld (ui), a
    ld a, b
    ld (box_n), a
    ld c, b
    ld b, 0
    ld de, box_str
    ldir                        ; box_str = the ids
    xor a
    ld (box_i), a
    ld (box_line), a
    ld a, SFX_DIALOG
    call sfx
    jr pager_draw

; -> a = 1 (NZ) when the pager is finished
; Otherwise turns to the next page (the rest of this string, or the next
; string), plays a blip and shows it; a = 0 (Z). Clobbers every register
; except IX, IY.
pager_next:
    call box_clear
    ld a, (box_i)
    ld hl, box_str
    call add_hl_a_read
    ld hl, box_line
    ld b, (hl)
    ld c, 1
    call box_string             ; (only to learn whether more lines follow)
    jr z, +
    ld hl, box_line             ; more of this string
    ld a, (hl)
    add a, BOX_LINES
    ld (hl), a
    jr ++
+:  xor a                       ; the next string
    ld (box_line), a
    ld hl, box_i
    inc (hl)
    ld a, (hl)
    ld hl, box_n
    cp (hl)
    jr c, ++
    ld a, 1                     ; past the last: finished
    or a
    ret
++: ld a, SFX_BLIP
    call sfx
    call pager_draw
    xor a
    ret

; the door-code box: the prompt string (pw_prompt) from row 1, then on
; row 10 "> " and four places: the digits entered, the digit being
; chosen, '_' for the rest; the help lines on rows 12 and 13. Clobbers
; every register except IX, IY.
password_draw:
    call box_clear
    ld a, (pw_prompt)
    ld bc, $0001                ; from line 0, at row 1
    call box_string
    ; the digits so far, the one being chosen, then '_'
    ld hl, line1
    ld c, 0                     ; c = place 0..3
-:  ld a, c
    ld de, code_len
    ex de, hl
    cp (hl)                     ; place vs code_len
    ex de, hl
    jr nc, +
    push hl
    ld hl, code
    call add_hl_a_read          ; an entered digit
    pop hl
    jr +++
+:  jr nz, ++
    ld a, (code_digit)          ; place = code_len: the digit being chosen
    add a, '0'
    jr +++
++: ld a, '_'
+++:
    ld (hl), a
    inc hl
    inc c
    ld a, c
    cp 4
    jr nz, -
    ld (hl), 0
    ld bc, $0A00 + (DLG_W - 8) / 2
    ld hl, str_prompt
    call box_put                ; "> " on row 10
    ld bc, $0A02 + (DLG_W - 8) / 2
    ld hl, line1
    call box_put                ; the digits after it
    ld bc, $0C00 + BOX_HELP_COL
    ld hl, str_pw_help1
    call box_put
    ld bc, $0D00 + BOX_HELP_COL
    ld hl, str_pw_help2
    call box_put
    jp svc_dialog_show          ; the engine shows the box

; starts door-code entry (UI_PASSWORD) with nothing entered
open_password:
    xor a
    ld (code_len), a
    ld (code_digit), a
    ld a, UI_PASSWORD
    ld (ui), a
    jr password_draw

; hl = title, de = line 1, bc = line 2: a box with "1:PLAY AGAIN"
; (title on row 5, the lines on rows 8 and 9.) Clobbers every register
; except IX, IY.
screen_box:
    push bc
    push de
    push hl
    call box_clear
    pop hl
    ld bc, $0500 + (DLG_W - 16) / 2
    call box_put                ; the title
    pop hl
    ld bc, $0800 + BOX_HELP_COL
    call box_put                ; line 1
    pop hl
    ld bc, $0900 + BOX_HELP_COL
    call box_put                ; line 2
    ld hl, str_play_again
    call box_footer
    jp svc_dialog_show          ; the engine shows the box

; hl = where, de = a string: copied (without its 0) -> hl after it
; (de ends at the 0); clobbers a
str_cat:
    ld a, (de)
    or a
    ret z
    ld (hl), a
    inc hl
    inc de
    jr str_cat

; hl = where, de = a number: its digits -> hl after them; clobbers a, bc, de
num_cat:
    push hl
    ex de, hl
    call utoa10                 ; hl = the digits
    ex de, hl
    pop hl
    jr str_cat

; the end of a game: "YOU HAVE DIED" (UI_DEAD) if S_DEAD, else, if the
; sector is complete, "<title> COMPLETE" with the kills, the level and
; the credits (UI_DONE). Clobbers every register except IX, IY.
end_screens:
    ld a, (S_DEAD)
    or a
    jr z, +
    ld a, UI_DEAD
    ld (ui), a
    ld hl, str_died_title
    ld de, str_died_line
    ld bc, str_empty
    jr screen_box
+:  ld a, (S_LEVEL_DONE)
    or a
    ret z
    ld a, UI_DONE
    ld (ui), a
    ; KILLS k/t: the map's monsters, and how many are dead
    ld bc, 0                    ; b = killed, c = total
    ld e, 0
-:  ld a, e
    ld hl, n_map_ents
    cp (hl)
    jr nc, ++
    push bc
    push de
    call etype
    pop de
    pop bc
    cp ET_ENEMY
    jr nz, +
    inc c                       ; a monster
    ld a, e
    ld hl, S_E_FLAGS
    call add_hl_a_read
    and F_ALIVE
    jr nz, +
    inc b                       ; a dead one
+:  inc e
    jr -
++: push bc                     ; line1 = "KILLS k/t"
    ld hl, line1
    ld de, str_kills
    call str_cat
    pop bc
    push bc
    ld e, b
    ld d, 0
    call num_cat
    ld (hl), '/'
    inc hl
    pop bc
    ld e, c
    ld d, 0
    call num_cat
    ld (hl), 0
    ld hl, line2                ; line2 = "LEVEL n  CREDITS n"
    ld de, str_level_
    call str_cat
    ld a, (S_LEVEL)
    ld e, a
    ld d, 0
    call num_cat
    ld de, (TX_CREDITS_MID)
    call str_cat
    ld de, (S_CREDITS)
    call num_cat
    ld (hl), 0
    ld hl, strbuf               ; "<the map's title> COMPLETE"
    push hl
    ld hl, (lv_tab + 2*LT_TITLE)
    ex de, hl
    pop hl
    call str_cat
    ld de, str_complete
    call str_cat
    ld (hl), 0
    ld hl, strbuf
    ld de, line1
    ld bc, line2
    call screen_box
    ld a, SFX_COMPLETE
    jp sfx

; after anything happened: the picture, the status bar, then any box the
; rules opened
; In order: a red flash if the player was hurt (6 frames), the view and
; sprite (refresh_picture), the status bar, then the end screens, a
; dialog (dialog_str) or a door code (pw_want); otherwise back to
; UI_PLAY. Clobbers every register except IX, IY.
after_action:
    ld a, (player_hurt)
    or a
    jr z, +
    xor a
    ld (player_hurt), a
    ld a, 6
    ld (ENG_FLASH), a           ; the engine tints the view red for 6 frames
+:  xor a
    call refresh_picture        ; not forced: only what changed
    call hud_update
    ld a, (S_DEAD)
    ld hl, S_LEVEL_DONE
    or (hl)
    jp nz, end_screens
    ld a, (dialog_str)
    cp $FF
    jr z, +
    ld hl, dialog_str           ; a pager of one string: dialog_str itself
    ld b, 1
    ld a, UI_DIALOG
    jp open_pager
+:  ld a, (pw_want)
    cp $FF
    jp nz, open_password
    xor a
    ld (ui), a                  ; UI_PLAY
    ret

; closes any box: back to UI_PLAY, and the view drawn again (forced, as
; the box covered it). Clobbers every register except IX, IY.
close_box:
    xor a
    ld (ui), a
    inc a
    jp refresh_picture

; a fresh game: the rules' state (game_init), the play screen, the level's
; title as the first message, then the first picture. Clobbers every
; register except IX, IY.
new_game:
    call game_init              ; the level, the player, the monsters (actions.asm)
    call msg_clear
    xor a
    ld (ui), a
    ld a, $FF
    ld (cur_x), a               ; nothing drawn yet: the view must be drawn
    ld (cur_def), a
    ld hl, (lv_tab + 2*LT_TITLE) ; the map's title (the level data)
    call say
    ld a, SFX_SELECT
    call sfx
    call svc_game_screen        ; the engine sets up the border and the status bar
    jp after_action

;-----------------------------------------------------------------------
; the pause menu: resume, notebook, view size, how the view is drawn,
; detail, walls
;-----------------------------------------------------------------------
; the box row of each menu item (pause_cur 0..5): resume, notebook, view
; size, draw view, detail, walls
pause_rows:
    .db 3, 4, 6, 7, 8, 9

; draws the pause menu: the labels, each setting's current value (read
; from the engine variables), the credits, the '>' cursor on row
; pause_rows[pause_cur], and the footer; then shows it. Each box_put
; writes one label or value. Clobbers every register except IX, IY.
pause_draw:
    call box_clear
    ld bc, $0100 + (DLG_W - 6) / 2
    ld hl, str_paused
    call box_put
    ld bc, $0300 + PAUSE_LABEL_COL
    ld hl, str_resume
    call box_put
    ld bc, $0400 + PAUSE_LABEL_COL
    ld hl, (TX_NOTEBOOK_ITEM)
    call box_put
    ld bc, $0600 + PAUSE_LABEL_COL
    ld hl, str_view_size
    call box_put
    ld a, (ENG_VSIZE)
    or a
    ld hl, str_large
    jr z, +
    ld hl, str_small
+:  ld bc, $0600 + PAUSE_VALUE_COL
    call box_put
    ld bc, $0700 + PAUSE_LABEL_COL
    ld hl, str_draw_view
    call box_put
    ld a, (ENG_VMODE)
    or a
    ld hl, str_outward
    jr z, +
    ld hl, str_at_once
+:  ld bc, $0700 + PAUSE_VALUE_COL
    call box_put
    ld bc, $0800 + PAUSE_LABEL_COL
    ld hl, str_detail
    call box_put
    ld a, (ENG_DETAIL)
    or a
    ld hl, str_high
    jr z, +
    ld hl, str_low
+:  ld bc, $0800 + PAUSE_VALUE_COL
    call box_put
    ld bc, $0900 + PAUSE_LABEL_COL
    ld hl, str_walls
    call box_put
    ld a, (ENG_SOLID)
    or a
    ld hl, str_textured
    jr z, +
    ld hl, str_solid
+:  ld bc, $0900 + PAUSE_VALUE_COL
    call box_put
    ld bc, $0B00 + (DLG_W - 10) / 2     ; the credits
    ld hl, str_credit1
    call box_put
    ld bc, $0C00 + (DLG_W - 11) / 2
    ld hl, str_credit2
    call box_put
    ld bc, $0D00 + (DLG_W - 11) / 2
    ld hl, str_credit3
    call box_put
    ld a, (pause_cur)
    ld hl, pause_rows
    call add_hl_a_read          ; the cursor's row
    ld b, a
    ld c, PAUSE_LABEL_COL - 2
    ld hl, str_cursor
    call box_put
    ld hl, str_pause_footer
    call box_footer
    jp svc_dialog_show          ; the engine shows the box

; a = the new input (edge bits)
; Up / Down move the cursor (wrapping round the 6 items). On Resume or
; Notebook only button 1 acts: Resume closes the menu, Notebook opens the
; notes (or says there are none). On a setting, button 1, Left or Right
; flips it (the engine uses the new value for the next picture). Button 2
; closes the menu. Clobbers every register except IX, IY.
pause_input:
    ld e, a
    ld hl, pause_cur
    bit 0, e                    ; up
    jr z, +
    ld a, (hl)
    or a
    ld a, 5                     ; from the top to the bottom
    jr z, ++
    ld a, (hl)
    dec a
++: ld (hl), a
    jp _pi_changed
+:  bit 1, e                    ; down
    jr z, +
    ld a, (hl)
    cp 5
    ld a, 0                     ; from the bottom to the top
    jr nc, ++
    ld a, (hl)
    inc a
++: ld (hl), a
    jp _pi_changed
+:  ld a, e
    and PAD_B1 | PAD_LEFT | PAD_RIGHT
    jr z, _pi_b2
    ld a, (hl)
    cp 2
    jr nc, _pi_setting          ; items 2..5 are settings
    bit 4, e                    ; resume / notebook: button 1 only
    ret z
    or a
    jp z, close_box             ; 0: resume
    ld a, (S_N_NOTES)           ; 1: the notebook
    or a
    jr z, +
    ld b, a
    ld hl, S_NOTES
    ld a, UI_NOTES
    jp open_pager               ; a pager of the notes' strings
+:  call close_box
    ld hl, str_no_notes
    jp say
_pi_setting:
    cp 2
    jr nz, +
    ld hl, ENG_VSIZE            ; LARGE (0) <-> SMALL (1)
    ld a, (hl)
    xor 1
    ld (hl), a
    jr _pi_changed
+:  cp 3
    jr nz, ++
    ld hl, ENG_VMODE            ; OUTWARD (0) <-> AT ONCE (1)
    ld a, (hl)
    xor 1
    and 1
    ld (hl), a
    jr _pi_changed
++: cp 4
    jr nz, +
    ld hl, ENG_DETAIL           ; HIGH (0) <-> LOW (1)
    ld a, (hl)
    xor 1
    ld (hl), a
    jr _pi_changed
+:  ld hl, ENG_SOLID            ; TEXTURED (0) <-> SOLID (1)
    ld a, (hl)
    xor 1
    ld (hl), a
    jr _pi_changed
_pi_b2:
    bit 5, e                    ; button 2: resume
    ret z
    jp close_box
_pi_changed:                    ; something changed: blip and redraw the menu
    ld a, SFX_BLIP
    call sfx
    jp pause_draw

;-----------------------------------------------------------------------
; input
;-----------------------------------------------------------------------
; ui_edge = buttons just pressed, ui_pad = buttons held
; Called by game_main for each frame with a new press. Clears waiting
; messages, then by `ui`:
;   UI_DEAD, UI_DONE  button 1 starts a fresh game
;   UI_PAUSE          pause_input
;   UI_NOTES          button 1: next page (closes after the last);
;                     button 2: close
;   UI_DIALOG         button 1: next page; after the last, close and
;                     resume the event that opened it
;   UI_PASSWORD       password_input
;   UI_PLAY           play_input; then the picture of the player's own
;                     move, the monsters' turn (settle) and after_action
; Clobbers every register except IX, IY.
handle:
    call msg_clear
    ld a, (ui_edge)
    ld e, a
    ld a, (ui)
    cp UI_DEAD
    jr z, +
    cp UI_DONE
    jr nz, ++
+:  bit 4, e                    ; button 1
    ret z
    jp new_game
++: cp UI_PAUSE
    jr nz, +
    ld a, e
    jp pause_input
+:  cp UI_NOTES
    jr nz, ++
    bit 4, e                    ; button 1: the next page
    jr z, +
    call pager_next
    call nz, close_box          ; past the last page: close
+:  ld a, (ui_edge)
    bit 5, a                    ; button 2: close
    ret z
    jp close_box
++: cp UI_DIALOG
    jr nz, ++
    bit 4, e                    ; button 1 only
    ret z
    call pager_next
    ret z                       ; more pages
    call close_box
    ld hl, 0
    call resume                 ; the event goes on after the dialog
    jp after_action
++: cp UI_PASSWORD
    jp z, password_input
    ; UI_PLAY: the view is drawn as soon as the player's own move is done,
    ; and the monsters take their turn after that
    ld a, 1
    ld (hold_monsters), a       ; settle() waits
    call play_input
    push af
    xor a
    ld (hold_monsters), a
    pop af
    ret z                       ; no action
    ld a, (monsters_pending)
    or a
    jr z, +
    call modal
    jr nz, +                    ; a box is waiting: the monsters wait too
    xor a
    call refresh_picture        ; the view after the player's move
    call settle                 ; then the monsters' turn
+:  jp after_action

; -> a = 1 (NZ) if an action was taken
; Reads ui_edge (and ui_pad for button 2 held). Opening the notebook or
; "NO NOTES YET" counts as no action. Clobbers every register except
; IX, IY.
play_input:
    ld a, (ui_pad)
    bit 5, a                    ; button 2 held?
    ld a, (ui_edge)
    jr z, ++
    ; button 2 held
    bit 2, a                    ; left: the previous weapon
    jr z, +
    ld a, -1
    call act_cycle_weapon
    jr _pl_yes
+:  bit 3, a                    ; right: the next weapon
    jr z, +
    ld a, 1
    call act_cycle_weapon
    jr _pl_yes
+:  bit 0, a                    ; up: a medkit
    jr z, +
    call act_health_item
    jr _pl_yes
+:  bit 1, a                    ; down: pass the turn
    jr z, +
    call act_pass
    jr _pl_yes
+:  bit 4, a                    ; button 1: the notebook
    jr z, _pl_no
    ld a, (S_N_NOTES)           ; the notebook
    or a
    jr z, +
    ld b, a
    ld hl, S_NOTES
    ld a, UI_NOTES
    call open_pager
    jr _pl_no
+:  ld hl, str_no_notes
    call say
    jr _pl_no
++: bit 0, a                    ; up: step forward
    jr z, +
    xor a
    call act_move
    jr _pl_yes
+:  bit 1, a                    ; down: step back
    jr z, +
    ld a, 1
    call act_move
    jr _pl_yes
+:  bit 2, a                    ; left: turn left
    jr z, +
    ld a, -1
    call act_turn
    jr _pl_yes
+:  bit 3, a                    ; right: turn right
    jr z, +
    ld a, 1
    call act_turn
    jr _pl_yes
+:  bit 4, a                    ; button 1: use / attack
    jr z, _pl_no
    call act_use
_pl_yes:
    ld a, 1
    or a
    ret
_pl_no:
    xor a
    ret

; door-code entry: Up/Down change the digit, Right adds it, Left takes
; one back, button 1 enters, button 2 cancels
; e = the new presses. At most 3 digits are added with Right; button 1
; enters them plus the digit being chosen (so a code is 1..4 digits),
; then resumes the event with the code; button 2 resumes it with 0
; (cancelled). Otherwise the box is redrawn. Clobbers every register
; except IX, IY.
password_input:
    bit 0, e                    ; up: digit + 1 (9 -> 0)
    jr z, +
    ld a, (code_digit)
    inc a
    cp 10
    jr c, ++
    xor a
++: ld (code_digit), a
+:  bit 1, e                    ; down: digit - 1 (0 -> 9)
    jr z, +
    ld a, (code_digit)
    add a, 9
    cp 10
    jr c, ++
    sub 10
++: ld (code_digit), a
+:  bit 3, e                    ; right: add the digit (up to 3)
    jr z, +
    ld a, (code_len)
    cp 3
    jr nc, +
    ld hl, code
    call add_hl_a               ; hl = &code[code_len]
    ld a, (code_digit)
    add a, '0'
    ld (hl), a
    ld hl, code_len
    inc (hl)
    xor a
    ld (code_digit), a
+:  bit 2, e                    ; left: take the last digit back to edit
    jr z, +
    ld a, (code_len)
    or a
    jr z, +
    dec a
    ld (code_len), a
    ld hl, code
    call add_hl_a_read
    sub '0'
    ld (code_digit), a
+:  ld a, e
    and PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT
    jr z, +
    push de
    ld a, SFX_BLIP
    call sfx
    pop de
+:  bit 4, e
    jr z, +
    ld a, (code_len)            ; the code: the digits and the one shown
    ld hl, code
    call add_hl_a
    ld a, (code_digit)
    add a, '0'
    ld (hl), a
    inc hl
    ld (hl), 0
    call close_box
    ld hl, code
    call resume                 ; checks it and carries on (events.asm)
    jp after_action
+:  bit 5, e
    jp z, password_draw         ; still entering: redraw
    call close_box
    ld hl, 0
    call resume                 ; cancelled
    jp after_action

;-----------------------------------------------------------------------
; the game: title, a fresh game, then one input at a time
;-----------------------------------------------------------------------
; The entry point (jumped to from $1000, C_ENTRY in rom/main.asm); never
; returns. Each frame: ENG_IDLE = 1 and ENG_MODE = ui (for the tests),
; wait for the frame, tick the messages, read the pad. A pause press
; (ENG_PAUSE, set by the NMI) opens the menu in play or closes it. Any
; other new press (ui_edge) goes to `handle`, with ENG_IDLE = 0
; meanwhile. ENG_TURN_SEQ counts handled inputs (for the tests).
game_main:
    ld sp, $DFF0                ; the stack (below $DFF0, as the engine's init)
    xor a
    ld (ENG_TURN_SEQ), a
    ld (pad_prev), a
    call svc_title              ; the title screen
    call new_game
_gm_loop:
    ld a, 1
    ld (ENG_IDLE), a
    ld a, (ui)
    ld (ENG_MODE), a
    call svc_wait_frame         ; wait for the next frame
    call msg_tick
    call svc_read_pad           ; l = the buttons held
    ld a, l
    ld (ui_pad), a
    ld hl, pad_prev
    ld b, (hl)
    ld (hl), a
    ld c, a
    ld a, b
    cpl
    and c                       ; held now and not last frame
    ld (ui_edge), a
    ld a, (ENG_PAUSE)           ; the pause button
    or a
    jr z, +
    xor a
    ld (ENG_PAUSE), a
    ld (ENG_IDLE), a
    call sfx_flush
    call msg_clear
    ld a, (ui)
    or a
    jr nz, ++                   ; not in play
    ld a, UI_PAUSE              ; in play: open the menu
    ld (ui), a
    xor a
    ld (pause_cur), a
    ld a, SFX_DIALOG
    call sfx
    call pause_draw
    jr +++
++: cp UI_PAUSE
    call z, close_box           ; in the menu: close it (ignored in other boxes)
+++:
    ld hl, ENG_TURN_SEQ
    inc (hl)
    jr _gm_loop
+:  ld a, (ui_edge)
    and $3F                     ; any of the six buttons newly pressed?
    jr z, _gm_loop
    xor a
    ld (ENG_IDLE), a
    call sfx_flush              ; drop the last turn's waiting sounds
    call handle
    ld hl, ENG_TURN_SEQ
    inc (hl)
    jr _gm_loop

;-----------------------------------------------------------------------
; the UI's fixed text (str_key_letters is not used)
str_hud_health:     .db "HEALTH", 0
str_hud_ar:         .db "AR", 0
str_hud_ammo:       .db "AMMO", 0
str_hud_level:      .db "LEVEL", 0
str_hud_xp:         .db "XP", 0
str_hud_keys:       .db "KEYS", 0
str_hud_medkits:    .db "MEDKITS", 0
str_hud_soul:       .db "SOUL", 0
.IFDEF GAMEGEAR
str_hud_hp:         .db "HP", 0
str_hud_lv:         .db "LV", 0
str_hud_cr:         .db "CR", 0
str_hud_mk:         .db "MK", 0
.ENDIF
str_slash:          .db "/", 0
str_plus:           .db "+", 0
str_key_letters:    .db "GYBR"
str_more:           .db "1:MORE", 0
str_ok:             .db "1:OK", 0
str_prompt:         .db "> ", 0
.IFDEF GAMEGEAR
str_pw_help1:       .db "UP/DN DIGIT  RT ADD", 0
str_pw_help2:       .db "LT DEL 1 OK 2 CANCEL", 0
.ELSE
str_pw_help1:       .db "UP/DOWN DIGIT  RIGHT ADD", 0
str_pw_help2:       .db "LEFT DEL 1:OK 2:CANCEL", 0
.ENDIF
str_play_again:     .db "1:PLAY AGAIN", 0
str_died_title:     .db "  YOU HAVE DIED", 0
str_died_line:      .db "THE DEMONS GOT YOU.", 0
str_complete:       .db " COMPLETE", 0
str_kills:          .db "KILLS ", 0
str_level_:         .db "LEVEL ", 0
str_no_notes:       .db "NO NOTES YET", 0
str_paused:         .db "PAUSED", 0
str_credit1:        .db "MADE BY AI", 0
str_credit2:        .db "DIRECTED BY", 0
str_credit3:        .db "UNFNKNBLVBL", 0
str_resume:         .db "RESUME", 0
str_view_size:      .db "VIEW SIZE", 0
str_large:          .db "LARGE", 0
str_small:          .db "SMALL", 0
str_draw_view:      .db "DRAW VIEW", 0
str_outward:        .db "OUTWARD", 0
str_at_once:        .db "AT ONCE", 0
str_detail:         .db "DETAIL", 0
str_high:           .db "HIGH", 0
str_low:            .db "LOW", 0
str_walls:          .db "WALLS", 0
str_textured:       .db "TEXTURED", 0
str_solid:          .db "SOLID", 0
str_cursor:         .db ">", 0
str_pause_footer:   .db "1:CHANGE  2:RESUME", 0
