#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The engine the Doom RPG editor carries: this ROM with nothing of the
original game in it. Every part made from DoomRPG.jar is blanked (zeros)
and listed, so the editor can make it again from the user's own jar (its
C++ ports of the same tools give the same bytes) and put the maps after it.
The header's checksum and ROM end and the areas table's address ($1D8),
which the editor works out for each ROM, are zeroed too: the engine is the
same from a build with the jar and from one without it (build.sh with
DEIMOS_NOJAR=1, tools/nojar.py).

  make_engine.py build/deimos.elf out/deimos.md build/gen OUT.eng

Engine file (little-endian), the format shared with the Master System's:
  "DEIMENG1", u8 console (1 = Mega Drive), u8 0, u16 0
  u32 template size, u32 number of regions
  per region: u8 name length, name, u32 offset, u32 size
  u8 version length, version (shown on the title screen)
  u32 the maps' block address (Mega Drive), u32 the end of its space
  u32 packed size, then the template (ZX0)
Licence: GPL-2.0-or-later.
"""
import os, shutil
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ZX0 = os.environ.get("ZX0", "zx0")  # the ZX0 compressor (an external program)
UI = ["ui_font", "ui_faces", "ui_icons", "ui_hud_pal", "ui_dlg_pal", "title_tiles", "title_map", "title_pal"]  # the UI arrays made from the jar's images (build_md_ui.py)


def c_arrays(path):
    """The `const unsigned char/short name[] = {...}` arrays of a generated C
    file: {name: bytes}, shorts as big-endian words (as in the ROM).
    """
    out = {}
    for m in re.finditer(r'const unsigned (char|short) (\w+)\[\d*\][^=]*= \{([^}]*)\}', Path(path).read_text()):
        t, name, body = m.groups()
        vals = [int(v) for v in body.replace("\n", "").split(",") if v.strip()]
        out[name] = bytes(vals) if t == "char" else b"".join(v.to_bytes(2, "big") for v in vals)
    return out


def songs(path):
    """The song_* byte blocks of music_data.s (build_music.py's output): {label: bytes}."""
    out, cur = {}, None
    for line in Path(path).read_text().splitlines():
        m = re.match(r"song_(\w+):", line)
        if m:
            cur = "song_" + m.group(1)
            out[cur] = bytearray()
            continue
        line = line.strip()
        if cur and line.startswith(".byte"):
            out[cur] += bytes(int(v) for v in line[5:].split(","))
        elif cur and line:
            cur = None
    return {k: bytes(v) for k, v in out.items()}


def main():
    """make_engine.py <elf> <rom> <gen dir> <out.eng>: finds each generated
    block (UI graphics, songs, sound effects) in the ROM by its symbol's
    address, checks the bytes match, zeroes them, cuts the ROM at the
    campaign block, compresses it with ZX0 and writes the engine file
    (format in the module docstring).
    """
    elf, rom_path, gen, out = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4])
    rom = bytearray(rom_path.read_bytes())
    nm = subprocess.run([os.environ.get("CROSS", "m68k-elf-") + "nm", elf], capture_output=True, text=True, check=True).stdout
    addr = {l.split()[2]: int(l.split()[0], 16) for l in nm.splitlines() if len(l.split()) == 3}  # symbol -> address
    blocks = {}
    ui = c_arrays(gen / "ui_data.c")
    for n in UI:
        blocks[n] = ui[n]
    blocks.update(songs(gen / "music_data.s"))
    blocks.update({k: v for k, v in c_arrays(gen / "sfx_data.c").items() if k.startswith("sfx_")})
    camp = struct.unpack_from(">I", rom, 0x1D0)[0]  # the ROM directory at $1C8: "DEIMOSMD", then the block's address ...
    limit = struct.unpack_from(">I", rom, 0x1D4)[0]  # ... and the end of its space
    regions = []
    for name in sorted(blocks, key=lambda n: addr[n]):
        a, b = addr[name], blocks[name]
        if rom[a:a + len(b)] != b:
            raise SystemExit(f"{name}: the ROM's bytes are not the generated ones")
        rom[a:a + len(b)] = bytes(len(b))
        regions.append((name, a, len(b)))
    # what the editor works out for each ROM it makes: the header's checksum
    # ($18E) and the ROM's end address ($1A4) - so the engine is the same
    # whatever maps (and jar) it was built with
    rom[0x18E:0x190] = bytes(2)
    rom[0x1A4:0x1A8] = bytes(4)
    rom[0x1D8:0x1DC] = bytes(4)     # (and the areas table's address, after the maps: the editor's)
    template = bytes(rom[:camp])  # the ROM up to the campaign block (the editor writes the maps after it)
    version = (Path(__file__).resolve().parent.parent / "VERSION").read_text().strip()
    with tempfile.TemporaryDirectory() as td:
        Path(td, "i").write_bytes(template)
        subprocess.run([ZX0, "-f", "-q", str(Path(td, "i")), str(Path(td, "o"))], check=True, capture_output=True)
        packed = Path(td, "o").read_bytes()
    e = bytearray(b"DEIMENG1" + bytes([1, 0, 0, 0]))
    e += struct.pack("<II", len(template), len(regions))
    for name, a, n in regions:
        e += bytes([len(name)]) + name.encode() + struct.pack("<II", a, n)
    e += bytes([len(version)]) + version.encode()
    e += struct.pack("<II", camp, limit)
    e += struct.pack("<I", len(packed)) + packed
    out.write_bytes(e)
    print(f"{out}: engine {len(template) // 1024} KB ({len(packed) // 1024} KB packed), {len(regions)} regions "
          f"blanked ({sum(n for _, _, n in regions)} bytes), maps at ${camp:06X}")


if __name__ == "__main__":
    main()
