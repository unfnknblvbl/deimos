#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The SNES ROM: the linked code (banks $C0-$C1, 128 KB), the generated
banks (tools/snes_tables.py) from $C2, the resources made from the
original game (tools/snes_ui_res.py) from the next bank (its bank in the
header at $FFB7), then the campaign's block (the Mega Drive game's
tools/md_campaign.py) from the next (its bank at $FFB6), padded to a
power of two (at least 256 KB, at most 4 MB: HiROM banks $C0-$FF), with
the header's size code and checksum.

Usage: snes_rom.py <code.bin> <tables.bin> <res.bin> <campaign.bin> <out.sfc>
       snes_rom.py --fx <code.bin> <tables.bin> <gsu.bin> <res.bin> <campaign.bin> <out.sfc>

--fx: the SuperFX build (the GSU's map, LoROM-style: the header at $7FB0,
at most 2 MB, banks numbered from $40): the code (two banks), the tables'
first two banks, the GSU's program (a bank), the resources, the block (the
trimmed one, tools/snes_fx_block.py).

The code finds the resources and the block through the header's $FFB7 /
$FFB6 (or $7FB7 / $7FB6) bytes, so the editor can rebuild them. Unused
space is $FF.
Licence: GPL-2.0-or-later.
"""
import sys
from pathlib import Path

BLOCK_AT = 0x20000              # the code's two 64 KB banks: the tables start here
MAX = 0x400000                  # 4 MB (HiROM)


def finish(rom: bytearray, hdr=0xFFB0, max_size=MAX) -> bytearray:
    """rom padded with $FF to a power of two (256 KB at least, max_size at
    most), its header (at hdr: the $FFB0 area) given the size code ($FFD7)
    and the checksum and its complement ($FFDC..$FFDF)"""
    size = 0x40000
    while size < len(rom):
        size *= 2
    if size > max_size:
        raise SystemExit(f"the ROM needs {len(rom) // 1024} KB: more than the {max_size // 1048576} MB it may have")
    rom += b"\xff" * (size - len(rom))
    rom[hdr + 0x27] = (size // 1024).bit_length() - 1      # 2^n KB
    rom[hdr + 0x2C:hdr + 0x30] = b"\xff\xff\x00\x00"     # (a stand-in pair: its bytes add up to $1FE, as the final pair's will)
    chk = sum(rom) & 0xFFFF
    rom[hdr + 0x2C] = (chk ^ 0xFFFF) & 0xFF
    rom[hdr + 0x2D] = (chk ^ 0xFFFF) >> 8
    rom[hdr + 0x2E] = chk & 0xFF
    rom[hdr + 0x2F] = chk >> 8
    return rom


def bank_pad(b):
    """b padded with $FF to a 64 KB bank boundary"""
    return b + b"\xff" * (-len(b) % 0x10000)


def main_fx():
    """the --fx layout (see the module's docstring)"""
    code, tables, gsu, res, block = (Path(a).read_bytes() for a in sys.argv[2:7])
    out = sys.argv[7]
    if len(code) > BLOCK_AT:
        raise SystemExit("the code is bigger than its two banks")
    rom = bytearray(code) + b"\xff" * (BLOCK_AT - len(code)) + bank_pad(tables[:0x20000]) + bank_pad(gsu)
    rom[0x7FB7] = 0x40 + (len(rom) >> 16)       # (the resources' bank)
    rom += bank_pad(res)
    at = len(rom)
    rom[0x7FB6] = 0x40 + (at >> 16)             # (the block's bank)
    rom += block
    rom = finish(rom, 0x7FB0, 0x200000)
    Path(out).write_bytes(bytes(rom))
    print(f"{out}: {len(rom) // 1024} KB (block {len(block) // 1024} KB at ${0x40 + (at >> 16):02X}0000)")


def main():
    """Command line: see the module's docstring."""
    if sys.argv[1] == "--fx":
        return main_fx()
    code, tables, res, block = (Path(a).read_bytes() for a in sys.argv[1:5])
    out = sys.argv[5]
    if len(code) > BLOCK_AT:
        raise SystemExit("the code is bigger than its two banks")
    rom = bytearray(code) + b"\xff" * (BLOCK_AT - len(code)) + bank_pad(tables)
    rom[0xFFB7] = 0xC0 + (len(rom) >> 16)       # (the resources' bank)
    rom += bank_pad(res)
    at = len(rom)
    rom[0xFFB6] = 0xC0 + (at >> 16)             # (the block's bank)
    rom += block
    rom = finish(rom)
    Path(out).write_bytes(bytes(rom))
    print(f"{out}: {len(rom) // 1024} KB (block {len(block) // 1024} KB at ${0xC0 + (at >> 16):02X}0000)")


if __name__ == "__main__":
    main()
