#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Generates the data the game logic needs, from analysis/gamedata.json and
tools/game_rules.py (the same sources the Python reference model reads, so
the ROM and the model can't disagree):

  game/gen/data.h, data.c   the RULES tables that never change (weapons,
                            monster classes, damage multipliers) + limits
  game/gen/state.h          the game state struct (fixed maximum sizes)
  game/gen/layout.json      its byte layout, read by the lockstep test
  build/gfx/level.bin       the LEVEL blob: everything about this map the
                            rules need. It is data, not code: the ROM reads
                            it at run time from bank 2, LEVEL_ADDR, so the
                            editor can replace the map without a compiler.
  build/gfx/strings.bin     the level's strings (bank 4, $8000)

The level blob format is specified in docs/rom_format.md; the editor
writes byte-identical blobs.

MegaDrive version: everything is big-endian and every table starts on an
even address (the 68000 reads words and longs only at even addresses).

Usage: python3 gen_cdata.py analysis/gamedata.json build/gen build/gen

(The paths above are the Master System version's. In this repository
build.sh runs `gen_cdata.py analysis/campaign.json build/gen
build/gen/maps`, which writes into build/gen: data.h, data.c, state.h,
state.inc (the state offsets as assembler .equ lines, included by
src/rules.s and src/render.s), texts.h and texts.inc (the indices TX_* of
the game's texts, ../tools/game_text.py; the texts themselves are read
from the jar into the campaign block) and layout.json (read by md_lockstep.py),
and per campaign map build/gen/maps/<name>/level.bin and strings.bin,
which tools/md_campaign.py puts into the campaign block. There are no
banks on the Mega Drive: LEVEL_ADDR is only written into data.h, and
src/pack.c points the code at the map's blob. docs/rom_format.md is not
in this repository.)

Terms: the "level blob" is one map's game-logic tables (level_blob);
the "game state" S is the RAM block the rules change as the game is
played, which the lockstep test compares with the Python model.
"""
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))   # (the repository's tools: game_text)
from game_rules import WEAPONS, MONSTERS, weapon_damage_mult
from key_text import md_text
import game_text

# limits shared by the ROM, the model's test layout and the editor
# every MAX_* below is the most the ROM's RAM state has room for; level_blob refuses a map that needs more
MAX_DEFS = 64  # entity definitions per map
MAX_MAP_ENTS = 280  # the map's own entities
N_DROPS = 8  # extra entity slots for dropped items (round robin)
MAX_ENTS = MAX_MAP_ENTS + N_DROPS
MAX_MONS = 100  # monsters per map (each has a stats slot)
MAX_LINES = 64  # door/wall line entities
MAX_EVENTS = 160  # tile events
MAX_CMDS = 448  # event commands, all events together
MAX_ACTIVE = 64  # monsters in the active list at once
MAX_NOTES = 16  # notebook entries
MAX_STRINGS = 255  # string ids are bytes
MAX_SPAWNS = 8                # eSub-7 monsters per map that release lost souls
LEVEL_ADDR = 0xA000           # bank 2
LEVEL_MAX = 0x8000
STRINGS_MAX = 0x4000

# (eType, eSub) of the things a monster or crate can drop, in DD_* order
DROP_TYPES = [(3, 20), (3, 21), (3, 22), (3, 23), (4, 25), (4, 26)] + \
             [(6, s) for s in range(5)] + [(16, s) for s in range(5)]


def c_array(name, ctype, values, per_line=16):
    """A C array definition as text: `const ctype name[n] = { ... };` with
    per_line values on each line. Returns the string (generated code).
    """
    out = [f"const {ctype} {name}[{len(values)}] = {{"]
    for i in range(0, len(values), per_line):
        out.append("    " + ", ".join(str(v) for v in values[i:i + per_line]) + ",")
    out.append("};")
    return "\n".join(out)


def state_layout():
    """(name, size) in struct order. Everything up to 'end_compared' is
    compared byte for byte against the Python model by the lockstep test.
    Words (2) and longs (4) are big-endian and start on even offsets.

    This is the game state block S in the 68000's RAM (src/rules.s): the
    player, then per-entity arrays (e_*), per-monster stats (m_*), line
    flags, event states, the one-shot command bitmap (cmd_dead, a bit per
    command), the hidden-prop bitmap (static_hidden, a bit per cell),
    notes and the portal. The pad* bytes keep the next word even.
    """
    return [("rng", 2), ("px", 1), ("pz", 1), ("facing", 1),
            ("hp", 1), ("maxhp", 1), ("armor", 1), ("maxarmor", 1),
            ("defense", 1), ("strength", 1), ("agility", 1), ("accuracy", 1),
            ("level", 1), ("xp", 2), ("nextxp", 2), ("credits", 2),
            ("weapons", 2), ("weapon", 1), ("ammo", 6), ("inventory", 5), ("keys", 1),
            ("dead", 1), ("map", 1), ("map_given", 1), ("drop_index", 1),
            ("berserk", 1), ("store", 1), ("secret_turn", 1), ("chg_set", 1), ("exit_set", 1),
            ("god", 1), ("pad2", 1), ("completed", 2), ("disabled_weapons", 2), ("chg", 4), ("exit", 4),
            ("floor_rgb", 3), ("ceil_rgb", 3),
            ("n_active", 2), ("active", 2 * MAX_ACTIVE),
            ("e_def", MAX_ENTS), ("e_x", MAX_ENTS), ("e_z", MAX_ENTS), ("e_flags", MAX_ENTS),
            ("e_move", MAX_ENTS),
            ("m_hp", MAX_MONS), ("m_maxhp", MAX_MONS), ("m_armor", MAX_MONS), ("m_maxarmor", MAX_MONS),
            ("m_def", MAX_MONS), ("m_str", MAX_MONS), ("m_agi", MAX_MONS), ("m_acc", MAX_MONS),
            ("l_flags", MAX_LINES), ("ev_state", MAX_EVENTS), ("cmd_dead", (MAX_CMDS + 7) // 8),
            ("static_hidden", 128),
            ("n_notes", 1), ("notes", MAX_NOTES), ("pad0", 1),
            ("portal_mon", 2), ("portal", 1), ("portal_cnt", 1), ("couplings", 1), ("pad1", 1),
            ("end_compared", 0)]


# order of the blob's tables; the header holds a u16 offset for each
BLOB_TABLES = ["def_etype", "def_esub", "def_parm", "def_name", "ent_def0", "ent_x0", "ent_z0",
               "ent_hidden0", "ent_mon", "line_x", "line_z", "line_etype", "line_locked0", "line_kind",
               "ev_x", "ev_z", "ev_state0", "ev_flags", "ev_cmd0", "ev_ncmd", "ev_at",
               "cmd_op", "cmd_a1", "cmd_a2", "wall_bits", "static_block", "thin", "line_cells", "drop_def",
               "spawns"]
BLOB_HEADER = 24 + 2 * len(BLOB_TABLES)
LINE_EXIT, LINE_SECRET = 1, 2         # line_kind bits
# thin-wall bits per cell (Game_trace's invisible blockers): edge W E N S, for eType 14 then 15
THIN_BIT = {(14, "W"): 1, (14, "E"): 2, (14, "N"): 4, (14, "S"): 8,
            (15, "W"): 16, (15, "E"): 32, (15, "N"): 64, (15, "S"): 128}


def thin_edge(t):
    """Which edge ('W', 'E', 'N' or 'S') of its cell the thin wall `t` lies on (pos is in world units, 64 per cell)."""
    if t["axis"] == "x":
        return "W" if t["pos"] == t["x"] * 64 else "E"
    return "N" if t["pos"] == t["z"] * 64 else "S"


def map_target(g, a1, map_index):
    """CHANGEMAP's argument with its string id (the target .bsp) replaced by
    the target map's index in the campaign (255: not in it - the end)."""
    name = g["strings"][a1 & 0xFF].strip("/").rsplit(".", 1)[0]
    return (a1 & ~0xFF) | map_index.get(name, 255)


E = ">"          # 68000 byte order


def level_blob(g, map_index=None, map_id=0):
    """gamedata dict -> bytes (docs/rom_format.md). map_index: campaign map
    name -> index, for CHANGEMAP's targets.

    The "level blob" is one map's game-logic data as the ROM reads it at
    run time. Layout (big-endian):
      0   "LVB2", then version 2 and the counts (see the comment on hdr)
      24  one u16 offset per table of BLOB_TABLES (from the blob's start)
      ... the tables, each starting on an even offset
    map_id: the original's map number, kept in the header.
    Raises ValueError if the map exceeds a MAX_* limit or LEVEL_MAX.
    """
    map_index = map_index or {}
    defs, ents, lines, events = g["defs"], g["entities"], g["lines"], g["events"]
    cmds = [c for e in events for c in e["cmds"]]
    n_mon = 0
    mon_slot = []  # per entity: its monster stats slot, 255 if not a monster
    for e in ents:
        if defs[e["def"]]["eType"] == 1:
            mon_slot.append(n_mon)
            n_mon += 1
        else:
            mon_slot.append(255)
    checks = [("defs", len(defs), MAX_DEFS), ("map entities", len(ents), MAX_MAP_ENTS),
              ("monsters", n_mon, MAX_MONS), ("doors/lines", len(lines), MAX_LINES),
              ("events", len(events), MAX_EVENTS), ("event commands", len(cmds), MAX_CMDS),
              ("strings", len(g["strings"]), MAX_STRINGS)]
    for what, n, mx in checks:
        if n > mx:
            raise ValueError(f"too many {what}: {n} (max {mx})")
    ev_at = [255] * 1024  # per cell (z*32 + x): its event index, 255 = none
    for i, e in enumerate(events):
        ev_at[e["z"] * 32 + e["x"]] = i
    wall_bits = [0] * 128  # 1024 cells, one bit each (bit i&7 of byte i>>3)
    for i, w in enumerate(g["wall"]):
        if w:
            wall_bits[i >> 3] |= 1 << (i & 7)
    line_cells = [0] * 128  # bitmap: cells that hold a line
    for l in lines:
        k = l["z"] * 32 + l["x"]
        line_cells[k >> 3] |= 1 << (k & 7)
    starts, k = [], 0  # index of each event's first command in the flat command table
    for e in events:
        starts.append(k)
        k += len(e["cmds"])

    def drop_def(et, es):
        """The index of the first definition of type (et, es) in this map, or 255 if the map has none."""
        for i, d in enumerate(defs):
            if (d["eType"], d["eSub"]) == (et, es):
                return i
        return 255

    names = b""
    name_off = []
    t = {}
    t["def_etype"] = bytes(d["eType"] for d in defs)
    t["def_esub"] = bytes(d["eSub"] for d in defs)
    t["def_parm"] = b"".join(struct.pack(E + "i", d["parm"]) for d in defs)
    for d in defs:
        name_off.append(len(names))
        names += d["name"].upper().encode("latin1") + b"\0"
    # u16 offset per name (from the table's start), then the NUL-terminated upper-case names
    t["def_name"] = b"".join(struct.pack(E + "H", 2 * len(defs) + o) for o in name_off) + names
    if len(defs) > 255 or n_mon > 255:
        raise ValueError("defs and monster slots are bytes")
    t["ent_def0"] = bytes(e["def"] for e in ents)
    t["ent_x0"] = bytes(e["x"] for e in ents)
    t["ent_z0"] = bytes(e["z"] for e in ents)
    t["ent_hidden0"] = bytes(1 if e["hidden"] else 0 for e in ents)
    t["ent_mon"] = bytes(mon_slot)
    t["line_x"] = bytes(l["x"] for l in lines)
    t["line_z"] = bytes(l["z"] for l in lines)
    t["line_etype"] = bytes(l["eType"] for l in lines)
    t["line_locked0"] = bytes(1 if l["locked"] else 0 for l in lines)
    t["line_kind"] = bytes((LINE_EXIT if l.get("tex") == 7 else 0) | (0 if l.get("flags", 4) & 4 else LINE_SECRET)
                           for l in lines)
    t["ev_x"] = bytes(e["x"] for e in events)
    t["ev_z"] = bytes(e["z"] for e in events)
    t["ev_state0"] = bytes(e["state"] for e in events)
    t["ev_flags"] = bytes(e["flags"] for e in events)
    t["ev_cmd0"] = b"".join(struct.pack(E + "H", k) for k in starts)
    t["ev_ncmd"] = bytes(len(e["cmds"]) for e in events)
    t["ev_at"] = bytes(ev_at)
    t["cmd_op"] = bytes(c["op"] for c in cmds)
    t["cmd_a1"] = b"".join(struct.pack(E + "I", (map_target(g, c["a1"], map_index) if c["op"] == 2  # op 2 = CHANGEMAP
                                                  else c["a1"]) & 0xFFFFFFFF) for c in cmds)
    t["cmd_a2"] = b"".join(struct.pack(E + "I", c["a2"] & 0xFFFFFFFF) for c in cmds)
    t["wall_bits"] = bytes(wall_bits)
    t["static_block"] = bytes(g["static_block"])
    thin = [0] * 1024
    for th in g.get("thin", []):
        thin[th["z"] * 32 + th["x"]] |= THIN_BIT[(th["type"], thin_edge(th))]
    t["thin"] = bytes(thin)  # per cell: THIN_BIT bits
    sp = [(i, *e["spawns"]) for i, e in enumerate(ents) if "spawns" in e]
    if len(sp) > MAX_SPAWNS:
        raise ValueError(f"too many lost-soul spawners: {len(sp)}")
    t["spawns"] = struct.pack(E + "H", len(sp)) + b"".join(struct.pack(E + "3H", *x) for x in sp)  # count, then (entity, its two souls)
    t["line_cells"] = bytes(line_cells)
    t["drop_def"] = bytes(drop_def(*dt) for dt in DROP_TYPES)  # per DD_* drop kind: its definition here
    sx, sz, sf = g["spawn"]
    # 0 "LVB2", 4 version, 5 defs, 6 entities (w), 8 monsters, 9 lines, 10 events, 11 strings,
    # 12 commands (w), 14 spawn x, z, facing, 17 map id, 18 floor rgb, 21 ceiling rgb, 24 table offsets (w)
    hdr = b"LVB2" + struct.pack(E + "BBHBBBBHBBBB", 2, len(defs), len(ents), n_mon, len(lines), len(events),
                                len(g["strings"]), len(cmds), sx, sz, sf, map_id & 0xFF) + \
        bytes(g["floor_rgb"]) + bytes(g["ceiling_rgb"])
    assert len(hdr) == 24, len(hdr)
    body = b""
    offs = []
    for name in BLOB_TABLES:
        if len(body) & 1:  # keep every table word-aligned
            body += b"\0"
        offs.append(BLOB_HEADER + len(body))
        body += t[name]
    blob = hdr + b"".join(struct.pack(E + "H", o) for o in offs) + body
    if len(blob) > LEVEL_MAX:
        raise ValueError(f"level data too big: {len(blob)} bytes (max {LEVEL_MAX})")
    return blob


def strings_blob(strs):
    """The level's strings as one blob: a u16 big-endian offset per string
    (from the blob's start), then the NUL-terminated Latin-1 texts, each
    with the phone's keys it names made into button tokens (key_text.py's
    md_text: the game shows them as the buttons' names).
    Raises ValueError if it exceeds STRINGS_MAX.
    """
    offs, body = [], bytearray()
    base = 2 * len(strs)
    for s in strs:
        offs.append(base + len(body))
        body += md_text(s).encode("latin1") + b"\0"
    blob = b"".join(o.to_bytes(2, "big") for o in offs) + bytes(body)
    if len(blob) > STRINGS_MAX:
        raise ValueError(f"strings too big: {len(blob)} bytes (max {STRINGS_MAX})")
    return blob


# state fields that are 16-bit words and 32-bit longs (the rest are bytes or byte arrays)
WORDS = ("rng", "xp", "nextxp", "credits", "weapons", "completed", "disabled_weapons", "n_active", "portal_mon")
LONGS = ("chg", "exit")


def main():
    """gen_cdata.py <analysis/campaign.json> <out dir> <maps out dir>: the
    tables, the state layout, and per map maps/<name>/level.bin, strings.bin

    Writes to <out dir>: data.h and data.c (the rule tables, as C), state.h
    (struct state), state.inc (the same offsets as .equ lines for the
    assembler), layout.json (the offsets for the lockstep test). Prints a
    one-line summary.
    """
    camp_path, out_dir, gfx_dir = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    out_dir.mkdir(parents=True, exist_ok=True)
    camp = json.loads(camp_path.read_text())["maps"]
    map_index = {m["name"]: i for i, m in enumerate(camp)}
    sizes = []
    for m in camp:
        g = json.loads((camp_path.parent / "maps" / f"{m['name']}.json").read_text())
        d = gfx_dir / m["name"]
        d.mkdir(parents=True, exist_ok=True)
        blob = level_blob(g, map_index, m["id"])
        (d / "level.bin").write_bytes(blob)
        strs = strings_blob(g["strings"])
        (d / "strings.bin").write_bytes(strs)
        sizes.append(len(blob) + len(strs))

    # data.h: the limits, the rule tables' declarations, the drop kinds and the blob table indices
    h = ["/* Generated by tools/gen_cdata.py - do not edit */",
         "#ifndef GEN_DATA_H", "#define GEN_DATA_H",
         f"#define MAX_DEFS {MAX_DEFS}", f"#define MAX_MAP_ENTS {MAX_MAP_ENTS}", f"#define N_DROPS {N_DROPS}",
         f"#define MAX_ENTS {MAX_ENTS}", f"#define MAX_MONS {MAX_MONS}", f"#define MAX_LINES {MAX_LINES}",
         f"#define MAX_EVENTS {MAX_EVENTS}", f"#define MAX_CMDS {MAX_CMDS}",
         f"#define MAX_ACTIVE {MAX_ACTIVE}", f"#define MAX_NOTES {MAX_NOTES}",
         f"#define N_WEAPONS {len(WEAPONS)}", f"#define LEVEL_ADDR 0x{LEVEL_ADDR:04X}",
         f"#define LEVEL_N_TABLES {len(BLOB_TABLES)}",
         "extern const unsigned char wpn_min[], wpn_max[], wpn_rmin[], wpn_rmax[], wpn_ammo[], wpn_use[], wpn_armor[], wpn_shots[];",
         "extern const unsigned char mon_base[], mon_atk[], mon_shots[];",
         "extern const unsigned short dmg_mult[];",
         "#define DD_HEALTH 0\n#define DD_ARMOR 1\n#define DD_CREDIT1 2\n#define DD_CREDIT5 3\n#define DD_SMMED 4\n"
         "#define DD_LGMED 5\n#define DD_AMMO 6  /* +sub 0..4 */\n#define DD_AMMO2 11 /* +sub 0..4 */",
         "/* level blob table indices (header offsets) */"]
    for i, name in enumerate(BLOB_TABLES):
        h.append(f"#define LT_{name.upper()} {i}")
    h.append("#endif")
    # data.c: one array per WEAPONS column, the monster classes and the damage multiplier table [class][weapon]
    c = ["/* Generated by tools/gen_cdata.py - do not edit */", '#include "data.h"']
    for nm, col in (("wpn_min", 1), ("wpn_max", 2), ("wpn_rmin", 3), ("wpn_rmax", 4), ("wpn_ammo", 5),
                    ("wpn_use", 6), ("wpn_armor", 7), ("wpn_shots", 8)):
        c.append(c_array(nm, "unsigned char", [w[col] for w in WEAPONS]))
    c.append(c_array("mon_base", "unsigned char", [min(255, v) for m in MONSTERS for v in m[0]], 6))
    c.append(c_array("mon_atk", "unsigned char", [v for m in MONSTERS for v in m[1]], 2))
    c.append(c_array("mon_shots", "unsigned char", [m[2] for m in MONSTERS]))
    c.append(c_array("dmg_mult", "unsigned short",
                     [weapon_damage_mult(mc, w) for mc in range(len(MONSTERS)) for w in range(len(WEAPONS))],
                     len(WEAPONS)))
    h.insert(-1, "extern const unsigned char *level_blob, *strings_blob;")
    (out_dir / "data.h").write_text("\n".join(h) + "\n")
    (out_dir / "data.c").write_text("\n\n".join(c) + "\n")
    # the indices of the game's texts (their words come from the jar:
    # tools/md_campaign.py puts the texts table into the campaign block)
    (out_dir / "texts.h").write_text(game_text.c_header())
    (out_dir / "texts.inc").write_text(game_text.gas_include())

    # assign each state field its offset
    lay = state_layout()
    off = 0
    layout = {}
    for name, size in lay:
        layout[name] = [off, size]
        off += size
    (out_dir / "layout.json").write_text(json.dumps({"fields": layout, "max_ents": MAX_ENTS,
                                                     "max_mons": MAX_MONS, "n_drops": N_DROPS,
                                                     "endian": "big"}, indent=1))
    st = ["/* Generated by tools/gen_cdata.py - game state layout (do not edit) */",
          "#ifndef GEN_STATE_H", "#define GEN_STATE_H", '#include "data.h"', "struct state {"]
    for name, size in lay:
        if name == "end_compared":
            st.append("    /* ---- end of the part compared against the Python model ---- */")
            continue
        if name in WORDS:
            st.append(f"    unsigned short {name};")
        elif name in LONGS:
            st.append(f"    unsigned long {name};")
        elif name == "active":
            st.append(f"    unsigned short {name}[{size // 2}];")
        elif size == 1:
            st.append(f"    unsigned char {name};")
        else:
            st.append(f"    unsigned char {name}[{size}];")
    st += ["};", "#endif"]
    (out_dir / "state.h").write_text("\n".join(st) + "\n")
    # the same for the assembler: field offsets in S, limits, table indices
    inc = ["| Generated by tools/gen_cdata.py - do not edit", "| offsets of the game state's fields (S)"]
    for name, (o, sz) in layout.items():
        inc.append(f"    .equ    S_{name.upper()}, {o}")
    inc.append(f"    .equ    S_SIZE, {off}")
    for nm, v in (("MAX_DEFS", MAX_DEFS), ("MAX_MAP_ENTS", MAX_MAP_ENTS), ("N_DROPS", N_DROPS),
                  ("MAX_ENTS", MAX_ENTS), ("MAX_MONS", MAX_MONS), ("MAX_LINES", MAX_LINES),
                  ("MAX_EVENTS", MAX_EVENTS), ("MAX_CMDS", MAX_CMDS), ("MAX_ACTIVE", MAX_ACTIVE),
                  ("MAX_NOTES", MAX_NOTES), ("N_WEAPONS", len(WEAPONS)), ("MAX_SPAWNS", MAX_SPAWNS),
                  ("N_MAPS", len(camp))):
        inc.append(f"    .equ    {nm}, {v}")
    for i, name in enumerate(BLOB_TABLES):
        inc.append(f"    .equ    LT_{name.upper()}, {i}")
    for nm, v in (("DD_HEALTH", 0), ("DD_ARMOR", 1), ("DD_CREDIT1", 2), ("DD_CREDIT5", 3), ("DD_SMMED", 4),
                  ("DD_LGMED", 5), ("DD_AMMO", 6), ("DD_AMMO2", 11)):
        inc.append(f"    .equ    {nm}, {v}")
    (out_dir / "state.inc").write_text("\n".join(inc) + "\n")
    for name, (o, sz) in layout.items():
        if (name in WORDS or name in LONGS or name == "active") and o & 1:
            raise SystemExit(f"state field {name} at an odd offset")
    print(f"gen_cdata: {len(camp)} maps, level data {min(sizes, default=0)}..{max(sizes, default=0)} B; game state {off} B")


if __name__ == "__main__":
    main()
