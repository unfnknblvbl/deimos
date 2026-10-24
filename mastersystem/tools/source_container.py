#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The editable level ("project") as stored inside the ROM, shared by the
Master System and Mega Drive ports (the same file in both repositories),
so the editor can open either ROM and move a level between them.

Container DRPGSRC2 (little-endian, docs/rom_format.md):
  "DRPGSRC2", u32 total size, u16 entry count, then per entry
  u8 name length, name, u32 raw size, u32 packed size, u8 method (1 = ZX0),
  packed bytes.

Entries:
  project.json     the level: map grid, walls, scenery, doors/lines,
                   entities, entity types, scripted events, strings, spawn,
                   colours; and the art as references:
                     textures {id: {shape, pal}}   wall textures
                     sprites  {tile: {shape, pal}} things and props
                     border   {shape, pal}         the rock around the SMS view
                     weapons  [{shape, pal}] x 18  the player's weapon
                                                   (9 weapons x idle, firing)
                   pal = 16 [r, g, b] colours.
  shape/<key>      a drawing: 64x64 palette indices (4096 bytes). Textures
                   are [v][u]; sprites and weapons [row][col], 255 =
                   transparent. The original game shares one drawing
                   between several textures/sprites with different
                   palettes (t<offset> = wall drawing, s<offset> = sprite).

The Mega Drive's campaign block holds one container for all its maps
(campaign_container): campaign.json (the map list), map/<name>.json (one
project per map) and every shape once. The packing is ZX0, an external
compressor (the ZX0 environment variable, else `zx0` on the PATH).
docs/rom_format.md is in the Master System repository.

Licence: GPL-2.0-or-later.
"""
import os, shutil
import json
import struct
import subprocess
import tempfile
from pathlib import Path

import numpy as np

from extract_textures import load_palettes, load_mappings, resolve_wall_id, resolve_sprite_id, extract_wall_texture
from extract_sprites import load_bitshapes, decode_shape

PORTAL_CLOSED_TEX = {34: 92, 33: 91}     # as tools/build_render_data.py's

ZX0 = Path(os.environ.get("ZX0") or shutil.which("zx0") or "zx0")  # the ZX0 compressor (an external program)
BORDER_TEX = 11          # the jar's brown rock (wall_table slot), around the SMS view
WEAPON_TILE = 240        # sprite 240 + weapon, + 1 while firing


def zx0(data: bytes) -> bytes:
    """data compressed with the external ZX0 program (an empty input is compressed as one zero byte)."""
    with tempfile.TemporaryDirectory() as td:
        a, b = Path(td) / "i", Path(td) / "o"
        a.write_bytes(data or b"\0")
        subprocess.run([str(ZX0), "-f", "-q", str(a), str(b)], check=True, capture_output=True)
        return b.read_bytes()


class JarArt:
    """the jar's drawings and palettes"""

    def __init__(self, jar_dir: Path):
        """Reads the palettes, mappings, wall texels, shapes and sprite texels from jar_dir."""
        self.pal = load_palettes(jar_dir / "palettes.bin")
        self.maps = load_mappings(jar_dir / "mappings.bin")
        self.wtex = (jar_dir / "wtexels.bin").read_bytes()
        self.shapes = load_bitshapes(jar_dir / "bitshapes.bin")
        self.stexels = (jar_dir / "stexels.bin").read_bytes()
        self.boundary = struct.unpack_from("<I", self.wtex, 0)[0] * 2

    def colours(self, poff):
        """The 16 colours from palette offset poff, as [r, g, b] lists (magenta beyond the end)."""
        return [list(self.pal[poff + k]) if poff + k < len(self.pal) else [255, 0, 255] for k in range(16)]

    def wall_slot(self, tid):
        """wall_table slot -> (key, 64x64 [v][u] indices, palette)

        The key "t<offset>" names the drawing, so textures sharing it share one shape.
        """
        off, poff = self.maps["wall_table"][tid * 2], self.maps["wall_table"][tid * 2 + 1]
        grid = bytes(v for row in extract_wall_texture(self.wtex, off) for v in row)
        return f"t{off}", grid, self.colours(poff)

    def texture(self, raw_id):
        """A raw wall texture id (as in the .bsp) -> wall_slot's result."""
        return self.wall_slot(resolve_wall_id(self.maps, raw_id))

    def sprite_slot(self, sid):
        """sprite_table slot -> (key, 64x64 [row][col] indices or 255, palette);
        the same placement as tools/sprite_art.py

        The key is "s<record offset>".
        """
        rec, poff = self.maps["sprite_table"][sid * 2], self.maps["sprite_table"][sid * 2 + 1]
        sh = decode_shape(self.shapes, rec)
        base = 4 + (sh["file_offset"] - self.boundary) // 2
        nib = []
        for byte in self.stexels[base:base + (sh["pixel_count"] + 1) // 2]:
            nib += [byte & 15, byte >> 4]
        grid = bytearray([255] * 4096)
        r0, c0 = sh["left"], sh["top"]
        for col, runs in enumerate(sh["rows"]):
            for start, n, p0 in runs:
                for i in range(n):
                    r, c = r0 + start + i, c0 + col
                    if r < 64 and c < 64:
                        grid[r * 64 + c] = nib[p0 + i] if p0 + i < len(nib) else 0
        return f"s{rec}", bytes(grid), self.colours(poff)

    def sprite(self, tile):
        """A sprite tile number -> its drawing; tiles 305 and up are wall textures."""
        if tile >= 305:          # a wall texture shown as a sprite (tools/sprite_art.py)
            return self.texture(tile - 305)
        if tile in (82, 84, 86, 88, 90):
            # an ammo pack: its single item, tile - 1, three times side by
            # side (tools/sprite_art.py's PLURAL_TILES and PLURAL_OFFSETS,
            # the middle copy last); the key gets "x3"
            key, one, pal = self.sprite(tile - 1)
            grid = bytearray([255] * 4096)
            for dx in (-16, 16, 0):
                for r in range(64):
                    for c in range(64):
                        sc = c - dx
                        if 0 <= sc < 64 and one[r * 64 + sc] != 255:
                            grid[r * 64 + c] = one[r * 64 + sc]
            return key + "x3", bytes(grid), pal
        return self.sprite_slot(resolve_sprite_id(self.maps, tile))


def thin_edge(t):
    """an invisible blocker's cell edge: W E N S

    (The same as gen_cdata.thin_edge.)
    """
    if t["axis"] == "x":
        return "W" if t["pos"] == t["x"] * 64 else "E"
    return "N" if t["pos"] == t["z"] * 64 else "S"


def project(level, g, title=None):
    """the level in the editor's canonical form (editor/src/project.cpp
    writes exactly these keys, in this order). A campaign map (one with a
    map_id: the Mega Drive's campaign) also keeps its original map id, its
    thin invisible blockers, the props' cells and the lost-soul spawners.

    level: the parsed .bsp; g: the map's game data (gamedata.py); title:
    the map's name. Key order matters: the editor must produce the same
    bytes when it writes the project back.
    """
    camp = "map_id" in g
    out = {"version": 2, "name": title or g.get("title", "Sector 1")}
    if g.get("new_game"):
        out["new_game"] = True            # (the Master System: a new game's kit at the start)
    if camp:
        out["map_id"] = g["map_id"]
    out.update({
        "floor_rgb": list(level["header"]["color_e"]), "ceiling_rgb": list(level["header"]["color_d"]),
        **({"sms_ceiling_rgb": list(g["sms_ceiling_rgb"]), "sms_floor_rgb": list(g["sms_floor_rgb"])}
           if "sms_ceiling_rgb" in g else {}),
        "spawn": list(g["spawn"]), "wall": list(g["wall"]), "static_block": list(g["static_block"])})
    if camp:
        out["thin"] = [{"x": t["x"], "z": t["z"], "type": t["type"], "edge": thin_edge(t)} for t in g["thin"]]
    out.update({
        "walls": [{k: w[k] for k in ("ax", "az", "bx", "bz", "tex", "flags")} for w in level["walls"]],
        "scenery": [{k: s[k] for k in (("tile", "x", "z", "info", "cell") if camp else ("tile", "x", "z", "info"))}
                    for s in g["scenery"]],
        "defs": [{k: d[k] for k in ("tile", "eType", "eSub", "parm", "name")} for d in g["defs"]],
        "entities": [{**{k: e[k] for k in ("def", "x", "z", "hidden")},
                      **({"spawns": list(e["spawns"])} if "spawns" in e else {})} for e in g["entities"]],
        "lines": [{"wall": l["line"], "x": l["x"], "z": l["z"], "eType": l["eType"], "eSub": l["eSub"],
                   "name": l["name"], "locked": l["locked"]} for l in g["lines"]],
        "events": [{"x": e["x"], "z": e["z"], "state": e["state"], "flags": e["flags"],
                    "cmds": [{k: c[k] for k in ("op", "a1", "a2")} for c in e["cmds"]]} for e in g["events"]],
        "strings": list(g["strings"])})
    return out


def make_project(level, g, jar_dir: Path, title=None):
    """-> (project dict, {shape key: 4096 bytes})

    Adds to project() the art the map uses, as references to shapes: its
    wall textures (and the portal's closed ones), its sprites, the SMS
    view's border and the 18 weapon frames.
    """
    jar = JarArt(jar_dir)
    proj = project(level, g, title)
    shapes = {}

    def ref(key, grid, pal):
        """Records a drawing under `key` (asserting one key always means one drawing) and returns the {shape, pal} reference."""
        assert shapes.get(key, grid) == grid
        shapes[key] = grid
        return {"shape": key, "pal": pal}
    used = {w["tex"] for w in proj["walls"]}
    # (+ the textures the Reactor's portal takes once it is closed)
    used |= {PORTAL_CLOSED_TEX[t] for t in used if t in PORTAL_CLOSED_TEX}
    proj["textures"] = {str(t): ref(*jar.texture(t)) for t in sorted(used)}
    proj["sprites"] = {str(t): ref(*jar.sprite(t))
                       for t in sorted({d["tile"] for d in proj["defs"]} | {s["tile"] for s in proj["scenery"]})}
    proj["border"] = ref(*jar.wall_slot(BORDER_TEX))
    proj["weapons"] = [ref(*jar.sprite_slot(resolve_sprite_id(jar.maps, WEAPON_TILE + w) + f))
                       for w in range(9) for f in range(2)]
    return proj, shapes


def art_rgba(proj, shapes, ref):
    """{shape, pal} -> 64x64x4 uint8 (alpha 0 where 255)

    (proj is not used.)
    """
    g = np.frombuffer(shapes[ref["shape"]], np.uint8).reshape(64, 64)
    pal = np.array(ref["pal"], np.uint8)
    out = np.zeros((64, 64, 4), np.uint8)
    on = g != 255
    out[on, :3] = pal[g[on] & 15]
    out[on, 3] = 255
    return out


def pack_entries(entries) -> bytes:
    """The DRPGSRC2 container of [(name, bytes)]: each entry ZX0-compressed,
    the total size patched in at offset 8 at the end.
    """
    out = bytearray(b"DRPGSRC2")
    out += struct.pack("<IH", 0, len(entries))
    for name, data in entries:
        packed = zx0(data)
        nb = name.encode()
        out += bytes([len(nb)]) + nb + struct.pack("<IIB", len(data), len(packed), 1) + packed
    out[8:12] = struct.pack("<I", len(out))
    return bytes(out)


def locked_container() -> bytes:
    """A locked ROM's container: one entry, "locked" (b"1"), in place of the
    level's editable copy (the editor's locked_container: the same bytes).
    The editor refuses to open such a ROM. The Master System, Game Gear,
    SNES, SNES SuperFX and 32X ROMs are made locked, to save room."""
    return pack_entries([("locked", b"1")])


def to_json(proj) -> bytes:
    """The project as compact JSON bytes (no spaces), as the editor writes it."""
    return json.dumps(proj, separators=(",", ":")).encode()


def container(proj, shapes) -> bytes:
    """One level's container: project.json, then every shape (sorted by key)."""
    entries = [("project.json", to_json(proj))]
    for key in sorted(shapes):
        entries.append((f"shape/{key}", shapes[key]))
    return pack_entries(entries)


def campaign_container(maps, shapes) -> bytes:
    """the Mega Drive campaign's editable maps: campaign.json (the maps in
    ROM order: name, title, original id), map/<name>.json (each map's
    project) and every drawing once. maps: [(name, title, id, project)]
    """
    camp = {"maps": [{"name": n, "title": t, "id": i} for n, t, i, _p in maps]}
    entries = [("campaign.json", to_json(camp))]
    entries += [(f"map/{n}.json", to_json(p)) for n, _t, _i, p in maps]
    entries += [(f"shape/{key}", shapes[key]) for key in sorted(shapes)]
    return pack_entries(entries)
