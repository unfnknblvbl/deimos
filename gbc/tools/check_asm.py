#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
A check of SDCC's sm83 code (build.sh runs it on every .asm): SDCC 4.5 can
shift register l in a loop right after it has loaded hl with a stack address
(ldhl sp) for the loop's count, so the shift works on the address, not the
value (seen in core/rules.c's run_event: commands skipped at random). Any
such loop fails the build. Licence: GPL-2.0-or-later.
  check_asm.py FILE.asm...

The pattern (PAT) is the code SDCC emits for such a loop: "ldhl sp, #n"
and "ld a, (hl)" load the count, "inc a" and a jump to the loop's test,
then the loop's label and a shift of h or l; at that point hl still holds
the stack address, so the shift damages it. Exits 1 when any is found.
"""
import re
import sys

PAT = re.compile(r"\tldhl\tsp,\t#\d+\n\tld\ta, \(hl\)\n(?:\tpush\taf\n\tpop\taf\n)?\tinc\ta\n\tj[rp]\t(\d+\$)\n"
                 r"(\d+\$):\n\t(?:srl|sla|sra|rr|rl|rlc|rrc)\t([hl])\n")


def main():
    """Report every match in the files given; exit 1 if there is one."""
    bad = 0
    for f in sys.argv[1:]:
        txt = open(f).read()
        for m in PAT.finditer(txt):
            line = txt[:m.start()].count("\n") + 1    # (the match's line number)
            print(f"{f}:{line}: a shift of {m.group(3)} after ldhl sp (SDCC's bug)")
            bad += 1
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
