#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The engine the Doom RPG editor carries for the SNES: the ROM up to the
campaign's block, with the resources made from DoomRPG.jar (the bank
tools/snes_ui_res.py writes: graphics, music and sound effects) blanked,
so the editor can make them again from the user's own jar (its C++ gives
the same bytes) and put the maps after it.

  snes_make_engine.py out/deimos.sfc out/res.bin <version> OUT.eng [console [game.3dsx|game.32x|game.gbc|game.gba]]

Engine file: the format shared with the other consoles' (the Mega Drive game's
tools/make_engine.py): "DEIMENG1", u8 console (3 = SNES, 4 = SNES with the
SuperFX, 5 = the 3DS, 6 = the 32X, 7 = the Game Boy Color, 8 = the Game Boy Advance), regions ("snes_res"), the version, the maps' block
address and the end of its space, the template (ZX0); the 3DS's then has
the .3dsx before its romfs's cartridge image (u32 size, u32 packed size,
ZX0): the 3DS game reads the SNES layout (3ds/build.sh); the 32X's, the
cartridge's first 512 KB, its code (32x/build.sh); the Game Boy Color's, its
cartridge's first 1 MB (gbc/build.sh); the Game Boy Advance's, its
cartridge's first 512 KB, its code (gba/build.sh).

The engine file in order: "DEIMENG1", u8 console, 3 zero bytes; u32 the
template's size, u32 1 (the number of regions); the region: u8 name
length, "snes_res", u32 its offset, u32 its size; u8 version length, the
version; u32 the block's address, u32 the end of its space; u32 the packed
template's size, the template (ZX0); then the console's extra part (u32
size, u32 packed size, ZX0 data) for the 32X, the GBA, the GBC and the 3DS.
All little-endian. ZX0 compression is the external zx0 tool (ZX0=).
Licence: GPL-2.0-or-later.
"""
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ZX0 = os.environ.get("ZX0", "zx0")


def zx0(data):
    """data compressed by the zx0 tool (-f: overwrite, -q: quick)"""
    with tempfile.TemporaryDirectory() as td:
        Path(td, "i").write_bytes(data)
        subprocess.run([ZX0, "-f", "-q", str(Path(td, "i")), str(Path(td, "o"))], check=True, capture_output=True)
        return Path(td, "o").read_bytes()


def main():
    """Command line: see the module's docstring."""
    rom = bytearray(Path(sys.argv[1]).read_bytes())
    res = Path(sys.argv[2]).read_bytes()
    version, out = sys.argv[3], Path(sys.argv[4])
    console = int(sys.argv[5]) if len(sys.argv) > 5 else 3
    if console == 4:            # the SuperFX build: the header at $7FB0, banks from $40, 2 MB
        hdr, base, limit = 0x7FB0, 0x40, 0x200000
    elif console == 6:          # the 32X: its image (32x/tools/mars_image.py) after its code's 512 KB of 4 MB
        hdr, base, limit = 0xFFB0, 0xC0, 0x380000
    else:
        hdr, base, limit = 0xFFB0, 0xC0, 0x400000
    res_at = (rom[hdr + 7] - base) << 16    # (the header's resource bank -> file offset)
    camp = (rom[hdr + 6] - base) << 16      # (and the campaign block's)
    if rom[res_at:res_at + len(res)] != res or rom[camp:camp + 4] != b"MDCP":
        raise SystemExit("the ROM is not laid out as tools/snes_rom.py makes it")
    rom[res_at:res_at + len(res)] = bytes(len(res))     # (the jar's resources blanked: not distributed)
    template = bytes(rom[:camp])            # (the ROM up to the maps)
    packed = zx0(template)
    e = bytearray(b"DEIMENG1" + bytes([console, 0, 0, 0]))
    e += struct.pack("<II", len(template), 1)
    e += bytes([len("snes_res")]) + b"snes_res" + struct.pack("<II", res_at, len(res))
    e += bytes([len(version)]) + version.encode()
    e += struct.pack("<II", camp, limit)
    e += struct.pack("<I", len(packed)) + packed
    if console == 6:            # the 32X: its first 512 KB (the code); the SNES image from $80000
        x = Path(sys.argv[6]).read_bytes()
        wrap = x[:0x80000].rstrip(b"\xff")      # (the editor pads it back to 512 KB; long $FF runs
                                                # are also very slow for zx0 to pack)
        if x[0x80000:0x80000 + 0x10000] != Path(sys.argv[1]).read_bytes()[:0x10000]:
            raise SystemExit("the 32X cartridge does not hold the image at $80000")
        wp = zx0(wrap)
        e += struct.pack("<II", len(wrap), len(wp)) + wp
    if console == 8:            # the Game Boy Advance: its first 512 KB (the code); the SNES image from $80000
        x = Path(sys.argv[6]).read_bytes()
        wrap = x[:0x80000].rstrip(b"\xff")      # (the editor pads it back to 512 KB)
        if x[0x80000:0x80000 + 0x10000] != Path(sys.argv[1]).read_bytes()[:0x10000]:
            raise SystemExit("the GBA cartridge does not hold the image at $80000")
        wp = zx0(wrap)
        e += struct.pack("<II", len(wrap), len(wp)) + wp
    if console == 7:            # the Game Boy Color: its first 1 MB (the code); the SNES data from $100000
        x = Path(sys.argv[6]).read_bytes()
        wrap = x[:0x100000].rstrip(b"\xff")     # (the editor pads it back to 1 MB)
        if x[0x100000:0x100000 + len(res)] != res:
            raise SystemExit("the Game Boy Color cartridge does not hold the SNES data at $100000")
        wp = zx0(wrap)
        e += struct.pack("<II", len(wrap), len(wp)) + wp
    if console == 5:            # the 3DS: the .3dsx without the cartridge image at its end
        x = Path(sys.argv[6]).read_bytes()
        whole = Path(sys.argv[1]).read_bytes()
        if not x.endswith(whole):
            raise SystemExit("the .3dsx does not end with the cartridge image")
        wrap = x[:len(x) - len(whole)]
        wp = zx0(wrap)
        e += struct.pack("<II", len(wrap), len(wp)) + wp
    out.write_bytes(e)
    print(f"{out}: engine {len(template) // 1024} KB ({len(packed) // 1024} KB packed), resources "
          f"{len(res) // 1024} KB blanked, maps at ${camp:06X}")


if __name__ == "__main__":
    main()
