#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.
"""Looks for the original game's texts in Deimos's own files.

Deimos must hold no text of the original game: every text a ROM shows is
read from the user's DoomRPG.jar when the ROM is built. This tool reads
every string of the jar's .str tables and searches, case-insensitively and
with runs of spaces and the game's '|' line breaks counted as one space:

  - every file git tracks in the repository (text and binary alike);
  - the editor's engines (editor/engines/*.eng, unpacked here: their
    template and, for the wrapped consoles, their wrap);
  - any further files named on the command line (built ROMs, say).

What is searched for: each whole string of 6 or more characters, and each
piece of 6 or more characters between a string's "%1"-style placeholders
(the game fills those in, so code could hold the pieces), whole words only
(a match may not have a letter or digit just before or after it). Strings
that are a single plain-English word ("Continue", "Options", "Barrel")
match a great deal of code (C's `continue`), so a single word is reported
only where it appears as a text (between quotes, or as the only thing on a
line of data), in the case the game spells it, or wholly in capitals.

A few of Deimos's own words are also strings of the game (ALLOWED): they
are listed apart and not counted.

Usage: jar_text_scan.py [--jar DoomRPG.jar] [extra files...]
Prints each hit (file and line, the jar's string, the table it is in) and
the number of hits; exits 1 when there are any.
"""
import argparse
import re
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Deimos's own words that are also one of the game's strings, allowed where
# they are used as they are here (reported, not counted): (files whose
# path starts so, the jar's string, why it is not the game's text)
ALLOWED = [
    ("editor/src/", "Cancel", "the editor's own button for cancelling a window"),
    ("editor/src/keytext.cpp", "Notebook", "the editor's name for the notebook button's token, {NOTEBOOK}"),
    ("editor/src/project.cpp", "Portal", "PORTAL: the name of script command 38 (an identifier)"),
    ("megadrive/tools/gamedata.py", "Portal", "PORTAL: the name of script command 38 (an identifier)"),
    ("mastersystem/tools/gamedata.py", "Portal", "PORTAL: the name of script command 38 (an identifier)"),
]


def read_str_table(data: bytes):
    """One .str table (u16 count, then per string a u16 length and that
    many Latin-1 bytes, little-endian) as a list of strings."""
    n = struct.unpack_from('<H', data, 0)[0]
    p, out = 2, []
    for _ in range(n):
        ln = struct.unpack_from('<H', data, p)[0]
        out.append(data[p + 2:p + 2 + ln].decode('latin1'))
        p += 2 + ln
    return out


def norm(s: str) -> str:
    """s lowercased, with '|' as a space and every run of white space as
    one space (how texts are compared)."""
    return re.sub(r'\s+', ' ', s.replace('|', ' ')).lower()


def zx0_decompress(d: bytes, p: int = 0) -> bytes:
    """Unpacks ZX0 data starting at d[p] (the format editor/src/zx0.cpp
    reads): literals and copies, Elias gamma lengths, an offset of 256
    ending it."""
    out = bytearray()
    st = {'p': p, 'mask': 0, 'val': 0, 'last': 0, 'back': False}

    def byte():
        # the next input byte (kept as the last one for backtracking)
        st['last'] = d[st['p']]
        st['p'] += 1
        return st['last']

    def bit():
        # the next control bit, MSB first (or bit 0 of the last byte read)
        if st['back']:
            st['back'] = False
            return st['last'] & 1
        st['mask'] >>= 1
        if not st['mask']:
            st['mask'] = 128
            st['val'] = byte()
        return 1 if st['val'] & st['mask'] else 0

    def gamma(inv):
        # an interlaced Elias gamma number
        v = 1
        while not bit():
            v = (v << 1) | (bit() ^ inv)
        return v

    def copy(off, ln):
        # ln bytes from off bytes back (may overlap)
        for _ in range(ln):
            out.append(out[-off])

    last_off = 1
    state = 'lit'
    while True:
        if state == 'lit':
            ln = gamma(0)
            for _ in range(ln):
                out.append(byte())
            if bit():
                state = 'new'
                continue
            copy(last_off, gamma(0))
            state = 'new' if bit() else 'lit'
            continue
        hi = gamma(1)
        if hi == 256:
            return bytes(out)
        last_off = hi * 128 - (byte() >> 1)
        st['back'] = True
        copy(last_off, gamma(0) + 1)
        state = 'new' if bit() else 'lit'


def engine_parts(path: Path):
    """The unpacked template (and wrap, when there is one) of an engine
    file (format in editor/src/engine.h)."""
    d = path.read_bytes()
    if d[:8] != b'DEIMENG1':
        return [d]
    nreg = struct.unpack_from('<I', d, 16)[0]
    p = 20
    for _ in range(nreg):
        p += 1 + d[p] + 8                       # name, offset, size
    p += 1 + d[p]                               # the version string
    packed = struct.unpack_from('<I', d, p + 8)[0]
    p += 12
    parts = [zx0_decompress(d, p)]
    p += packed
    if p + 8 <= len(d):                         # the wrapped consoles' code
        parts.append(zx0_decompress(d, p + 8))
    return parts


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--jar', default=str(ROOT / 'import' / 'DoomRPG.jar'))
    ap.add_argument('files', nargs='*', help='more files to search (ROMs)')
    a = ap.parse_args()

    # ---- the needles: (normalised text, the jar's string, its table)
    z = zipfile.ZipFile(a.jar)
    needles = {}
    words = {}       # single words: normalised -> (original, table)
    for name in sorted(n for n in z.namelist() if n.endswith('.str')):
        for s in read_str_table(z.read(name)):
            if s.startswith('/') and s.endswith('.bsp'):
                continue                        # a script's map path, not a text
            # the whole string, and the pieces between its placeholders
            # that are more than one word
            pieces = [s] + [x for x in re.split(r'%\d', s) if ' ' in norm(x).strip()]
            for piece in pieces:
                t = norm(piece).strip()
                if len(t) < 6:
                    continue
                if re.fullmatch(r"[a-z]+[:!.?]*", t):
                    words.setdefault(t, (piece.strip(), name))
                else:
                    needles.setdefault(t, (s, name))
    # one pass per file: every needle as an alternative of one regex, the
    # longest first, inside a lookahead so that every position is tried
    order = sorted(needles, key=len, reverse=True)
    big = re.compile(r'(?<![a-z0-9])(?=(' + '|'.join(re.escape(t) for t in order) + r')(?![a-z0-9]))')
    # a single word counts as a text between quotes or alone on a line,
    # spelt as the game spells it or in capitals
    spell = {}
    for t, (orig, tb) in words.items():
        for x in (orig, orig.upper()):
            spell[x] = (orig, tb)
    alts = '|'.join(re.escape(x) for x in sorted(spell, key=len, reverse=True))
    wbig = re.compile(r'(?:["\'>\x00][ \t]*(%s)[ \t]*["\'<\x00])|(?:^[ \t]*(%s)[ \t]*$)' % (alts, alts), re.M)

    # ---- the haystacks
    targets = []
    files = subprocess.run(['git', '-C', str(ROOT), 'ls-files'], capture_output=True,
                           text=True, check=True).stdout.split('\n')
    for rel in files:
        p = ROOT / rel
        # (this script itself names the allowed words, so it is left out)
        if not rel or not p.is_file() or rel.startswith('editor/third_party/') or rel == 'tools/jar_text_scan.py':
            continue                            # (others' code, under their licences)
        if p.suffix == '.eng':
            for i, part in enumerate(engine_parts(p)):
                targets.append((f'{rel} (unpacked {"template" if i == 0 else "wrap"})', part))
        else:
            targets.append((rel, p.read_bytes()))
    for f in a.files:
        targets.append((f, Path(f).read_bytes()))

    def search(raw):
        """The jar's strings found in the text raw: {(string, table): None}
        in the order found."""
        found = {}
        for m in big.finditer(norm(raw)):
            found.setdefault(needles[m.group(1)], None)
        for m in wbig.finditer(re.sub(r'[ \t]+', ' ', raw.replace('|', ' '))):
            found.setdefault(spell[m.group(1) or m.group(2)], None)
        return found

    hits = 0
    allowed = []
    for label, data in targets:
        raw = data.decode('latin1')
        found = search(raw)
        for key in list(found):
            why = next((w for pre, t, w in ALLOWED if label.startswith(pre) and t == key[0]), None)
            if why:
                allowed.append(f'{label}: {key[0]!r} allowed: {why}')
                del found[key]
        if not found:
            continue
        hits += len(found)
        if '\0' in raw:                         # a binary file: no lines to point at
            for s, tb in found:
                print(f'{label}: {s!r} ({tb})')
            continue
        # a text file: each line holding one, then any found only across lines
        for no, line in enumerate(raw.split('\n'), 1):
            for s, tb in search(line):
                if (s, tb) in found:
                    found[(s, tb)] = no
                    print(f'{label}:{no}: {s!r} ({tb}): {line.strip()[:100]}')
        for (s, tb), no in found.items():
            if no is None:
                print(f'{label}: {s!r} ({tb}) (across lines)')
    for a in allowed:
        print(a)
    print(f'{hits} hits ({len(allowed)} allowed uses listed) of {len(needles)} texts and {len(words)} words from the jar in {len(targets)} files')
    return 1 if hits else 0


if __name__ == '__main__':
    sys.exit(main())
