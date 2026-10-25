#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The engine the Doom RPG editor carries: this ROM with nothing of the
original game in it. Every part made from DoomRPG.jar is blanked (zeros):
the status bar, fonts and title (listed, so the editor makes them again
from the user's own jar - its C++ port of tools/build_ui_gfx.py gives the
same bytes) and the level's own parts (the editor's level builder writes
those: the texts table and the level blob in bank 2, bank 3, the strings
and sprite directories, banks 9 on).

  make_engine.py rom/deimos.sms rom/deimos.sym build/gfx OUT.eng
  make_engine.py rom/deimos.gg rom/deimos_gg.sym build/gfx OUT.eng --gg   (the Game Gear: console 2,
                 its own status bar, font index and title, *_gg.bin)

Engine file: as megadrive/tools/make_engine.py (console 0 = Master System,
no maps' block). Layout, all integers little-endian:
  "DEIMENG1", console byte (0 SMS, 2 Game Gear), 3 zero bytes;
  u32 unpacked template size, u32 number of blanked regions;
  per region: name length byte, name, u32 ROM offset, u32 size;
  version length byte, version string (the VERSION file);
  two zero u32s (no maps' block);
  u32 packed size, then the template compressed with zx0.
The template is the ROM's first FIRST_DATA_BANK banks (0..8) after
blanking; everything from bank 9 on is level data the editor writes anew.

Inputs: the built ROM, the wlalink symbol file (to find where each piece of
generated UI art sits) and build/gfx (the generated .bin files, compared
with the ROM bytes as a safety check before they are blanked). Needs the
zx0 compressor (the ZX0 environment variable overrides its path).
Licence: GPL-2.0-or-later.
"""
import os, shutil
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ZX0 = os.environ.get("ZX0", "zx0")
BANK = 0x4000                   # one Sega-mapper bank: 16 KB
# generated UI art made from the jar: build/gfx file stem -> ROM label of its .incbin
UI = {"hud_palette": "hud_palette_data", "font_index": "font_index_data", "hud_tiles": "hud_tiles_data",
      "hud_static": "hud_static_data", "dlg_font": "dlg_font_data", "title_tiles": "title_tiles_data",
      "title_map": "title_map_data"}
# what the editor's level builder (editor/src/sms_build.cpp) rewrites:
# TEXTS_ADDR/TEXTS_MAX the texts table (the game's texts the code shows,
# from the jar: ../../tools/game_text.py) at $8000 in bank 2, 8 KB;
# LEVEL_ADDR/LEVEL_MAX the level blob (the game data compiled by gen_cdata.py)
# at $A000 in bank 2, 8 KB; STRINGS_MAX the first 8 KB of MISC_BANK (bank 4,
# the strings); SPR_DIR_SMALL the small-view sprite directory at $B000 in
# bank 4 (4 KB); FIRST_DATA_BANK the first of the level's data banks (9)
LEVEL_ADDR, LEVEL_MAX, STRINGS_MAX, SPR_DIR_SMALL, FIRST_DATA_BANK, MISC_BANK = 0xA000, 0x2000, 0x2000, 0xB000, 9, 4
TEXTS_ADDR, TEXTS_MAX = 0x8000, 0x2000


def labels(sym):
    """Read the [labels] section of a wlalink symbol file into {name: ROM offset}.

    Each line is "BB:AAAA name" (bank and CPU address, hex). Addresses
    below $8000 are fixed banks 0 and 1, so the address is the offset;
    above, they are in slot 2 ($8000-$BFFF), so the offset is
    bank * 16 KB + (address - $8000).
    """
    out, sec = {}, None
    for line in Path(sym).read_text().splitlines():
        line = line.strip()
        if line.startswith("["):
            sec = line
            continue
        m = re.match(r"([0-9a-f]+):([0-9a-f]{4}) (\S+)", line, re.I)
        if m and sec == "[labels]":
            bank, a = int(m.group(1), 16), int(m.group(2), 16)
            out[m.group(3)] = a if a < 0x8000 else bank * BANK + (a - 0x8000)
    return out


def main():
    """Blank the jar-made parts of a built ROM and write the engine file.

    Arguments come from sys.argv (see the usage above). Writes OUT.eng and
    prints a one-line summary. Raises SystemExit if a generated UI file does
    not match the ROM bytes at its label (the ROM is stale).
    """
    rom_path, sym, gfx, out = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3]), Path(sys.argv[4])
    gg = "--gg" in sys.argv
    # UI parts with a separate Game Gear version (build/gfx/<name>_gg.bin)
    GG_FILES = {"hud_tiles", "font_index", "hud_static", "title_tiles", "title_map"}
    rom = bytearray(rom_path.read_bytes())
    lab = labels(sym)
    regions = []
    # blank each UI part, in ROM order, after checking it is the generated one
    for name, label in sorted(UI.items(), key=lambda kv: lab[kv[1]]):
        b = (gfx / f"{name}{'_gg' if gg and name in GG_FILES else ''}.bin").read_bytes()
        a = lab[label]
        if rom[a:a + len(b)] != b:
            raise SystemExit(f"{name}: the ROM's bytes are not the generated ones")
        rom[a:a + len(b)] = bytes(len(b))
        regions.append((name, a, len(b)))
    # the level's parts: the texts table and the level blob in bank 2, all
    # of bank 3 (level header and small tables), the strings and small
    # sprite directory in bank 4, and every bank from 9 on
    t = 2 * BANK + (TEXTS_ADDR - 0x8000)
    rom[t:t + TEXTS_MAX] = bytes(TEXTS_MAX)
    rom[2 * BANK + (LEVEL_ADDR - 0x8000):2 * BANK + (LEVEL_ADDR - 0x8000) + LEVEL_MAX] = bytes(LEVEL_MAX)
    rom[3 * BANK:4 * BANK] = bytes(BANK)
    rom[MISC_BANK * BANK:MISC_BANK * BANK + STRINGS_MAX] = bytes(STRINGS_MAX)
    o = MISC_BANK * BANK + (SPR_DIR_SMALL - 0x8000)
    rom[o:o + 0x1000] = bytes(0x1000)
    rom[FIRST_DATA_BANK * BANK:] = bytes(len(rom) - FIRST_DATA_BANK * BANK)
    template = bytes(rom[:FIRST_DATA_BANK * BANK])     # banks 0..8: the engine proper
    version = (Path(__file__).resolve().parent.parent / "VERSION").read_text().strip()
    with tempfile.TemporaryDirectory() as td:
        Path(td, "i").write_bytes(template)
        # compress the template with zx0 (-f overwrite, -q quick mode)
        subprocess.run([ZX0, "-f", "-q", str(Path(td, "i")), str(Path(td, "o"))], check=True, capture_output=True)
        packed = Path(td, "o").read_bytes()
    # the engine file (layout in the module docstring)
    e = bytearray(b"DEIMENG1" + bytes([2 if gg else 0, 0, 0, 0]))
    e += struct.pack("<II", len(template), len(regions))
    for name, a, n in regions:
        e += bytes([len(name)]) + name.encode() + struct.pack("<II", a, n)
    e += bytes([len(version)]) + version.encode()
    e += struct.pack("<II", 0, 0)                      # no maps' block
    e += struct.pack("<I", len(packed)) + packed
    out.write_bytes(e)
    print(f"{out}: engine {len(template) // 1024} KB ({len(packed) // 1024} KB packed), {len(regions)} regions "
          f"blanked ({sum(n for _, _, n in regions)} bytes)")


if __name__ == "__main__":
    main()
