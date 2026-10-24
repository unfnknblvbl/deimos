; Deimos - Doom RPG on retro consoles
; Copyright (C) 2026 unfnknblvbl and contributors
; SPDX-License-Identifier: GPL-2.0-or-later
; Free software under the GNU General Public License, version 2 or (at your
; option) any later version, with NO WARRANTY: see the LICENSE file.

; Doom RPG for the SNES with the SuperFX: the 3D view, drawn by the GSU.
;
; The same arithmetic as view/view.c's VIEW_FX reference (the build
; machine's pictures and these are compared byte for byte). The main CPU
; puts the camera, the map's walls, colour remaps, texture and sprite
; addresses and the list of things in the GSU's RAM (snes/src/fx.c), runs
; this from 0 and waits for its STOP.
;
; Code fetched from the ROM costs 5 cycles a byte, from the GSU's 512-byte
; cache 1, so the work is done in passes, each a loop small enough for the
; cache (CACHE at its start), with its own copies of the routines it calls
; (the M_ macros):
;   P0  the picture and the per-column tables cleared
;   PA  every wall: doors, the side seen, view space, the edges -> CAND
;   PB  CAND: clipped, its columns, its hq's divisor -> VW (FX_MAX_VW)
;   PC  VW: hq along each wall -> the nearest per column (COLHQ, COLW)
;   PD  per column: its wall's texture column -> COLP
;   PE  the walls' columns drawn
;   PF  the things (TH) -> billboards and decals (PT)
;   PF3 the decals' quads projected (few: not cached)
;   PS  PT sorted far first (PTORD)
;   PG1 PT -> the sprite columns to draw (SCOL, FX_MAX_SC)
;   PG2 the sprite columns drawn
;
; GSU RAM ($70:0000; $E000.. is the saves', never touched): the variables
; ($0000-$01FF, where LMS/SMS reach), the picture (FB, view/view.h's
; order), the tables below.
; Subroutines: LINK #4 / IWT R15 / NOP (CALL), back by JMP R11; the ones
; that call others keep R11 in their own variable.
; Where it fits: snes/build_fx.sh assembles this file into out/gsu_view.bin,
; which tools/snes_rom.py places in the cartridge (the GSU runs it from
; ROM; its loops from the instruction cache). Each picture, the CPU side
; (view/view.c's view_render_cam, built with VIEW_FX for the SNES, then
; snes/src/fx.c's fx_draw) fills the GSU's RAM: the camera ($00-$0B), the
; counts and the sprite tables' ROM addresses ($0C-$17), OPEN, TEXMAP and
; the things list TH; at a map change it also loads the walls (WALLS,
; BLK), TEXADDR / SPRADDR and the remaps (fx_map_load). Then it starts
; the GSU at address 0 and waits; afterwards it uploads FB to the video
; RAM (the picture is FB_BYTES, colour 0 the floor and ceiling).
;
; The view's size: VIEW_W x VIEW_H (view/view.h's; snes/build_fx.sh
; passes the SNES game's 104 x 72 with -D, the default is the original
; 128 x 88). The comments give the sums at 128 wide, where VIEW_W = 2 x
; the focal length = 128 and the focal length VIEW_FOCAL = 64; at another
; width 128 is VIEW_W and 64 VIEW_FOCAL throughout, as in view.c (SX_NUM,
; TIMES_W, HQ_NUM, CANVAS_OF).
;
; GSU notes for the newcomer: R15 is the program counter; the instruction
; after any jump, branch or LOOP is always executed (the delay slot, here
; mostly NOP). LOOP decrements R12 and jumps to R13 while R12 is not 0
; ("MOVE R13,R15" just before a loop body sets R13 to the body's start).
; FROM / TO / WITH pick the source and destination of the next
; instruction (R0 by default). LMULT: R4 (low) and the destination (high)
; = source x R6, signed 16 x 16. MERGE: (R7 & $FF00) | (R8 >> 8). HIB /
; LOB: the high / low byte. GETB reads the ROM byte at ROMBR:R14 (ROMB
; sets ROMBR); LDB / STB / LDW / STW read and write the RAM bank ($70);
; LMS / SMS the RAM's first 512 bytes by a short address. After SUB or
; CMP the carry is set when there was no borrow (unsigned >=).
;
; Fixed point as in view.c: hq = height x 16 (12.4), v and dv 8.8 texture
; rows, view-space positions in world units (64 per cell), fwd / right
; vectors 8.8.
; wla-superfx. Licence: GPL-2.0-or-later.

.MEMORYMAP
    SLOTSIZE $10000
    DEFAULTSLOT 0
    SLOT 0 START $0000
.ENDME
.ROMBANKSIZE $10000
.ROMBANKS 1
.BANK 0 SLOT 0
.ORG 0

.IFNDEF VIEW_W
.DEFINE VIEW_W  128             ; the view's width in pixels (a multiple of 8)
.ENDIF
.IFNDEF VIEW_H
.DEFINE VIEW_H  88              ; its height (88, or a multiple of 8 below)
.ENDIF
.DEFINE VIEW_HALF_H (VIEW_H / 2)        ; the horizon's row
.DEFINE VIEW_FOCAL (VIEW_W / 2)         ; the focal length in pixels
; the picture's layout (view.h's FB_OFF): at 128 x 88, strips of 3 blocks
; of 32 rows (a column runs down FB_GROUP = 32 rows, then skips FB_SKIP
; bytes to its next block); at any other size tiled (FB_TILED: 8 x 8
; tiles, TILE_ROWS down a strip, a column's 8 rows in each)
.IF VIEW_W == 128 && VIEW_H == 88
.DEFINE FB_TILED 0
.DEFINE FB_ROWS 96
.DEFINE FB_GROUP 32
.DEFINE FB_SKIP 224             ; 256 - 32
.ELSE
.DEFINE FB_TILED 1
.DEFINE FB_ROWS VIEW_H
.DEFINE FB_GROUP 8
.DEFINE FB_SKIP 56              ; 64 - 8
.ENDIF
.DEFINE TILE_ROWS (FB_ROWS / 8)         ; the Mode 7 tiles down a strip
.DEFINE FB_BYTES (VIEW_W * FB_ROWS)     ; the picture's bytes (12288 at 128 x 88, 7488 at 104 x 72)
.DEFINE HQ_NUM (1024 * VIEW_FOCAL)      ; a thing's hq at depth 1 (65536 at 128 wide)
; W_TOP: VIEW_W's highest set bit (7 at 128), for M_TIMESW
.IF VIEW_W >= 128
.DEFINE W_TOP 7
.ELSE
.IF VIEW_W >= 64
.DEFINE W_TOP 6
.ELSE
.DEFINE W_TOP 5
.ENDIF
.ENDIF
.DEFINE FB      $0200           ; the picture: FB_BYTES (12 KB at 128 wide), FB_OFF order
                                ; (the 12 KB from here are RAW's while a map loads)
; (the tables: each name's [count] and record layout; "lo, bank": a ROM
; address as a 16-bit offset and a bank byte)
.DEFINE OPEN    $3200           ; [64] the locks' flags (bit 0: the door is open)
.DEFINE TEXMAP  $3240           ; [48] the portal's swaps
.DEFINE TEXADDR $3280           ; [48] lo, bank (words)
.DEFINE SPRADDR $3340           ; [64] lo, bank
.DEFINE TEXRM   $3600           ; [48][32]
.DEFINE SPRRM   $3C00           ; [64][16]
.DEFINE COLHQ   $4000           ; [VIEW_W] words (room for 128)
.DEFINE COLW    $4100           ; [VIEW_W] (room for 128)
.DEFINE BUF     $4180           ; [64] one column's colours
.DEFINE TH      $4200           ; [255] x, z, spr, -, info (8 bytes)
.DEFINE PT      $4A00           ; [255] 32 bytes, see PF
.DEFINE PTORD   $6A00           ; [255]
.DEFINE VW      $6B00           ; [254] da la dd dl tex ua du - x0 x1 qdn qk (24 bytes)
; (WALLS: per wall ax az bx bz tex line ua|flags ub, as the map pack;
; CAND and SCOL share their space: CAND is finished with before PG1)
.DEFINE WALLS   $8300           ; [1024] the map's walls: 8 words each
.DEFINE CAND    $C300           ; [320] da la db lb tex ua du (16 bytes)
.DEFINE SCOL    $C300           ; [640] x hq c spr (8 bytes) (after PB)
.DEFINE BLK     $D700           ; [64] the walls' blocks: start count x0 z0 x1 z1 (view.c's fx_blk)
.DEFINE VBL     $DA00           ; [64] the blocks in view (PA0)
.DEFINE COLP    $DB00           ; [VIEW_W] lo bank rm h (8 bytes) (room for 128)
.DEFINE MAX_CAND 320            ; the most walls PB looks at past PA (view.h FX_MAX_CAND)
.DEFINE MAX_SC  640             ; the most sprite columns (view.h FX_MAX_SC)
.DEFINE TABLES  $42             ; the column table's ROM bank (tools/snes_rom.py --fx)

; variables (from the CPU: $00..$17)
; (VX, VZ: the camera's world position; FWX, FWZ: forward, RTX, RTZ:
; right, 8.8; NWALLS: walls in WALLS; NTH: things in TH; SCOLS: the ROM
; address of the sprites' column spans (spr_cols), SCOFF: of the
; columns' data offsets (spr_coff); STOPAT: see the passes)
.DEFINE VX      $00
.DEFINE VZ      $02
.DEFINE FWX     $04
.DEFINE FWZ     $06
.DEFINE RTX     $08
.DEFINE RTZ     $0A
.DEFINE NWALLS  $0C
.DEFINE NTH     $0E
.DEFINE SCOLSLO $10
.DEFINE SCOLSBK $12
.DEFINE SCOFFLO $14
.DEFINE SCOFFBK $16
.DEFINE STOPAT  $18             ; (testing: STOP after that pass)
; the GSU's
; (Q*: the quad being projected: its view-space ends (QDA QLA QDB QLB),
; screen columns QX0 .. QX1 - 1, c as 32 bits (QCLO QCHI), the hq ramp
; (QHQ, QST: 16.8 each as two words), QDN: c shortened to 16 bits, QK:
; how many halvings that took. R*: the routines' saved return addresses
; (R11). The rest are each pass's working variables, named where used.)
.DEFINE QDA     $20
.DEFINE QLA     $22
.DEFINE QDB     $24
.DEFINE QLB     $26
.DEFINE QX0     $28
.DEFINE QX1     $2A
.DEFINE QCLO    $2C
.DEFINE QCHI    $2E
.DEFINE QHQLO   $30
.DEFINE QHQHI   $32
.DEFINE QSTLO   $34
.DEFINE QSTHI   $36
.DEFINE QDN     $38
.DEFINE QK      $3A
.DEFINE NVW     $3C
.DEFINE NCAND   $3E
.DEFINE CPTR    $40
.DEFINE AX      $42
.DEFINE AZ      $44
.DEFINE BX      $46
.DEFINE BZ      $48
.DEFINE UA      $4A
.DEFINE UB      $4C
.DEFINE WTEX    $4E
.DEFINE WFL     $50
.DEFINE CDA     $52
.DEFINE CLA     $54
.DEFINE CDB     $56
.DEFINE CLB     $58
.DEFINE XA      $5A
.DEFINE XB      $5C
.DEFINE CNT     $5E
.DEFINE RPH     $60
.DEFINE RPROJ   $62
.DEFINE RRAMP   $64
.DEFINE RHQAT   $66
.DEFINE RSDIV   $68
.DEFINE RCOLUMN $6A
.DEFINE RT8     $6C
.DEFINE RBB     $6E
.DEFINE RDECAL  $70
.DEFINE RFXCOL  $72
.DEFINE RCLIP   $74
.DEFINE REMIT   $76
.DEFINE CX      $80
.DEFINE HH      $82
.DEFINE SPRF    $84
.DEFINE TOP     $86
.DEFINE LEN     $88
.DEFINE SPR     $8A
.DEFINE HQ      $8C
.DEFINE CC      $8E
.DEFINE VK      $90
.DEFINE NSC     $92
.DEFINE T1      $96
.DEFINE T2      $98
.DEFINE T3      $9A
.DEFINE NPT     $9C
.DEFINE TI      $9E
.DEFINE PTP     $A0
.DEFINE CURD    $A4
.DEFINE CURL    $A6
.DEFINE XEND    $AA
.DEFINE NEG     $AC
.DEFINE QDD     $AE
.DEFINE QDL     $B0
.DEFINE INFO    $B2
.DEFINE TX      $B4
.DEFINE TZ      $B6
.DEFINE OI      $B8
.DEFINE OV      $BC
.DEFINE OHQ     $BE
.DEFINE SCP     $C0
.DEFINE LASTLO  $C2
.DEFINE LASTBK  $C4
.DEFINE LASTRM  $C6
.DEFINE TLO     $C8
.DEFINE TBK     $CA
.DEFINE TRM     $CC
.DEFINE DA2     $CE
.DEFINE DB2     $D0
.DEFINE BI      $D2
.DEFINE BP      $D4
.DEFINE CM      $D6
.DEFINE BX0     $D8
.DEFINE BZ0     $DA
.DEFINE BX1     $DC
.DEFINE BZ1     $DE
.DEFINE NVB     $E0
; (PG1's billboard columns at a width other than 128: the canvas column's
; k d / VIEW_W as a whole part BBQ and a remainder BBR, and their steps
; per column BBA, BBB: 2d = VIEW_W BBA + BBB)
.DEFINE BBQ     $E2
.DEFINE BBR     $E4
.DEFINE BBA     $E6
.DEFINE BBB     $E8

; CALL label: call a subroutine (R11 = the return address, after the NOP)
.MACRO CALL
    LINK #4
    IWT R15,#\1
    NOP
.ENDM

; GO label: jump (no return)
.MACRO GO
    IWT R15,#\1
    NOP
.ENDM

; ======================================================================
; the routines (each pass has its own copies)

; fbaddr: R9 a row, CX a column -> R1 = FB + FB_OFF(CX, R9), R8 = R9 &
; (FB_GROUP - 1): the row within its group (R9 & 31; tiled R9 & 7). (R0)
; (colofs(x) + rowofs(row) as draw.s has them: (x >> 3) * 768 + (x & 7) *
; 32 + (row >> 5) * 256 + (row & 31), or tiled (x >> 3) * TILE_ROWS * 64 +
; (x & 7) * 8 + (row >> 3) * 64 + (row & 7))
.MACRO M_FBADDR
    LMS R0,(CX)
    LSR
    LSR
    LSR
.IF FB_TILED == 1
    UMULT #TILE_ROWS
    SWAP
    LSR
    LSR                         ; (x >> 3) * TILE_ROWS * 64: the strip's tiles
.ELSE
    UMULT #3
    SWAP                        ; (x >> 3) * 3 * 256: the strip's 3 blocks of 256 bytes
.ENDIF
    MOVE R1,R0
    LMS R0,(CX)
    AND #7
    ADD R0
    ADD R0
    ADD R0                      ; (tiled: + (x & 7) * 8)
.IF FB_TILED == 0
    ADD R0
    ADD R0                      ; + (x & 7) * 32
.ENDIF
    ADD R1
    MOVE R1,R0
    MOVE R0,R9
    LSR
    LSR
    LSR
.IF FB_TILED == 1
    SWAP
    LSR
    LSR                         ; + (row >> 3) * 64: its tile
.ELSE
    LSR
    LSR
    SWAP                        ; + (row >> 5) * 256: its 32-row block
.ENDIF
    ADD R1
    IWT R1,#FB
    ADD R1
    MOVE R1,R0
    MOVE R0,R9
    IBT R8,#FB_GROUP-1
    AND R8
    MOVE R8,R0                  ; R8 = the row within its group
    ADD R1
    MOVE R1,R0                  ; R1 = FB + FB_OFF(CX, row)
.ENDM

; timesw: R0 = R0 x VIEW_W (16 bits, wrapping), R2 holding R0's value on
; entry: shift and add from the top bit down (seven doublings at 128 wide)
.MACRO M_TIMESW
.REPT W_TOP INDEX I
    ADD R0
.IF ((VIEW_W >> (W_TOP - 1 - I)) & 1) == 1
    ADD R2
.ENDIF
.ENDR
.ENDM

; udiv: R1:R2 / R3 -> R2 quotient, R1 remainder (R1 < R3 on entry)
; (R0, R12, R13) - (R1:R2 is a 32-bit number: R1 the high word; restoring division, 16 steps)
.MACRO M_UDIV
\1:
    IBT R12,#16                 ; 16 quotient bits
    MOVE R13,R15                ; the loop body starts at the next instruction
    WITH R2
    ADD R2                      ; shift R1:R2 left one bit
    WITH R1
    ROL
    BCS @sub                    ; a bit fell out of R1: the divisor surely fits
    NOP
    FROM R1
    CMP R3                      ; R1 >= R3?
    BCC @next
    NOP
@sub:
    WITH R1
    SUB R3                      ; subtract the divisor
    INC R2                      ; and set the quotient bit
@next:
    LOOP                        ; 16 times
    NOP
    JMP R11                     ; return
    NOP
.ENDM

; toview: world (R1, R2) -> view R1 = d, R2 = l (R0, R4, R6-R10)
; (the camera's sums: d = (rx FWX + rz FWZ) >> 8, l = (rx RTX + rz RTZ) >> 8,
; with rx, rz relative to the camera; as view.c's fx_view)
.MACRO M_TOVIEW
\1:
    LMS R0,(VX)
    WITH R1
    SUB R0                      ; rx
    LMS R0,(VZ)
    WITH R2
    SUB R0                      ; rz
    LMS R6,(FWX)
    FROM R1
    TO R7
    LMULT                       ; R7:R4 = rx FWX
    MOVE R8,R4
    LMS R6,(FWZ)
    FROM R2
    TO R9
    LMULT                       ; R9:R4 = rz FWZ
    WITH R8
    ADD R4
    WITH R7                     ; add the two 32-bit products (R7:R8)
    ADC R9
    WITH R7
    ; (SWAP then MERGE: bits 8-23 of the sum, i.e. sum >> 8)
    SWAP
    TO R10
    MERGE                       ; d
    LMS R6,(RTX)
    FROM R1
    TO R7
    LMULT
    MOVE R8,R4
    LMS R6,(RTZ)
    FROM R2
    TO R9
    LMULT
    WITH R8
    ADD R4
    WITH R7
    ADC R9
    WITH R7
    SWAP
    TO R2
    MERGE                       ; l
    MOVE R1,R10
    JMP R11
    NOP
.ENDM

; copycol: R12 bytes (>= 1) from the ROM at R10:R5 to RAM at R1, through
; the remap at R6 (255 stays 255) (R0, R9, R13, R14)
; (the ROM address's offset wraps into the next bank)
; (copies a texture or sprite column into BUF, remapped to palette slots)
.MACRO M_COPYCOL
\1:
    FROM R10
    ROMB                        ; ROMBR = R10
    MOVE R14,R5
    IWT R9,#255                 ; the hole colour
    MOVE R13,R15
@l:
    GETB                        ; R0 = ROM byte at ROMBR:R14
    INC R14
    BEQ @wrap                   ; offset wrapped to 0: next bank
    NOP
@back:
    CMP R9
    BEQ @hole                   ; a hole stays 255
    NOP
    ADD R6                      ; R0 = the remap's byte for it
    LDB (R0)
@hole:
    STB (R1)                    ; store at R1
    LOOP                        ; R12 times
    INC R1
    JMP R11
    NOP
@wrap:
    INC R10
    FROM R10
    ROMB
    IBT R14,#0
    BRA @back
    NOP
.ENDM

; drawcol: R1 the picture at the first row, R2 v (8.8), R3 dv, R7 rows
; (>= 1), R8 the first row & 31, R10 0: a wall, else a sprite (255 a hole)
; (R0, R5, R9, R12, R13)
; (draws R7 rows of a column into FB, reading BUF[v >> 8] and stepping v
; by dv per row; FB holds a column 32 rows at a time, so it goes in
; segments to the next multiple of 32 rows, then skips 224 bytes to the
; same column of the next 32-row group - tiled, 8 rows at a time and 56
; bytes: FB_GROUP, FB_SKIP; R8 is the first row & (FB_GROUP - 1))
.MACRO M_DRAWCOL
\1:
    IWT R5,#BUF
    IWT R9,#255
@seg:
    IBT R0,#FB_GROUP            ; rows left in this 32-row group
    SUB R8
    FROM R7
    CMP R0
    BCS @keep                   ; enough rows left for the whole group?
    NOP
    MOVE R0,R7                  ; no: only the rows left
@keep:
    MOVE R12,R0                 ; R12 = this segment's rows
    WITH R7
    SUB R0                      ; R7 = rows after it
    MOVES R10,R10
    BNE @spr                    ; a sprite: holes are skipped
    NOP
    MOVE R13,R15
    FROM R2
    HIB                         ; the texture row (v's whole part)
    ADD R5                      ; in BUF
    LDB (R0)
    STB (R1)
    INC R1
    WITH R2
    ADD R3
    LOOP                        ; next row
    NOP
    BRA @next
    NOP
@spr:
    MOVE R13,R15
    FROM R2
    HIB
    ADD R5
    LDB (R0)
    CMP R9
    BEQ @skip
    NOP
    STB (R1)
@skip:
    INC R1
    WITH R2
    ADD R3
    LOOP
    NOP
@next:
    MOVES R7,R7                 ; any rows left?
    BEQ @done
    NOP
    IWT R0,#FB_SKIP             ; on to the next 32-row group: + 256 - 32 (FB_SKIP)
    WITH R1
    ADD R0
    IBT R8,#0
    BRA @seg
    NOP
@done:
    JMP R11
    NOP
.ENDM

; column: draws column CX, height HH, from BUF (SPRF: a sprite's)
; \2 udiv, \3 drawcol
; (the column's rows are those of view.c: it starts at y0 = 44 - (7 h + 8)
; / 16, rows above the view (y0 < 0) skipped with v advanced to match,
; and stops at row 88 (VIEW_HALF_H and VIEW_H at any size); then the
; picture's address of (CX, first row).
; Clobbers R0-R13; T1.)
.MACRO M_COLUMN
\1:
    SMS (RCOLUMN),R11
    ; dv = 16384 / h
    IBT R1,#0
    IWT R2,#16384
    LMS R3,(HH)
    CALL \2                     ; R2 = 16384 / h: dv (8.8)
    SMS (T1),R2
    ; y0 = 44 - (7 h + 8) >> 4
    LMS R0,(HH)
    MOVE R1,R0                  ; 7 h: 8 h - h (ADD R0 three times doubles 3 times)
    ADD R0
    ADD R0
    ADD R0
    SUB R1
    ADD #8
    LSR
    LSR
    LSR
    LSR
    MOVE R1,R0
    IBT R0,#VIEW_HALF_H
    SUB R1
    MOVE R5,R0                  ; R5 = y0
    ; k0 = max(0, -y0); first = y0 + k0; v = dv / 2 + k0 dv
    IBT R6,#0
    MOVES R5,R5
    BPL @k0
    NOP
    IBT R0,#0
    SUB R5
    MOVE R6,R0                  ; R6 = k0 = -y0 (rows above the view)
@k0:
    LMS R0,(T1)
    TO R2
    LMULT                       ; R4 = k0 dv (low word)
    LMS R0,(T1)
    LSR
    TO R2
    ADD R4                      ; R2 = v
    ; first (R9), rows = min(88, y0 + h) - first
    FROM R5
    TO R9
    ADD R6                      ; R9 = first
    LMS R0,(HH)
    ADD R5                      ; y0 + h
    IBT R1,#VIEW_H
    CMP R1
    BLT @lt
    NOP
    MOVE R0,R1
@lt:
    TO R7                       ; R7 = rows
    SUB R9
    ; the picture: colofs(x) + rowofs(first)
    M_FBADDR                    ; R1 = FB + FB_OFF(CX, first), R8 = first & 31
    LMS R3,(T1)
    LMS R10,(SPRF)
    CALL \3                     ; draw it
    LMS R11,(RCOLUMN)
    JMP R11
    NOP
.ENDM

; sdivp: R1:R2 (signed 32) / R3 (> 0) -> R2 (toward 0; |q| < 65536)
; (R1:R2 negated if negative, divided, the quotient negated back; uses
; NEG; clobbers R0-R3, R12, R13)
.MACRO M_SDIVP
\1:
    SMS (RSDIV),R11
    IBT R0,#0
    SMS (NEG),R0
    MOVES R1,R1
    BPL @pos
    NOP
    IBT R0,#0
    SUB R2                      ; negate R1:R2 (32 bits)
    MOVE R2,R0
    IBT R0,#0
    SBC R1
    MOVE R1,R0
    IBT R0,#1
    SMS (NEG),R0
@pos:
    CALL \2                     ; the unsigned division
    LMS R0,(NEG)
    SUB #0
    BEQ @done
    NOP
    IBT R0,#0
    SUB R2
    MOVE R2,R0
; (R1 is clobbered by the division)
@done:
    LMS R11,(RSDIV)
    JMP R11
    NOP
.ENDM

; clip: R1 = dA, R2 = lA, R3 = dB, R5 = lB (dA < 1 <= dB) ->
; R0 = lA + (lB - lA) (1 - dA) / (dB - dA)
; (the wall's end at depth NEAR = 1: the lateral offset where the line
; from A to B crosses depth 1; as view.c's project; clobbers R1-R6, T2)
.MACRO M_CLIP
\1:
    SMS (RCLIP),R11
    SMS (T2),R2
    FROM R5
    TO R6
    SUB R2                      ; R6 = lB - lA
    IBT R0,#1
    SUB R1                      ; R0 = 1 - dA
    FROM R3
    TO R3
    SUB R1                      ; R3 = dB - dA
    TO R1
    LMULT                       ; R1:R4 = (1 - dA)(lB - lA)
    MOVE R2,R4
    CALL \2                     ; the division (R2 the quotient)
    LMS R0,(T2)
    ADD R2                      ; + lA
    LMS R11,(RCLIP)
    JMP R11
    NOP
.ENDM

; fxcol: R1 = d, R2 = l -> R0 the first column right of the point (0..128)
; (view.c's fx_col: (64 << 8) + (l << 14) / d, rounded: + 127, >> 8 - at
; any width (F << 8) + l F 256 / d, F = VIEW_FOCAL, 0 .. VIEW_W; clobbers
; R0-R4, R6, R12, R13, NEG)
.MACRO M_FXCOL
\1:
    SMS (RFXCOL),R11
    FROM R2
    CMP R1
    BGE @right                  ; l >= d: right of the view
    NOP
    FROM R2
    ADD R1
    BMI @left                   ; l + d <= 0: left of it
    NOP
    BEQ @left
    NOP
    ; |l| < d: q = |l| F 256 / d (< F 256)
    MOVE R3,R1
    IBT R0,#0
    SMS (NEG),R0
    MOVES R1,R2
    BPL @p
    NOP
    IBT R0,#0
    SUB R1
    MOVE R1,R0
    IBT R0,#1
    SMS (NEG),R0
@p:
    IWT R6,#VIEW_FOCAL*256
    FROM R1
    TO R1
    LMULT                       ; R1:R4 = |l| F 256 (|l| << 14)
    MOVE R2,R4
    CALL \2                     ; / d
    LMS R0,(NEG)
    SUB #0
    BEQ @pp
    NOP
    IBT R0,#0
    SUB R2
    MOVE R2,R0
@pp:
    IWT R0,#VIEW_FOCAL*256+127  ; 64 x 256 + 127 (the rounding)
    ADD R2
    HIB                         ; >> 8
    LMS R11,(RFXCOL)
    JMP R11
    NOP
@right:
    IWT R0,#VIEW_W
    JMP R11
    NOP
@left:
    IBT R0,#0
    JMP R11
    NOP
.ENDM

; sxq: R1 = d, R2 = L (|L| < d) -> R0 = 16384 + (L << 14) / d
; (the screen x in 8.8, unrounded: view.c's screen_x, for billboards - at
; any width (F << 8) + L F 256 / d; shares RFXCOL with fxcol; clobbers
; R0-R4, R6, R12, R13, NEG)
.MACRO M_SXQ
\1:
    SMS (RFXCOL),R11
    MOVE R3,R1
    IBT R0,#0
    SMS (NEG),R0
    MOVES R1,R2
    BPL @p
    NOP
    IBT R0,#0
    SUB R1
    MOVE R1,R0
    IBT R0,#1
    SMS (NEG),R0
@p:
    IWT R6,#VIEW_FOCAL*256
    FROM R1
    TO R1
    LMULT                       ; R1:R4 = |L| F 256 (|L| << 14)
    MOVE R2,R4
    CALL \2                     ; / d
    LMS R0,(NEG)
    SUB #0
    BEQ @pp
    NOP
    IBT R0,#0
    SUB R2
    MOVE R2,R0
@pp:
    IWT R0,#VIEW_FOCAL*256      ; 64 x 256
    ADD R2
    LMS R11,(RFXCOL)
    JMP R11
    NOP
.ENDM

; hqat: R0 = x -> R0 = hq at column x (the quad: QDD QDL, QDN QK)
; (view.c's hq_at: 512 num2 / c, both shortened as c was: QK halvings;
; 0 if negative, $FFFF if it does not fit 16 bits. Clobbers R0-R8,
; R12, R13)
.MACRO M_HQAT
\1:
    SMS (RHQAT),R11
    ; num2 = 128 dl - (2x + 1 - 128) dd
    ADD R0                      ; 2x
    IWT R1,#(1-VIEW_W)&$FFFF
    ADD R1                      ; 2x + 1 - 128
    MOVE R6,R0
    LMS R0,(QDD)
    TO R1
    LMULT                       ; R1:R4 = (2x + 1 - 128) dd
    LMS R0,(QDL)
    MOVE R2,R0
    M_TIMESW                    ; dl x 128 (VIEW_W)
    SUB R4                      ; num2 = 128 dl - (that), 16 bits
    ; n = num2 << 9 as R7:R8
    MOVE R7,R0
    ADD R0
    LOB
    TO R8
    SWAP
    WITH R7
    ASR
    WITH R7
    ASR
    WITH R7
    ASR
    WITH R7
    ASR
    WITH R7
    ASR
    WITH R7
    ASR
    WITH R7
    ASR
    ; shifted as q_c was
    LMS R12,(QK)
    MOVES R12,R12
    BEQ @shifted
    NOP
    MOVE R13,R15                ; QK times:
    WITH R7
    ASR                         ; shift R7:R8 right (arithmetic)
    WITH R8
    ROR
    LOOP
    NOP
@shifted:
    LMS R3,(QDN)
    MOVE R0,R7
    OR R8                       ; n == 0: hq 0
    BEQ @zero
    NOP
    MOVE R0,R7
    XOR R3                      ; n and c of different signs: hq < 0
    BMI @zero
    NOP
    MOVES R7,R7
    BPL @npos                   ; make both positive
    NOP
    IBT R0,#0
    SUB R8
    MOVE R8,R0
    IBT R0,#0
    SBC R7
    MOVE R7,R0
    IBT R0,#0
    SUB R3
    MOVE R3,R0
@npos:
    FROM R7
    CMP R3                      ; the quotient would be >= 65536
    BCS @max
    NOP
    MOVE R1,R7
    MOVE R2,R8
    CALL \2                     ; R2 = n / c
    MOVE R0,R2
    LMS R11,(RHQAT)
    JMP R11
    NOP
@max:
    IWT R0,#$FFFF
    LMS R11,(RHQAT)
    JMP R11
    NOP
@zero:
    IBT R0,#0
    LMS R11,(RHQAT)
    JMP R11
    NOP
.ENDM

; hqramp: QHQ = (hq(x0) << 8) + 128, QST = ((hq(x1 - 1) - it) << 8) / (n - 1)
; (view.c's hq_ramp, with the 16.8 step's division done in two 16-bit
; steps: the high byte first, then the rest with its remainder. Clobbers
; R0-R13, T1, T3, OHQ, NEG)
.MACRO M_HQRAMP
\1:
    SMS (RRAMP),R11
    LMS R0,(QX0)
    CALL \2                     ; hq at the first column
    SMS (OHQ),R0
    MOVE R1,R0
    HIB
    SMS (QHQHI),R0
    MOVE R0,R1
    LOB
    SWAP
    IWT R1,#128
    OR R1
    SMS (QHQLO),R0              ; QHQ low word: (h0 & 255) << 8 | 128
    IBT R0,#0
    SMS (QSTLO),R0
    SMS (QSTHI),R0
    LMS R0,(QX1)
    LMS R1,(QX0)
    SUB R1
    DEC R0
    SMS (T3),R0                 ; T3 = n - 1
    SUB #0
    BMI @done                   ; a single column: no step
    NOP
    BEQ @done
    NOP
    LMS R0,(QX1)
    DEC R0
    CALL \2                     ; hq at the last column
    IBT R1,#0
    SMS (NEG),R1
    LMS R1,(OHQ)
    SUB R1
    BCS @dpos                   ; h1 - h0 >= 0
    NOP
    IBT R1,#0
    WITH R1
    SUB R0
    MOVE R0,R1                  ; negative: work with |h1 - h0|
    IBT R1,#1
    SMS (NEG),R1
@dpos:
    SMS (T1),R0
    HIB
    MOVE R2,R0
    IBT R1,#0
    LMS R3,(T3)
    CALL \3                     ; (diff >> 8) / (n - 1): the step's high word
    SMS (QSTHI),R2
    LMS R0,(T1)
    LOB
    TO R2
    SWAP
    CALL \3                     ; (remainder:low byte << 8) / (n - 1): its low word
    LMS R1,(QSTHI)
    LMS R0,(NEG)
    SUB #0
    BEQ @store
    NOP
    IBT R0,#0
    SUB R2
    MOVE R2,R0
    IBT R0,#0
    SBC R1
    MOVE R1,R0                  ; negate the 32-bit step
@store:
    SMS (QSTLO),R2
    SMS (QSTHI),R1
@done:
    LMS R11,(RRAMP)
    JMP R11
    NOP
.ENDM

; t8of: R1 = m2, R2 = num2 -> R0 = t (0..255) (view.c's t8_of)
; (clobbers R0-R3, R12, R13)
.MACRO M_T8OF
\1:
    SMS (RT8),R11
    MOVES R2,R2                 ; num2 < 0: negate both
    BPL @pos
    NOP
    IBT R0,#0
    SUB R2
    MOVE R2,R0
    IBT R0,#0
    SUB R1
    MOVE R1,R0
@pos:
    MOVES R1,R1
    BMI @zero                   ; m2 <= 0: 0
    NOP
    BEQ @zero
    NOP
    FROM R1
    CMP R2
    BGE @full                   ; m2 >= num2: 255
    NOP
    IWT R3,#255
@half:
    FROM R2
    CMP R3                      ; halve both until num2 fits 8 bits
    BEQ @small
    NOP
    BCC @small
    NOP
    WITH R2
    LSR
    WITH R1
    LSR
    BRA @half
    NOP
@small:
    MOVES R2,R2
    BEQ @zero
    NOP
    MOVE R3,R2
    MOVE R0,R1
    LOB
    TO R2
    SWAP
    FROM R1
    TO R1
    HIB
    CALL \2                     ; (m2 << 8) / num2
    MOVE R0,R2
    IWT R1,#255
    CMP R1
    BCC @ok                     ; held to 255
    NOP
    MOVE R0,R1
@ok:
    LMS R11,(RT8)
    JMP R11
    NOP
@zero:
    IBT R0,#0
    LMS R11,(RT8)
    JMP R11
    NOP
@full:
    IWT R0,#255
    LMS R11,(RT8)
    JMP R11
    NOP
.ENDM

; m2num2: R0 = x, the quad QDA QLA QDD QDL -> R1 = m2, R2 = num2
; (view.c's m2_at and num2_at together: with k = 2x + 1 - 128,
; m2 = k da - 128 la and num2 = 128 dl - k dd, 16 bits each, wrapping;
; 128 is VIEW_W. Clobbers R0, R4, R6)
.MACRO M_M2NUM2
\1:
    ADD R0
    IWT R1,#(1-VIEW_W)&$FFFF    ; R0 = k = 2x + 1 - 128 ($FF81 = -127)
    ADD R1
    MOVE R6,R0
    LMS R0,(QDA)
    TO R1
    LMULT                       ; R1:R4 = k da
    MOVE R1,R4
    LMS R0,(QLA)
    MOVE R2,R0
    M_TIMESW
    MOVE R2,R0                  ; R2 = 128 la (7 doublings)
    WITH R1
    SUB R2                      ; R1 = m2
    LMS R0,(QDD)
    TO R2
    LMULT                       ; R2:R4 = k dd
    LMS R0,(QDL)
    MOVE R2,R0
    M_TIMESW
    TO R2
    SUB R4                      ; R2 = 128 dl - k dd = num2
    JMP R11
    NOP
.ENDM


; colsetup: column CX, height HH -> R1 the picture at the first row, R2 v,
; R3 dv, R7 rows, R8 the first row & 31 (\2 udiv)
; (from the precomputed column table instead of dividing: per height h,
; 8 bytes at TABLES:8h (see tools/snes_tables.py): first row, rows,
; v0 (8.8, little-endian), dv. Clobbers R0, R9, R14; ROMBR = TABLES)
.MACRO M_COLSETUP
\1:
    ; the table (tools/snes_tables.py, bank TABLES): first row, rows, v0, dv
    IWT R0,#TABLES
    ROMB                        ; ROMBR = the table's bank
    LMS R0,(HH)
    ADD R0
    ADD R0
    ADD R0
    MOVE R14,R0                 ; R14 = 8 h: the entry
    GETB
    MOVE R9,R0                  ; R9 = first row
    INC R14
    TO R7
    GETB                        ; R7 = rows
    INC R14
    TO R2
    GETB
    INC R14
    FROM R2
    TO R2
    GETBH                       ; R2 = v (low byte, then GETBH the high)
    INC R14
    TO R3
    GETB
    INC R14
    FROM R3
    TO R3
    GETBH                       ; R3 = dv
    ; the picture: colofs(x) + rowofs(first)
    M_FBADDR                    ; R1 = FB + FB_OFF(CX, first), R8 = first & 31
    JMP R11
    NOP
.ENDM

; drawwall: a wall's column (R1 R2 R3 R7 R8 as colsetup's) from BUF (R5)
; or, \2 = 1, straight from the ROM: the texture column at R5 (ROMBR set),
; the remap at R6
; (in segments of FB_GROUP rows, as drawcol; \2 = 1 reads each texel from the
; ROM (no copy into BUF first: for columns too short to be worth it).
; Clobbers R0, R1, R2, R7, R8, R12, R13, R14 (\2 = 1))
.MACRO M_DRAWWALL
\1:
@seg:
    IBT R0,#FB_GROUP
    SUB R8                      ; rows left in this 32-row group
    FROM R7
    CMP R0
    BCS @keep
    NOP
    MOVE R0,R7
@keep:
    MOVE R12,R0
    WITH R7
    SUB R0                      ; R7 = rows after this segment
    MOVE R13,R15
.IF \2 == 0
    FROM R2
    HIB
    ADD R5                      ; BUF + the texture row
    LDB (R0)
    STB (R1)
    INC R1
    WITH R2
    ADD R3
    LOOP
    NOP
.ELSE
    FROM R2
    HIB
    ADD R5
    MOVE R14,R0                 ; R14 = the texel's ROM offset (fetch starts)
    WITH R2
    ADD R3                      ; (the next v while the ROM byte is fetched)
    GETB
    ADD R6                      ; remap: R6 + texel
    LDB (R0)                    ; the slot
    STB (R1)
    LOOP
    INC R1                      ; (delay slot: runs every time)
.ENDIF
    MOVES R7,R7
    BEQ @done
    NOP
    IWT R0,#FB_SKIP             ; next 32-row group: + 256 - 32 (FB_SKIP)
    WITH R1
    ADD R0
    IBT R8,#0
    BRA @seg
    NOP
@done:
    JMP R11
    NOP
.ENDM

; fastcopy: 64 bytes from the ROM at R10:R5 (not across a bank) to BUF,
; through the remap at R6
.MACRO M_FASTCOPY
\1:
    FROM R10
    ROMB                        ; ROMBR = R10
    MOVE R14,R5
    IWT R1,#BUF
    IBT R12,#64
    MOVE R13,R15
    GETB                        ; texel
    INC R14
    ADD R6                      ; its slot from the remap
    LDB (R0)
    STB (R1)
    LOOP                        ; 64 times
    INC R1
    JMP R11
    NOP
.ENDM

; dof / lof: world (R1, R2) -> R0 = its depth / lateral (R1 R2 relative;
; R4, R6-R9)
; (\2, \3: FWX, FWZ for the depth; RTX, RTZ for the lateral offset.
; The 32-bit sum's bits 8-23 (sum >> 8), as fx_view)
.MACRO M_DOF
\1:
    LMS R0,(VX)
    WITH R1
    SUB R0
    LMS R0,(VZ)
    WITH R2
    SUB R0
    LMS R6,(\2)
    FROM R1
    TO R7
    LMULT                       ; R7:R4 = rx \2
    MOVE R8,R4
    LMS R6,(\3)
    FROM R2
    TO R9
    LMULT                       ; R9:R4 = rz \3
    WITH R8
    ADD R4
    WITH R7
    ADC R9                      ; R7:R8 = the sum
    WITH R7
    SWAP
    MERGE                       ; R0 = sum >> 8
    JMP R11
    NOP
.ENDM

; ======================================================================
; The two entry points the CPU starts the GSU at: 0 draws a picture, 4
; loads a new map's walls (each GO is 4 bytes).
start:
    GO main                     ; 0: the picture
    GO mapload                  ; 4: a new map's walls (below)

; main: the passes in order. STOPAT (set by the CPU) stops after a pass
; for testing: 0 after PA0, 1 after PA, ... 9 after PG1; any other
; value runs them all. Ends with STOP: the CPU, waiting on SFR's GO
; bit, then takes the picture.
main:
    CALL p0                     ; clear the picture and the column tables
    CALL pa0                    ; the blocks in view -> VBL
    LMS R0,(STOPAT)
    IBT R1,#0
    CMP R1
    BNE main_c1
    NOP
    GO halt
main_c1:
    CALL pa                     ; their walls -> CAND
    LMS R0,(STOPAT)
    IBT R1,#1
    CMP R1
    BNE main_c2
    NOP
    GO halt
main_c2:
    CALL pb                     ; CAND -> VW
    LMS R0,(STOPAT)
    IBT R1,#2
    CMP R1
    BNE main_c3
    NOP
    GO halt
main_c3:
    CALL pc                     ; the nearest wall per column
    LMS R0,(STOPAT)
    IBT R1,#3
    CMP R1
    BNE main_c4
    NOP
    GO halt
main_c4:
    CALL pd                     ; its texture column per column
    LMS R0,(STOPAT)
    IBT R1,#4
    CMP R1
    BNE main_c5
    NOP
    GO halt
main_c5:
    CALL pe                     ; draw the walls
    LMS R0,(STOPAT)
    IBT R1,#5
    CMP R1
    BNE main_c6
    NOP
    GO halt
main_c6:
    CALL pf                     ; the things -> PT
    LMS R0,(STOPAT)
    IBT R1,#6
    CMP R1
    BNE main_c7
    NOP
    GO halt
main_c7:
    CALL pf3                    ; the decals' quads
    LMS R0,(STOPAT)
    IBT R1,#7
    CMP R1
    BNE main_c8
    NOP
    GO halt
main_c8:
    CALL ps                     ; sort PT far to near
    LMS R0,(STOPAT)
    IBT R1,#8
    CMP R1
    BNE main_c9
    NOP
    GO halt
main_c9:
    CALL pg1                    ; PT -> sprite columns (SCOL)
    LMS R0,(STOPAT)
    IBT R1,#9
    CMP R1
    BNE main_c10
    NOP
    GO halt
main_c10:
    CALL pg2                    ; draw them
halt:
    STOP                        ; the GSU stops; the CPU sees it
    NOP

; ----------------------------------------------------------------------
; P0: the picture to 0, COLHQ to 0, COLW to 255
p0:
    CACHE                       ; load this pass into the cache
    IWT R1,#FB
    IWT R12,#FB_BYTES/2         ; FB_BYTES / 2 words: the picture (6144 at 128 wide)
    IBT R0,#0                   ; colour 0 (the backdrop)
    MOVE R13,R15
    STW (R1)                    ; STW: 2 bytes; the INCs (one in the delay slot) step 2
    INC R1
    LOOP
    INC R1
    IWT R1,#COLHQ
    IWT R12,#VIEW_W             ; R0 still 0: no wall's hq
    MOVE R13,R15
    STW (R1)
    INC R1
    LOOP
    INC R1
    IWT R1,#COLW
    IWT R12,#VIEW_W
    IWT R0,#255                 ; no wall
    MOVE R13,R15
    STB (R1)
    LOOP
    INC R1
    JMP R11
    NOP

; ----------------------------------------------------------------------
; PA0: the blocks of walls that can be in view -> VBL
pa0:
    CACHE
    GO pa0_main
; corner: a block's corner (R1, R2): CM's bits cleared unless it is
; behind (1), past the right edge (2), past the left (4) - with a unit to
; spare, so every wall in a block with a bit left is left out by PA's tests
; (In: R1, R2 a world point. View-space sums kept in 8.8, 32 bits: d in
; R7:R8, l in R9:R10; no shift, no division. Clobbers R0-R4, R6-R10,
; R12, R13, CM)
corner:
    LMS R0,(VX)
    WITH R1
    SUB R0
    LMS R0,(VZ)
    WITH R2
    SUB R0
    LMS R6,(FWX)
    FROM R1
    TO R7
    LMULT                       ; R7:R4 = rx FWX
    MOVE R8,R4
    LMS R6,(FWZ)
    FROM R2
    TO R9
    LMULT                       ; R9:R4 = rz FWZ
    WITH R8
    ADD R4
    WITH R7
    ADC R9                      ; R7:R8 = d (8.8)
    LMS R6,(RTX)
    FROM R1
    TO R9
    LMULT                       ; R9:R4 = rx RTX
    MOVE R10,R4
    LMS R6,(RTZ)
    FROM R2
    TO R3
    LMULT                       ; R3:R4 = rz RTZ
    WITH R10
    ADD R4
    WITH R9
    ADC R3                      ; R9:R10 = l (8.8)
    ; behind: d < 256
    IWT R4,#256
    FROM R8
    SUB R4
    IBT R4,#0
    FROM R7
    SBC R4                      ; d - 256, high word
    BMI @b                      ; d < 1 unit: behind, the bit stays
    NOP
    LMS R0,(CM)
    BIC #1                      ; not behind
    SMS (CM),R0
@b:
    ; right: l - d >= 256
    FROM R10
    TO R12
    SUB R8
    FROM R9
    TO R13
    SBC R7                      ; R13:R12 = l - d
    IWT R4,#256
    FROM R12
    SUB R4
    IBT R4,#0
    FROM R13
    SBC R4                      ; (l - d) - 256
    BPL @r                      ; >= 0: past the right edge, the bit stays
    NOP
    LMS R0,(CM)
    BIC #2
    SMS (CM),R0
@r:
    ; left: l + d < 256
    FROM R10
    TO R12
    ADD R8
    FROM R9
    TO R13
    ADC R7                      ; R13:R12 = l + d
    IWT R4,#256
    FROM R12
    SUB R4
    IBT R4,#0
    FROM R13
    SBC R4
    BMI @l                      ; < 0: past the left edge, the bit stays
    NOP
    LMS R0,(CM)
    BIC #4
    SMS (CM),R0
@l:
    JMP R11
    NOP

; pa0_main: for each of the 64 blocks with walls, tests its box's 4
; corners; a block whose CM still has a bit set (every corner behind,
; or every corner past the same edge) is left out; the rest are
; listed in VBL (NVB of them).
pa0_main:
    SMS (RPH),R11
    IBT R0,#0
    SMS (BI),R0                 ; BI: the block number
    SMS (NVB),R0                ; NVB: blocks in view
pa0_blk:
    LMS R0,(BI)
    IBT R1,#64
    CMP R1                      ; all 64 done?
    BNE @b
    NOP
    LMS R11,(RPH)
    JMP R11
    NOP
@b:
    UMULT #12                   ; its BLK record (12 bytes)
    IWT R1,#BLK
    ADD R1
    MOVE R3,R0
    INC R3
    INC R3
    LDW (R3)                    ; its wall count
    SUB #0
    BEQ @skip                   ; no walls: skip
    NOP
    IBT R0,#7                   ; CM: all three bits, cleared by any corner that fails a test
    SMS (CM),R0
    INC R3
    INC R3
    LDW (R3)
    SMS (BX0),R0
    INC R3
    INC R3
    LDW (R3)
    SMS (BZ0),R0
    INC R3
    INC R3
    LDW (R3)
    SMS (BX1),R0
    INC R3
    INC R3
    LDW (R3)
    SMS (BZ1),R0
    LMS R1,(BX0)
    LMS R2,(BZ0)
    CALL corner                 ; the 4 corners
    LMS R1,(BX1)
    LMS R2,(BZ0)
    CALL corner
    LMS R1,(BX0)
    LMS R2,(BZ1)
    CALL corner
    LMS R1,(BX1)
    LMS R2,(BZ1)
    CALL corner
    LMS R0,(CM)
    SUB #0
    BNE @skip                   ; a bit left: the whole block is out of view
    NOP
    LMS R0,(NVB)
    IWT R1,#VBL
    ADD R1
    MOVE R1,R0
    LMS R0,(BI)
    STB (R1)                    ; VBL[NVB] = BI
    LMS R0,(NVB)
    INC R0
    SMS (NVB),R0
@skip:
    LMS R0,(BI)
    INC R0
    SMS (BI),R0
    GO pa0_blk

; ----------------------------------------------------------------------
; PA: the walls of the blocks in view -> CAND (view space; the ones that may show)
; (each kept wall's record in CAND: da la db lb (words), tex, ua, du =
; ub - ua (bytes), 16 bytes in all. Up to MAX_CAND)
pa:
    CACHE
    GO pa_main
    M_DOF dof_a, FWX, FWZ       ; depth of (R1, R2): CALL dof_a
    M_DOF lof_a, RTX, RTZ       ; lateral offset: CALL lof_a
pa_main:
    SMS (RPH),R11
    IBT R0,#0
    SMS (NCAND),R0
    SMS (BI),R0
    IWT R5,#CAND                ; R5: where the next CAND record goes
pa_blk:
    LMS R0,(BI)
    LMS R1,(NVB)                ; all the blocks in view done?
    CMP R1
    BNE @b
    NOP
    GO pa_end
@b:
    IWT R1,#VBL
    ADD R1
    LDB (R0)                    ; the block's number
    UMULT #12                   ; its BLK record
    IWT R1,#BLK
    ADD R1
    MOVE R1,R0
    LDW (R1)                    ; R0 = its first wall (start)
    ADD R0
    ADD R0
    ADD R0
    ADD R0                      ; x 16 bytes
    IWT R3,#WALLS
    WITH R3
    ADD R0                      ; R3 = its first wall's record in WALLS
    INC R1
    INC R1
    LDW (R1)
    SMS (CNT),R0                ; CNT = its wall count
    LMS R0,(BI)
    INC R0
    SMS (BI),R0
pa_loop:
    LMS R0,(CNT)
    SUB #0
    BNE pa_go                   ; walls left in this block
    NOP
    GO pa_blk
pa_end:
    LMS R11,(RPH)
    JMP R11
    NOP
pa_go:
    DEC R0
    SMS (CNT),R0
    ; an open door's wall: not there
    FROM R3
    ADD #10                     ; the wall's line (word 5, low byte)
    LDB (R0)
    IBT R1,#64
    CMP R1
    BCS @closed                 ; >= 64 (255: a plain wall): no door
    NOP
    IWT R1,#OPEN
    ADD R1
    LDB (R0)
    AND #1                      ; bit 0 (L_OPEN): the door is open
    BEQ @closed
    NOP
    GO pa_next                  ; skip it
@closed:
    MOVE R1,R3
    LDW (R1)
    SMS (AX),R0
    INC R1
    INC R1
    LDW (R1)
    SMS (AZ),R0
    INC R1
    INC R1
    LDW (R1)
    SMS (BX),R0
    INC R1
    INC R1
    LDW (R1)
    SMS (BZ),R0
    ; both behind: not seen
    LMS R1,(AX)
    LMS R2,(AZ)
    CALL dof_a                  ; depth of A
    SMS (QDA),R0
    LMS R1,(BX)
    LMS R2,(BZ)
    CALL dof_a                  ; depth of B
    SMS (QDB),R0
    DEC R0
    BPL @infront                ; dB >= 1: in front
    NOP
    LMS R0,(QDA)
    DEC R0
    BPL @infront                ; dA >= 1: in front
    NOP
    GO pa_next                  ; both behind: skip
@infront:
    FROM R3
    ADD #8
    MOVE R1,R0
    LDW (R1)
    SMS (WTEX),R0               ; WTEX = texture slot (word 4)
    INC R1
    INC R1
    INC R1
    INC R1
    LDW (R1)
    SMS (WFL),R0                ; WFL = word 6: ua | 0x100 if two-sided
    LOB
    SMS (UA),R0
    INC R1
    INC R1
    LDW (R1)
    SMS (UB),R0                 ; UB = word 7
    ; the side it is seen from: (ax - vx)(bz - az) - (az - vz)(bx - ax)
    LMS R0,(BZ)
    LMS R1,(AZ)
    SUB R1
    MOVE R6,R0
    LMS R0,(AX)
    LMS R1,(VX)
    SUB R1
    TO R7
    LMULT                       ; R7:R4 = (ax - vx)(bz - az)
    MOVE R8,R4
    LMS R0,(BX)
    LMS R1,(AX)
    SUB R1
    MOVE R6,R0
    LMS R0,(AZ)
    LMS R1,(VZ)
    SUB R1
    TO R9
    LMULT                       ; R9:R4 = (az - vz)(bx - ax)
    WITH R8
    SUB R4
    WITH R7
    SBC R9                      ; R7:R8 = the cross product
    BMI @back                   ; < 0: seen from behind
    NOP
    BNE @front                  ; > 0: from the front
    NOP
    MOVES R8,R8
    BNE @front
    NOP
@back:
    LMS R0,(WFL)
    HIB
    LSR
    BCS @twosided               ; bit 8: two-sided
    NOP
    GO pa_next                  ; one-sided, from behind: skip
@twosided:
    LMS R0,(AX)                 ; two-sided: swap A and B (ends, u, depths)
    LMS R1,(BX)
    SMS (AX),R1
    SMS (BX),R0
    LMS R0,(AZ)
    LMS R1,(BZ)
    SMS (AZ),R1
    SMS (BZ),R0
    LMS R0,(UA)
    LMS R1,(UB)
    SMS (UA),R1
    SMS (UB),R0
    LMS R0,(QDA)
    LMS R1,(QDB)
    SMS (QDA),R1
    SMS (QDB),R0
@front:
    LMS R1,(AX)
    LMS R2,(AZ)
    CALL lof_a                  ; lateral of A
    SMS (QLA),R0
    LMS R1,(BX)
    LMS R2,(BZ)
    CALL lof_a                  ; lateral of B
    SMS (QLB),R0
    ; past the right edge: la >= da and lb >= db
    LMS R0,(QLA)
    LMS R1,(QDA)
    SUB R1                      ; la - da
    BLT @notright
    NOP
    LMS R0,(QLB)
    LMS R1,(QDB)
    SUB R1
    BGE @no                     ; lb >= db too: wholly right of the view
    NOP
@notright:
    ; past the left: la + da <= 0 and lb + db <= 0
    LMS R0,(QLA)
    LMS R1,(QDA)
    ADD R1
    BMI @l1
    NOP
    BNE @keep                   ; la + da > 0: not left
    NOP
@l1:
    LMS R0,(QLB)
    LMS R1,(QDB)
    ADD R1
    BMI @no
    NOP
    BNE @keep                   ; lb + db > 0: not left
    NOP
@no:
    GO pa_next                  ; outside the view: skip
@keep:
    MOVE R1,R5                  ; keep it: the CAND record
    LMS R0,(QDA)
    STW (R1)
    INC R1
    INC R1
    LMS R0,(QLA)
    STW (R1)
    INC R1
    INC R1
    LMS R0,(QDB)
    STW (R1)
    INC R1
    INC R1
    LMS R0,(QLB)
    STW (R1)
    INC R1
    INC R1
    LMS R0,(WTEX)
    STB (R1)
    INC R1
    LMS R0,(UA)
    STB (R1)
    INC R1
    LMS R2,(UA)
    LMS R0,(UB)
    SUB R2
    STB (R1)                    ; du = ub - ua (8 bits)
    IBT R0,#16
    WITH R5
    ADD R0                      ; the next record
    LMS R0,(NCAND)
    INC R0
    SMS (NCAND),R0
    IWT R1,#MAX_CAND
    CMP R1
    BNE pa_next                 ; CAND full?
    NOP
    GO pa_end                   ; stop looking
pa_next:
    IBT R0,#16
    WITH R3
    ADD R0                      ; the next wall's record
    GO pa_loop

; ----------------------------------------------------------------------
; PB: CAND -> VW: clipped at the near plane, its columns, q_c shortened
; (each kept wall's VW record, 24 bytes: da la dd dl (words), tex, ua,
; du, -, x0, x1, qdn, qk (words): see pb_store. Up to 254 walls)
pb:
    CACHE
    GO pb_main
    M_UDIV udiv_b               ; udiv_b: R1:R2 / R3
    M_SDIVP sdivp_b, udiv_b     ; sdivp_b: signed
    M_CLIP clip_b, sdivp_b      ; clip_b: the end at depth 1
    M_FXCOL fxcol_b, udiv_b     ; fxcol_b: a point's column
pb_main:
    SMS (RPH),R11
    IBT R0,#0
    SMS (NVW),R0
    IWT R0,#CAND
    SMS (CPTR),R0
    LMS R0,(NCAND)
    SMS (CNT),R0                ; CNT: candidates left
pb_loop:
    LMS R0,(CNT)
    SUB #0
    BNE pb_go
    NOP
pb_end:
    LMS R11,(RPH)
    JMP R11
    NOP
pb_go:
    DEC R0
    SMS (CNT),R0
    LMS R0,(NVW)
    IWT R1,#254
    CMP R1                      ; VW full (254)?
    BCS pb_end
    NOP
    LMS R3,(CPTR)               ; R3: the candidate's record
    LDW (R3)
    SMS (QDA),R0
    SMS (CDA),R0
    INC R3
    INC R3
    LDW (R3)
    SMS (QLA),R0
    SMS (CLA),R0
    INC R3
    INC R3
    LDW (R3)
    SMS (QDB),R0
    SMS (CDB),R0
    INC R3
    INC R3
    LDW (R3)
    SMS (QLB),R0
    SMS (CLB),R0
    IBT R0,#10
    WITH R3
    ADD R0
    SMS (CPTR),R3               ; on to the next record
    ; clip at the near plane (d = 1)
    LMS R0,(QDA)
    DEC R0
    BPL @anear                  ; dA >= 1: A needs no clipping
    NOP
    LMS R1,(QDA)
    LMS R2,(QLA)
    LMS R3,(QDB)
    LMS R5,(QLB)
    CALL clip_b                 ; clip A to depth 1 (R0 = its lateral there)
    SMS (CLA),R0
    IBT R0,#1
    SMS (CDA),R0
    BRA @clipped
    NOP
@anear:
    LMS R0,(QDB)
    DEC R0
    BPL @clipped                ; dB >= 1 too: nothing to clip
    NOP
    LMS R1,(QDB)
    LMS R2,(QLB)
    LMS R3,(QDA)
    LMS R5,(QLA)
    CALL clip_b                 ; clip B
    SMS (CLB),R0
    IBT R0,#1
    SMS (CDB),R0
@clipped:
    LMS R1,(CDA)
    LMS R2,(CLA)
    CALL fxcol_b                ; xA: A's first column
    SMS (XA),R0
    LMS R1,(CDB)
    LMS R2,(CLB)
    CALL fxcol_b                ; xB
    LMS R1,(XA)
    CMP R1
    BCS @ordered                ; in order: R1 = the left, R0 = the right
    NOP
    MOVE R2,R0
    MOVE R0,R1
    MOVE R1,R2
@ordered:
    CMP R1
    BEQ @no                     ; no column: skip the wall
    NOP
    BCS @cols
    NOP
@no:
    GO pb_loop
@cols:
    SMS (QX1),R0                ; QX0 .. QX1 - 1: its columns
    SMS (QX0),R1
    ; q_c = (lb - la) da - (db - da) la
    LMS R0,(QLB)
    LMS R1,(QLA)
    SUB R1
    MOVE R6,R0
    LMS R0,(QDA)
    TO R7
    LMULT                       ; R7:R4 = (lb - la) da
    MOVE R8,R4
    LMS R0,(QDB)
    LMS R1,(QDA)
    SUB R1
    MOVE R6,R0
    LMS R0,(QLA)
    TO R9
    LMULT                       ; R9:R4 = (db - da) la
    WITH R8
    SUB R4
    WITH R7
    SBC R9                      ; R7:R8 = c (32 bits)
    MOVE R0,R7
    OR R8
    BEQ @no                     ; c = 0 (edge-on): skip
    NOP
    IBT R3,#0                   ; R3 = QK: halvings so far
; (shorten c until it fits a signed 16-bit word: R7 must equal the sign
; of R8, i.e. R7 + (R8's bit 15) is 0)
@norm:
    FROM R8
    ADD R8
    FROM R7
    ADC #0
    BEQ @fits                   ; fits
    NOP
    WITH R7
    ASR                         ; halve R7:R8 (arithmetic)
    WITH R8
    ROR
    BRA @norm
    INC R3                      ; (delay slot) one more halving
@fits:
    ; VW[nvw]: da la dd dl tex ua du - x0 x1 qdn qk
    LMS R0,(NVW)
    UMULT #12
    ADD R0                      ; x 24: the VW record
    IWT R1,#VW
    TO R1
    ADD R1
    LMS R0,(QDA)
    STW (R1)
    INC R1
    INC R1
    LMS R0,(QLA)
    STW (R1)
    INC R1
    INC R1
    LMS R0,(QDB)
    LMS R2,(QDA)
    SUB R2
    STW (R1)
    INC R1
    INC R1
    LMS R0,(QLB)
    LMS R2,(QLA)
    SUB R2
    STW (R1)
    INC R1
    INC R1
    ; tex ua du from CAND
    LMS R2,(CPTR)
    IBT R0,#8
    WITH R2
    SUB R0
    LDW (R2)                    ; tex and ua (CAND's bytes 8, 9; CPTR is 16 past the record)
    STW (R1)
    INC R1
    INC R1
    INC R2
    INC R2
    LDB (R2)
    STW (R1)                    ; du, as a word
    INC R1
    INC R1
    LMS R0,(QX0)
    STW (R1)
    INC R1
    INC R1
    LMS R0,(QX1)
    STW (R1)
    INC R1
    INC R1
    FROM R8
    STW (R1)                    ; qdn: c shortened
    INC R1
    INC R1
    FROM R3
    STW (R1)                    ; qk
    LMS R0,(NVW)
    INC R0
    SMS (NVW),R0
    GO pb_loop

; ----------------------------------------------------------------------
; PC: VW -> COLHQ, COLW (the nearest wall per column)
pc:
    CACHE
    GO pc_main
    M_UDIV udiv_c               ; udiv_c
    M_HQAT hqat_c, udiv_c       ; hqat_c: hq at a column
    M_HQRAMP hqramp_c, hqat_c, udiv_c ; hqramp_c: QHQ, QST
pc_main:
    SMS (RPH),R11
    IBT R0,#0
    SMS (VK),R0                 ; VK: the wall's index in VW
pc_loop:
    LMS R0,(VK)
    LMS R1,(NVW)
    CMP R1
    BCC @go                     ; VK < NVW: go on
    NOP
    LMS R11,(RPH)
    JMP R11
    NOP
@go:
    UMULT #12
    ADD R0                      ; x 24
    IWT R1,#VW + 4
    TO R3
    ADD R1                      ; R3 = the record's dd
    LDW (R3)
    SMS (QDD),R0
    INC R3
    INC R3
    LDW (R3)
    SMS (QDL),R0
    IBT R0,#6
    WITH R3
    ADD R0
    LDW (R3)
    SMS (QX0),R0
    INC R3
    INC R3
    LDW (R3)
    SMS (QX1),R0
    INC R3
    INC R3
    LDW (R3)
    SMS (QDN),R0
    INC R3
    INC R3
    LDW (R3)
    SMS (QK),R0
    CALL hqramp_c               ; QHQ and QST for its columns
    ; the columns
    LMS R3,(QX0)
    LMS R1,(QHQLO)
    LMS R2,(QHQHI)
    LMS R5,(QSTLO)
    LMS R6,(QSTHI)
    LMS R9,(VK)                 ; R9 = the wall's index (for COLW)
    LMS R11,(QX1)               ; R11 = the end column (RPH holds the return address)
    ; R2:R1 = hq (16.8, stepped), R6:R5 = the step, R3 = the column
@col:
    MOVE R7,R2
    WITH R7
    SWAP
    MOVE R8,R1
    MERGE                       ; R0 = hq >> 8: this column's hq
    FROM R3
    TO R10
    ADD R3
    IWT R4,#COLHQ
    WITH R10
    ADD R4                      ; R10 = COLHQ + 2x
    LDW (R10)                   ; the hq it has
    MOVE R4,R0
    MERGE                       ; (hq again: LDW took R0)
    CMP R4
    BEQ @not
    NOP
    BCC @not                    ; not higher: a nearer wall already has the column
    NOP
    STW (R10)                   ; this wall is nearer
    IWT R4,#COLW
    FROM R3
    TO R10
    ADD R4
    FROM R9
    STB (R10)                   ; COLW[x] = the wall
@not:
    WITH R1
    ADD R5
    WITH R2
    ADC R6                      ; hq += step (32 bits)
    INC R3
    FROM R11
    CMP R3
    BNE @col                    ; to the last column
    NOP
    LMS R0,(VK)
    INC R0
    SMS (VK),R0
    GO pc_loop

; ----------------------------------------------------------------------
; PD: per column, its wall's texture column -> COLP (lo bank rm h)
; (COLP per column, 8 bytes: the texture column's ROM offset and bank
; (words), the remap's address, the height h in pixels; h = 0: no
; wall. As view.c's column_params)
pd:
    CACHE
    GO pd_main
    M_UDIV udiv_d               ; udiv_d
    M_M2NUM2 m2num2_d           ; m2num2_d: m2, num2 at a column
    M_T8OF t8of_d, udiv_d       ; t8of_d: t along the wall
pd_main:
    SMS (RPH),R11
    IBT R0,#0
    SMS (CX),R0
pd_loop:
    LMS R0,(CX)
    ADD R0
    ADD R0
    ADD R0
    IWT R1,#COLP + 6            ; COLP + 8x + 6: its h
    ADD R1
    MOVE R3,R0
    IBT R0,#0
    STW (R3)                    ; h = 0 until known
    LMS R0,(CX)
    IWT R1,#COLW
    ADD R1
    LDB (R0)                    ; the column's wall
    IWT R1,#255
    CMP R1
    BNE @wall                   ; 255: none
    NOP
    GO pd_next
@wall:
    UMULT #12
    ADD R0                      ; x 24: its VW record (R9)
    IWT R1,#VW
    TO R9
    ADD R1
    LMS R0,(CX)
    ADD R0
    IWT R1,#COLHQ
    ADD R1
    LDW (R0)
    SMS (HQ),R0                 ; HQ = the column's hq
    IBT R1,#8
    CMP R1
    BCS @tall                   ; hq >= 8: tall enough to draw
    NOP
    GO pd_next
@tall:
    LDW (R9)
    SMS (QDA),R0
    INC R9
    INC R9
    LDW (R9)
    SMS (QLA),R0
    INC R9
    INC R9
    LDW (R9)
    SMS (QDD),R0
    INC R9
    INC R9
    LDW (R9)
    SMS (QDL),R0
    INC R9
    INC R9
    SMS (T2),R9                 ; T2: the record's tex byte
    LMS R0,(CX)
    CALL m2num2_d               ; R1 = m2, R2 = num2
    CALL t8of_d                 ; R0 = t (0..255)
    ; u = (ua + (t du) >> 8) & 63
    LMS R9,(T2)
    MOVE R6,R0
    FROM R9
    TO R1
    ADD #2
    LDB (R1)
    SEX                         ; du, signed
    LMULT                       ; R4 = t du
    MOVE R0,R4
    HIB
    SEX                         ; (t du) >> 8, signed
    MOVE R2,R0
    FROM R9
    TO R1
    ADD #1
    LDB (R1)
    ADD R2                      ; + ua
    IBT R1,#63
    AND R1
    SWAP                        ; u x 256 ...
    LSR
    LSR                         ; ... / 4 = u x 64: the column's offset in the texture
    MOVE R5,R0
    ; t = tex_map[tex]
    LDB (R9)
    IWT R1,#TEXMAP
    ADD R1
    LDB (R0)                    ; the slot after the portal's swaps
    MOVE R7,R0
    ; COLP: lo bank rm h
    LMS R0,(CX)
    ADD R0
    ADD R0
    ADD R0
    IWT R1,#COLP
    TO R3
    ADD R1
    MOVE R0,R7
    ADD R0
    ADD R0
    IWT R1,#TEXADDR
    ADD R1
    MOVE R1,R0
    LDW (R1)
    ADD R5                      ; lo: the texture's offset + 64 u
    STW (R3)
    INC R3
    INC R3
    INC R1
    INC R1
    LDW (R1)
    ADC #0                      ; bank, with the carry
    STW (R3)
    INC R3
    INC R3
    MOVE R0,R7
    ADD R0
    ADD R0
    ADD R0
    ADD R0
    ADD R0
    IWT R1,#TEXRM
    ADD R1                      ; rm: TEXRM + 32 t
    STW (R3)
    INC R3
    INC R3
    LMS R0,(HQ)
    ADD #8
    ROR                         ; (hq + 8) as 17 bits
    LSR
    LSR
    LSR                         ; h = (hq + 8) >> 4
    STW (R3)
pd_next:
    LMS R0,(CX)
    INC R0
    SMS (CX),R0
    IWT R1,#VIEW_W
    CMP R1                      ; all VIEW_W columns done?
    BEQ @done
    NOP
    GO pd_loop
@done:
    LMS R11,(RPH)
    JMP R11
    NOP

; ----------------------------------------------------------------------
; PE: the walls' columns drawn: short ones straight from the ROM, the
; others through BUF (kept while the next column uses the same)
; (each column from COLP's h, texture address and remap. A column no
; taller than 64 pixels whose 64 texels do not cross a ROM bank reads
; the ROM directly; others copy the texture column into BUF first,
; once per texture column (LASTLO / LASTBK / LASTRM remember what BUF
; holds))
pe:
    CACHE
    GO pe_main
    M_UDIV udiv_e               ; udiv_e
    M_DRAWWALL wallbuf_e, 0     ; wallbuf_e: a column from BUF
    M_DRAWWALL direct_e, 1      ; direct_e: a column straight from the ROM
    M_FASTCOPY fastcopy_e       ; fastcopy_e: a texture column into BUF
    M_COLSETUP colsetup_e       ; colsetup_e: a column's start, from the table
    M_COPYCOL copycol_e         ; copycol_e: a column into BUF (across a bank)
pe_main:
    SMS (RPH),R11
    IBT R0,#0
    SMS (CX),R0
    IWT R0,#$FFFF
    SMS (LASTLO),R0             ; BUF holds nothing yet
pe_loop:
    LMS R0,(CX)
    ADD R0
    ADD R0
    ADD R0
    IWT R1,#COLP
    TO R3
    ADD R1                      ; R3 = COLP + 8 x
    FROM R3
    ADD #6
    LDW (R0)
    SUB #0                      ; h = 0: no wall here
    BNE @wall
    NOP
    GO pe_next
@wall:
    SMS (HH),R0                 ; HH = h
    MOVE R9,R0
    LDW (R3)
    MOVE R5,R0                  ; R5 = the texture column's offset
    SMS (TLO),R0
    INC R3
    INC R3
    LDW (R3)
    MOVE R10,R0                 ; R10 = its bank
    SMS (TBK),R0
    INC R3
    INC R3
    LDW (R3)
    MOVE R6,R0                  ; R6 = the remap
    SMS (TRM),R0
    ; short and not across a bank: straight
    IWT R0,#$FFC0               ; $FFC0: the last offset whose 64 bytes stay in the bank
    CMP R5
    BCC @buf                    ; it would cross: through BUF
    NOP
    IBT R0,#64
    CMP R9
    BCC @buf                    ; taller than 64: through BUF
    NOP
    CALL colsetup_e             ; R1 R2 R3 R7 R8 for the column
    LMS R0,(TBK)
    ROMB                        ; ROMBR = the texture's bank
    LMS R5,(TLO)
    LMS R6,(TRM)
    CALL direct_e               ; draw it from the ROM
    GO pe_next
@buf:
    ; the same texture column as the last one through BUF?
    LMS R0,(LASTLO)
    CMP R5
    BNE @copy
    NOP
    LMS R0,(LASTBK)
    CMP R10
    BNE @copy
    NOP
    LMS R0,(LASTRM)
    CMP R6
    BEQ @copied                 ; all three equal: BUF already holds it
    NOP
@copy:
    SMS (LASTLO),R5
    SMS (LASTBK),R10
    SMS (LASTRM),R6
    IWT R0,#$FFC0
    CMP R5
    BCC @slow                   ; crosses a bank: the slow copy
    NOP
    CALL fastcopy_e             ; 64 texels into BUF
    BRA @copied
    NOP
@slow:
    IWT R1,#BUF
    IBT R12,#64
    CALL copycol_e              ; 64 texels into BUF, across the bank
@copied:
    CALL colsetup_e             ; the column's start
    IWT R5,#BUF
    CALL wallbuf_e              ; draw it from BUF
pe_next:
    LMS R0,(CX)
    INC R0
    SMS (CX),R0
    IWT R1,#VIEW_W
    CMP R1                      ; all VIEW_W columns?
    BEQ @done
    NOP
    GO pe_loop
@done:
    LMS R11,(RPH)
    JMP R11
    NOP

; ----------------------------------------------------------------------
; PF: the things (TH) -> PT: hq d l x0 x1 spr decal th | the decal's quad
; (PF3): qda qla qdd qdl qhqlo qhqhi qstlo qsthi, shown (+30)
; (PT, 32 bytes per thing: hq d l x0 x1 (words), spr (byte 10), decal
; (byte 11: 1 a decal), th (12: its TH index), then a decal's quad from
; byte 14, and byte 30 "shown" (PF3: 1 if any of it is on screen).
; TH, 8 bytes per thing: x z (world), spr, -, info. Up to 255)
pf:
    CACHE
    GO pf_main
    M_UDIV udiv_f               ; udiv_f
    M_SXQ sxq_f, udiv_f         ; sxq_f: screen x, 8.8
    M_DOF dof_f, FWX, FWZ       ; dof_f: depth
    M_DOF lof_f, RTX, RTZ       ; lof_f: lateral offset
pf_main:
    SMS (RPH),R11
    IBT R0,#0
    SMS (NPT),R0
    SMS (TI),R0                 ; TI: the thing's index
pf_loop:
    LMS R0,(TI)
    LMS R1,(NTH)
    CMP R1
    BCC @one                    ; TI < NTH: one more
    NOP
    LMS R11,(RPH)
    JMP R11
    NOP
@one:
    ADD R0
    ADD R0
    ADD R0
    IWT R1,#TH
    ADD R1                      ; TH + 8 TI
    MOVE R3,R0
    LDW (R3)
    SMS (TX),R0
    INC R3
    INC R3
    LDW (R3)
    SMS (TZ),R0
    INC R3
    INC R3
    LDB (R3)
    SMS (SPR),R0
    INC R3
    INC R3
    LDW (R3)
    SMS (INFO),R0
    LMS R0,(NPT)
    IWT R1,#255
    CMP R1
    BCS @next                   ; PT full (255)?
    NOP
    LMS R0,(SPR)
    IBT R1,#64
    CMP R1
    BCS @next                   ; a sprite slot past 64: skip (view.c's V_MAX_SPR)
    NOP
    LMS R0,(INFO)
    IWT R1,#$78
    AND R1                      ; info & $78: a decal on a wall
    BEQ @bb
    NOP
    CALL decal_f                ; a decal
    BRA @next
    NOP
@bb:
    CALL billboard_f            ; a billboard
@next:
    LMS R0,(TI)
    INC R0
    SMS (TI),R0
    GO pf_loop

; billboard: TX TZ SPR -> a PT entry (or none)
; (as view.c's fx_billboard: d brought 16 nearer, kept if >= 4; its
; columns from l - 32 to l + 32; hq = 65536 / d (HQ_NUM / d); left out if every
; column's wall is nearer. Clobbers R0-R13, CURD, CURL, QX0, QX1, OHQ)
billboard_f:
    SMS (RBB),R11
    LMS R1,(TX)
    LMS R2,(TZ)
    CALL dof_f                  ; R0 = d
    IBT R1,#16
    SUB R1
    SMS (CURD),R0
    SUB #4                      ; d - 4 >= 0: near enough
    BGE @near
    NOP
@no:
    LMS R11,(RBB)
    JMP R11
    NOP
@near:
    LMS R1,(TX)
    LMS R2,(TZ)
    CALL lof_f                  ; R0 = l
    SMS (CURL),R0
    MOVE R2,R0
    LMS R1,(CURD)
    ; l - 32 > d: past the right; l + 32 < -d: past the left
    IWT R0,#$FFE0
    ADD R2
    SUB R1                      ; l - 32 - d
    BEQ @r
    NOP
    BGE @no                     ; > 0: past the right edge
    NOP
@r:
    IBT R0,#32
    ADD R2
    ADD R1
    BMI @no                     ; l + 32 + d < 0: past the left
    NOP
    ; x0: L = l - 32 (L >= d: nothing)
    IWT R0,#$FFE0
    ADD R2
    MOVE R2,R0
    FROM R2
    CMP R1
    BGE @no                     ; L >= d: no column
    NOP
    FROM R2
    ADD R1
    BMI @x0zero                 ; L + d <= 0: from column 0
    NOP
    BEQ @x0zero
    NOP
    CALL sxq_f                  ; R0 = 16384 + (L << 14) / d
    HIB                         ; x0 = its whole part
    BRA @x0
    NOP
@x0zero:
    IBT R0,#0
@x0:
    SMS (QX0),R0
    BRA @x0go
    NOP
@no2:
    LMS R11,(RBB)
    JMP R11
    NOP
@x0go:
    ; x1: L = l + 32
    LMS R1,(CURD)
    LMS R0,(CURL)
    ADD #15
    ADD #15
    ADD #2                      ; l + 32 (IBT reaches only 127: 15 + 15 + 2)
    MOVE R2,R0
    FROM R2
    CMP R1
    BGE @x1full                 ; L >= d: to the right edge
    NOP
    FROM R2
    ADD R1
    BMI @no2                    ; L + d <= 0: none
    NOP
    BEQ @no2
    NOP
    CALL sxq_f                  ; the screen x, 8.8
    IWT R1,#255
    ADD R1
    HIB                         ; x1 = rounded up
    IWT R1,#VIEW_W
    CMP R1
    BCC @x1                     ; x1 < 128
    NOP
@x1full:
    IWT R0,#VIEW_W
@x1:
    SMS (QX1),R0
    LMS R1,(QX0)
    CMP R1
    BEQ @no2
    NOP
    BCC @no2                    ; x1 <= x0: nothing
    NOP
    ; hq = 65536 / d
    IBT R1,#HQ_NUM>>16
    IWT R2,#HQ_NUM&$FFFF        ; R1:R2 = 65536 (HQ_NUM)
    LMS R3,(CURD)
    CALL udiv_f                 ; R2 = hq = 65536 / d
    SMS (OHQ),R2
    ; behind the walls everywhere?
    LMS R3,(QX0)
    LMS R5,(QX1)
    ; (is any column's wall farther than the thing? stops at the first)
@scan:
    FROM R3
    TO R0
    ADD R3
    IWT R1,#COLHQ
    ADD R1
    LDW (R0)                    ; that column's wall's hq
    FROM R2
    CMP R0
    BCS @shows                  ; hq >= it: the thing shows there
    NOP
    INC R3
    FROM R5
    CMP R3
    BNE @scan
    NOP
    LMS R11,(RBB)
    JMP R11
    NOP
@shows:
    CALL newpt_f                ; R3 = a new PT entry
    LMS R0,(OHQ)
    STW (R3)
    INC R3
    INC R3
    LMS R0,(CURD)
    STW (R3)
    INC R3
    INC R3
    LMS R0,(CURL)
    STW (R3)
    INC R3
    INC R3
    LMS R0,(QX0)
    STW (R3)
    INC R3
    INC R3
    LMS R0,(QX1)
    STW (R3)
    INC R3
    INC R3
    LMS R0,(SPR)
    STW (R3)
    LMS R11,(RBB)
    JMP R11
    NOP

; newpt: R3 = the next PT entry (NPT += 1; its th index stored)
; (In: TI. Out: R3 = PT + 32 NPT (before the + 1). Clobbers R0, R1)
newpt_f:
    LMS R0,(NPT)
    MOVE R1,R0
    INC R1
    SMS (NPT),R1
    SWAP
    LSR                         ; x 32
    LSR
    LSR
    IWT R1,#PT
    TO R3
    ADD R1
    FROM R3
    ADD #12
    MOVE R1,R0
    LMS R0,(TI)
    STW (R1)                    ; th = TI at +12
    JMP R11
    NOP

; decal: TX TZ SPR INFO -> a PT entry (or none)
; (as view.c's fx_decal: kept unless its centre is more than 32 behind
; or a 64-wide margin past an edge; hq from the centre's depth ($FFFF
; when d < 2). Its quad is worked out in PF3. Clobbers R0-R3, R12, R13)
decal_f:
    SMS (RDECAL),R11
    LMS R1,(TX)
    LMS R2,(TZ)
    CALL dof_f                  ; R0 = d
    SMS (CURD),R0
    IWT R1,#32
    ADD R1
    BMI @no                     ; d + 32 < 0: behind
    NOP
    LMS R1,(TX)
    LMS R2,(TZ)
    CALL lof_f                  ; R0 = l
    MOVE R2,R0
    LMS R1,(CURD)
    FROM R2
    SUB R1
    IWT R3,#64
    SUB R3                      ; l - d - 64
    BEQ @r
    NOP
    BGE @no                     ; > 0: past the right
    NOP
@r:
    FROM R2
    ADD R1
    IWT R3,#64
    ADD R3
    BMI @no                     ; l + d + 64 < 0: past the left
    NOP
    IWT R2,#$FFFF               ; hq $FFFF ...
    FROM R1
    SUB #2
    BLT @hq                     ; ... when d < 2
    NOP
    MOVE R3,R1
    IBT R1,#HQ_NUM>>16
    IWT R2,#HQ_NUM&$FFFF
    CALL udiv_f                 ; else 65536 / d (HQ_NUM / d)
@hq:
    SMS (OHQ),R2
    CALL newpt_f                ; R3 = a new PT entry
    LMS R0,(OHQ)
    STW (R3)
    IBT R0,#10
    WITH R3
    ADD R0
    LMS R0,(SPR)
    IWT R1,#$100
    OR R1                       ; spr | $100: byte 11, the decal flag
    STW (R3)
@no:
    LMS R11,(RDECAL)
    JMP R11
    NOP

; ----------------------------------------------------------------------
; PF3: each decal's quad projected, its hq ramp (few: from the ROM)
; (for each decal: its 64-unit quad (A, B) from the thing's x, z and
; info, as view.c's fx_decal; projected (fx_project's two passes) and
; its hq ramp, stored in PT from byte 14; byte 30 = 1 if any of it is
; on screen. Not in the cache: it calls the routines kept with PB / PC)
pf3:
    SMS (RPH),R11
    IBT R0,#0
    SMS (OI),R0                 ; OI: the PT index
pf3_loop:
    LMS R0,(OI)
    LMS R1,(NPT)
    CMP R1
    BCC @one                    ; OI < NPT
    NOP
    LMS R11,(RPH)
    JMP R11
    NOP
@one:
    SWAP
    LSR
    LSR
    LSR                         ; x 32
    IWT R1,#PT
    ADD R1
    SMS (PTP),R0                ; PTP: the entry
    MOVE R3,R0
    IBT R0,#30
    ADD R3
    MOVE R1,R0
    IBT R0,#0
    STB (R1)                    ; shown = 0
    IBT R0,#11
    ADD R3
    LDB (R0)                    ; the decal flag
    SUB #0
    BNE @decal
    NOP
    GO pf3_next
@decal:
    IBT R0,#12
    ADD R3
    LDW (R0)                    ; th
    ADD R0
    ADD R0
    ADD R0                      ; x 8
    IWT R1,#TH
    ADD R1                      ; R3: its TH record
    MOVE R3,R0
    LDW (R3)
    MOVE R1,R0                  ; R1 = x
    INC R3
    INC R3
    LDW (R3)
    MOVE R2,R0                  ; R2 = z
    INC R3
    INC R3
    INC R3
    INC R3
    LDW (R3)
    SMS (INFO),R0               ; INFO
    ; s = 32 sgn
    IBT R5,#32                  ; s = 32 ...
    HIB
    LSR
    LSR
    BCC @sgn                    ; ... or -32 if info bit 9 ($200, flipped)
    NOP
    IWT R5,#$FFE0
@sgn:
    ; info $800 (the original's $8000000): seen from both sides; then s is
    ; chosen so that the camera is in front (B to A from behind, the
    ; picture mirrored, as view.c's fx_decal): s = 32 if the camera's side
    ; D > 0, else -32
    LMS R0,(INFO)
    IWT R3,#$800
    AND R3
    BEQ @tone                   ; one-sided: s as it is
    NOP
    LMS R0,(INFO)
    IBT R3,#8
    AND R3
    BEQ @ts1
    NOP
    LMS R0,(VZ)                 ; $08: D = vz - z
    SUB R2
    BRA @tsd
    NOP
@ts1:
    LMS R0,(INFO)
    IBT R3,#$10
    AND R3
    BEQ @ts2
    NOP
    LMS R3,(VZ)                 ; $10: D = z - vz
    FROM R2
    SUB R3
    BRA @tsd
    NOP
@ts2:
    LMS R0,(INFO)
    IWT R3,#$40
    AND R3
    BEQ @ts3
    NOP
    LMS R3,(VX)                 ; $40: D = x - vx
    FROM R1
    SUB R3
    BRA @tsd
    NOP
@ts3:
    LMS R0,(VX)                 ; else: D = vx - x
    SUB R1
@tsd:
    IBT R5,#32
    SUB #0
    BEQ @tsn                    ; D <= 0: -32
    NOP
    BPL @tone
    NOP
@tsn:
    IWT R5,#$FFE0
@tone:
    LMS R0,(INFO)
    IBT R3,#8
    AND R3
    BEQ @n8                     ; info bit 3: the quad along x, A = x - s
    NOP
    FROM R1
    SUB R5
    SMS (AX),R0
    FROM R1
    ADD R5
    SMS (BX),R0
    SMS (AZ),R2
    SMS (BZ),R2
    BRA @quad
    NOP
@n8:
    LMS R0,(INFO)
    IBT R3,#$10
    AND R3
    BEQ @n10                    ; bit 4: along x, A = x + s
    NOP
    FROM R1
    ADD R5
    SMS (AX),R0
    FROM R1
    SUB R5
    SMS (BX),R0
    SMS (AZ),R2
    SMS (BZ),R2
    BRA @quad
    NOP
    ; (otherwise along z: bit 6 A = z - s, else A = z + s)
@n10:
    SMS (AX),R1
    SMS (BX),R1
    LMS R0,(INFO)
    IWT R3,#$40
    AND R3
    BEQ @n40
    NOP
    FROM R2
    SUB R5
    SMS (AZ),R0
    FROM R2
    ADD R5
    SMS (BZ),R0
    BRA @quad
    NOP
@n40:
    FROM R2
    ADD R5
    SMS (AZ),R0
    FROM R2
    SUB R5
    SMS (BZ),R0
@quad:
    CALL project_r              ; project the quad (R0 = 1 if any of it shows)
    SUB #0
    BNE @seen
    NOP
    GO pf3_next
@seen:
    LMS R0,(QDB)
    LMS R1,(QDA)
    SUB R1
    SMS (QDD),R0                ; QDD = dB - dA
    LMS R0,(QLB)
    LMS R1,(QLA)
    SUB R1
    SMS (QDL),R0                ; QDL = lB - lA
    CALL hqramp_r               ; its hq ramp
    ; into the entry: x0 x1 (+6) | qda qla qdd qdl qhq qst (+14) shown (+30)
    LMS R3,(PTP)
    IBT R0,#6
    WITH R3
    ADD R0
    LMS R0,(QX0)
    STW (R3)
    INC R3
    INC R3
    LMS R0,(QX1)
    STW (R3)
    IBT R0,#6
    WITH R3
    ADD R0
    LMS R0,(QDA)
    STW (R3)
    INC R3
    INC R3
    LMS R0,(QLA)
    STW (R3)
    INC R3
    INC R3
    LMS R0,(QDD)
    STW (R3)
    INC R3
    INC R3
    LMS R0,(QDL)
    STW (R3)
    INC R3
    INC R3
    LMS R0,(QHQLO)
    STW (R3)
    INC R3
    INC R3
    LMS R0,(QHQHI)
    STW (R3)
    INC R3
    INC R3
    LMS R0,(QSTLO)
    STW (R3)
    INC R3
    INC R3
    LMS R0,(QSTHI)
    STW (R3)
    INC R3
    INC R3
    IBT R0,#1
    STB (R3)                    ; shown = 1
pf3_next:
    LMS R0,(OI)
    INC R0
    SMS (OI),R0
    GO pf3_loop

; project_r: AX AZ BX BZ -> R0 0 or 1; QDA QLA QDB QLB QX0 QX1 QDN QK
; (as view.c's fx_project: fx_quad_view's tests, then fx_quad_rest's
; clipping, columns and c shortened to 16 bits (QDN, QK). Clobbers
; R0-R13 and the C*, X* variables)
project_r:
    SMS (RPROJ),R11
    LMS R1,(AX)
    LMS R2,(AZ)
    CALL toview_r               ; R1 = dA, R2 = lA
    SMS (QDA),R1
    SMS (QLA),R2
    SMS (CDA),R1
    SMS (CLA),R2
    LMS R1,(BX)
    LMS R2,(BZ)
    CALL toview_r               ; R1 = dB, R2 = lB
    SMS (QDB),R1
    SMS (QLB),R2
    SMS (CDB),R1
    SMS (CLB),R2
    LMS R0,(QDA)
    DEC R0
    BPL @infront                ; dA >= 1: in front
    NOP
    MOVES R1,R1
    BMI @no                     ; dA < 1 and dB <= 0: behind
    NOP
    BEQ @no
    NOP
@infront:
    LMS R0,(QLA)
    LMS R1,(QDA)
    SUB R1
    BLT @notright               ; la < da: not past the right edge
    NOP
    LMS R0,(QLB)
    LMS R1,(QDB)
    SUB R1
    BGE @no                     ; lb >= db too: past the right edge
    NOP
@notright:
    LMS R0,(QLA)
    LMS R1,(QDA)
    ADD R1
    BMI @l1
    NOP
    BNE @notleft                ; la + da > 0: not past the left
    NOP
@l1:
    LMS R0,(QLB)
    LMS R1,(QDB)
    ADD R1
    BMI @no
    NOP
    BNE @notleft                ; lb + db > 0: not past the left
    NOP
@no:
    IBT R0,#0                   ; R0 = 0: not seen
    LMS R11,(RPROJ)
    JMP R11
    NOP
@notleft:
    LMS R0,(QDA)
    DEC R0
    BPL @anear
    NOP
    LMS R1,(QDA)
    LMS R2,(QLA)
    LMS R3,(QDB)
    LMS R5,(QLB)
    CALL clip_r                 ; clip A to depth 1
    SMS (CLA),R0
    IBT R0,#1
    SMS (CDA),R0
    BRA @clipped
    NOP
@anear:
    LMS R0,(QDB)
    DEC R0
    BPL @clipped
    NOP
    LMS R1,(QDB)
    LMS R2,(QLB)
    LMS R3,(QDA)
    LMS R5,(QLA)
    CALL clip_r                 ; clip B
    SMS (CLB),R0
    IBT R0,#1
    SMS (CDB),R0
@clipped:
    LMS R1,(CDA)
    LMS R2,(CLA)
    CALL fxcol_r                ; xA
    SMS (XA),R0
    LMS R1,(CDB)
    LMS R2,(CLB)
    CALL fxcol_r                ; xB
    LMS R1,(XA)
    CMP R1
    BCS @ordered                ; in order
    NOP
    MOVE R2,R0
    MOVE R0,R1
    MOVE R1,R2
@ordered:
    CMP R1
    BEQ @no2                    ; no column
    NOP
    BCS @ok2
    NOP
@no2:
    IBT R0,#0
    LMS R11,(RPROJ)
    JMP R11
    NOP
@ok2:
    SMS (QX1),R0
    SMS (QX0),R1
    LMS R0,(QLB)
    LMS R1,(QLA)
    SUB R1
    MOVE R6,R0
    LMS R0,(QDA)
    TO R7
    LMULT                       ; R7:R4 = (lb - la) da
    MOVE R8,R4
    LMS R0,(QDB)
    LMS R1,(QDA)
    SUB R1
    MOVE R6,R0
    LMS R0,(QLA)
    TO R9
    LMULT                       ; R9:R4 = (db - da) la
    WITH R8
    SUB R4
    WITH R7
    SBC R9                      ; R7:R8 = c
    MOVE R0,R7
    OR R8
    BEQ @no2                    ; c = 0: not seen
    NOP
    IBT R3,#0
    ; (c halved until it fits 16 bits, as in PB)
@norm:
    FROM R8
    ADD R8
    FROM R7
    ADC #0
    BEQ @fits
    NOP
    WITH R7
    ASR
    WITH R8
    ROR
    BRA @norm
    INC R3                      ; (delay slot) one more halving
@fits:
    SMS (QDN),R8
    SMS (QK),R3
    IBT R0,#1                   ; R0 = 1: seen
    LMS R11,(RPROJ)
    JMP R11
    NOP
    M_UDIV udiv_r               ; the routines for PF3's project_r and hqramp_r
    M_TOVIEW toview_r
    M_SDIVP sdivp_r, udiv_r
    M_CLIP clip_r, sdivp_r
    M_FXCOL fxcol_r, udiv_r
    M_HQAT hqat_r, udiv_r
    M_HQRAMP hqramp_r, hqat_r, udiv_r

; ----------------------------------------------------------------------
; PS: PTORD, by hq (insertion, as view.c's)
; (PTORD[0 .. NPT - 1]: PT indices, smallest hq (farthest) first; a
; stable insertion sort. Note PS returns by JMP R11 directly: it calls
; nothing)
ps:
    CACHE
    LMS R0,(NPT)
    SUB #0
    BNE @some                   ; no things: nothing to sort
    NOP
    JMP R11
    NOP
@some:
    MOVE R12,R0                 ; NPT entries
    IWT R1,#PTORD
    IBT R0,#0
    MOVE R13,R15
    STB (R1)                    ; PTORD[k] = k
    INC R1
    LOOP
    INC R0                      ; (delay slot) next k
    IBT R0,#1
    SMS (OI),R0                 ; OI = i, from 1
@iloop:
    LMS R0,(OI)
    LMS R1,(NPT)
    CMP R1
    BCC @ins                    ; i < NPT: insert it
    NOP
    JMP R11
    NOP
@ins:
    IWT R1,#PTORD
    ADD R1
    LDB (R0)
    SMS (OV),R0                 ; OV = the entry being inserted
    SWAP
    LSR
    LSR
    LSR
    IWT R1,#PT
    ADD R1
    LDW (R0)                    ; its hq
    MOVE R5,R0                  ; R5 = the key
    LMS R6,(OI)                 ; R6 = j = i
@jloop:
    MOVES R6,R6
    BEQ @put                    ; j = 0: at the front
    NOP
    IWT R0,#PTORD - 1
    ADD R6                      ; PTORD + j - 1
    MOVE R7,R0
    LDB (R0)
    MOVE R8,R0                  ; R8 = PTORD[j - 1]
    SWAP
    LSR
    LSR
    LSR
    IWT R1,#PT
    ADD R1
    LDW (R0)                    ; its hq
    CMP R5
    BEQ @put
    NOP
    BCC @put                    ; <= the key: stop (stable)
    NOP
    INC R7
    FROM R8
    STB (R7)                    ; PTORD[j] = PTORD[j - 1]
    DEC R6                      ; j - 1
    BRA @jloop
    NOP
@put:
    IWT R0,#PTORD
    ADD R6
    MOVE R1,R0
    LMS R0,(OV)
    STB (R1)                    ; PTORD[j] = the entry
    LMS R0,(OI)
    INC R0
    SMS (OI),R0
    BRA @iloop
    NOP

; ----------------------------------------------------------------------
; PG1: PT, far first -> SCOL (x hq c spr)
; (SCOL, 8 bytes per sprite column to draw: x, hq, c (the canvas
; column 0..63), spr (words); NSC of them, up to MAX_SC. As view.c's
; fx_things' drawing loop, but only listing the columns: PG2 draws them)
pg1:
    CACHE
    GO pg1_main
    M_UDIV udiv_g               ; udiv_g
    M_M2NUM2 m2num2_g           ; m2num2_g: m2, num2 at a column
    M_T8OF t8of_g, udiv_g       ; t8of_g: t along the quad
pg1_main:
    SMS (RPH),R11
    IBT R0,#0
    SMS (OI),R0
    SMS (NSC),R0
    IWT R0,#SCOL
    SMS (SCP),R0                ; SCP: where the next SCOL record goes
pg1_loop:
    LMS R0,(OI)
    LMS R1,(NPT)
    CMP R1
    BCC @one                    ; OI < NPT
    NOP
    LMS R11,(RPH)
    JMP R11
    NOP
@one:
    IWT R1,#PTORD
    ADD R1
    LDB (R0)                    ; PTORD[OI]: the next thing, far first
    SWAP
    LSR
    LSR
    LSR                         ; x 32
    IWT R1,#PT
    ADD R1
    MOVE R3,R0
    LDW (R3)
    SMS (HQ),R0                 ; HQ
    INC R3
    INC R3
    LDW (R3)
    SMS (CURD),R0               ; CURD = d
    INC R3
    INC R3
    LDW (R3)
    SMS (CURL),R0               ; CURL = l
    INC R3
    INC R3
    LDW (R3)
    SMS (CX),R0                 ; CX = x0
    INC R3
    INC R3
    LDW (R3)
    SMS (XEND),R0               ; XEND = x1
    INC R3
    INC R3
    LDW (R3)
    LOB
    SMS (SPR),R0                ; SPR
    LDW (R3)
    HIB                         ; the decal flag
    SUB #0
.IF VIEW_W == 128
    BEQ pg1_bb
    NOP
.ELSE
    BEQ pg1_bbgo
    NOP
.ENDIF
    GO pg1_decal                ; a decal
.IF VIEW_W != 128
pg1_bbgo:
    GO pg1_bbinit               ; (out of the cache: once a billboard)
.ENDIF
pg1_bb:
    ; the billboard's columns
    LMS R0,(CX)
    ADD R0
    IWT R1,#COLHQ
    ADD R1
    LDW (R0)                    ; the column's wall's hq
    LMS R1,(HQ)
    FROM R1
    CMP R0
    BCC @bnext                  ; hq < it: the wall hides this column
    NOP
.IF VIEW_W == 128
    LMS R0,(CX)
    ADD R0
    IWT R1,#$FF81
    ADD R1                      ; 2x + 1 - 128
    LMS R6,(CURD)
    TO R7
    LMULT                       ; R7:R4 = (2x + 1 - 128) d
    MOVE R8,R4
    WITH R8
    ADD R8
    WITH R7
    ROL                         ; x 2 ...
    WITH R7
    SWAP
    MERGE                       ; ... and bits 8-23: the product >> 7
.ELSE
    LMS R0,(BBQ)                ; k d / VIEW_W, rounded down
.ENDIF
    LMS R1,(CURL)
    SUB R1                      ; - l
    IBT R1,#32
    ADD R1                      ; c = ... + 32
    BMI @bnext                  ; c < 0: off the sprite
    NOP
    IBT R1,#64
    CMP R1
    BCS @bnext                  ; c >= 64: off the sprite
    NOP
    SMS (CC),R0
    CALL emit_g                 ; list the column
@bnext:
.IF VIEW_W != 128
    LMS R0,(BBR)                ; the next column: k d + 2d, q += a, r += b
    LMS R1,(BBB)
    ADD R1
    IWT R1,#VIEW_W
    CMP R1
    BCC @nocarry
    NOP
    SUB R1                      ; r went past VIEW_W - 1: carry 1 into q
    LMS R1,(BBQ)
    INC R1
    SMS (BBQ),R1
@nocarry:
    SMS (BBR),R0
    LMS R0,(BBQ)
    LMS R1,(BBA)
    ADD R1
    SMS (BBQ),R0
.ENDIF
    LMS R0,(CX)
    INC R0
    SMS (CX),R0
    LMS R1,(XEND)
    CMP R1
    BNE pg1_bb                  ; to x1
    NOP
pg1_next:
    LMS R0,(OI)
    INC R0
    SMS (OI),R0
    GO pg1_loop

pg1_decal:
    ; R3 at +10; shown?
    ; (a decal: its quad and ramp from PF3, stepped column by column as
    ; view.c's fx_things; skipped unless PF3 found it on screen)
    IBT R0,#20
    ADD R3
    LDB (R0)
    SUB #0
    BEQ pg1_next                ; not shown: skip
    NOP
    INC R3
    INC R3
    INC R3
    INC R3
    LDW (R3)
    SMS (QDA),R0
    INC R3
    INC R3
    LDW (R3)
    SMS (QLA),R0
    INC R3
    INC R3
    LDW (R3)
    SMS (QDD),R0
    INC R3
    INC R3
    LDW (R3)
    SMS (QDL),R0
    INC R3
    INC R3
    LDW (R3)
    SMS (QHQLO),R0
    INC R3
    INC R3
    LDW (R3)
    SMS (QHQHI),R0
    INC R3
    INC R3
    LDW (R3)
    SMS (QSTLO),R0
    INC R3
    INC R3
    LDW (R3)
    SMS (QSTHI),R0
@loop:
    LMS R7,(QHQHI)
    WITH R7
    SWAP
    LMS R8,(QHQLO)
    MERGE                       ; R0 = hq >> 8: this column's h
    SUB #0
    BEQ @next                   ; h = 0: nothing
    NOP
    SMS (HQ),R0
    ; h + h >> 6 < COLHQ[x]: hidden (17 bits)
    MOVE R1,R0
    LSR
    LSR
    LSR
    LSR
    LSR
    LSR
    ADD R1
    BCS @shows                  ; over 16 bits: surely in front
    NOP
    MOVE R2,R0
    LMS R0,(CX)
    ADD R0
    IWT R1,#COLHQ
    ADD R1
    LDW (R0)
    FROM R2
    CMP R0
    BCC @next                   ; the wall is nearer: hidden
    NOP
@shows:
    LMS R0,(CX)
    CALL m2num2_g               ; m2, num2 at the column
    CALL t8of_g                 ; t (0..255)
    LSR
    LSR                         ; c = t / 4 (0..63)
    SMS (CC),R0
    CALL emit_g                 ; list the column
@next:
    LMS R0,(QHQLO)
    LMS R1,(QSTLO)
    ADD R1
    SMS (QHQLO),R0
    LMS R0,(QHQHI)
    LMS R1,(QSTHI)
    ADC R1                      ; hq += step (32 bits)
    SMS (QHQHI),R0
    LMS R0,(CX)
    INC R0
    SMS (CX),R0
    LMS R1,(XEND)
    CMP R1
    BNE @loop
    NOP
    GO pg1_next

; emit: (CX, HQ, CC, SPR) -> SCOL (up to MAX_SC)
; (each record: x, hq, c, spr as words; NSC + 1. Clobbers R0, R1)
emit_g:
    LMS R0,(NSC)
    IWT R1,#MAX_SC
    CMP R1
    BCS @full                   ; full: dropped
    NOP
    INC R0
    SMS (NSC),R0
    LMS R1,(SCP)
    LMS R0,(CX)
    STW (R1)
    INC R1
    INC R1
    LMS R0,(HQ)
    STW (R1)
    INC R1
    INC R1
    LMS R0,(CC)
    STW (R1)
    INC R1
    INC R1
    LMS R0,(SPR)
    STW (R1)
    INC R1
    INC R1
    SMS (SCP),R1                ; SCP: the next record
@full:
    JMP R11
    NOP

.IF VIEW_W != 128
; pg1_bbinit: (another width) a billboard's canvas column at its first
; column, k d / VIEW_W rounded down as view.c's CANVAS_OF (BBQ, BBR), and
; its step per column (BBA, BBB), so the loop (pg1_bb) needs no product
; or division per column. In: CX = x0, CURD = d. Placed after the cached
; loop: it runs once a billboard. Clobbers R0-R7, R12, R13.
pg1_bbinit:
    ; (another width: the canvas column's k d / VIEW_W, rounded down as
    ; view.c's CANVAS_OF, at the first column, and its step per column,
    ; so no product or division per column: BBQ, BBR, BBA, BBB)
    LMS R0,(CX)
    ADD R0
    IWT R1,#(1-VIEW_W)&$FFFF
    ADD R1                      ; k = 2 x0 + 1 - VIEW_W
    LMS R6,(CURD)
    TO R7
    LMULT                       ; R7:R4 = N = k d
    MOVES R7,R7
    BPL @npos
    NOP
    IBT R0,#0                   ; N < 0: |N|
    SUB R4
    MOVE R4,R0
    IBT R0,#0
    SBC R7
    MOVE R7,R0
@npos:
    MOVE R1,R7
    MOVE R2,R4
    IWT R3,#VIEW_W
    CALL udiv_g                 ; R2 = |N| / VIEW_W, R1 = the rest
    LMS R0,(CX)                 ; (N's sign is k's: d > 0, and k is odd)
    ADD R0
    IWT R3,#(1-VIEW_W)&$FFFF
    ADD R3
    BPL @qpos                   ; k > 0: N > 0, as it is
    NOP
    IBT R0,#0                   ; N < 0, rounded down: q = -(|N| / W) - 1 and
    SUB R2                      ; r = W - rest, unless the rest is 0
    MOVE R2,R0
    MOVES R1,R1
    BEQ @qpos
    NOP
    DEC R2
    IWT R0,#VIEW_W
    SUB R1
    MOVE R1,R0
@qpos:
    SMS (BBQ),R2
    SMS (BBR),R1
    LMS R0,(CURD)               ; the step: 2d = VIEW_W a + b
    ADD R0
    MOVE R2,R0
    IBT R1,#0
    IWT R3,#VIEW_W
    CALL udiv_g
    SMS (BBA),R2
    SMS (BBB),R1
    GO pg1_bb
.ENDIF

; ----------------------------------------------------------------------
; PG2: the sprite columns drawn
; (in SCOL order, far to near, so nearer columns paint over. Each: the
; column's opaque span (top row, length) from spr_cols, its data from
; spr_coff and SPRADDR, copied into BUF (holes $FF around it) through
; the sprite's remap, then drawn by column_g2 with the height from
; hq (the division, not the wall table). As view.c's sprite_col)
pg2:
    CACHE
    GO pg2_main
    M_UDIV udiv_g2              ; udiv_g2
    M_COPYCOL copycol_g2        ; copycol_g2: texels into BUF
    M_DRAWCOL drawcol_g2        ; drawcol_g2: BUF into the picture
    M_COLUMN column_g2, udiv_g2, drawcol_g2 ; column_g2: a column of height HH at CX
pg2_main:
    SMS (RPH),R11
    IBT R0,#1
    SMS (SPRF),R0               ; SPRF = 1: sprites (holes skipped)
    IWT R0,#SCOL
    SMS (SCP),R0
pg2_loop:
    LMS R0,(NSC)
    SUB #0                      ; columns left?
    BNE @one
    NOP
    LMS R11,(RPH)
    JMP R11
    NOP
@one:
    DEC R0
    SMS (NSC),R0
    LMS R3,(SCP)
    LDW (R3)
    SMS (CX),R0
    INC R3
    INC R3
    LDW (R3)
    SMS (HQ),R0
    INC R3
    INC R3
    LDW (R3)
    SMS (CC),R0
    INC R3
    INC R3
    LDW (R3)
    SMS (SPR),R0
    INC R3
    INC R3
    SMS (SCP),R3
    ; spr * 128 + c * 2
    LMS R0,(SPR)
    SWAP
    LSR                         ; spr x 128
    LMS R1,(CC)
    ADD R1
    ADD R1                      ; + 2 c: the column's pair in spr_cols / spr_coff
    MOVE R5,R0
    ; top, len
    LMS R0,(SCOLSLO)
    ADD R5                      ; the ROM address of (top, len)
    MOVE R14,R0
    LMS R0,(SCOLSBK)
    ADC #0                      ; bank + the carry
    ROMB                        ; ROMBR
    IBT R0,#0
    WITH R14
    ADD R0                      ; R14 written again: the ROM fetch starts with the new bank
    GETB
    SMS (TOP),R0
    INC R14
    GETB
    SMS (LEN),R0                ; LEN
    SUB #0
    BEQ pg2_loop                ; empty column: next
    NOP
    LMS R0,(HQ)
    ADD #8
    ROR
    LSR
    LSR
    LSR                         ; h = (hq + 8) >> 4
    BEQ pg2_loop                ; 0: too small to draw
    NOP
    SMS (HH),R0
    ; off (big-endian)
    LMS R0,(SCOFFLO)
    ADD R5                      ; the ROM address of its data offset
    MOVE R14,R0
    LMS R0,(SCOFFBK)
    ADC #0
    ROMB                        ; ROMBR
    IBT R0,#0
    WITH R14
    ADD R0                      ; (R14 written again: the fetch)
    GETB
    INC R14
    SWAP                        ; the high byte ...
    TO R5
    GETBL                       ; ... and the low: R5 = off
    ; the data: spr_addr[spr] + off
    LMS R0,(SPR)
    ADD R0
    ADD R0
    IWT R1,#SPRADDR
    ADD R1
    MOVE R1,R0
    LDW (R1)
    WITH R5
    ADD R0                      ; R5 = the sprite's offset + off
    INC R1
    INC R1
    LDW (R1)
    ADC #0                      ; its bank, with the carry
    MOVE R10,R0                 ; R10 = the bank
    ; BUF: holes, then the column from its top row
    IWT R1,#BUF
    IBT R12,#32                 ; 32 words: BUF all holes
    IWT R0,#$FFFF
    MOVE R13,R15
    STW (R1)
    INC R1
    LOOP
    INC R1
    LMS R0,(TOP)
    IWT R1,#BUF
    WITH R1
    ADD R0                      ; R1 = BUF + top
    LMS R0,(SPR)
    SWAP
    LSR
    LSR
    LSR
    LSR                         ; spr x 16
    IWT R6,#SPRRM
    WITH R6
    ADD R0                      ; R6 = its remap (SPRRM + 16 spr)
    LMS R12,(LEN)               ; LEN texels
    CALL copycol_g2             ; into BUF
    CALL column_g2              ; draw the column
    GO pg2_loop

; ----------------------------------------------------------------------
; MAPLOAD (entry 4): the map's walls as they are in the block (big-endian,
; NWALLS of them, put at RAW by the CPU) -> WALLS, by block (view.c's
; fx_walls_load: the block of each wall's middle, the walls of a block in
; their order), BLK (start count x0 z0 x1 z1)
.DEFINE RAW     $0200
.DEFINE BOF     $DB00           ; [768] each wall's block - (in COLP's space: free while a map loads)
.DEFINE BCNT    $DA40           ; [64] words - (in VBL's space, after its 64 bytes)
; (RAW: the CPU's copy of the map pack's wall table, 16 bytes a wall,
; in the picture's space ($0200), which the next picture overwrites.
; It works as view.c's fx_walls_load, a counting sort in three passes:
; each wall's block and the blocks' counts and boxes; each block's
; start; each wall copied to WALLS at its block's start + its rank,
; byte-swapped to the GSU's little-endian words. Ends with STOP)
mapload:
    CACHE
    ; BLK: nothing, boxes empty
    IWT R1,#BLK
    IBT R12,#64                 ; 64 records of 12 bytes
    MOVE R13,R15
    IBT R0,#0                   ; start, count 0
    STW (R1)
    INC R1
    INC R1
    STW (R1)
    INC R1
    INC R1
    IWT R0,#$7FFF               ; x0, z0: the largest
    STW (R1)
    INC R1
    INC R1
    STW (R1)
    INC R1
    INC R1
    IWT R0,#$8001               ; x1, z1: the smallest (-32767)
    STW (R1)
    INC R1
    INC R1
    STW (R1)
    INC R1
    LOOP
    INC R1                      ; (delay slot: the 12th byte)
    ; each wall's block, the counts and boxes
    IWT R3,#RAW
    IWT R5,#BOF
    LMS R12,(NWALLS)
    MOVES R12,R12
    BNE @p1
    NOP
    STOP                        ; no walls: done
    NOP
@p1:
    ; (pass 1: per wall)
    MOVE R13,R15
    LDW (R3)
    TO R7
    SWAP                        ; R7 = ax (byte-swapped)
    INC R3
    INC R3
    LDW (R3)
    TO R8
    SWAP                        ; R8 = az
    INC R3
    INC R3
    LDW (R3)
    TO R9
    SWAP                        ; R9 = bx
    INC R3
    INC R3
    LDW (R3)
    TO R10
    SWAP                        ; R10 = bz
    IBT R0,#10
    WITH R3
    ADD R0                      ; the next wall (16 bytes)
    ; block: ((ax + bx) >> 1) >> 8, ((az + bz) >> 1) >> 8, 0..7 each
    FROM R7
    ADD R9
    ASR
    HIB
    SEX                         ; signed
    CALL clamp7                 ; held to 0..7: the block's x
    MOVE R6,R0
    FROM R8
    ADD R10
    ASR
    HIB
    SEX
    CALL clamp7                 ; its z
    ADD R0
    ADD R0
    ADD R0
    ADD R6                      ; b = 8 bz + bx
    STB (R5)                    ; BOF[wall] = b
    INC R5
    ; BLK[b]: count, box
    UMULT #12                   ; BLK + 12 b
    IWT R1,#BLK + 2
    TO R1
    ADD R1
    LDW (R1)
    INC R0                      ; count + 1
    STW (R1)
    INC R1
    INC R1
    ; x0 = min(x0, ax, bx)
    MOVE R2,R7
    CALL smin                   ; x0 = min(x0, ax, bx)
    MOVE R2,R9
    CALL smin
    INC R1
    INC R1
    MOVE R2,R8
    CALL smin                   ; z0 = min(z0, az, bz)
    MOVE R2,R10
    CALL smin
    INC R1
    INC R1
    MOVE R2,R7
    CALL smax                   ; x1 = max
    MOVE R2,R9
    CALL smax
    INC R1
    INC R1
    MOVE R2,R8
    CALL smax                   ; z1 = max
    MOVE R2,R10
    CALL smax
    LOOP
    NOP
    ; starts; BCNT 0
    ; (pass 2: each block's start = the counts before it)
    IWT R1,#BLK
    IWT R2,#BCNT
    IBT R3,#0
    IBT R12,#64
    MOVE R13,R15
    FROM R3
    STW (R1)                    ; start
    INC R1
    INC R1
    LDW (R1)                    ; its count
    WITH R3
    ADD R0                      ; R3 = the next start
    IBT R0,#0
    STW (R2)                    ; BCNT[b] = 0: placed so far
    INC R2
    INC R2
    IBT R0,#10
    WITH R1
    ADD R0                      ; the next record
    LOOP
    NOP
    ; the walls to their places, as words
    ; (pass 3: per wall, in order)
    IWT R3,#RAW
    IWT R5,#BOF
    LMS R12,(NWALLS)
    MOVE R13,R15
    LDB (R5)                    ; its block
    INC R5
    MOVE R6,R0
    UMULT #12
    IWT R1,#BLK
    ADD R1
    LDW (R0)                    ; R2 = the block's start
    MOVE R2,R0
    MOVE R0,R6
    ADD R0
    IWT R1,#BCNT
    ADD R1
    MOVE R1,R0
    LDW (R1)                    ; placed so far in that block
    WITH R2
    ADD R0                      ; R2 = start + n
    INC R0
    STW (R1)                    ; BCNT + 1
    ; WALLS + 16 (start + n)
    MOVE R0,R2
    ADD R0
    ADD R0
    ADD R0
    ADD R0
    IWT R1,#WALLS
    TO R1
    ADD R1
    IBT R4,#8                   ; 8 words
@w:
    LDW (R3)
    SWAP                        ; big-endian to little-endian
    STW (R1)
    INC R3
    INC R3
    INC R1
    DEC R4
    BNE @w
    INC R1                      ; (delay slot: the second INC)
    LOOP
    NOP
    STOP                        ; done: the CPU sees the GSU stop
    NOP

; clamp7: R0 (signed) held to 0..7. Clobbers R2.
clamp7:
    MOVES R0,R0
    BPL @pos
    NOP
    IBT R0,#0
    JMP R11
    NOP
@pos:
    IBT R2,#7
    CMP R2
    BCC @ok
    NOP
    MOVE R0,R2
@ok:
    JMP R11
    NOP

; smin / smax: the word at R1 = min / max (signed) of it and R2
; (signed, BGE; clobber R0)
smin:
    LDW (R1)
    FROM R2
    CMP R0
    BGE @k
    NOP
    FROM R2
    STW (R1)
@k:
    JMP R11
    NOP
smax:
    LDW (R1)
    CMP R2
    BGE @k
    NOP
    FROM R2
    STW (R1)
@k:
    JMP R11
    NOP
