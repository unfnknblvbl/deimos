#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Compiles the original level data (parsed .bsp + entities.db/.str + the
level's .str strings) into the game-logic tables used by both the Python
reference model (tools/game_model.py) and the Z80 engine.

What goes where (rules follow the original game, as understood from the
J2ME decompile and the DoomRPG-RE reimplementation):

  dynamic entities  - things the rules can change: monsters (eType 1),
                      humans (2), pickups (3), inventory items (4),
                      weapons (5), ammo (6, 16), fire (10), destructibles
                      (12). One record each; state lives in RAM.
  static blockers   - props that never change (eType 7 lamps/cabinets,
                      13 chairs/tables, 14/15 invisible blockers): baked
                      into a ROM "solid" bit per cell (they block movement
                      and monster pathing exactly like the original's
                      trace masks say) and drawn into the pre-rendered views.
  scenery           - sprites with no entity def (decals, pipes, ...):
                      render only, baked into the views.
  switchable lines  - every line an event opens/closes/locks/unlocks
                      (doors, sliding secret walls). Each blocks one cell
                      while closed; the view picker uses their state.
  events            - per-cell tile events with their byte-code, args
                      pre-translated (line index -> line slot, sprite
                      index -> entity index).

Writes analysis/gamedata.json (human-readable, used by the model, tests and
the editor) and build/gfx/gd_*.bin for the ROM.

Usage: python3 gamedata.py <level01.json> <entities.db> <entities.str> <level01.str> <out_dir>

(This file is shared with the Master System version, hence the Z80 and
the pre-rendered views above. On the Mega Drive the views are drawn live,
the rules are the 68000 code of src/rules.s, and the ROM's tables come
from the JSON through tools/gen_cdata.py; build() writes only the JSON.
tools/build_campaign.py calls build() once per map of the campaign, with
out_json = analysis/maps/<name>.json.)
"""
import json
import struct
import sys
from pathlib import Path

# entity types (eType) of entities.db, as the original numbers them (same values as game_model.ET_*)
ET_LINE, ET_ENEMY, ET_HUMAN, ET_PICKUP, ET_INV, ET_WEAPON, ET_AMMO = 0, 1, 2, 3, 4, 5, 6
ET_SPECIAL, ET_FIRE, ET_FIRE2, ET_DESTRUCT, ET_DECOR, ET_BLOCK14, ET_BLOCK15, ET_AMMO2 = 7, 10, 11, 12, 13, 14, 15, 16
# types that become entities with state in RAM, and props that only block movement
DYNAMIC_TYPES = {ET_ENEMY, ET_HUMAN, ET_PICKUP, ET_INV, ET_WEAPON, ET_AMMO, ET_AMMO2, ET_FIRE, ET_FIRE2, ET_DESTRUCT}
STATIC_BLOCK_TYPES = {ET_SPECIAL, ET_DECOR, ET_BLOCK14, ET_BLOCK15}

# event command op -> its name in the original's script (for reading the JSON)
EV_NAMES = {1: 'GOTO', 2: 'CHANGEMAP', 3: 'TRIGGER', 4: 'MESSAGE', 5: 'PAIN', 6: 'MOVELINE', 7: 'SHOW',
            8: 'DIALOG', 9: 'GIVEMAP', 10: 'PASSWORD', 11: 'CHANGESTATE', 12: 'LOCK', 13: 'UNLOCK',
            14: 'TOGGLELOCK', 15: 'OPENLINE', 16: 'CLOSELINE', 17: 'MOVELINE2', 18: 'HIDE', 19: 'NEXTSTATE',
            20: 'PREVSTATE', 21: 'INCSTAT', 22: 'DECSTAT', 23: 'REQSTAT', 24: 'FORCEMESSAGE', 25: 'ANIM',
            26: 'DIALOGNOBACK', 27: 'SAVEGAME', 28: 'ABORTMOVE', 29: 'SHAKE', 30: 'FLOORCOL', 31: 'CEILCOL',
            32: 'ENABLEWEAPONS', 33: 'OPENSTORE', 34: 'CHANGESPRITE', 35: 'PARTICLES', 36: 'REFRESHVIEW',
            37: 'WAIT', 38: 'PORTAL', 39: 'CHKLEVEL', 40: 'NOTE', 41: 'CHECKKEY', 42: 'SOUND'}
LINE_OPS = {6, 12, 13, 14, 15, 16, 17}  # ops whose argument is a line (wall) index

# Things a monster can drop (Entity_spawnDropItem) - always included in
# the def table even if the map itself has none.
DROP_DEFS = [(3, 20), (3, 21), (3, 22), (3, 23), (4, 25), (4, 26),
             (6, 1), (6, 2), (6, 3), (6, 4), (16, 1), (16, 2), (16, 3), (16, 4)]


def read_str_table(path: Path):
    """Reads one of the jar's .str string tables: u16 count, then per string
    a u16 length and that many Latin-1 bytes (all little-endian).
    Returns the list of strings.
    """
    s = path.read_bytes()
    n = struct.unpack_from('<H', s, 0)[0]
    p = 2
    out = []
    for _ in range(n):
        L = struct.unpack_from('<H', s, p)[0]
        p += 2
        out.append(s[p:p + L].decode('latin1'))
        p += L
    return out


def read_defs(db: Path, names: Path):
    """Reads entities.db (u16 count, then 8 bytes per definition: u16 tile =
    sprite number, u8 eType, u8 eSub, s32 parm; little-endian) and the
    names from entities.str. Returns a list of dicts {"tile", "eType",
    "eSub", "parm", "name"}.
    """
    d = db.read_bytes()
    n = struct.unpack_from('<H', d, 0)[0]
    nm = read_str_table(names)
    defs = []
    for i in range(n):
        tile, et, es, parm = struct.unpack_from('<HBBi', d, 2 + i * 8)
        defs.append({"tile": tile, "eType": et, "eSub": es, "parm": parm,
                     "name": nm[i] if i < len(nm) else f"def{i}"})
    return defs


def line_cell(w):
    """Cell a line entity is linked to (Game_loadMapEntities), after the
    loader's 3-unit nudge of one-sided walls (Render_beginLoadMapData) - which
    puts both halves of a sliding secret wall on the cell between them.

    w: a wall from the parsed .bsp (endpoints ax, az, bx, bz in world units,
    64 per cell; flags). Flag bits used: 8 / 16 nudge +3 / -3, along x
    (512) or z (256); 0x800, 0x1000, 0x2000, 0x4000 move the midpoint one
    unit north, south, east or west so it falls inside the right cell.
    Returns (x, z) in cells.
    """
    f = w["flags"]
    ax, az, bx, bz = w["ax"], w["az"], w["bx"], w["bz"]
    nudge = 3 if f & 8 else -3 if f & 16 else 0
    if f & 512:
        ax, bx = ax + nudge, bx + nudge
    elif f & 256:
        az, bz = az + nudge, bz + nudge
    x = ax + int((bx - ax) / 2)  # midpoint, rounded towards a
    y = az + int((bz - az) / 2)
    if f & 0x800:
        y -= 1
    elif f & 0x2000:
        x += 1
    elif f & 0x1000:
        y += 1
    elif f & 0x4000:
        x -= 1
    return x >> 6, y >> 6


# the maps' names in the game (the .bsp file -> title)
MAP_TITLES = {"intro": "UAC Base", "junction": "Junction", "junction_destroyed": "Junction",
              "reactor": "Reactor", "endgame": "Ending", "items": "Items",
              **{f"level0{i}": f"Sector {i}" for i in range(1, 8)}}


def build(level_json, db, db_names, level_str, out_dir: Path, out_json=Path("analysis/gamedata.json"), sms=False):
    """sms: the Master System's form - an invisible blocker blocks its whole
    cell (its rules have no thin walls), no campaign fields, SHOW of a
    non-entity is $FF (entity indices are bytes there).

    Inputs: level_json (bsp_parser.py's JSON of the map), db and db_names
    (entities.db, entities.str), level_str (the map's strings). Writes the
    game data to out_json and returns it as a dict:
      spawn [x, z, facing]; floor_rgb, ceiling_rgb; wall (1024 cells, 1 =
      solid); static_block (per cell the type of a blocking prop, 0 =
      none); thin (invisible thin walls); defs (only the definitions this
      map uses, plus the drop items; entity "def" values index this list);
      entities; scenery (render-only sprites); lines; events (per cell:
      state, flags, cmds [{"op", "name", "a1", "a2"}]); strings.
    out_dir is only created; nothing is written there.
    """
    L = json.loads(Path(level_json).read_text())
    defs_all = read_defs(Path(db), Path(db_names))
    strings = read_str_table(Path(level_str))
    by_tile = {d["tile"]: i for i, d in enumerate(defs_all)}
    by_type = {(d["eType"], d["eSub"]): i for i, d in enumerate(defs_all)}

    hdr = L["header"]
    misc = hdr["misc"]
    spawn_index = misc[0] | (misc[1] << 8)  # cell index z*32 + x
    spawn_angle = misc[2]  # 0..255: 0 east, 64 north, 128 west, 192 south
    facing = {64: 0, 0: 1, 192: 2, 128: 3}[spawn_angle]
    spawn = [spawn_index % 32, spawn_index // 32, facing]

    grid = L["grid"]
    wall = [1 if (v & 3) == 1 else 0 for v in grid]  # grid low bits 1 = solid cell

    used_defs = {}       # def index in defs_all -> compact id

    def use_def(i):
        """Gives definition i (index in defs_all) a compact id in this map's own def list, in order of first use."""
        if i not in used_defs:
            used_defs[i] = len(used_defs)
        return used_defs[i]

    entities, scenery, static_block, thin = [], [], [0] * 1024, []
    sprite_to_entity = {}
    for si, s in enumerate(L["sprites"]):
        info = s["raw_c"]
        t = info & 511  # the sprite's tile number (9 bits)
        if info & 0x40000:  # tiles 305 and up (wall-texture sprites)
            t += 305
        x, z = s["x"], s["z"]
        di = by_tile.get(t)
        hidden = bool(info & 0x10000)  # hidden until an event SHOWs it
        # the cell the thing is linked to (Render_beginLoadMapData's 1-unit
        # nudge, then Game_loadMapEntities' offsets for wall-mounted things)
        px, pz = x, z
        if info & 0x800000:  # a wall-mounted thing: 0x100000 / 0x80000 / 0x200000 / 0x400000 give the wall's side
            if info & 0x100000:
                pz += 1
            elif info & 0x80000:
                pz -= 1
            elif info & 0x200000:
                px += 1
            elif info & 0x400000:
                px -= 1
        # the point whose cell it is linked to
        lx, lz = px, pz
        if info & 0x200000:
            lx -= 64
        elif info & 0x100000:
            lz -= 64
        elif info & 0x80000:
            lz += 32
        elif info & 0x400000:
            lx += 32
        cx, cz = lx >> 6, lz >> 6  # world units -> cells (64 per cell)
        if info & 0x1000000:              # not an entity in the original (render only)
            di = None if di is not None and defs_all[di]["eType"] in DYNAMIC_TYPES else di
        if di is not None and defs_all[di]["eType"] in DYNAMIC_TYPES:
            sprite_to_entity[si] = len(entities)
            entities.append({"sprite": si, "def": use_def(di), "x": cx, "z": cz, "hidden": hidden,
                             "name": defs_all[di]["name"],
                             "_es7": (defs_all[di]["eType"], defs_all[di]["eSub"]) == (ET_ENEMY, 7)})
            continue
        # render-only (baked into views)
        scenery.append({"sprite": si, "tile": t, "x": x, "z": z, "info": info,
                        "def": defs_all[di]["name"] if di is not None else None,
                        # an entity in the original (HIDE / CHANGESPRITE can take it away)
                        "cell": cz * 32 + cx if di is not None or info & 0x20000 else -1})
        btype = 0
        if di is not None and defs_all[di]["eType"] in STATIC_BLOCK_TYPES:
            btype = defs_all[di]["eType"]
        elif info & 0x20000:
            btype = ET_BLOCK15 if t == 216 else ET_BLOCK14  # info 0x20000: an invisible blocker sprite
        if btype and not hidden and not info & 0x1000000:
            if btype in (ET_BLOCK14, ET_BLOCK15):
                # Game_trace: an invisible blocker is a thin wall at the sprite's
                # x (or z), stopping only the traces that cross it; without its
                # orientation bits it stops nothing
                if info & 0x20000 and info & 0x180000:
                    thin.append({"x": cx, "z": cz, "type": btype, "axis": "z", "pos": pz})
                    if sms:
                        static_block[cz * 32 + cx] = btype
                elif info & 0x20000 and info & 0x600000:
                    thin.append({"x": cx, "z": cz, "type": btype, "axis": "x", "pos": px})
                    if sms:
                        static_block[cz * 32 + cx] = btype
            else:
                static_block[cz * 32 + cx] = btype

    for e in entities:                    # Entity_died: an eSub-7 monster's next two sprites appear
        if e.pop("_es7", False):
            e["spawns"] = [sprite_to_entity.get(e["sprite"] + k, 0xFFFF) for k in (1, 2)]  # 0xFFFF: not an entity

    for et, es in DROP_DEFS:
        if (et, es) in by_type:
            use_def(by_type[(et, es)])

    # switchable lines: every line referenced by a line op
    W = L["walls"]
    line_refs = sorted({a["f1"] for a in L["actions"] if a["type"] in LINE_OPS})
    lines = []
    line_slot = {}
    for li in line_refs:
        w = W[li]
        cx, cz = line_cell(w)
        line_slot[li] = len(lines)
        ld = by_tile.get(305 + w["tex"])  # the line's own definition (door, jammed door...) by its texture
        if ld is not None:
            etype, esub, name = defs_all[ld]["eType"], defs_all[ld]["eSub"], defs_all[ld]["name"]
        elif w["flags"] & 24:  # (both branches give a plain wall)
            etype, esub, name = 9, 0, "Wall"
        else:
            etype, esub, name = 9, 0, "Wall"
        lines.append({"line": li, "x": cx, "z": cz, "tex": w["tex"], "flags": w["flags"],
                      "eType": etype, "eSub": esub, "name": name,
                      "locked": bool(w["flags"] & 0x400)})  # 0x400: starts locked

    # events
    events = []
    A = L["actions"]
    for ev in L["triggers"]:
        # a trigger word: bits 0..9 cell, 10..18 first action, 19..24 action count,
        # 25..28 starting state, 29..31 flags
        cell = ev & 1023
        ci, cc = (ev >> 10) & 511, (ev >> 19) & 63
        cmds = []
        for k in range(ci, ci + cc):
            a = A[k]
            op, a1, a2 = a["type"], a["f1"], a["f2"]
            if op in LINE_OPS:
                a1 = line_slot[a1]
            elif op == 7:     # SHOW: sprite index -> entity index (frame byte dropped)
                si = a1 & 0xFFFF
                a1 = sprite_to_entity.get(si, 0xFF if sms else 0xFFFF)
            cmds.append({"op": op, "name": EV_NAMES.get(op, str(op)), "a1": a1, "a2": a2})
        events.append({"x": cell % 32, "z": cell // 32, "state": (ev >> 25) & 15,
                       "flags": (ev >> 29) & 7, "cmds": cmds})

    defs = [None] * len(used_defs)
    for i, c in used_defs.items():
        d = dict(defs_all[i])
        d["id"] = c
        defs[c] = d

    out = {"map_id": hdr["byte_e"], "spawn": spawn, "floor_rgb": hdr["color_e"], "ceiling_rgb": hdr["color_d"],  # the original's map number and colours
           "wall": wall, "static_block": static_block, "thin": thin,
           "defs": defs, "entities": entities, "scenery": scenery,
           "lines": lines, "events": events, "strings": strings}
    if sms:
        if out["map_id"] == 1:            # the UAC Base: the game starts here (a new game's kit)
            out["new_game"] = True
        out["title"] = MAP_TITLES.get(Path(level_json).stem, Path(level_json).stem)
        del out["map_id"], out["thin"]
        for e in entities:
            e.pop("spawns", None)
        for sc in scenery:
            sc.pop("cell", None)
    out_dir.mkdir(parents=True, exist_ok=True)
    Path(out_json).parent.mkdir(parents=True, exist_ok=True)
    Path(out_json).write_text(json.dumps(out, indent=1))

    print(f"spawn {spawn}; {len(defs)} defs, {len(entities)} dynamic entities, {len(scenery)} baked sprites, "
          f"{sum(1 for b in static_block if b)} static-blocker cells, {len(lines)} switchable lines, {len(events)} events, "
          f"{sum(len(e['cmds']) for e in events)} cmds, {len(strings)} strings")
    return out


if __name__ == "__main__":
    # gamedata.py level.json entities.db entities.str level.str out_dir [out.json] [--sms]
    args = [a for a in sys.argv[1:] if a != "--sms"]
    build(*args[:4], Path(args[4]), Path(args[5]) if len(args) > 5 else Path("analysis/gamedata.json"),
          sms="--sms" in sys.argv)
