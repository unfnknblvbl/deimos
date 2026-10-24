; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

; a test of the GSU: fills the frame buffer ($70:0000, 12 KB) with a pattern
;
; Built by snes/build_fx_test.sh (wla-superfx, linked to a flat binary,
; out/gsu_test.bin, put in the cartridge by tools/snes_rom.py) for the
; SNES's GSU bring-up test (snes/src/test_fx.c): the main CPU starts the
; GSU at main (gsu_cpu.s gsu_run) and waits for STOP, then shows the
; frame buffer.
; Each byte at address a (0..12287) gets (a >> 4) & 255, so the picture
; shows bands 16 bytes wide: an easy pattern to recognise.
;
; Registers: R1 the address in the GSU's RAM bank (set by the CPU through
; RAMBR, $70 here), R12 the loop count, R13 the loop's address (the GSU's
; LOOP instruction uses both), R0 the byte to store. Clobbers R0, R1,
; R12, R13. Ends with STOP (the GSU halts; the CPU sees SFR's GO bit clear).
;
; The program is assembled for address 0 of a single 64 KB bank.
.MEMORYMAP
    SLOTSIZE $10000
    DEFAULTSLOT 0
    SLOT 0 START $0000
.ENDME
.ROMBANKSIZE $10000
.ROMBANKS 1
.BANK 0 SLOT 0
.ORG 0

main:
    IWT R1, #0                  ; the first byte of the frame buffer
    IWT R12, #12288             ; 12288 bytes: the 128 x 96 picture
    IWT R13, #loop              ; where LOOP jumps back to
loop:
    MOVE R0, R1                 ; R0 = the address
    LSR                         ; four shifts right: R0 = address / 16
    LSR
    LSR
    LSR
    STB (R1)                    ; store R0's low byte at R1 (in the RAM bank)
    LOOP                        ; R12 - 1; back to R13 while not zero
    INC R1                      ; (runs after LOOP either way: the GSU's delay slot)
    STOP                        ; done: the GSU halts
    NOP                         ; (the instruction fetched after STOP)
