#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The 32X's copy of the SNES game's cartridge image, without what only the
SNES runs: the SNES build's image (tools/snes_rom.py) holds the 65816's
code (banks $C0-$C1) and its generated banks (the 3D view's column table
and compiled scalers, $C2-...), about 1 MB the 32X never reads. The 32X
reads only the header (bank $C0's $FFB0..), the SPC driver's tables and
samples ($C3A000 in the SNES image) and the resources and the campaign's
block (the banks the header names). So its image is:

  bank $C0   the SNES image's bank $C0 (the header, as the editor knows it)
  bank $C1   the SPC driver's part: the SNES image's $C3A000..$C3FFFF at
             $C1A000 (sh2/sound32x.c reads it there)
  bank $C2.. the resources, then the block (the header's $FFB7, $FFB6 set)

which leaves about 1 MB more of the 32X's 4 MB for the maps.

  mars_image.py snes.sfc OUT.sfc
Licence: GPL-2.0-or-later.
"""
import sys
from pathlib import Path

SPC_FROM = 0x3A000          # $C3A000
SPC_TO = 0x1A000            # $C1A000


def main():
    """Read the SNES image and write the 32X's (see the module docstring)."""
    full = Path(sys.argv[1]).read_bytes()
    res_at = (full[0xFFB7] - 0xC0) << 16    # the resources' offset (header byte $FFB7: their bank)
    blk_at = (full[0xFFB6] - 0xC0) << 16    # the campaign block's (header byte $FFB6)
    # (the block starts with the magic "MDCP", and must come after the resources)
    if full[blk_at:blk_at + 4] != b"MDCP" or res_at >= blk_at:
        raise SystemExit("not the SNES game's image (tools/snes_rom.py)")
    img = bytearray(full[:0x10000])         # bank $C0 as it is
    bank1 = bytearray(b"\xff" * 0x10000)
    # the SPC part: $C3A000..$C3FFFF -> $C1A000..$C1FFFF (24 KB)
    bank1[SPC_TO - 0x10000:] = full[SPC_FROM:SPC_FROM + 0x10000 - (SPC_TO - 0x10000)]
    img += bank1
    img[0xFFB7] = 0xC2                      # the resources now start at bank $C2,
    img[0xFFB6] = 0xC2 + ((blk_at - res_at) >> 16)  # the block as far after them as before
    img += full[res_at:]                    # the resources, the block and the rest, unchanged
    # the header's size code and checksum, of the image padded to a power
    # of two (as the editor's finish_snes)
    size = 0x40000
    while size < len(img):
        size *= 2
    img += b"\xff" * (size - len(img))
    code = 0                                # the size code: size = 1 KB << code
    while 1024 << (code + 1) <= size:
        code += 1
    img[0xFFD7] = code
    # the checksum's complement (at $FFDC) and the checksum ($FFDE), both
    # little-endian, summed with the usual placeholder $FFFF, $0000 in place
    img[0xFFDC:0xFFE0] = b"\xff\xff\x00\x00"
    s = sum(img) & 0xFFFF
    img[0xFFDC:0xFFE0] = bytes([(s ^ 0xFFFF) & 0xFF, (s ^ 0xFFFF) >> 8, s & 0xFF, s >> 8])
    while len(img) and img[-1] == 0xFF:     # the padding off again (make_rom.py pads the cartridge)
        img.pop()
    Path(sys.argv[2]).write_bytes(bytes(img))
    print(f"{sys.argv[2]}: {len(img) // 1024} KB (the SNES image's {len(full.rstrip(bytes([255]))) // 1024} KB)")


if __name__ == "__main__":
    main()
