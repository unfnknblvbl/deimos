| Deimos - Doom RPG on retro consoles
| Copyright (C) 2026 unfnknblvbl and contributors
| SPDX-License-Identifier: GPL-2.0-or-later
| Free software under the GNU General Public License, version 2 or (at your
| option) any later version, with NO WARRANTY: see the LICENSE file.

| The Mega Drive game - start-up code: vectors, cartridge header, hardware init.
| Licence: GPL-2.0-or-later.
|
| Overview. This is the first code in the ROM and the home of the two
| interrupt handlers that matter. The linker script puts the sections in
| this order at address 0:
|   .text.vectors  0x000-0x0FF  the 68000's 64 exception vectors
|   .text.header   0x100-0x1FF  the Mega Drive cartridge header
|   .text.start    the reset code (_start), which clears RAM, copies the
|                  initialised C data and calls main (src/ui.c)
| It also holds:
|   - the exception handlers: unexpected exceptions are counted in
|     exc_count; bus errors, address errors and illegal instructions are
|     recorded in crash_info and stop the program (for the tests);
|   - vblank_handler, the level 6 frame interrupt, which calls the C code
|     in vdp.c (vblank_c, then vblank_sound: sound.c and music.s);
|   - hblank_handler, the level 4 H interrupt, which runs the "raster
|     effects" on the game screen: colour changes part-way down the
|     picture, plus queued DMA transfers (dma.s).
|
| Screen layout used by the H interrupt (H32 mode, 224 lines, 8-line tile
| rows): tile row 0 (lines 0-7) is the message line; the 3D view is tile
| rows 3-18 (lines 24-151, VIEW_ROW0 / VIEW_LINE0 / VIEW_H in md.h),
| with a black border round it (rows 1-2 and 19-20, and 3 columns each
| side); the status bar is in the window plane (rows 21-27). The view's
| floor and ceiling are drawn in 8 "bands" of 16 lines, each with its own
| two colours.
|
| Work RAM is 0xFF0000-0xFFFFFF (64 KB); the stack starts at its top and
| grows down.

    .section .text.vectors, "ax"
    .global _start
| The 68000 vector table: vector n is the long at address 4n. Vector 0 is
| the stack pointer loaded at reset, vector 1 the reset PC.
    .long   0x01000000          | initial stack: top of work RAM
                                | (the stack pre-decrements, so the
                                | first push goes to 0xFFFFFC)
    .long   _start              | reset
    .long   exc_group0          | 2 bus error
    .long   exc_group0          | 3 address error
    .long   exc_other           | 4 illegal instruction
    .long   exc_v5              | 5 division by zero (counted)
| vectors 6 .. 23 (CHK, TRAPV, privilege violation, trace, line A, line F,
| reserved, uninitialised interrupt): only counted
    .rept   18
    .long   exc_default         | 6 .. 23
    .endr
    .long   exc_v24             | 0x60 spurious
    .long   exc_v25             | 0x64 level 1
    .long   exc_v26             | 0x68 level 2 (external)
    .long   exc_v27             | 0x6C level 3
    .long   hblank_handler      | 0x70 level 4: H interrupt
    .long   exc_default         | 0x74 level 5
    .long   vblank_handler      | 0x78 level 6: V interrupt
    .long   exc_default         | 0x7C level 7
| vectors 32 .. 63: TRAP #0..15, then reserved and user vectors (unused)
    .rept   32
    .long   exc_default         | traps 0..15, reserved
    .endr

| The cartridge header at 0x100 (Sega's fixed layout; every field is a
| fixed-width ASCII string or big-endian number). The console's TMSS only
| checks "SEGA" at 0x100; emulators and flash carts read the rest (ROM and
| RAM size, save RAM, region, the 6-button pad flag).
    .section .text.header, "ax"
    .ascii  "SEGA MEGA DRIVE "                                  | 0x100 system
    .ascii  "(C)UNFK 2026.OCT"                                  | 0x110 copyright
    .ascii  "DEIMOS                                          " | 0x120 domestic name (48)
    .ascii  "DEIMOS                                          " | 0x150 overseas name (48)
    .ascii  "GM 00000000-00"                                    | 0x180 serial
    .word   0x0000                                            | 0x18E checksum (filled by the build)
    .ascii  "J6              "                                  | 0x190 I/O: 6-button pad
    .long   0x00000000, 0x003FFFFF                            | 0x1A0 ROM range
    .long   0x00FF0000, 0x00FFFFFF                            | 0x1A8 RAM range
    .ascii  "RA"                                                | 0x1B0 battery-backed RAM:
    .byte   0xF8, 0x20                                        |   odd bytes, saved
    .long   0x00200001, 0x00203FFF                            |   8 KB
                                        | (0xF8 = kept by a battery, 0x20 =
                                        | on odd addresses only: the 8 KB
                                        | sit at 0x200001, 0x200003, ...
                                        | 0x203FFF, one byte per word)
    .ascii  "            "                                      | 0x1BC modem
| 0x1C8: the notes field, used here as the ROM directory: the "DEIMOSMD"
| tag, then three big-endian longs. The code itself reaches the campaign
| block through the md_campaign symbol (pack.c); the directory is for the
| tools outside the ROM (tools/make_engine.py, which blanks the block for
| the Deimos Editor's engine; the editor writes a new block there).
| 8 + 12 + 20 bytes = the field's 40 bytes.
    .ascii  "DEIMOSMD"                                          | 0x1C8 notes (40): the ROM directory
    .long   md_campaign                                       |   the campaign (tools/md_campaign.py)
    .long   0x00400000                                        |   end of the space it may use
    .long   md_areas                                          |   the areas' colours (tools/md_areas.c)
    .ascii  "                    "
    .ascii  "JUE             "                                  | 0x1F0 regions

    .section .text.start, "ax"
| _start: the reset entry point (vector 1). The 68000 arrives here in
| supervisor mode with the stack pointer from vector 0.
| In: nothing. Out: never returns (main is not expected to return; if it
| does, the program spins at label 5).
| Steps: mask interrupts, unlock the VDP (TMSS), park the Z80, zero work
| RAM (which also zeroes every .bss variable), copy .data from ROM, then
| call main. Uses d0, a0-a2.
_start:
    move    #0x2700, sr                 | interrupts off
                                        | (SR 0x2700: supervisor, mask 7)
    | TMSS: consoles with a version > 0 need "SEGA" written to $A14000
    move.b  0xA10001, d0                | version register: bits 0-3 =
    andi.b  #0x0F, d0                   | hardware revision
    beq.s   1f                          | revision 0 has no TMSS
    move.l  #0x53454741, 0xA14000       | "SEGA" in ASCII unlocks the VDP
1:
    | Z80: take its bus for good (the 68000 drives the sound chips), pulse
    | its reset (which also resets the YM2612) and let go of the reset: the
    | YM2612 is held in reset with it, while the Z80 stays stopped by the bus
    | request
    move.w  #0x100, 0xA11100            | Z80 bus request: Z80 stops
    move.w  #0x000, 0xA11200            | Z80 reset line asserted
    moveq   #31, d0
0:  dbra    d0, 0b                      | hold the reset for 32 loops
    move.w  #0x100, 0xA11200            | release the reset
    | clear work RAM
    lea     0xFF0000, a0
    move.w  #0x3FFF, d0                 | 0x4000 longs = 64 KB
2:  clr.l   (a0)+
    dbra    d0, 2b
    | copy initialised data from ROM
    | (__data_load: the ROM copy; __data_start .. __data_end: where it
    | lives in RAM. All three come from the linker script. Copied a word
    | at a time, so the section is assumed to start on an even address.)
    lea     __data_load, a0
    lea     __data_start, a1
    lea     __data_end, a2
3:  cmpa.l  a2, a1                      | reached the end?
    bcc.s   4f
    move.w  (a0)+, (a1)+
    bra.s   3b
4:
    jsr     main                        | the game (main in src/ui.c)
5:  bra.s   5b                          | main returned: stop here

| Unexpected exceptions and interrupts: each adds one to a byte counter
| and returns, so the program goes on. The tests can read exc_count to
| see that something happened.
|   exc_count+0  every vector sent to exc_default (CHK, TRAPV, privilege,
|                trace, line A/F, levels 5 and 7, TRAP #n, reserved)
|   exc_count+1  vector 5, division by zero
|   exc_count+2  vector 24, spurious interrupt
|   exc_count+3  vector 25, level 1 interrupt
|   exc_count+4  vector 26, level 2 interrupt (external, from the pad port)
|   exc_count+5  vector 27, level 3 interrupt
| The counters wrap at 256. Clobbers nothing (addq.b to memory only
| changes the flags, and rte restores SR).
exc_default:
    addq.b  #1, exc_count
    rte
exc_v5:
    addq.b  #1, exc_count+1
    rte
exc_v24:
    addq.b  #1, exc_count+2
    rte
exc_v25:
    addq.b  #1, exc_count+3
    rte
exc_v26:
    addq.b  #1, exc_count+4
    rte
exc_v27:
    addq.b  #1, exc_count+5
    rte
    .bss
    .global exc_count
exc_count:  .space 6
    .text

| a bus or address error, or an illegal instruction, is a bug: record where
| (crash_info: the faulting address and the PC, for the tests) and stop
|
| exc_group0 (vectors 2, 3): the 68000 pushes a 14-byte frame for these:
| +0 word access type (read/write, function code), +2 long the address it
| tried, +6 word the instruction's first word, +8 word SR, +10 long PC.
| exc_other (vector 4): the normal 6-byte frame, +0 word SR, +2 long PC.
| Both mask all interrupts and spin forever; the test harness
| (tools/md_campaign_lockstep.py) reads crash_info and reports it.
|
| crash_info layout (22 bytes):
|   +0   long  the stack pointer at the exception (where the frame is)
|   +4   16 bytes copied from the frame (exc_group0: the whole 14-byte
|        frame plus 2 bytes above it; exc_other: only +4..+11 written)
|   +20  byte  1 = bus or address error, 2 = illegal instruction,
|              0 = no crash
|   +21  unused
    .global crash_info
exc_group0:
    move.w  #0x2700, sr                 | mask every interrupt
    move.l  sp, crash_info              | where the frame is
    move.l  (sp), crash_info+4          | the frame: status word, address,
    move.l  4(sp), crash_info+8         |   instruction, SR, PC
    move.l  8(sp), crash_info+12
    move.l  12(sp), crash_info+16
    move.b  #1, crash_info+20           | kind: bus or address error
1:  bra.s   1b                          | stop
exc_other:
    move.w  #0x2700, sr                 | mask every interrupt
    move.l  sp, crash_info
    move.l  (sp), crash_info+4          | SR, PC
    move.l  4(sp), crash_info+8
    move.b  #2, crash_info+20           | kind: illegal instruction
2:  bra.s   2b                          | stop

    .bss
crash_info: .space 22
| vblank_pc (8 bytes, for the tests): at each frame interrupt, +0 the PC
| the program was interrupted at, +4 the long on top of its stack (often
| the return address of the routine it was in). Shows where the game
| spends its time or where it hangs.
    .global vblank_pc
vblank_pc:  .space 8
    .text

| level 6: the frame interrupt (C does the work): the flip and the
| palettes first, then the sound with the H interrupts let in (the
| raster effects of the next frame must not wait for it)
|
| In: the interrupt frame (SR, PC). Out: nothing.
| Calls vblank_c (vdp.c: frame counter, view buffer flip, palettes,
| damage flash, the floor and ceiling's first band, queued DMAs) and
| vblank_sound (vdp.c: sound effects, then music.s's music_update).
| Saves d0-d1/a0-a1, the registers C code may clobber; C keeps the rest.
| Stack after the movem: 16 bytes of saved registers, then the frame:
| 16(sp) SR, 18(sp) PC, 22(sp) the interrupted code's stack top.
| Note: SR 0x2300 sets the mask to 3, which lets level 4 (H) and level 6
| (this interrupt) in again. A second frame interrupt can only nest here
| if the sound update ran for a whole frame.
vblank_handler:
    movem.l d0-d1/a0-a1, -(sp)
    move.l  18(sp), vblank_pc           | (the tests: where the program was,
    move.l  22(sp), vblank_pc+4         |  and what is on its stack)
    jsr     vblank_c                    | flip, palettes, DMAs (vdp.c)
    move.w  #0x2300, sr                 | let the H interrupt in from now
    jsr     vblank_sound                | sound effects and music (vdp.c)
    movem.l (sp)+, d0-d1/a0-a1
    rte

| level 4: the H interrupt, every 8 lines on the game screen (the end of
| lines 7, 15, ...). Worked out from the V counter (an interrupt held up
| by a DMA or another interrupt still does the right thing): in the view,
| the floor and ceiling's two colours of its 16-line band (palette 3,
| slots 13 and 14: src/backdrop.c), then the queued DMAs (src/vdp.c) that
| fit before the next band's (the interrupt in between waits for them:
| it has nothing to do on time); after the view (line VIEW_END - 1 = 151
| on), once a frame: the status bar's palette into palette 2, with the display off for
| the moment (the writes would show as dots), then DMAs to line ~220.
|
| The H interrupt is switched on only on the game screen (vdp.c
| vdp_raster: register 0 bit 4, register 10 = 7 for one interrupt every 8
| lines). Shared state with vdp.c:
|   top_done   set here once the view's palette 2 is in for this frame;
|              cleared by vblank_c.
|   hud_done   the same for the status bar's palette 2; cleared by
|              vblank_c.
|   pal2_view  the 16 colours of the view's palette 2 (vblank_c fills it,
|              with the red damage tint when it applies).
|   bd_rows_p  pointer to one long per band: the two backdrop colours of
|              that band (palette 3, slots 13 and 14), from backdrop.c.
|   vdp_reg1   the value of VDP register 1 (bit 6 = display on).
| Saves and restores d0-d1/a0-a1; dma_run_d0 keeps d2-d4 itself.
    .equ    VDP_DATA, 0xC00000          | VDP data port
    .equ    VDP_CTRL, 0xC00004          | VDP control port
    .equ    VIEW_LINE0, 24              | the view's first line (md.h VIEW_LINE0)
    .equ    VIEW_END, VIEW_LINE0 + 128  | the line after its last (+ md.h VIEW_H)
    .equ    BAND_IRQ0, VIEW_LINE0 - 1   | band 0's interrupt: the end of the line
                                        | before the view (23; bands follow every 16)
hblank_handler:
    move.w  #0x2700, sr                 | (the frame interrupt waits: both run DMAs)
    movem.l d0-d1/a0-a1, -(sp)
    moveq   #0, d1
    move.b  0xC00008, d1                | V
                                        | (high byte of the HV counter:
                                        | the line being drawn)
    move.w  d1, d0
    cmpi.w  #VIEW_END - 2, d0           | below the view's last band?
    bcc     2f                          | yes: the status bar part
| In the view: band = (V - BAND_IRQ0) / 16, never below 0. Bands are 16
| lines, and this interrupt comes every 8, so each band is handled twice;
| the second time just writes the same colours again and runs more DMAs.
| The interrupts above the view (lines 7, 15: the border) load band 0.
    subi.w  #BAND_IRQ0, d0              | band (v - 23) >> 4
    bcc.s   1f
    moveq   #0, d0                      | (before line 23: band 0)
1:  lsr.w   #4, d0
| First interrupt of the frame in the view: load the view's palette 2.
| Above the view, the message line uses palette 2 for the status bar's
| colours (vblank_c put those in), so it must change here.
    tst.b   top_done                    | the view's top: its palette 2 (the
    bne.s   11f                         | message line above has the status
    st      top_done                    | bar's), the display off meanwhile
    move.w  d0, -(sp)                   | keep the band number
    moveq   #0, d0
    move.b  vdp_reg1, d0
    andi.w  #0xBF, d0                   | register 1 with bit 6 (display)
    ori.w   #0x8100, d0                 | clear: display off
    move.w  d0, VDP_CTRL
    move.l  #0xC0400000, VDP_CTRL       | CRAM write at $40: palette 2
    lea     pal2_view, a0
    move.l  (a0)+, VDP_DATA             | 8 longs = 16 colours
    move.l  (a0)+, VDP_DATA
    move.l  (a0)+, VDP_DATA
    move.l  (a0)+, VDP_DATA
    move.l  (a0)+, VDP_DATA
    move.l  (a0)+, VDP_DATA
    move.l  (a0)+, VDP_DATA
    move.l  (a0)+, VDP_DATA
    ori.w   #0x40, d0                   | display on again
    move.w  d0, VDP_CTRL
    move.w  (sp)+, d0                   | the band number back
| Every view interrupt: the band's floor or ceiling colours.
11: move.l  #0xC07A0000, VDP_CTRL       | CRAM $7A: palette 3, slot 13
    lsl.w   #2, d0                      | band * 4: one long per band
    movea.l bd_rows_p, a0
    move.l  0(a0,d0.w), VDP_DATA        | slots 13 and 14 in one long write
| Then queued DMAs, as many words as fit before the next band's
| interrupt: lines left = 16 * band + BAND_IRQ0 + 14 - V.
    lsl.w   #2, d0                      | the next band's interrupt (23 + 16 (b + 1)),
    addi.w  #BAND_IRQ0 + 16 - 2, d0     | less 2 lines
    sub.w   d1, d0
    ble     9f                          | no time left: skip the DMAs
    move.b  dq_head, d1
    cmp.b   dq_tail, d1                 | queue empty?
    beq     9f
    mulu.w  #6, d0                      | (6 words a line, set-ups too)
    jsr     dma_run_d0                  | run queued DMAs (dma.s), d0 words
    bra     9f
| Below the view: lines 150-223, once a frame, the status bar's palette.
| Lines 224 and on are the vertical blank, where vblank_c does the work.
2:  cmpi.w  #224, d0
    bcc     9f
    tst.b   hud_done                    | already done this frame?
    bne     9f
    st      hud_done
    moveq   #0, d0
    move.b  vdp_reg1, d0
    andi.w  #0xBF, d0                   | register 1, bit 6 clear
    ori.w   #0x8100, d0
    move.w  d0, VDP_CTRL                | display off
    move.l  #0xC0400000, VDP_CTRL       | CRAM $40: palette 2
    lea     ui_hud_pal, a0              | the status bar's 16 colours
                                        | (generated: tools/build_md_ui.py)
    move.l  (a0)+, VDP_DATA
    move.l  (a0)+, VDP_DATA
    move.l  (a0)+, VDP_DATA
    move.l  (a0)+, VDP_DATA
    move.l  (a0)+, VDP_DATA
    move.l  (a0)+, VDP_DATA
    move.l  (a0)+, VDP_DATA
    move.l  (a0)+, VDP_DATA
    ori.w   #0x40, d0
    move.w  d0, VDP_CTRL                | display on
    jsr     dma_run_hud                 | queued DMAs: to line ~220
9:  movem.l (sp)+, d0-d1/a0-a1
    rte
