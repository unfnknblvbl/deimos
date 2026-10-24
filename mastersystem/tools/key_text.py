#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The phone's keys in the game's texts, made into the console's buttons.

The original game's help texts name the phone's keys: "press the OK|button",
"the * and 7 keys", "the # key", "the 0|key", "the 9 button" ('|' is a line
break in a text). tokenise() turns each such key into a token byte, a
control code below 32 that no text has otherwise, naming the action the key
does, and its "key"/"keys" into "button"/"buttons":

  KEY_USE    1   use / attack (the OK key)        KEY_PREV   4   previous weapon (*)
  KEY_PASS   2   pass the turn (9)                KEY_NEXT   5   next weapon (7)
  KEY_MEDKIT 3   use a medkit (no text has it)    KEY_NOTES  6   notebook (#, the automap)
  KEY_DETAIL 7   full detail (Mega Drive only;    KEY_MENU  16   the pause menu (0): Start
                 no text has it)

so a token is 1 + the action's number in the ports' ACT_* order. The Mega
Drive (src/ui.c) and the ports (snes/src/ui.c, gbc/src/ui.c) replace a token
with the name of the button that does that action now (the options screen
can move them) when they put a line of text into a box. As the names are
longer than the one character they replace (up to 9: the Game Boy's
"SEL+RIGHT"), md_text() then re-wraps the lines that might no longer fit a
box's 21 columns (a box is 24 wide, text from column 3), counting each
token as its widest name. The Master System and Game Gear have fixed
buttons: sms_text() writes the names in and re-wraps to that box's width
(tools/gen_cdata.py: strings.bin and strings_gg.bin).

Texts without such a key come out unchanged. The editor (editor/src/
keytext.cpp) does exactly the same, so that its ROMs stay byte-identical
with these builds. This file is a copy of megadrive/tools/key_text.py
(whose gen_cdata.py makes the Mega Drive's and the other ports' strings
with md_text): keep the two the same.
"""

KEY_USE, KEY_PASS, KEY_MEDKIT, KEY_PREV, KEY_NEXT, KEY_NOTES, KEY_DETAIL = range(1, 8)
KEY_MENU = 16
TOKENS = (KEY_USE, KEY_PASS, KEY_MEDKIT, KEY_PREV, KEY_NEXT, KEY_NOTES, KEY_DETAIL, KEY_MENU)

# the phone's key (as the texts write it) -> the token of what it does
# (the jar's key codes: * previous weapon, 7 next, # automap, 0 menu, 9 pass)
KEYS = {"OK": KEY_USE, "*": KEY_PREV, "7": KEY_NEXT, "#": KEY_NOTES, "0": KEY_MENU, "9": KEY_PASS}
# the word after the key -> what it becomes (longest first: they are tried in this order)
NOUNS = (("buttons", "buttons"), ("button", "button"), ("keys", "buttons"), ("key", "button"))

BOX_COLS = 21                   # a box line's columns from the text's column (3) to the edge (24)
WIDEST_ACTION = 9               # the longest button name an action can have ("SEL+RIGHT", "SEL+START")
WIDEST_MENU = 5                 # the pause menu's button ("START")

# the Master System's and the Game Gear's fixed buttons (game/ui.asm)
SMS_NAMES = {KEY_USE: "1", KEY_PASS: "2+DOWN", KEY_MEDKIT: "2+UP", KEY_PREV: "2+LEFT", KEY_NEXT: "2+RIGHT",
             KEY_NOTES: "2+1", KEY_DETAIL: "", KEY_MENU: "PAUSE"}
SMS_COLS, GG_COLS = 21, 19      # their boxes' text columns (24 - 3; the Game Gear's 20 - 1)


def _word_end(s, i):
    """The index just past the word starting at s[i] (a word ends at a space,
    a '|' or the text's end)."""
    while i < len(s) and s[i] not in " |":
        i += 1
    return i


def _noun_at(s, i):
    """(the noun's length, what it becomes) if one of NOUNS starts at s[i]
    and is a whole word (not followed by a letter or a '-', as in
    "key-|card"), else None."""
    for word, new in NOUNS:
        j = i + len(word)
        if s.startswith(word, i) and (j == len(s) or not (s[j].isascii() and (s[j].isalpha() or s[j] == "-"))):
            return len(word), new
    return None


def _match(s, i):
    """At s[i] (the word "the"): (what replaces it, the index past it) if a
    key phrase starts here, else None. The phrase is "the K NOUN" or "the K
    and K NOUN" (K a key of KEYS, NOUN one of NOUNS), the words separated by
    one space or '|', which are kept as they were."""
    if not (s.startswith("the", i) and (i == 0 or s[i - 1] in " |") and i + 4 < len(s) and s[i + 3] in " |"):
        return None
    out = "the" + s[i + 3]
    i += 4
    for n_keys in (1, 2):                   # the first key, then perhaps "and" and a second
        e = _word_end(s, i)
        if s[i:e] not in KEYS or e >= len(s):
            return None
        out += chr(KEYS[s[i:e]]) + s[e]     # the key's token and the separator after it
        i = e + 1
        noun = _noun_at(s, i)
        if noun:
            return out + noun[1], i + noun[0]
        if n_keys == 2 or not (s.startswith("and", i) and i + 4 < len(s) and s[i + 3] in " |"):
            return None
        out += "and" + s[i + 3]
        i += 4
    return None


def tokenise(s):
    """s with each key phrase (see _match: "the * and 7 keys" and the like)
    made into tokens: "the T button(s)" / "the T and T buttons"."""
    out, i = [], 0
    while i < len(s):
        m = _match(s, i)
        if m:
            out.append(m[0])
            i = m[1]
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def has_token(s):
    """True if text s holds a token (a byte of TOKENS)."""
    return any(ord(c) in TOKENS for c in s)


def rewrap(s, width, cols):
    """Text s with its lines ('|') re-wrapped where they are wider than cols
    columns, width(c) giving each character's columns: such a line is broken
    at its last space that leaves the part before it within cols, and the
    rest goes to the front of the next line (joined with a space, or with
    nothing after a '-'), or onto a new line before an empty line or at the
    end. Then the next line is looked at, and so on. A line that cannot be
    broken (no such space) is left as it is: the box cuts it."""
    lines = s.split("|")
    i = 0
    while i < len(lines):
        ln = lines[i]
        w, cut = 0, -1                      # (w: the columns of ln[:p])
        for p, c in enumerate(ln):
            if c == " " and p > 0 and w <= cols:
                cut = p
            w += width(c)
        if w > cols and cut > 0:
            head, rest = ln[:cut], ln[cut + 1:]
            lines[i] = head
            if rest:
                if i + 1 < len(lines) and lines[i + 1]:
                    lines[i + 1] = rest + ("" if rest.endswith("-") else " ") + lines[i + 1]
                else:
                    lines.insert(i + 1, rest)
        i += 1
    return "|".join(lines)


def md_width(c):
    """A character's columns at most in a Mega Drive or SNES-family box: a
    token's widest button name, else 1."""
    o = ord(c)
    return WIDEST_MENU if o == KEY_MENU else WIDEST_ACTION if o in TOKENS else 1


def md_text(s):
    """Text s as the Mega Drive's campaign block holds it (for the Mega
    Drive and every port that reads the block): its keys made into tokens
    and, if it has any, re-wrapped for the widest button names. A text
    without keys is returned unchanged."""
    t = tokenise(s)
    return rewrap(t, md_width, BOX_COLS) if has_token(t) else s


def sms_text(s, gg=False):
    """Text s as the Master System (gg: the Game Gear) shows it: its keys
    named as that console's buttons (SMS_NAMES; the Game Gear's pause menu
    is on START) and, if it had any, re-wrapped to that box's width. A text
    without keys is returned unchanged."""
    t = tokenise(s)
    if not has_token(t):
        return s
    names = dict(SMS_NAMES)
    if gg:
        names[KEY_MENU] = "START"
    t = "".join(names[ord(c)] if ord(c) in TOKENS else c for c in t)
    return rewrap(t, lambda c: 1, GG_COLS if gg else SMS_COLS)
