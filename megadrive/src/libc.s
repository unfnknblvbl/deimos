| Deimos - Doom RPG on retro consoles
| Copyright (C) 2026 unfnknblvbl and contributors
| SPDX-License-Identifier: GPL-2.0-or-later
| Free software under the GNU General Public License, version 2 or (at your
| option) any later version, with NO WARRANTY: see the LICENSE file.

| The Mega Drive game - what the C compiler expects: memcpy, memset (struct copies).
| Licence: GPL-2.0-or-later.
|
| Overview: the game is linked without a C library (no newlib), but GCC
| emits calls to memcpy and memset on its own, for struct assignments,
| struct initialisers and large local arrays. This file supplies those
| two symbols. Any C file may end up calling them, usually without a
| visible call in the source.
|
| Calling convention (GCC m68k, the default ABI used by this project):
| arguments are pushed on the stack right to left, each as a 32-bit long,
| so after the jsr 4(sp) is the first argument, 8(sp) the second and
| 12(sp) the third. The result comes back in d0. d0-d1/a0-a1 are scratch
| registers that the caller does not expect to be kept; every other
| register must be preserved.
|
| Both routines are simple byte loops: they are only used for small
| amounts of data, so speed does not matter here. The renderer has its
| own fast copy and fill code.

    .text
| void *memcpy(void *d, const void *s, size_t n)
| Copies n bytes from s to d, one byte at a time, from low to high
| addresses. Overlapping areas are not handled specially (as the C
| standard allows).
| In:  4(sp) d (destination), 8(sp) s (source), 12(sp) n (byte count,
|      32 bits; 0 copies nothing).
| Out: d0 = d (as the C standard says memcpy returns its destination).
| Clobbers: d0, d1, a0, a1 (all scratch in the C ABI). Flags undefined.
    .global memcpy
memcpy:
    movea.l 4(sp), a0                   | a0 = destination pointer
    movea.l 8(sp), a1                   | a1 = source pointer
    move.l  12(sp), d1                  | d1 = bytes left to copy
    move.l  a0, d0                      | return value: the destination
    bra.s   2f                          | test the count first (n may be 0)
1:  move.b  (a1)+, (a0)+                | copy one byte
2:  subq.l  #1, d1                      | count down; a borrow (carry set)
    bcc.s   1b                          | means d1 was 0: all bytes done
    rts

| void *memset(void *d, int c, size_t n)
| Fills n bytes at d with the low byte of c.
| In:  4(sp) d (destination), 8(sp) c (fill value; only bits 0-7 used),
|      12(sp) n (byte count, 32 bits; 0 writes nothing).
| Out: d0 = d (the C standard return value).
| Clobbers: d0, d1, a0 (scratch in the C ABI). Flags undefined.
| Stack: d is kept on the stack during the loop because d0 holds the
| fill byte.
    .global memset
memset:
    movea.l 4(sp), a0                   | a0 = destination pointer
    move.l  8(sp), d0                   | d0 = fill value (low byte used)
    move.l  12(sp), d1                  | d1 = bytes left to fill
    move.l  a0, -(sp)                   | save d for the return value
    bra.s   2f                          | test the count first (n may be 0)
1:  move.b  d0, (a0)+                   | store one byte
2:  subq.l  #1, d1                      | count down; carry set when d1
    bcc.s   1b                          | was already 0: finished
    move.l  (sp)+, d0                   | return d
    rts
