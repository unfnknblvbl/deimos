#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Where the time goes while the Master System draws a view: a sampling
profiler on the Gearsystem core. The autopilot plays the start of Sector 1;
at the end of every emulated frame spent drawing (the engine isn't waiting
for input) the program counter is looked up in rom/deimos.sym. Each
sample is one frame (1/60 s).

Usage: profile_render.py rom/deimos.sms [steps] [large|small] [black] [low] [solid]

steps defaults to 60 actions, the size to large; black, low and solid
switch on the pause menu's "draw at once", low detail and solid walls.
Prints the busy frames in total and the 25 routines most often caught,
as a percentage of the samples. The symbols always come from
rom/deimos.sym (the Master System build), whatever ROM is given.
Licence: GPL-2.0-or-later.
"""
import bisect
import collections
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import test_lockstep as T
import test_render as R
import game_model as gm
import autopilot

ROOT = Path(__file__).resolve().parent.parent      # the repository root


def symbols():
    """Read rom/deimos.sym into {bank: sorted [(address, label)]}.

    Only "BB:AAAA name" lines are used; local labels (starting with _ or @)
    are skipped so samples are credited to the enclosing routine.
    """
    out = collections.defaultdict(list)
    for line in (ROOT / "rom" / "deimos.sym").read_text().splitlines():
        p = line.split()
        if len(p) == 2 and ":" in p[0] and not p[1].startswith(("_", "@")):
            b, a = p[0].split(":")
            out[int(b, 16)].append((int(a, 16), p[1]))
    for v in out.values():
        v.sort()
    return out


def main():
    """Play the autopilot with the profiler sampling, then print the report."""
    rom_path = sys.argv[1]
    steps = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    size = sys.argv[3] if len(sys.argv) > 3 else "large"
    black = 1 if "black" in sys.argv else 0
    low = 1 if "low" in sys.argv else 0
    solid = 1 if "solid" in sys.argv else 0
    syms = symbols()
    rom = T.Rom(rom_path)              # boot the ROM on the Gearsystem core
    rom.run(30)
    rom.run(3, ("a",))                 # button 1: start the game
    rom.run(2)
    rom.wait_idle()                    # until the engine waits for input
    # set the view options through the pause menu (test_render's driver)
    chk = R.Checker.__new__(R.Checker)
    chk.rom = rom
    R.Checker.set_size(chk, size, black, low, solid)
    w = gm.load()                      # the rules model, kept in step to answer dialogs
    pilot = autopilot.Autopilot()
    F = T.LAYOUT["fields"]
    counts = collections.Counter()
    frames = 0

    def sample():
        """Credit the routine the Z80 is in now, unless the engine is idle.

        Finds the bank mapped at the program counter (slot 0 is always
        bank 0; slots 1 and 2 from the mapper registers' copies in RAM)
        and the nearest label at or below the PC in that bank.
        """
        nonlocal frames
        if rom.ram()[T.ENG_IDLE - 0xC000]:     # waiting for input: not drawing
            return
        pc = rom.e.regs()["pc"]
        r = rom.ram()
        if pc < 0x4000:
            bank = 0
        elif pc < 0x8000:
            bank = r[0x1FFE]          # the mapper's slot-1 register (mirrored in RAM)
        else:
            bank = r[0x1FFF]          # the slot-2 register ($FFFF, mirrored at $DFFF)
        lst = syms.get(bank, [])
        names = [a for a, _ in lst]
        i = bisect.bisect_right(names, pc) - 1     # the last label at or below pc
        name = lst[i][1] if i >= 0 else f"?{pc:04x}"
        counts[name] += 1
        frames += 1

    orig_run = rom.run

    def run(n, buttons=()):
        """rom.run, one frame at a time, taking a sample after each frame."""
        for _ in range(n):
            orig_run(1, buttons)
            sample()
    rom.run = run                      # every frame the harness runs is now sampled
    for _ in range(steps):
        if w.dead or w.level_done:
            break
        if w.p.hp < 100:
            # keep the player alive: HP and max HP to 200 in ROM and model
            for name in ("hp", "maxhp"):
                rom.e.poke(T.STATE_ADDR + F[name][0], 200)
            w.p.hp = w.p.maxhp = 200
        if w.dialog is not None:
            # page through the dialog
            for _ in range(T.pages(w, w.g["strings"].index(w.dialog))):
                rom.press(("a",))
            w.resume()
            continue
        if w.password is not None:
            # type the right door code
            for b in T.code_inputs(w.password[0]):
                rom.press(b)
            w.resume(w.password[0])
            continue
        act = pilot.choose(w)          # the autopilot's next action
        # plain moves are one button; the rest are button 2 + a direction
        buttons = {"fwd": ("up",), "back": ("down",), "left": ("left",), "right": ("right",), "use": ("a",)}.get(act)
        if buttons is None:
            buttons = {"pass": ("2", "down"), "wnext": ("2", "right"), "wprev": ("2", "left"), "heal": ("2", "up")}[act]
            rom.press(("2",))
            rom.press(buttons[1:], held=("2",))
            rom.run(2)
        else:
            rom.press(buttons)
        # the same action in the model
        {"fwd": lambda: w.move(False), "back": lambda: w.move(True), "left": lambda: w.turn(-1),
         "right": lambda: w.turn(1), "use": w.use, "pass": w.pass_turn,
         "wnext": lambda: w.cycle_weapon(1), "wprev": lambda: w.cycle_weapon(-1),
         "heal": w.use_health_item}[act]()
        rom.wait_idle()                # let the step finish (sampled)
    print(f"{frames} busy frames ({frames / 60:.1f} s) over {steps} actions, {size} view{' low detail' if low else ''}{' solid walls' if solid else ''}")
    for name, n in counts.most_common(25):
        print(f"  {100 * n / frames:5.1f}%  {name}")


if __name__ == "__main__":
    main()
