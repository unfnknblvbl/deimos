#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Parser for Doom RPG (J2ME, 2005) ".bsp" level files.

Format reverse-engineered from the decompiled DoomRPG.jar (class "r", method g(),
originally obfuscated, re-derived by hand). This is a straight-line transliteration
of that Java loading code into a read-only Python parser - it does not execute any
code from the jar, it only mirrors the documented byte layout.

File layout (all integers little-endian):
  Header (13 bytes):
    rgb color "e" (3 bytes)
    rgb color "d" (3 bytes)
    rgb color "f" (3 bytes)
    byte "e2"
    3 miscellaneous bytes (ambient/music - not needed for geometry)
      (in fact gamedata.py reads them as the player's spawn: cell index
      z*32+x as a u16, then the spawn angle)
  BSP nodes:
    u16 count
    count * 10 bytes: bbox(4 bytes, each <<3) + flags/split (1+1 bytes) + 2x u16 child/leaf fields
    (skipped - not needed to recover wall/sprite/grid data)
  Walls ("lines"):
    u16 count
    count * 8 bytes: Ax,Az,Bx,Bz (each byte<<3) + s16 textureId + u16 flags
  Sprites ("things"):
    u16 count
    count * 5 bytes: X,Z (each byte<<3) + u8 typeLow + s16 flagsHi (typeLow | flagsHi<<16)
  Triggers:
    u16 count
    count * 4 bytes: u32 packed (cell index in low 10 bits)
  Actions/scripts:
    u16 count
    count * 9 bytes: u8 type, u32 field1, u32 field2
  Terrain grid:
    256 bytes, 4 cells/byte (2 bits each) -> 1024 = 32x32 grid, row-major (z*32+x)
"""
import struct
import json
import sys
from pathlib import Path


def short_b(byte_val: int) -> int:
    """Byte coordinate -> world units (x8 scale)."""
    return (byte_val & 0xFF) << 3


class Reader:
    """A cursor over the file's bytes: each method reads the next value (little-endian) and moves on."""
    def __init__(self, data: bytes):
        """Starts reading `data` at offset 0."""
        self.data = data
        self.pos = 0

    def bytes(self, n):
        """The next n bytes; raises EOFError if the file ends first."""
        b = self.data[self.pos:self.pos + n]
        if len(b) != n:
            raise EOFError(f"wanted {n} bytes at {self.pos}, got {len(b)} (file len {len(self.data)})")
        self.pos += n
        return b

    def u8(self):
        """The next unsigned byte."""
        return self.bytes(1)[0]

    def u16(self):
        """The next unsigned 16-bit little-endian number."""
        return struct.unpack_from("<H", self.bytes(2))[0]

    def s16(self):
        """The next signed 16-bit little-endian number."""
        return struct.unpack_from("<h", self.bytes(2))[0]

    def u32(self):
        """The next unsigned 32-bit little-endian number."""
        return struct.unpack_from("<I", self.bytes(4))[0]


def parse_bsp(path: Path) -> dict:
    """Parses one .bsp map file (layout in the module docstring) into a dict:
      header    {"color_e", "color_d", "color_f": RGB triples (gamedata.py
                uses e as the floor colour and d as the ceiling), "byte_e":
                the map's number, "misc": 3 bytes (spawn cell low/high byte
                and spawn angle, as gamedata.py reads them)}
      walls     [{"ax", "az", "bx", "bz" (world units, 64 per cell), "tex",
                "flags"}]
      sprites   [{"x", "z" (world units), "type" (9-bit tile), "raw_c" (type
                and flags, 32 bits)}]
      triggers  the raw u32 tile-event words; actions [{"type", "f1", "f2"}]
      grid      1024 cells: bits 0..1 terrain (1 = solid wall), bit 2 a door
                line's cell, bit 3 a cell with an event
      trailing_bytes  bytes left unread (0 for a well-formed file)
    """
    data = path.read_bytes()
    r = Reader(data)

    header = r.bytes(13)
    color_e = tuple(header[0:3])
    color_d = tuple(header[3:6])
    color_f = tuple(header[6:9])
    byte_e = header[9]
    misc = tuple(header[10:13])

    node_count = r.u16()
    r.bytes(node_count * 10)  # BSP tree - not needed for grid/geometry extraction

    wall_count = r.u16()
    walls = []
    GRID_W = 32
    grid = [0] * (GRID_W * GRID_W)
    for _ in range(wall_count):
        ax = short_b(r.u8())
        az = short_b(r.u8())
        bx = short_b(r.u8())
        bz = short_b(r.u8())
        tex_id = r.s16()
        flags = r.u16()
        walls.append({"ax": ax, "az": az, "bx": bx, "bz": bz, "tex": tex_id, "flags": flags})
        if tex_id == 7:  # door line
            cx = (ax + (bx - ax) // 2) >> 6  # the door's midpoint, in cells
            cz = (az + (bz - az) // 2) >> 6
            if 0 <= cx < GRID_W and 0 <= cz < GRID_W:
                grid[cz * GRID_W + cx] |= 4

    sprite_count = r.u16()
    sprites = []
    for _ in range(sprite_count):
        x = short_b(r.u8())
        z = short_b(r.u8())
        type_low = r.u8()
        flags_hi = r.s16()
        c = (type_low & 0xFF) | ((flags_hi & 0xFFFF) << 16)  # the original's packed sprite info
        sprite_type = c & 0x1FF
        sprites.append({"x": x, "z": z, "type": sprite_type, "raw_c": c})

    trigger_count = r.u16()
    triggers = []
    for _ in range(trigger_count):
        n2 = r.u32()
        triggers.append(n2)
        if n2 & 0x1F80000:  # bits 19..24: the event's action count (non-zero = has commands)
            cell = n2 & 0x3FF
            grid[cell] |= 8

    action_count = r.u16()
    actions = []
    for _ in range(action_count):
        a_type = r.u8()
        f1 = r.u32()
        f2 = r.u32()
        actions.append({"type": a_type, "f1": f1, "f2": f2})

    terrain_bytes = r.bytes(256)
    for byte_idx, byte_val in enumerate(terrain_bytes):
        for sub in range(4):
            cell = byte_idx * 4 + sub
            val = (byte_val >> (sub * 2)) & 3
            grid[cell] |= val

    remaining = len(data) - r.pos

    return {
        "header": {
            "color_e": color_e, "color_d": color_d, "color_f": color_f,
            "byte_e": byte_e, "misc": misc,
        },
        "node_count": node_count,
        "walls": walls,
        "sprites": sprites,
        "triggers": triggers,
        "actions": actions,
        "grid": grid,
        "grid_w": GRID_W,
        "grid_h": GRID_W,
        "trailing_bytes": remaining,
    }


def grid_to_ascii(level: dict) -> str:
    """base type (bits0-1) only: 0=? 1=? 2=? 3=? plus door(4)/trigger(8) overlay markers.

    One text row per grid row: 'D' a door cell, 'T' an event cell, else
    the terrain digit (1 = solid wall, as gamedata.py reads it; the other
    values are not used by these tools). For looking at a map.
    """
    w, h = level["grid_w"], level["grid_h"]
    grid = level["grid"]
    lines = []
    for z in range(h):
        row = []
        for x in range(w):
            v = grid[z * w + x]
            base = v & 3
            door = v & 4
            trig = v & 8
            if door:
                ch = 'D'
            elif trig:
                ch = 'T'
            else:
                ch = str(base)
            row.append(ch)
        lines.append(''.join(row))
    return '\n'.join(lines)


if __name__ == "__main__":
    in_path = Path(sys.argv[1])
    level = parse_bsp(in_path)
    print(f"=== {in_path.name} ===")
    print("header:", level["header"])
    print("nodes:", level["node_count"])
    print("walls:", len(level["walls"]))
    print("sprites:", len(level["sprites"]))
    print("triggers:", len(level["triggers"]))
    print("actions:", len(level["actions"]))
    print("trailing bytes (should be 0):", level["trailing_bytes"])
    print()
    print("Grid (base type 0-3, D=door cell, T=trigger cell):")
    print(grid_to_ascii(level))
    print()
    tex_ids = sorted(set(w["tex"] for w in level["walls"]))
    print("Distinct wall texture ids used:", tex_ids)
    sprite_types = sorted(set(s["type"] for s in level["sprites"]))
    print("Distinct sprite type ids used:", sprite_types)

    if len(sys.argv) > 2:
        out_path = Path(sys.argv[2])
        out_path.write_text(json.dumps(level, indent=1))
        print("Wrote", out_path)
