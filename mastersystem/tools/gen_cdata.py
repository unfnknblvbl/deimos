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

  game/gen/defs.inc         limits, level blob table numbers and the game
                            state's field addresses (S_*) for the assembler
  game/gen/tables.inc       the RULES tables that never change (weapons,
                            monster classes, damage multipliers)
  game/gen/layout.json      its byte layout, read by the lockstep test
  build/gfx/level.bin       the LEVEL blob: everything about this map the
                            rules need. It is data, not code: the ROM reads
                            it at run time from bank 2, LEVEL_ADDR, so the
                            editor can replace the map without a compiler.
  build/gfx/strings.bin     the level's strings (bank 4, $8000)
  build/gfx/strings_gg.bin  the same for the Game Gear (its buttons, its box)
  game/gen/texts.inc        where each of the game's texts is found (TX_*:
                            the address of its pointer in the texts table)
  build/gfx/texts.bin       the texts table (bank 2, $8000): the game's texts
                            the code shows (messages, menus, the weapons'
                            names), read from the jar (../tools/game_text.py)
  build/gfx/texts_gg.bin    the same for the Game Gear

The level blob format is specified in docs/rom_format.md; the editor
writes byte-identical blobs.

Usage: python3 gen_cdata.py analysis/gamedata.json game/gen build/gfx assets/jar
(assets/jar: the jar's files, for the texts table)
Run by build.sh step 1, after gamedata.py. (The name is from when the game
logic was C; the outputs are now WLA-DX includes for the Z80 assembler.)

The "game state" is the block of RAM at STATE_ADDR ($C400) holding all the
rules' changing data (player, entities, monsters, lines, events). Its field
order comes from state_layout(); the lockstep test reads the same layout
from layout.json and compares the ROM's bytes with the Python model's.

Level blob layout: 16-byte header ("LVL1", version 2, counts of defs,
entities, monsters, lines, events, commands low byte, strings, spawn x, z,
facing, commands high byte), then a u16 offset (from the blob start) per
table in BLOB_TABLES order, then the tables.
"""
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))   # (the repository's tools: game_text)
from game_rules import WEAPONS, MONSTERS, weapon_damage_mult
from key_text import sms_text
import game_text

# limits shared by the ROM, the model's test layout and the editor
MAX_DEFS = 64                 # entity definitions per level
MAX_MAP_ENTS = 192            # entities placed in the map
N_DROPS = 8                   # extra entity slots for dropped items
MAX_ENTS = MAX_MAP_ENTS + N_DROPS
MAX_MONS = 56                 # monsters (each has a monster slot for its stats)
MAX_LINES = 40                # switchable lines (doors, secret walls)
MAX_EVENTS = 96               # tile events
MAX_CMDS = 320                # event commands in all
MAX_ACTIVE = 64               # active (awake, thinking) monsters, in activation order
MAX_NOTES = 16                # notebook entries
MAX_STRINGS = 255
LEVEL_ADDR = 0xA000           # bank 2
LEVEL_MAX = 0x2000            # 8 KB for the level blob
STRINGS_MAX = 0x2000          # bank 4, $8000-$9FFF
TEXTS_MAX = 0x2000  # the texts table: bank 2, $8000-$9FFF

# (eType, eSub) of the things a monster or crate can drop, in DD_* order
DROP_TYPES = [(3, 20), (3, 21), (3, 22), (3, 23), (4, 25), (4, 26)] + \
             [(6, s) for s in range(5)] + [(16, s) for s in range(5)]


def c_array(name, ctype, values, per_line=16):
    """A C array definition as text (left from the C version; not called any more)."""
    out = [f"const {ctype} {name}[{len(values)}] = {{"]
    for i in range(0, len(values), per_line):
        out.append("    " + ", ".join(str(v) for v in values[i:i + per_line]) + ",")
    out.append("};")
    return "\n".join(out)


def state_layout():
    """(name, size) in struct order. Everything up to 'end_compared' is
    compared byte for byte against the Python model by the lockstep test.

    Prefixes: e_ per entity (MAX_ENTS each), m_ per monster slot, l_ per
    line, ev_ per event; cmd_dead is a bit per event command (one-shot
    commands that have run). Sizes are in bytes; multi-byte values are
    little-endian."""
    return [("rng", 2), ("px", 1), ("pz", 1), ("facing", 1),
            ("hp", 1), ("maxhp", 1), ("armor", 1), ("maxarmor", 1),
            ("defense", 1), ("strength", 1), ("agility", 1), ("accuracy", 1),
            ("level", 1), ("xp", 2), ("nextxp", 2), ("credits", 2),
            ("weapons", 2), ("weapon", 1), ("ammo", 6), ("inventory", 5), ("keys", 1),
            ("dead", 1), ("level_done", 1), ("map_given", 1), ("drop_index", 1),
            ("n_active", 1), ("active", MAX_ACTIVE),
            ("e_def", MAX_ENTS), ("e_x", MAX_ENTS), ("e_z", MAX_ENTS), ("e_flags", MAX_ENTS),
            ("e_move", MAX_ENTS),
            ("m_hp", MAX_MONS), ("m_maxhp", MAX_MONS), ("m_armor", MAX_MONS), ("m_maxarmor", MAX_MONS),
            ("m_def", MAX_MONS), ("m_str", MAX_MONS), ("m_agi", MAX_MONS), ("m_acc", MAX_MONS),
            ("l_flags", MAX_LINES), ("ev_state", MAX_EVENTS), ("cmd_dead", (MAX_CMDS + 7) // 8),
            ("n_notes", 1), ("notes", MAX_NOTES),
            ("end_compared", 0)]


# order of the blob's tables; the header holds a u16 offset for each
BLOB_TABLES = ["def_etype", "def_esub", "def_parm", "def_name", "ent_def0", "ent_x0", "ent_z0",
               "ent_hidden0", "ent_mon", "line_x", "line_z", "line_etype", "line_locked0",
               "ev_x", "ev_z", "ev_state0", "ev_flags", "ev_cmd0", "ev_ncmd", "ev_at",
               "cmd_op", "cmd_a1", "cmd_a2", "wall_bits", "static_block", "line_cells", "drop_def", "start", "title"]
BLOB_HEADER = 16 + 2 * len(BLOB_TABLES)    # header + offset table: where the first table starts


def level_blob(g):
    """gamedata dict -> bytes (docs/rom_format.md)

    Raises ValueError if the level exceeds any MAX_* limit or the blob is
    over LEVEL_MAX. Entity, line and event tables are one byte per item
    unless noted; the "0" suffix marks a starting value the game copies
    into the game state.
    """
    defs, ents, lines, events = g["defs"], g["entities"], g["lines"], g["events"]
    cmds = [c for e in events for c in e["cmds"]]
    # each monster entity gets the next monster slot; others get 255
    n_mon = 0
    mon_slot = []
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
    ev_at = [255] * 1024                       # per cell: its event index, 255 none
    for i, e in enumerate(events):
        ev_at[e["z"] * 32 + e["x"]] = i
    wall_bits = [0] * 128                      # 1024 cells, one bit each (bit i&7 of byte i>>3)
    for i, w in enumerate(g["wall"]):
        if w:
            wall_bits[i >> 3] |= 1 << (i & 7)
    line_cells = [0] * 128                     # the same, for cells that hold a switchable line
    for l in lines:
        k = l["z"] * 32 + l["x"]
        line_cells[k >> 3] |= 1 << (k & 7)
    starts, k = [], 0                          # each event's first command index
    for e in events:
        starts.append(k)
        k += len(e["cmds"])

    def drop_def(et, es):
        """The level's def index for (eType, eSub), or 255 if the level has none."""
        for i, d in enumerate(defs):
            if (d["eType"], d["eSub"]) == (et, es):
                return i
        return 255

    names = b""
    name_off = []
    t = {}
    t["def_etype"] = bytes(d["eType"] for d in defs)
    t["def_esub"] = bytes(d["eSub"] for d in defs)
    t["def_parm"] = b"".join(struct.pack("<i", d["parm"]) for d in defs)
    for d in defs:
        name_off.append(len(names))
        names += d["name"].upper().encode("latin1") + b"\0"
    # names: u16 offsets (from the table start) then the zero-ended upper-case names
    t["def_name"] = b"".join(struct.pack("<H", 2 * len(defs) + o) for o in name_off) + names
    t["ent_def0"] = bytes(e["def"] for e in ents)
    t["ent_x0"] = bytes(e["x"] for e in ents)
    t["ent_z0"] = bytes(e["z"] for e in ents)
    t["ent_hidden0"] = bytes(1 if e["hidden"] else 0 for e in ents)
    t["ent_mon"] = bytes(mon_slot)
    t["line_x"] = bytes(l["x"] for l in lines)
    t["line_z"] = bytes(l["z"] for l in lines)
    t["line_etype"] = bytes(l["eType"] for l in lines)
    t["line_locked0"] = bytes(1 if l["locked"] else 0 for l in lines)
    t["ev_x"] = bytes(e["x"] for e in events)
    t["ev_z"] = bytes(e["z"] for e in events)
    t["ev_state0"] = bytes(e["state"] for e in events)
    t["ev_flags"] = bytes(e["flags"] for e in events)
    t["ev_cmd0"] = b"".join(struct.pack("<H", k) for k in starts)   # (words: up to MAX_CMDS)
    t["ev_ncmd"] = bytes(len(e["cmds"]) for e in events)
    t["ev_at"] = bytes(ev_at)
    t["cmd_op"] = bytes(c["op"] for c in cmds)
    t["cmd_a1"] = b"".join(struct.pack("<I", c["a1"] & 0xFFFFFFFF) for c in cmds)
    t["cmd_a2"] = b"".join(struct.pack("<I", c["a2"] & 0xFFFFFFFF) for c in cmds)
    t["wall_bits"] = bytes(wall_bits)
    t["static_block"] = bytes(g["static_block"])
    t["line_cells"] = bytes(line_cells)
    t["drop_def"] = bytes(drop_def(*dt) for dt in DROP_TYPES)
    t["start"] = bytes([1 if g.get("new_game") else 0])   # 1: the starting kit (the UAC Base)
    t["title"] = g.get("title", "Map").upper().encode("latin1")[:16] + b"\0"   # shown at the start and the end
    sx, sz, sf = g["spawn"]
    # version 2: ev_cmd0 words, the command count's high byte last
    hdr = b"LVL1" + bytes([2, len(defs), len(ents), n_mon, len(lines), len(events), len(cmds) & 0xFF,
                           len(g["strings"]), sx, sz, sf, len(cmds) >> 8])
    assert len(hdr) == 16
    body = b""
    offs = []
    for name in BLOB_TABLES:
        offs.append(BLOB_HEADER + len(body))
        body += t[name]
    blob = hdr + b"".join(struct.pack("<H", o) for o in offs) + body
    if len(blob) > LEVEL_MAX:
        raise ValueError(f"level data too big: {len(blob)} bytes (max {LEVEL_MAX})")
    return blob


def strings_blob(strs, gg=False):
    """The strings file: u16 offset per string (from the blob start), then zero-ended Latin-1 text,
    the phone's keys a text names written as the Master System's buttons (gg: the Game Gear's) and
    the text re-wrapped to fit that console's box (key_text.sms_text).

    Raises ValueError if over STRINGS_MAX.
    """
    offs, body = [], bytearray()
    base = 2 * len(strs)
    for s in strs:
        offs.append(base + len(body))
        body += sms_text(s, gg).encode("latin1") + b"\0"
    blob = b"".join(o.to_bytes(2, "little") for o in offs) + bytes(body)
    if len(blob) > STRINGS_MAX:
        raise ValueError(f"strings too big: {len(blob)} bytes (max {STRINGS_MAX})")
    return blob


def main():
    """Write level.bin, strings.bin, strings_gg.bin, texts.bin, texts_gg.bin,
    defs.inc, tables.inc, texts.inc and layout.json."""
    gd_path, out_dir, gfx_dir, jar = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4])
    out_dir.mkdir(parents=True, exist_ok=True)
    gfx_dir.mkdir(parents=True, exist_ok=True)
    g = json.loads(gd_path.read_text())

    blob = level_blob(g)
    (gfx_dir / "level.bin").write_bytes(blob)
    strs = strings_blob(g["strings"])
    (gfx_dir / "strings.bin").write_bytes(strs)
    # (the Game Gear's: its pause button is START, its box narrower)
    (gfx_dir / "strings_gg.bin").write_bytes(strings_blob(g["strings"], gg=True))
    # the game's texts from the jar (the Game Gear's names its own buttons)
    tables = game_text.load(jar)
    for name, gg in (("texts.bin", False), ("texts_gg.bin", True)):
        tx = game_text.sms_blob(tables, sms_text, gg)
        if len(tx) > TEXTS_MAX:
            raise ValueError(f"the game's texts are too long: {len(tx)} bytes (max {TEXTS_MAX})")
        (gfx_dir / name).write_bytes(tx)
    off = write_includes(out_dir)
    print(f"gen_cdata: level blob {len(blob)} B, strings {len(strs)} B; game state {off} B")


def write_includes(out_dir):
    """The level-independent part, for the assembler: texts.inc, defs.inc,
    tables.inc and layout.json in out_dir (nothing in them comes from the
    jar or the map, so the no-jar build, tools/nojar.py, writes the same).
    Returns the game state's size."""
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "texts.inc").write_text(game_text.wla_include())

    # ---- for the assembler (game/*.asm)
    d = ["; Generated by tools/gen_cdata.py - do not edit", "; limits"]
    for nm, v in (("MAX_DEFS", MAX_DEFS), ("MAX_MAP_ENTS", MAX_MAP_ENTS), ("N_DROPS", N_DROPS),
                  ("MAX_ENTS", MAX_ENTS), ("MAX_MONS", MAX_MONS), ("MAX_LINES", MAX_LINES),
                  ("MAX_EVENTS", MAX_EVENTS), ("MAX_CMDS", MAX_CMDS), ("MAX_ACTIVE", MAX_ACTIVE),
                  ("MAX_NOTES", MAX_NOTES), ("N_WEAPONS", len(WEAPONS)), ("LEVEL_ADDR", LEVEL_ADDR),
                  ("LEVEL_N_TABLES", len(BLOB_TABLES))):
        d.append(f".define {nm:<16} {v}")
    d += ["; what monsters and crates drop (index into the level's drop_def table)",
          ".define DD_HEALTH 0", ".define DD_ARMOR 1", ".define DD_CREDIT1 2", ".define DD_CREDIT5 3",
          ".define DD_SMMED 4", ".define DD_LGMED 5", ".define DD_AMMO 6         ; + ammo type 0..4",
          ".define DD_AMMO2 11        ; + ammo type 0..4", "; level blob tables (their u16 offsets follow the 16-byte header)"]
    for i, name in enumerate(BLOB_TABLES):
        d.append(f".define LT_{name.upper():<14} {i}")
    d.append("; the game state (at STATE_ADDR; compared with the Python model up to S_END_COMPARED)")
    d.append(".define STATE_ADDR $C400")
    off = 0
    for name, size in state_layout():
        d.append(f".define S_{name.upper():<14} STATE_ADDR+{off:<5}; {size} byte{'s' if size != 1 else ''}")
        off += size
    d.append(f".define STATE_SIZE {off}")
    (out_dir / "defs.inc").write_text("\n".join(d) + "\n")

    def db(name, vals, per=16, kind=".db"):
        """Assembler lines: label name, then vals as kind (.db or .dw) lines of per values."""
        out = [f"{name}:"]
        for i in range(0, len(vals), per):
            out.append(f"    {kind} " + ", ".join(str(v) for v in vals[i:i + per]))
        return out
    t = ["; Generated by tools/gen_cdata.py - do not edit",
         "; weapons (wpn_*), monster classes (mon_*) and damage multipliers: tools/game_rules.py"]
    for nm, col in (("wpn_min", 1), ("wpn_max", 2), ("wpn_rmin", 3), ("wpn_rmax", 4), ("wpn_ammo", 5),
                    ("wpn_use", 6), ("wpn_armor", 7), ("wpn_shots", 8)):
        t += db(nm, [w[col] for w in WEAPONS], 19)
    # (the weapons' names are the game's texts TX_WPN_0.., texts.inc)
    # monster tables: 6 base stats per class (clamped to a byte), 2 attacks,
    # shots; then damage multipliers as words, [class][weapon]
    t += db("mon_base", [min(255, v) for m in MONSTERS for v in m[0]], 6)
    t += db("mon_atk", [v for m in MONSTERS for v in m[1]], 2)
    t += db("mon_shots", [m[2] for m in MONSTERS], 14)
    t += db("dmg_mult", [weapon_damage_mult(mc, w) for mc in range(len(MONSTERS)) for w in range(len(WEAPONS))],
            len(WEAPONS), ".dw")
    (out_dir / "tables.inc").write_text("\n".join(t) + "\n")

    # the game state's byte layout for the Python side: name -> [offset, size]
    lay = state_layout()
    off = 0
    layout = {}
    for name, size in lay:
        layout[name] = [off, size]
        off += size
    (out_dir / "layout.json").write_text(json.dumps({"fields": layout, "max_ents": MAX_ENTS,
                                                     "max_mons": MAX_MONS, "n_drops": N_DROPS}, indent=1))
    return off


if __name__ == "__main__":
    main()
