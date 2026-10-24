| Deimos - Doom RPG on retro consoles
| Copyright (C) 2026 unfnknblvbl and contributors
| SPDX-License-Identifier: GPL-2.0-or-later
| Free software under the GNU General Public License, version 2 or (at your
| option) any later version, with NO WARRANTY: see the LICENSE file.

| The Mega Drive game - the DMA queue's runner (src/vdp.c fills the queue; the
| interrupts run it: crt0.s, vdp.c's frame interrupt), in assembler: it
| runs where every line counts.
|
| Licence: GPL-2.0-or-later.
|
| Overview. On the game screen the H interrupt (crt0.s) changes colours
| every 8 lines (the "raster effects"). A DMA transfer stops the 68000
| until it ends, so a long DMA started at the wrong moment would make
| those colour changes late. So while the raster effects are on, C code
| does not start DMAs itself: vdp_dma_queue (vdp.c) appends them to a
| ring buffer, the DMA queue, and the interrupts drain it here, each one
| only as many words as fit before the next colour change is due.
|
| Callers:
|   - crt0.s hblank_handler, in the view area: dma_run_d0, budget in d0.
|   - vdp.c dma_run_hud (from the H interrupt below the view) and
|     vblank_c (the frame interrupt): dma_run, budget on the stack.
|
| The queue (defined in vdp.c):
|   dq[DQ_N]   48 entries of 8 bytes each:
|                +0 long  src    source address in ROM or RAM (bytes,
|                                even)
|                +4 word  vram   destination VRAM address (bytes)
|                +6 word  words  words still to send; 0 marks a "flip"
|                                entry (see below)
|   dq_head    byte: index of the next entry to run (only this file
|              moves it).
|   dq_tail    byte: index of the first free entry (vdp_dma_queue moves
|              it). head == tail means the queue is empty.
|   dq_hold    byte: non-zero while the renderer is still queueing one
|              picture's transfers (render.s sets it); the interrupts then
|              leave the queue alone, so a picture is never half sent.
|
| A "flip" entry has words = 0, and the low byte of its vram field holds a
| view buffer number (0 or 1). Reaching it means every transfer of that
| picture is done, so the routine stores the number in pending_view_buf
| and the next frame interrupt (vdp.c vblank_c) shows that buffer.
|
| An entry may be split over several calls: when the budget runs out,
| src, vram and words are advanced in place and the entry stays at the
| head of the queue.

    .equ    VDP_CTRL, 0xC00004          | VDP control port (register writes,
                                        | address and DMA commands)
    .equ    DQ_N, 48                    | (vdp.c) entries in the ring buffer

    .text
| void dma_run(u16 budget): the queue's next DMAs, at most `budget` words;
| an entry of no words is the flip to the buffer it names (the picture
| being up). dma_run_d0: the same with the budget in d0.w (the H
| interrupt). Uses d0-d1/a0-a1.
|
| In:  dma_run: the budget as a C argument at 4(sp) (u16, passed as a
|      long; only the low word is used). dma_run_d0: budget in d0.w.
|      Memory: dq, dq_head, dq_tail, dq_hold, vdp_reg1 (vdp.c's shadow of
|      VDP register 1).
| Out: dq_head advanced past the finished entries; the entry at the head
|      updated if it was only partly sent; pending_view_buf set when a
|      flip entry is reached. Nothing is returned.
| Side effects: DMA transfers to VRAM (the 68000 is halted during each).
| Clobbers: d0, d1, a0, a1 and the flags. d2-d4 are saved and restored,
|      so it is safe to call from C and from the interrupt handlers (which
|      save d0-d1/a0-a1 themselves).
| Budget units: one DMA word moves one 16-bit word. The callers turn the
|      number of lines left into a word count (about 6 or 7 words a line
|      during the picture, 56 in the vertical blank: see vdp.c, crt0.s).
    .global dma_run, dma_run_d0
dma_run:
    move.l  4(sp), d0                   | the C argument (low word = budget)
dma_run_d0:
    tst.b   dq_hold                     | renderer still filling the queue?
    bne     9f                          | yes: touch nothing this time
    movem.l d2-d4, -(sp)
    lea     VDP_CTRL, a1                | a1 = VDP control port
| main loop: one queue entry (or the part of it the budget allows) a pass
0:  tst.w   d0                          | budget used up?
    beq     8f
    moveq   #0, d1
    move.b  dq_head, d1
    cmp.b   dq_tail, d1                 | queue empty?
    beq     8f
    lsl.w   #3, d1                      | the entry: src (long), vram, words
    lea     dq, a0                      | a0 = &dq[dq_head] (8 bytes an entry)
    adda.w  d1, a0
    move.w  6(a0), d1                   | d1 = words left in this entry
    bne     1f
    move.b  5(a0), pending_view_buf     | the flip: low byte of vram = the
                                        | buffer the frame interrupt shows
    bsr     next                        | drop the flip entry
    bra.s   0b                          | (costs no budget)
1:  cmp.w   d0, d1                      | n = min(words, budget, room to the
    bls.s   2f                          | source's next 128 KB boundary)
    move.w  d0, d1
| The VDP's DMA source counter only increments its low 16 bits (of the
| word address), so a transfer must not cross a 128 KB boundary of the
| source: room = (0x20000 - (src & 0x1FFFF)) / 2 words.
2:  move.l  (a0), d2                    | d2 = source address (bytes)
    move.l  d2, d3
    andi.l  #0x1FFFF, d3                | offset inside its 128 KB block
    neg.l   d3
    addi.l  #0x20000, d3                | bytes to the next boundary
    lsr.l   #1, d3                      | d3 = words to the boundary
    andi.l  #0xFFFF, d1                 | clear the high word for the cmp.l
    cmp.l   d3, d1
    bls.s   3f
    move.w  d3, d1                      | d1 = n, the words sent this time
3:  moveq   #0, d3                      | DMA enable
    move.b  vdp_reg1, d3
    ori.w   #0x8110, d3                 | register 1 = vdp_reg1 | 0x10 (M1:
    move.w  d3, (a1)                    | DMA allowed), display bits kept
    move.w  d1, d3                      | length
    andi.w  #0xFF, d3
    ori.w   #0x9300, d3                 | register 19: length, low byte
    move.w  d3, (a1)
    move.w  d1, d3
    lsr.w   #8, d3
    ori.w   #0x9400, d3                 | register 20: length, high byte
    move.w  d3, (a1)
    move.l  d2, d4                      | source (words)
    lsr.l   #1, d4                      | the VDP takes the address / 2
    move.w  d4, d3
    andi.w  #0xFF, d3
    ori.w   #0x9500, d3                 | register 21: source bits 1-8
    move.w  d3, (a1)
    move.w  d4, d3
    lsr.w   #8, d3
    ori.w   #0x9600, d3                 | register 22: source bits 9-16
    move.w  d3, (a1)
    swap    d4
    andi.w  #0x7F, d4                   | register 23: source bits 17-23;
    ori.w   #0x9700, d4                 | bit 7 clear = 68000 memory to VDP
    move.w  d4, (a1)
| The command that starts the DMA is two words: first 0x4000 | (vram &
| 0x3FFF) (code VRAM write), then 0x80 | (vram >> 14) (the DMA bit and
| the top two address bits). The DMA starts on the second word. They are
| built on the stack so that the final word is read from RAM: the usual
| advice for real hardware is that the word which triggers a 68000 to VDP
| DMA must not be fetched from ROM. vdp.c dma_now does the same.
    move.w  4(a0), d3                   | the command (from RAM, the stack:
    move.w  d3, d4                      | the second word starts it)
    lsr.w   #8, d4
    lsr.w   #6, d4                      | d4 = vram >> 14
    ori.w   #0x80, d4                   | second word: DMA bit + A15-A14
    move.w  d4, -(sp)
    andi.w  #0x3FFF, d3
    ori.w   #0x4000, d3                 | first word: VRAM write + A13-A0
    move.w  d3, -(sp)
    move.w  (sp)+, (a1)                 | first word
    move.w  (sp)+, (a1)                 | second word: the DMA runs now
                                        | (the 68000 waits until it ends)
    moveq   #0, d3                      | DMA off
    move.b  vdp_reg1, d3
    ori.w   #0x8100, d3                 | register 1 = vdp_reg1 (M1 clear)
    move.w  d3, (a1)
| Bookkeeping: take n off the budget and the entry, move its source and
| destination on by 2n bytes, so a part-sent entry resumes where it
| stopped. (d1's high word is zero here, so add.l adds just 2n.)
    sub.w   d1, d0                      | budget -= n
    sub.w   d1, 6(a0)                   | words -= n
    add.w   d1, d1                      | d1 = 2n bytes
    add.l   d1, (a0)                    | src += 2n
    add.w   d1, 4(a0)                   | vram += 2n
    tst.w   6(a0)                       | entry finished?
    bne     0b                          | no (budget or 128 KB limit): loop
    bsr     next                        | yes: on to the next entry
    bra     0b
8:  movem.l (sp)+, d2-d4
9:  rts

| next: advance dq_head by one, wrapping from DQ_N - 1 back to 0.
| In: dq_head. Out: dq_head updated. Clobbers: d3 and the flags.
| Called only from dma_run above.
next:
    move.b  dq_head, d3
    addq.b  #1, d3
    cmpi.b  #DQ_N, d3                   | past the last entry?
    bcs.s   1f
    moveq   #0, d3                      | wrap around to entry 0
1:  move.b  d3, dq_head
    rts
