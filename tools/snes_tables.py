#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The SNES engine's generated banks (from $C20000, before the campaign's
block): the column table and the compiled wall scalers of the 3D view
(snes/src/draw.s, view/view.c).

A column of height h (pixels) starts at row y0 = 44 - (28 h + 32) / 64 and
steps through the 64 texture rows by dv = 16384 / h (8.8) from dv / 2;
the rows past the view's top and bottom are skipped. (44 and the 88 rows
are the full 128 x 88 view's: --view W H gives another size, its horizon
at H / 2 and, at any size but 128 x 88, view.h's tiled frame buffer
layout (FB_TILED) - the SNES game's 104 x 72, snes/build.sh; the default
makes the tables the other ports' images carry.) Per h (1..4096), 8 bytes at
$C20000 + 8 h:
    u8 first row, u8 rows, u16 v at the first row (8.8), u16 dv, u16 0
Then, for h = 1..SCALERS, a compiled scaler: straight-line code that draws
the column's rows (65816; A 8-bit, X/Y 16-bit, B = 0, data bank $7E):
    X = the column's offset in the frame buffer, [$F0] = the texture column
    (long), ($F3) = its remap (bank $7E)
    per new texture row r: ldy #r / lda [$F0],y / tay / lda ($F3),y
    per screen row y:      sta $7F0000 + FB_OFF(0, y),x
    rtl
with their long addresses (3 bytes per h) at SCALER_PTRS. No scaler crosses
a bank boundary.

Sprite scalers, for h = 1..SPR_SCALERS: the same, but a sprite column
holds only the canvas rows top..end-1 ($F5: end) and 255 is a hole:
    per new texture row r: ldy #r / cpy $F5 / bcc +1 / rtl /
                           lda [$F0],y / cmp #$FF / beq (past its rows) /
                           tay / lda ($F3),y
    per screen row y:      sta $7F0000 + FB_OFF(0, y),x
    rtl
([$F0] points at canvas row 0.) The bank after the column table ($C3)
holds their entry points: per h (at 2 + 130 (h - 1)): the bank, a 0, then per
canvas row r (0..63) the address to start at for a column whose top is r.
At $C39000 in that bank: the Mega Drive tile conversion table (snes/src/
gfx.s md_tiles): at ((k * 4 + j) * 256 + b), bitplane k of the byte b's
two pixels (left: the high nibble) put at bits 7 - 2j and 6 - 2j.

At $C3A000: the sound driver and its tables for the SPC (tools/
snes_sound.py's spc_engine.bin, to be loaded at $0200): u16 its size,
then the bytes.

Usage: snes_tables.py <out.bin> [spc_engine.bin] [--view W H]      (prints the number of banks)

The bytes 0xA0 .. 0x6B written below are 65816 opcodes: A0 ldy #imm16, B7
lda [dp],y, A8 tay, B1 lda (dp),y, 9F sta long,x, C4 cpy dp, 90 bcc, C9
cmp #imm8, F0 beq, 6B rtl. FB_OFF is view/view.h's frame buffer layout
(columns of 8 pixels, 32-row blocks of 256 bytes).
Licence: GPL-2.0-or-later.
"""
import struct
import sys
from pathlib import Path

BANK0 = 0xC2                   # the first generated bank (HiROM)
VIEW_H = 88                    # the view's rows (view.h's VIEW_H; --view)
HALF_H = 44                    # its horizon (VIEW_HALF_H)
TILED = False                  # view.h's FB_TILED: any size but 128 x 88 (--view)
MAX_H = 4096                   # the column table's heights
SCALERS = 1024                 # compiled wall scalers for h = 1..1024
SCALER_PTRS = 0x8008           # after the column table (8 * 4097)
FB = 0x7F0000                  # the frame buffer (work RAM bank $7F)
DP_TEX, DP_RM, DP_END = 0xF0, 0xF3, 0xF5    # the direct-page arguments (see the docstring)
SPR_SCALERS = 256              # sprite scalers for h = 1..256
SPR_ENTRY = 2                  # the entry table's start in the second bank


def fb_off(x, y):
    """view.h's FB_OFF: pixel (x, y)'s offset in the frame buffer"""
    if TILED:                                           # (8 x 8 tiles, VIEW_H / 8 down a strip)
        return ((x >> 3) * (VIEW_H // 8) + (y >> 3)) * 64 + (x & 7) * 8 + (y & 7)
    return ((x >> 3) * 3 + (y >> 5)) * 256 + (x & 7) * 32 + (y & 31)


def column(h):
    """-> (first row, rows, v0, dv) and the texture row of each drawn row"""
    y0 = HALF_H - ((h * 28 + 32) >> 6)
    dv = 16384 // h
    k0 = -y0 if y0 < 0 else 0
    v = (dv >> 1) + k0 * dv
    first = y0 + k0
    rows = []
    y = first
    while y < VIEW_H and y < y0 + h:
        rows.append(v >> 8)
        v += dv
        y += 1
    return first, len(rows), (dv >> 1) + k0 * dv, dv, rows


def scaler(h):
    first, n, _v0, _dv, rows = column(h)
    code = bytearray()
    prev = None                                         # (the texture row loaded in A)
    for k, r in enumerate(rows):
        if r != prev:
            code += bytes([0xA0, r, 0x00, 0xB7, DP_TEX, 0xA8, 0xB1, DP_RM])    # (ldy #r, lda [$F0],y, tay, lda ($F3),y)
            prev = r
        a = FB + fb_off(0, first + k)
        code += bytes([0x9F, a & 0xFF, (a >> 8) & 0xFF, a >> 16])     # (sta long,x)
    code.append(0x6B)                                   # rtl
    return bytes(code)


def spr_scaler(h, base):
    """-> code, entry offsets per canvas row (relative to base)"""
    first, n, _v0, _dv, rows = column(h)
    code = bytearray()
    starts = {}
    k = 0
    while k < n:
        r = rows[k]
        run = 1
        while k + run < n and rows[k + run] == r:
            run += 1
        starts[r] = len(code)                           # (where texture row r's code starts)
        code += bytes([0xA0, r, 0x00, 0xC4, DP_END, 0x90, 0x01, 0x6B, 0xB7, DP_TEX, 0xC9, 0xFF,
                       0xF0, 4 * run + 3, 0xA8, 0xB1, DP_RM])    # (beq skips the tay, lda and the run's stores)
        for j in range(run):
            a = FB + fb_off(0, first + k + j)
            code += bytes([0x9F, a & 0xFF, (a >> 8) & 0xFF, a >> 16])
        k += run
    end = len(code)
    code.append(0x6B)
    entry = []
    for r in range(64):                                 # (a column whose top is r starts at the first row >= r)
        later = [o for rr, o in starts.items() if rr >= r]
        entry.append(base + (min(later) if later else end))
    return bytes(code), entry


def build(spc=b""):
    """the banks (bytes) and their number: the column table and wall scaler
    pointers ($C2), the sprite entries, md_tiles table and SPC engine ($C3),
    then the scalers"""
    bank0 = bytearray(0x10000)
    for h in range(1, MAX_H + 1):
        first, n, v0, dv, _rows = column(h)
        struct.pack_into("<BBHHH", bank0, 8 * h, first, n, v0, dv, 0)
    ent = bytearray(0x10000)                            # the sprite scalers' entry points
    # md_tiles' conversion table at $9000 (see the docstring)
    for k in range(4):
        for j in range(4):
            for b in range(256):
                v = (((b >> 4) >> k & 1) << 1 | ((b & 15) >> k & 1)) << (6 - 2 * j)
                ent[0x9000 + (k * 4 + j) * 256 + b] = v
    assert len(spc) < 0x6000
    struct.pack_into("<H", ent, 0xA000, len(spc))
    ent[0xA002:0xA002 + len(spc)] = spc
    banks = [bank0, ent]
    at = [SCALER_PTRS + 3 * (SCALERS + 1), 0]          # (offset, bank index)
    ptrs = []
    for h in range(1, SCALERS + 1):
        code = scaler(h)
        if at[0] + len(code) > 0x10000:
            banks.append(bytearray(0x10000))
            at = [0, len(banks) - 1]
        banks[at[1]][at[0]:at[0] + len(code)] = code
        ptrs.append((BANK0 + at[1]) << 16 | at[0])
        at[0] += len(code)
    for h, p in enumerate(ptrs, 1):
        bank0[SCALER_PTRS + 3 * h:SCALER_PTRS + 3 * h + 3] = p.to_bytes(3, "little")
    # the sprite scalers
    banks.append(bytearray(0x10000))
    at = [0, len(banks) - 1]
    for h in range(1, SPR_SCALERS + 1):
        code, _e = spr_scaler(h, 0)                     # (first only its size: does it fit this bank?)
        if at[0] + len(code) > 0x10000:
            banks.append(bytearray(0x10000))
            at = [0, len(banks) - 1]
        code, entry = spr_scaler(h, at[0])
        banks[at[1]][at[0]:at[0] + len(code)] = code
        o = SPR_ENTRY + 130 * (h - 1)
        ent[o] = BANK0 + at[1]
        for r, e in enumerate(entry):
            struct.pack_into("<H", ent, o + 2 + 2 * r, e)
        at[0] += len(code)
    out = b"".join(bytes(b) for b in banks)
    return out, len(banks)


def main():
    """Command line: see the module's docstring."""
    global VIEW_H, HALF_H, TILED
    args = sys.argv[1:]
    if "--view" in args:                                # (another view size: its height, horizon, layout)
        k = args.index("--view")
        w, VIEW_H = int(args[k + 1]), int(args[k + 2])
        HALF_H = VIEW_H // 2
        TILED = (w, VIEW_H) != (128, 88)
        del args[k:k + 3]
    out, n = build(Path(args[1]).read_bytes() if len(args) > 1 else b"")
    Path(args[0]).write_bytes(out)
    print(n)


if __name__ == "__main__":
    main()
