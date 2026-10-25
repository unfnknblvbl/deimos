#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The Game Boy Color cartridge (4 MB, MBC5):
  banks 0..63     GBDK's code (the editor keeps it: the "wrap")
  bank 64 on      the SNES game's data (its cartridge from the resources'
                  bank on: the resources the SNES makes from the original
                  game, then the campaign's block), as far as its last byte
                  that isn't $FF
  the next bank   the Game Boy's resources (tools/gb_res.py: made from that
                  data; the code finds them by their "GBRES1")
  $3E0000         a test build's script (tools/make_script.py's)
and the header's checksums. The editor's GBC output is made the same way.

  gb_pack.py code.gbc snes.sfc [script.bin] OUT.gbc

Unused ROM is $FF. The code finds the campaign's block and the resources at
run time (src/plat.c's plat_init looks for "GBRES1" from bank 64), so their
sizes may change without rebuilding the code.
Licence: GPL-2.0-or-later.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gb_res  # noqa: E402

DATA_AT = 64 * 0x4000                   # bank 64: the SNES data (16 KB banks)
SCRIPT_AT = 0x3E0000                    # the test script (src/plat.h's SCRIPT_BANK)
SIZE = 0x400000                         # 4 MB


def pack(code, snes, script=b""):
    """The cartridge from the linked code, the SNES ROM and a script; returns
    (rom, bytes of code used, SNES data size, resources size)."""
    # (the last byte of code that is not $FF: for the report)
    used = max((i for i in range(min(len(code), DATA_AT)) if code[i] != 0xFF), default=0)
    if len(code) > DATA_AT and any(b != 0xFF for b in code[DATA_AT:]):
        raise SystemExit("the code is past bank 63")
    res_at = (snes[0xFFB7] & 0x3F) * 0x10000
    if snes[res_at:res_at + 8] == b"DEIMRES1":
        s, res, _ = gb_res.build(snes)      # (the SNES data, the Game Boy's resources)
    else:
        # (no resources: a build without the jar, DEIMOS_NOJAR=1, for the
        # engine only - the SNES data as it is, no Game Boy resources; both
        # come after the code, where the editor writes them)
        end = len(snes)
        while end > res_at and snes[end - 1] == 0xFF:
            end -= 1
        s, res = type("Snes", (), {"data": snes[res_at:end]}), b""
    rom = bytearray(b"\xFF" * SIZE)
    rom[:min(len(code), DATA_AT)] = code[:DATA_AT]
    rom[DATA_AT:DATA_AT + len(s.data)] = s.data
    res_at = (DATA_AT + len(s.data) + 0x3FFF) & ~0x3FFF    # (the next bank's start)
    if res_at + len(res) > (SCRIPT_AT if script else SIZE):
        raise SystemExit("the data don't fit in 4 MB")
    rom[res_at:res_at + len(res)] = res
    if script:
        rom[SCRIPT_AT:SCRIPT_AT + len(script)] = script
    rom[0x148] = 0x07                       # 4 MB
    # the header checksum ($14D): over the title .. version bytes $134..$14C
    c = 0
    for i in range(0x134, 0x14D):
        c = (c - rom[i] - 1) & 0xFF
    rom[0x14D] = c
    # the global checksum ($14E, big-endian): every byte but its own two
    g = (sum(rom) - rom[0x14E] - rom[0x14F]) & 0xFFFF
    rom[0x14E], rom[0x14F] = g >> 8, g & 0xFF
    return rom, used, len(s.data), len(res)


def main():
    """Command line: see the module's docstring."""
    code = Path(sys.argv[1]).read_bytes()
    snes = Path(sys.argv[2]).read_bytes()
    # (build.sh always passes the script's argument; empty: no script)
    script = Path(sys.argv[3]).read_bytes() if len(sys.argv) > 4 and sys.argv[3] else b""
    out = sys.argv[-1]
    rom, used, data, res = pack(code, snes, script)
    Path(out).write_bytes(rom)
    print(f"{out}: code {used // 1024} KB, SNES data {data // 1024} KB, resources {res // 1024} KB")


if __name__ == "__main__":
    main()
