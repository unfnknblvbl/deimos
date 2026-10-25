#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The game's own texts that the consoles' code shows (the rules' messages,
the menus' and the vendor's words, the weapons' names, the maps' titles),
read from the user's DoomRPG.jar when a ROM is built. None of them is
written in Deimos's code: each is made by a "recipe" from the jar's string
tables (base.str, entities.str), so a ROM built from another jar shows that
jar's texts.

The "texts table" (TEXTS, in this order; the code refers to a text by its
index, TX_<name>) is put into every ROM:
  - every console but the Master System: in the campaign block
    (megadrive/tools/md_campaign.py, at the offset in the block header's
    bytes 24..27): u16 big-endian offset of each text from the table's
    start, then the texts, each ended by a 0 (md_blob);
  - the Master System and Game Gear: in bank 2 at $8000 (sms_blob): u16
    little-endian CPU address ($8000 + offset) of each text, then the texts.
Texts that name a phone key ("the 7 key") go through the same tokenising
as the maps' texts (key_text.md_text / sms_text), so the consoles name
their own buttons; the texts of the real jar have none.

A recipe is a list of parts joined together, then upper-cased (the consoles
show capitals): a part is (table, index, piece, op) - the string `index` of
the jar's `table`, or its piece number `piece` between the "%1"-style
placeholders the phone game fills in (None: the whole string), changed by
op:
  ""        as it is
  "colon"   without its ':'                   ("Credits:" -> "Credits")
  "bar"     without a trailing '|'            ("!|" -> "!")
  "count"   "10x Shells" -> "10 Shells" (the 'x' after a leading number)
  "word"    its first word only
or a plain string of Deimos's own (a space, or a name the game does not
spell this way). The Deimos Editor makes the same table with the same
recipes (editor/src/gametext.cpp), so its ROMs stay byte-identical.

The Python models (megadrive/tools/game_model.py and the Master System's)
use model_texts(): the same recipes without the upper-casing, as the phone
game writes them.

Usage: game_text.py JAR [--sms | --gg]   prints the texts table (a test)
Licence: GPL-2.0-or-later.
"""
import re
import struct
import sys
import zipfile
from pathlib import Path

B, E, I = "base", "entities", "items"   # the jar's tables: base.str, entities.str, items.str


def p(table, index, piece=None, op=""):
    """A recipe part: string `index` of `table` (its piece `piece`), op applied."""
    return (table, index, piece, op)


# the texts table, in order: (name, recipe parts, used by the ROMs). The
# rules' messages first (core/rules.c, megadrive/src/rules.s, the Master
# System's game/*.asm), then the menus' and boxes' words (the ports' ui.c,
# the Master System's game/ui.asm), the vendor's items and the weapons.
TEXTS = [
    # ---- the rules' messages
    ("GAINED", [p(B, 69, 0)]),                    # "GAINED " n " HEALTH"
    ("HEALTH_GAINED", [p(B, 69, 1)]),
    ("ARMOR_GAINED", [p(B, 68, 1)]),
    ("XP_GAINED", [p(B, 84, 1)]),
    ("LEVEL_UP", [p(B, 78), " ", p(B, 76, 0, "colon")]),   # "LEVEL UP! LEVEL " n
    ("DAMAGE", [p(B, 0, 1)]),                     # n " DAMAGE!"
    ("NEAR_DEATH", [p(B, 31)]),
    ("LOW_HEALTH", [p(B, 30)]),
    ("ARMOR_GONE", [p(B, 32)]),
    ("DOG_TOOK", [p(B, 29, 0)]),                  # "DOG TOOK " n " DAMAGE!"
    ("DOG_DIED", [p(B, 28)]),
    ("DIED", [p(B, 16, 2)]),                      # name " DIED!"
    ("ATTACKS", [p(B, 1, 1)]),                    # name " ATTACKS"
    ("CASTS_RAISE", [p(B, 5, 1)]),
    ("REVIVED", [p(B, 7, 1)]),
    ("RAISE_FAILED", [p(B, 6)]),
    ("DODGED", [p(B, 4)]),
    ("FIRE_CLEARED", [p(B, 33)]),
    ("TRAPPED", [p(B, 35)]),
    ("SECRET", [p(B, 79)]),
    ("PAIN", [p(B, 26)]),
    ("NEED_GREEN", [p(B, 88)]),                   # the keys in the rules' order: green, yellow, blue, red
    ("NEED_YELLOW", [p(B, 90)]),
    ("NEED_BLUE", [p(B, 87)]),
    ("NEED_RED", [p(B, 89)]),
    ("LOW_HP", [p(B, 82)]),                       # a cost not met: health, armour, credits, XP
    ("LOW_ARMOR", [p(B, 80)]),
    ("LOW_FUNDS", [p(B, 81)]),
    ("LOW_XP", [p(B, 83)]),
    ("CODE_BAD", [p(B, 242)]),
    ("CODE_GOOD", [p(B, 241)]),
    ("DOOR_CLEARED", [p(B, 34)]),
    ("NO_AMMO", [p(B, 25)]),
    ("MISSED", [p(B, 23)]),
    ("NO_EFFECT", [p(B, 24)]),
    ("ARMOR_MAX", [p(B, 37)]),
    ("HEALTH_MAX", [p(B, 39)]),
    ("GOT", [p(B, 40, 0)]),                       # "GOT " name
    ("AMMO_MAX", [p(B, 36)]),
    ("BURNS", [p(B, 50)]),
    ("BURNS_MORE", [p(B, 51)]),
    ("NOTHING", [p(B, 66)]),
    ("TURN_PASSED", [p(B, 67)]),
    ("PORTAL_SPAWNS", [p(B, 91, 0)]),                    # "PORTAL SPAWNS " name "!"
    ("PORTAL_END", [p(B, 91, 1)]),
    ("BERSERK_ON", [p(B, 85)]),
    ("BERSERK_OFF", [p(B, 86)]),
    ("NO_DOG", [p(B, 12)]),
    ("BAD_TARGET", [p(B, 10)]),
    ("WONT_FIT", [p(B, 13, 0)]),                  # "DOG COLLAR WON'T FIT " name
    # ---- the menus and boxes
    ("CREDITS_WORD", [p(B, 162, None, "colon")]),      # the status bar's word
    ("CREDITS_SP", [p(B, 162, None, "colon"), " "]),        # the vendor's "CREDITS " n
    ("CREDITS_MID", ["  ", p(B, 162, None, "colon"), " "]),  # a sector's "LEVEL l  CREDITS c"
    ("NOTEBOOK_ITEM", [p(B, 117)]),
    ("OPTIONS_ITEM", [p(B, 148)]),
    ("CONTINUE_ITEM", [p(B, 114)]),
    ("NEW_GAME", [p(B, 147)]),
    ("SAVE_GAME", [p(B, 154)]),
    ("GAME_SAVED", [p(B, 120)]),
    ("GAME_LOADED", [p(B, 115)]),
    ("NO_CREDITS", [p(B, 238)]),                  # the vendor: not enough credits
    ("TOO_MANY", [p(B, 217), " ", p(B, 218, None, "word")]),  # ... or too many already
    ("BERSERKER_ITEM", [p(E, 73)]),                    # the items box
    ("DOG_COLLAR", [p(E, 74)]),
    ("CAPTURED", [p(B, 11, 0)]),                  # "YOU CAPTURED|THE " name "!"
    ("CAPTURED_END", [p(B, 11, 1, "bar")]),
    # ---- the vendor's items, by the rules' item tag (0, 1, 6, 12: none)
    ("STORE_0", []), ("STORE_1", []),
    ("STORE_2", [p(B, 182)]), ("STORE_3", [p(B, 181)]), ("STORE_4", [p(B, 183)]), ("STORE_5", [p(B, 180)]),
    ("STORE_6", []),
    ("STORE_7", [p(B, 177, None, "count")]), ("STORE_8", [p(B, 175, None, "count")]),
    ("STORE_9", [p(B, 179, None, "count")]), ("STORE_10", [p(B, 176, None, "count")]),
    ("STORE_11", [p(B, 178, None, "count")]),
    ("STORE_12", []),
    ("STORE_13", [p(B, 184)]), ("STORE_14", [p(B, 185)]), ("STORE_15", [p(B, 187)]), ("STORE_16", [p(B, 186)]),
    # ---- the weapons' names (the rules' weapon numbers, game_rules.py
    # WEAPONS): the game's own where it spells them so, else Deimos's
    ("WPN_0", [p(E, 0)]), ("WPN_1", [p(E, 1)]), ("WPN_2", [p(E, 8)]), ("WPN_3", [p(E, 2)]),
    ("WPN_4", [p(E, 4)]), ("WPN_5", ["SUPER SHOTGUN"]), ("WPN_6", [p(E, 6)]), ("WPN_7", [p(E, 5)]),
    ("WPN_8", [p(E, 7)]), ("WPN_9", ["HELL HOUND"]), ("WPN_10", [p(E, 10)]), ("WPN_11", [p(E, 11)]),
    ("WPN_12", ["MELEE 1"]), ("WPN_13", ["MELEE 2"]), ("WPN_14", ["MELEE 3"]), ("WPN_15", ["MISSILE"]),
    ("WPN_16", ["BOSS MISSILE"]), ("WPN_17", ["ROCKET MISSILE"]), ("WPN_18", ["RAISE"]),
]
NAMES = [n for n, _ in TEXTS]
ID = {n: i for i, n in enumerate(NAMES)}        # name -> its index in the table

# texts only the Python models use (as the phone game writes them; the
# consoles say these their own way)
MODEL_ONLY = {
    "HOLD_MORE": [p(B, 41, 0)],                   # "Can't hold more " name "s"
    "HOLD_MORE_END": [p(B, 41, 1)],
    "CRIT": [p(B, 14, 0)],                        # "Crit! " n " damage!"
    "CAPTURED_FULL": [p(B, 11)],                  # the dialog, with "%1" for the name
}

# the maps' titles: the map's file name -> its title (a base.str string),
# or Deimos's own where the game has none
MAP_TITLE = {"intro": "UAC Base", "junction": p(B, 57), "junction_destroyed": p(B, 57), "reactor": p(B, 58),
             "endgame": "Ending", "items": p(I, 0), **{f"level0{i}": p(B, 58 + i) for i in range(1, 8)}}


def read_str_table(data: bytes):
    """One .str table (u16 count, then per string a u16 length and that
    many Latin-1 bytes, little-endian) as a list of strings."""
    n = struct.unpack_from("<H", data, 0)[0]
    pos, out = 2, []
    for _ in range(n):
        ln = struct.unpack_from("<H", data, pos)[0]
        out.append(data[pos + 2:pos + 2 + ln].decode("latin1"))
        pos += 2 + ln
    return out


def load(jar):
    """The jar's string tables {table: [strings]} from `jar`: the DoomRPG.jar
    file itself or a folder of its files (a build's assets/jar). A table
    the jar lacks is empty."""
    jar = Path(jar)
    out = {}
    for t in (B, E, I):
        if jar.is_dir():
            f = jar / f"{t}.str"
            data = f.read_bytes() if f.exists() else None
        else:
            with zipfile.ZipFile(jar) as z:
                data = z.read(f"{t}.str") if f"{t}.str" in z.namelist() else None
        out[t] = read_str_table(data) if data else []
    return out


def to_upper(s):
    """s with its letters a..z made capitals (only those: the consoles'
    fonts, and the editor's builders, change nothing else)."""
    return "".join(chr(ord(c) - 32) if "a" <= c <= "z" else c for c in s)


def part(tables, pt):
    """One recipe part's text (see the module docstring); a string the jar
    does not have is ""."""
    if isinstance(pt, str):
        return pt
    table, index, piece, op = pt
    rows = tables.get(table, [])
    s = rows[index] if index < len(rows) else ""
    if piece is not None:
        pieces = re.split(r"%[0-9]", s)
        s = pieces[piece] if piece < len(pieces) else ""
    if op == "colon":
        s = s.replace(":", "")
    elif op == "bar":
        s = s.rstrip("|")
    elif op == "count":
        s = re.sub(r"^([0-9]+)x ", r"\1 ", s)
    elif op == "word":
        s = s.split(" ")[0]
    return s


def make(tables, recipe, upper=True):
    """A recipe's text: its parts joined (upper-cased unless upper is False)."""
    s = "".join(part(tables, pt) for pt in recipe)
    return to_upper(s) if upper else s


def texts(tables):
    """The texts table as the consoles show it (before tokenising): a list
    in TEXTS order."""
    return [make(tables, r) for _, r in TEXTS]


def model_texts(tables):
    """{name: text} as the phone game writes it (not upper-cased), every
    TEXTS and MODEL_ONLY entry: for the Python models."""
    out = {n: make(tables, r, upper=False) for n, r in TEXTS}
    out.update({n: make(tables, r, upper=False) for n, r in MODEL_ONLY.items()})
    return out


def map_title(tables, name):
    """The title a map (by its file name) is shown with, as the game writes
    it ("" for a map MAP_TITLE does not know: the caller falls back)."""
    t = MAP_TITLE.get(name)
    if t is None:
        return ""
    return part(tables, t)


def md_blob(tables, md_text):
    """The texts table as the campaign block holds it: u16 big-endian offset
    of each text from the table's start, then the texts (Latin-1, each
    ended by a 0). md_text: key_text.md_text, the tokeniser."""
    ts = [md_text(s).encode("latin1") + b"\0" for s in texts(tables)]
    out = bytearray(2 * len(ts))
    for i, t in enumerate(ts):
        struct.pack_into(">H", out, 2 * i, len(out))
        out += t
    return bytes(out)


SMS_ADDR = 0x8000   # where the Master System's table is (bank 2, paged into slot 2)


def sms_blob(tables, sms_text, gg=False):
    """The texts table as the Master System (gg: the Game Gear) holds it:
    u16 little-endian CPU address of each text (SMS_ADDR + its offset),
    then the texts, each ended by a 0. sms_text: key_text.sms_text."""
    ts = [sms_text(s, gg).encode("latin1") + b"\0" for s in texts(tables)]
    out = bytearray(2 * len(ts))
    for i, t in enumerate(ts):
        struct.pack_into("<H", out, 2 * i, SMS_ADDR + len(out))
        out += t
    return bytes(out)


def c_header():
    """texts.h: an enum of the texts' indices (TX_<name>) for the C code."""
    h = ["/* Generated by tools/game_text.py - do not edit. The texts table's",
         "   indices: gtext(TX_...) is the text (read from DoomRPG.jar) */",
         "#ifndef GAME_TEXTS_H", "#define GAME_TEXTS_H", "enum {"]
    h += [f"    TX_{n} = {i}," for i, n in enumerate(NAMES)]
    h += [f"    TX_COUNT = {len(NAMES)}", "};", "#endif", ""]
    return "\n".join(h)


def gas_include():
    """texts.inc for the Mega Drive's 68000 assembler: .equ TX_<name>, index."""
    lines = ["| Generated by tools/game_text.py - do not edit: the texts table's indices"]
    lines += [f"    .equ    TX_{n}, {i}" for i, n in enumerate(NAMES)]
    return "\n".join(lines) + "\n"


def wla_include():
    """texts.inc for the Master System's WLA-DX assembler: TX_<name> is the
    address of the text's pointer in the table (ld hl, (TX_...) loads the
    text's address)."""
    lines = ["; Generated by tools/game_text.py - do not edit: where each text's",
             "; address is in the texts table (bank 2, $8000): ld hl, (TX_...)"]
    lines += [f".define TX_{n} ${SMS_ADDR + 2 * i:04X}" for i, n in enumerate(NAMES)]
    lines += [f".define TX_COUNT {len(NAMES)}"]
    return "\n".join(lines) + "\n"


def fmt(s, *args):
    """A phone-game string with its "%1", "%2" ... replaced by args (str()
    of each; a placeholder without an argument becomes "")."""
    return re.sub(r"%([0-9])", lambda m: str(args[int(m.group(1)) - 1]) if int(m.group(1)) <= len(args) else "", s)


def main():
    """game_text.py JAR [--sms | --gg]: prints each text of the table."""
    tables = load(sys.argv[1])
    for i, (n, s) in enumerate(zip(NAMES, texts(tables))):
        print(f"{i:3d} TX_{n:<14} {s!r}")


if __name__ == "__main__":
    main()
