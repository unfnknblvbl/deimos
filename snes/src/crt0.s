; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

; Doom RPG for the SNES: start-up, the interrupt handlers, the cartridge
; header. ca65 (65816).
;
; The code runs in bank $C0 (FastROM) with the data bank register at $7E:
; cc65's C code reads and writes everything through the data bank, so its
; data, BSS, C stack (and its constants, copied there from bank $C1 at
; start-up) are in the first 64 KB of work RAM. The direct page (cc65's
; zero page) and the hardware stack are in bank $00 ($0000-$1FFF, the same
; RAM). Hardware registers are only reached from assembler (long
; addresses, or the data bank set to $00 for a moment).
;
; C runs with 8-bit A, X and Y (cc65 generates 6502 code); assembler
; routines called from C must return that way.
; Licence: GPL-2.0-or-later.
;
; In the SuperFX build (SUPERFX defined) the code bank is $40 instead and
; the ROM runs at slow speed; the vectors point at small stubs in bank $00
; (segment STUBS) that jump into bank $40.
;
; Flow: the reset vector -> reset -> run (clears the hardware, copies C's
; data into RAM, zeroes the BSS, sets up cc65's C stack) -> _main (ui.c,
; or a test_*.c). Every NMI (the vertical blank, once enabled by the video
; code) -> nmi -> nmi_far -> nmi_body (hw.s).
;
; Linker symbols used (snes.cfg / snes_fx.cfg): __RODATA_LOAD__ and
; __DATA_LOAD__ (where C's constants and data are in the ROM bank
; DATA_ROM), __RODATA_RUN__, __DATA_RUN__ (where they go in bank $7E),
; their sizes, the BSS's place and size, and the C stack's area.

.p816
.smart -
.include "banks.inc"

.import _main
.import __RODATA_LOAD__, __RODATA_RUN__, __RODATA_SIZE__
.import __DATA_LOAD__, __DATA_RUN__, __DATA_SIZE__
.import __BSS_RUN__, __BSS_SIZE__
.import __CSTACK_START__, __CSTACK_SIZE__
.import nmi_body
.importzp c_sp
.export __STARTUP__ : absolute = 1
.export reset

.segment "STARTUP"

; ---- reset: native mode, FastROM, into bank $C0
; Entered from the reset vector in bank $00 (emulation mode, after power-on
; or reset). Never returns.
reset:
    sei                         ; no interrupts
    clc
    xce                         ; native mode
    rep #$38                    ; 16-bit A, X, Y; binary mode
    .a16
    .i16
    ldx #$1FFF
    txs                         ; the hardware stack: top of low RAM, growing down
    lda #$0000
    tcd                         ; direct page = $0000
    sep #$20
    .a8
.ifndef SUPERFX
    lda #$01
    sta $420D                   ; FastROM (MEMSEL bit 0: banks $80-$FF at 3.58 MHz)
.endif
    jml f:CODE_BANK * $10000 + run  ; from the bank $00 mirror into the code bank

run:
    lda #$80
    pha
    plb                         ; data bank = $80 (the registers are in banks $00-$3F, $80-$BF)
    lda #$80
    sta $002100                 ; forced blank
    stz $004200                 ; no NMI, no auto-joypad yet
    jsr clear_registers         ; every register to a known state
    ; ---- C's constants and data into RAM (MVN: X = source, Y = destination, A = count - 1)
    rep #$30
    .a16
    .i16
    lda #__RODATA_SIZE__
    beq :+                      ; (none: skip)
    dec
    ldx #.loword(__RODATA_LOAD__)
    ldy #__RODATA_RUN__
    mvn #DATA_ROM, #$7E
:   lda #__DATA_SIZE__
    beq :+
    dec
    ldx #.loword(__DATA_LOAD__)
    ldy #__DATA_RUN__
    mvn #DATA_ROM, #$7E
:   ; ---- the BSS: zeros (one zero byte, then MVN copies it along)
    lda #__BSS_SIZE__
    cmp #2
    bcc :+                      ; under 2 bytes: nothing to do (a 1-byte BSS would stay as it is)
    sep #$20
    .a8
    lda #0
    sta f:$7E0000 + __BSS_RUN__
    rep #$20
    .a16
    lda #.loword(__BSS_SIZE__ - 2)  ; size - 1 bytes to copy, minus one for MVN
    ldx #__BSS_RUN__
    ldy #__BSS_RUN__ + 1        ; each byte copied onto the next: the zero spreads
    mvn #$7E, #$7E
:   ; the data bank: $7E from here on
    sep #$20
    .a8
    lda #$7E
    pha
    plb
    ; the C stack (cc65's sp, growing down)
    rep #$20
    .a16
    lda #__CSTACK_START__ + __CSTACK_SIZE__
    sta c_sp
    sep #$30                    ; 8-bit A, X, Y: what cc65's code expects
    .a8
    .i8
    jsr _main                   ; the game (ui.c) or a test program; it never returns
@hang:
    bra @hang

; every PPU / CPU register to its power-on default (as the SNES dev manual
; suggests); the data bank is $80 (registers reached through it)
; In: data bank $80. Out: the registers set. Clobbers A, X (widths restored).
; Called once, by run.
clear_registers:
    php
    sep #$30
    .a8
    .i8
    lda #$80
    sta $2100                   ; INIDISP: forced blank, brightness 0
    ldx #$01
:   stz $2100,x                 ; $2101-$210C
    inx
    cpx #$0D
    bne :-
:   stz $2100,x                 ; $210D-$2114: scrolls, written twice
    stz $2100,x
    inx
    cpx #$15
    bne :-
    lda #$80
    sta $2115                   ; VMAIN: VRAM address steps after the high byte's write
    stz $2116                   ; VMADDL/H: VRAM address 0
    stz $2117
    stz $211A                   ; M7SEL: Mode 7 settings 0
    stz $211B                   ; M7A = $0100 (1.0): written low byte, then high
    lda #$01
    sta $211B
    stz $211C                   ; M7B = 0
    stz $211C
    stz $211D                   ; M7C = 0
    stz $211D
    stz $211E                   ; M7D = $0100 (1.0)
    sta $211E
    stz $211F                   ; M7X, M7Y (the centre) = 0
    stz $211F
    stz $2120
    stz $2120
    stz $2121                   ; CGADD: colour 0
    ldx #$23
:   stz $2100,x                 ; $2123-$2133
    inx
    cpx #$34
    bne :-
    lda #$30
    sta $2130                   ; CGWSEL: colour math off everywhere
    lda #$E0
    sta $2132                   ; COLDATA: the fixed colour's red, green and blue to 0
    stz $2133                   ; SETINI: no interlace, no overscan
    stz $4200                   ; NMITIMEN: no NMI, IRQ or auto-joypad
    lda #$FF
    sta $4201                   ; WRIO: the I/O port's lines high
    ldx #$02
:   stz $4200,x                 ; $4202-$420D
    inx                         ; (multiply, divide, IRQ timers, DMA and HDMA enables)
    cpx #$0D                    ; (the loop stops before $420D)
    bne :-
.ifndef SUPERFX
    lda #$01
    sta $420D                   ; MEMSEL: FastROM again
.endif
    plp
    rts

; ---- the interrupts: NMI (vblank) through nmi_body (src/hw.s), the rest return
; The NMI arrives in bank $00 (the vector); it jumps into the code bank
; (fast ROM), saves every register the body may change, sets the direct
; page to 0 and the data bank to $80 (so absolute addresses reach the
; hardware registers and low RAM), acknowledges the NMI and calls nmi_body.
; The interrupt itself saved P (the register widths) and the program bank;
; rti restores them.
nmi:
    jml f:CODE_BANK * $10000 + nmi_far
nmi_far:
    rep #$30
    .a16
    .i16
    pha
    phx
    phy
    phb
    phd
    lda #$0000
    tcd
    sep #$20
    .a8
    lda #$80
    pha
    plb
    lda $4210                   ; acknowledge (reading RDNMI clears the NMI flag)
    rep #$20
    .a16
    jsl f:CODE_BANK * $10000 + nmi_body   ; (a16 i16, data bank $80, direct page 0): video uploads, sound, frame count
    rep #$30                    ; 16-bit again, to pop what was pushed 16-bit
    pld
    plb
    ply
    plx
    pla
    rti

; IRQ, BRK, COP, ABORT: not used; return at once
irq:
    rti

.ifdef SUPERFX
; ---- bank $00's entry points (the vectors point here; the code is in $40)
.segment "STUBS"
reset_stub:
    sei
    clc
    xce                         ; native mode (reset does it again)
    jml f:CODE_BANK * $10000 + reset
nmi_stub:
    jml f:CODE_BANK * $10000 + nmi_far
irq_stub:
    rti

; ---- the cartridge header (the GSU's map: LoROM, slow ROM; a GSU-2 with
; 64 KB of battery-backed RAM; tools/snes_rom.py --fx fills in the rest)
.segment "HEADER"
    .byte "00"                  ; $FFB0 maker code
    .byte "DRPG"                ; game code
    .byte 0                     ; $FFB6: the campaign block's bank
    .byte 0                     ; $FFB7: the resources' bank
    .byte 0, 0, 0, 0            ; reserved
    .byte $00                   ; expansion flash
    .byte $06                   ; expansion RAM: 64 KB (the GSU's)
    .byte $00                   ; special version
    .byte $00                   ; cartridge sub-type
    .byte "DEIMOS FX            " ; $FFC0 title (21)
    .byte $20                   ; map mode: LoROM (the GSU's), slow ROM
    .byte $15                   ; ROM + SuperFX + RAM + battery
    .byte $0B                   ; ROM size (2 MB)
    .byte $00                   ; (the save RAM is the GSU's RAM)
    .byte $01                   ; North America
    .byte $33                   ; licensee: the extended header
    .byte $00                   ; version
    .word $0000                 ; checksum complement
    .word $FFFF                 ; checksum

.segment "VECTORS"
    .word 0, 0
    .word .loword(irq_stub)     ; COP
    .word .loword(irq_stub)     ; BRK
    .word .loword(irq_stub)     ; ABORT
    .word .loword(nmi_stub)     ; NMI
    .word 0
    .word .loword(irq_stub)     ; IRQ
    .word 0, 0
    .word .loword(irq_stub)     ; COP
    .word 0
    .word .loword(irq_stub)     ; ABORT
    .word .loword(irq_stub)     ; NMI
    .word .loword(reset_stub)   ; RESET
    .word .loword(irq_stub)     ; IRQ / BRK
.else
; ---- the cartridge header (HiROM, FastROM) at $FFB0 (tools/snes_rom.py
; fills in the size, the checksum and its complement)
.segment "HEADER"
    .byte "00"                  ; $FFB0 maker code
    .byte "DRPG"                ; game code
    .byte 0                     ; $FFB6: the campaign block's bank (tools/snes_rom.py)
    .byte 0, 0, 0, 0, 0, 0      ; reserved
    .byte $00                   ; expansion RAM
    .byte $00                   ; special version
    .byte $00                   ; cartridge sub-type
    .byte "DEIMOS               " ; $FFC0 title (21)
    .byte $31                   ; map mode: HiROM + FastROM
    .byte $02                   ; ROM + RAM + battery
    .byte $0C                   ; ROM size (4 MB; tools/snes_rom.py)
    .byte $03                   ; SRAM 8 KB
    .byte $01                   ; North America
    .byte $33                   ; licensee: the extended header
    .byte $00                   ; version
    .word $0000                 ; checksum complement
    .word $FFFF                 ; checksum

.segment "VECTORS"
    ; native: $FFE0..
    .word 0, 0
    .word .loword(irq)          ; COP
    .word .loword(irq)          ; BRK
    .word .loword(irq)          ; ABORT
    .word .loword(nmi)          ; NMI
    .word 0
    .word .loword(irq)          ; IRQ
    ; emulation: $FFF0..
    .word 0, 0
    .word .loword(irq)          ; COP
    .word 0
    .word .loword(irq)          ; ABORT
    .word .loword(irq)          ; NMI
    .word .loword(reset)        ; RESET
    .word .loword(irq)          ; IRQ / BRK
.endif
