/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the 32X: the few C library functions the game uses. Licence: GPL-2.0-or-later. */
/*
 * The SH-2 build links no C library (build.sh: -ffreestanding,
 * -fno-builtin, -nostdlib by way of a bare ld), so these are the standard
 * functions the game's C (core/, snes/src/, sh2/) calls, with the standard
 * meanings. GCC may also emit calls to memset/memcpy for struct copies and
 * clears. memset and memcpy go a long word (4 bytes) at a time where the
 * pointers allow it: the SH-2 has a 32-bit bus to SDRAM.
 */
#include "libc.h"

/* memset: fill n bytes at d with the byte c; returns d. */
void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    unsigned int v = (unsigned char)c;
    v |= v << 8;                /* the byte copied into all four bytes of v */
    v |= v << 16;
    while (n && ((unsigned int)p & 3)) {        /* single bytes up to a 4-byte boundary */
        *p++ = (unsigned char)c;
        n--;
    }
    while (n >= 16) {           /* then 16 bytes per pass, as four long words */
        ((unsigned int *)p)[0] = v;
        ((unsigned int *)p)[1] = v;
        ((unsigned int *)p)[2] = v;
        ((unsigned int *)p)[3] = v;
        p += 16;
        n -= 16;
    }
    while (n--) *p++ = (unsigned char)c;       /* the 0..15 bytes left */
    return d;
}

/* memcpy: copy n bytes from s to d (they must not overlap); returns d.
 * When both pointers are 4-byte aligned it copies long words, then the
 * rest bytewise; otherwise it goes byte by byte. */
void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    if ((((unsigned int)a | (unsigned int)b) & 3) == 0) {
        while (n >= 4) {
            *(unsigned int *)a = *(const unsigned int *)b;
            a += 4;
            b += 4;
            n -= 4;
        }
    }
    while (n--) *a++ = *b++;
    return d;
}

/* memmove: copy n bytes from s to d, which may overlap; returns d. */
void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    if (a <= b || a >= b + n) return memcpy(d, s, n);  /* a forward copy is safe */
    while (n--) a[n] = b[n];    /* d is inside the source, after s: copy backwards */
    return d;
}

/* memcmp: compare n bytes; returns <0, 0 or >0 by the first differing byte (unsigned). */
int memcmp(const void *x, const void *y, size_t n)
{
    const unsigned char *a = x, *b = y;
    for (; n; n--, a++, b++)
        if (*a != *b) return *a - *b;
    return 0;
}

/* strlen: the length of the 0-terminated string s. */
size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* strcmp: compare two 0-terminated strings by unsigned bytes; <0, 0 or >0. */
int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

/* strcpy: copy s, with its terminating 0, to d; returns d. */
char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++)) {}
    return r;
}
