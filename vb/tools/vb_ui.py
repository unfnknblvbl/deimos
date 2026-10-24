#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The Virtual Boy's copy of the Game Boy Color's ui.c (gbc/src/ui.c, the
menus and boxes made for a small screen), changed for the Virtual Boy's
384-pixel-wide screen and its buttons. The source is not changed; build.sh
compiles the copy (obj/ui.c).

  vb_ui.py ../gbc/src/ui.c obj/ui.c

The changes (each must apply, or the build stops):
  - the buttons an action can have: the SNES's X and Y are the right D-pad's
    down and up (R-DOWN, R-UP), its L and R the triggers (L, R), its Select
    the Virtual Boy's (SELECT): the names the options screen and the game's
    texts show;
  - the options screen's button names a column further right (after "PREV
    WEAPON" and "NEXT WEAPON", which fill 11 columns);
  - the status bar 25 columns wide, laid out as the SNES's and the Mega
    Drive's (snes/src/ui.c's hud_update: "AR 0/20", "AMMO", "KEYS", "SOUL"
    in full), in place of the Game Boy Color's 20-column one; gfx.c draws
    its 5 lines of 25 (gfx.h's HUD_COLS).
Licence: GPL-2.0-or-later.
"""
import sys
from pathlib import Path

# the Game Boy Color's status bar (its hud_update's lines, from the first
# hud_text up to the call of gfx_hud), and the SNES's layout in its place
# (snes/src/ui.c's rows 1..5 and columns 7.. as rows 0..4, columns 0..)
GBC_HUD_START = '    hud_text(0, 0, "HEALTH");'
GBC_HUD_END = '    gfx_hud(hud_new);'
VB_HUD = '''    hud_text(0, 0, "HEALTH");
    c = hud_num(0, 7, S.hp);
    hud_text(0, c, "/");
    hud_num(0, c + 1, S.maxhp);
    hud_text(0, 14, "AR");
    c = hud_num(0, 17, S.armor);
    hud_text(0, c, "/");
    hud_num(0, c + 1, S.maxarmor);

    hud_text(1, 0, wpn_name[S.weapon]);
    if (wpn_use[S.weapon]) {            /* (a weapon that uses ammunition) */
        hud_text(1, 14, "AMMO");
        hud_num(1, 19, S.ammo[wpn_ammo[S.weapon]]);
    }
    hud_text(2, 0, "LEVEL");
    hud_num(2, 6, S.level);
    hud_text(2, 10, "XP");
    c = hud_num(2, 13, G16(S.xp));      /* (G16: a big-endian u16 field of S) */
    hud_text(2, c, "/");
    hud_num(2, c + 1, G16(S.nextxp));

    hud_text(3, 0, "CREDITS");
    hud_num(3, 8, G16(S.credits));
    hud_text(3, 13, "KEYS");
    if (S.keys & 1) hud_text(3, 18, "G");  /* (key bits: green, yellow, blue, red) */
    if (S.keys & 2) hud_text(3, 19, "Y");
    if (S.keys & 4) hud_text(3, 20, "B");
    if (S.keys & 8) hud_text(3, 21, "R");

    hud_text(4, 0, "MEDKITS");
    hud_num(4, 8, S.inventory[0]);      /* (small medkits + large ones) */
    hud_text(4, 10, "+");
    hud_num(4, 11, S.inventory[1]);
    if (S.inventory[2]) {               /* (soul spheres) */
        hud_text(4, 14, "SOUL");
        hud_num(4, 19, S.inventory[2]);
    }
'''

# (old, new) text replacements, each to be found exactly once
CHANGES = [
    ('"SEL+B", "SEL+A", "SEL+LEFT", "SEL+RIGHT",', '"R-DOWN", "R-UP", "L", "R",'),
    ('"SEL+START"};', '"SELECT"};'),
    ("for (k = 0; b[k] && 11 + k < 20; k++) d[11 + k] = b[k];", "for (k = 0; b[k] && 12 + k < 20; k++) d[12 + k] = b[k];"),
    ("static char hud_new[5][21];", "static char hud_new[5][HUD_COLS + 1];"),
    ("for (c = 0; c < 20; c++) hud_new[r][c] = ' ';\n        hud_new[r][20] = 0;",
     "for (c = 0; c < HUD_COLS; c++) hud_new[r][c] = ' ';\n        hud_new[r][HUD_COLS] = 0;"),
    ("while (*s && col < 20) hud_new[row][col++] = *s++;", "while (*s && col < HUD_COLS) hud_new[row][col++] = *s++;"),
]


def main():
    """Read the Game Boy Color's ui.c, apply the changes, write the copy."""
    src = Path(sys.argv[1]).read_text()
    for old, new in CHANGES:
        if src.count(old) != 1:
            raise SystemExit(f"vb_ui.py: not found once in ui.c: {old!r}")
        src = src.replace(old, new)
    a, b = src.find(GBC_HUD_START), src.find(GBC_HUD_END)
    if a < 0 or b < a or src.count(GBC_HUD_START) != 1:
        raise SystemExit("vb_ui.py: the status bar's lines were not found in ui.c")
    src = src[:a] + VB_HUD + src[b:]
    Path(sys.argv[2]).write_text(src)


if __name__ == "__main__":
    main()
