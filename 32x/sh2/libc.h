/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the 32X: the few C library functions the game uses (libc.c). Licence: GPL-2.0-or-later. */
/* The SH-2 build has no C library; the game's sources include this in its
 * place for size_t, NULL and these standard functions. */
#ifndef LIBC32X_H
#define LIBC32X_H
typedef unsigned int size_t;           /* 32 bits on the SH-2 */
#define NULL ((void *)0)
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
char *strcpy(char *d, const char *s);
#endif
