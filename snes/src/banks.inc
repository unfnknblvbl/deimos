; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

; Doom RPG for the SNES: where things are in the ROM, per build (ca65).
; The plain build is HiROM (code bank $C0, FastROM); the SuperFX build
; (SUPERFX defined) uses the GSU's map (code bank $40, the ROM also at
; $00-$3F:8000).
; Included by the assembler files that need a bank or address (vdata.s,
; save.s, gfx.s, video.s, sound.s, crt0.s, ...); the SuperFX build
; scripts pass -D SUPERFX. banks.h holds the C side (HEADER_AT).
; The cartridge header has two bytes the build fills in
; (tools/snes_rom.py): HEADER_AT + 6, the campaign block's first bank, and
; HEADER_AT + 7, the resources' bank.
.ifdef SUPERFX
CODE_BANK  = $40                ; the bank the code runs in (64 KB mirror of the ROM's start)
DATA_ROM   = $41                ; C's constants and data, copied to RAM
TABLES1    = $430000            ; the tables' second bank (CONV, the SPC engine)
HEADER_AT  = $407FB0            ; the cartridge header (as the CPU reads it)
SRAM_AT    = $70E000            ; saves: the last 8 KB of the GSU's (battery-backed) RAM
FB_BANK    = $70                ; the view's picture: the GSU draws it in its RAM
FB_ADDR    = $0200              ; (snes/gsu/view.s's FB)
.else
CODE_BANK  = $C0                ; HiROM bank $C0: the code (FastROM speed)
DATA_ROM   = $C1                ; C's constants and data, copied to RAM at start-up
TABLES1    = $C30000            ; the tables' second bank (sprite scaler entries, CONV, the SPC engine)
HEADER_AT  = $C0FFB0            ; the cartridge header
SRAM_AT    = $306000            ; the battery RAM (8 KB, HiROM's $30:6000-$7FFF window)
FB_BANK    = $7F                ; the view's picture: the CPU draws it in work RAM bank $7F
FB_ADDR    = $0000
.endif
