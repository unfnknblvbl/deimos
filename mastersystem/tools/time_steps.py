#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
How long a step takes to draw: the autopilot plays the start of level01
on the Gearsystem core at each view setting and reports, for moves and
turns, the frames from the button press to the finished picture, and to
the end of the whole step (the monsters' turn and the status bar follow
the picture).

Usage: time_steps.py rom/deimos.sms [steps]   (steps: per setting, default 150)

Each setting is a combination of the pause menu's options: view size
(large/small), drawing the view (outward in place, or "at once" behind a
black view), low detail and solid walls. The script plays the ROM and the
Python model (game_model.World) side by side, as the lockstep test does,
with the autopilot choosing the moves; the model is only used to choose
the buttons and to know which moves really moved the player. Times are in
frames (60 per second).
Licence: GPL-2.0-or-later.
"""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import test_lockstep as T
import test_render as R
import game_model as gm
import autopilot


# RAM address of the engine's count of finished views: it changes when a
# picture is complete
VAR_VIEWS = R.engine_vars()["var_views"]


def run(rom_path, size, black, steps, low=0, solid=0):
    """Play up to steps autopilot actions at one view setting and time them.

    rom_path: the ROM; size "large" or "small"; black 1 to draw the view at
    once, 0 outward; low and solid the detail and wall settings (0 or 1).
    Returns (picture frames, whole-step frames) as numpy arrays, one entry
    per move or turn that changed the player's position or facing.
    The player's HP is kept at 200 in both ROM and model so the run lasts.
    """
    rom = T.Rom(rom_path)              # boot the ROM on the Gearsystem core
    rom.run(30)                        # let the title screen come up
    rom.run(3, ("a",))                 # button 1 starts the game
    rom.run(2)
    rom.wait_idle()                    # until the engine waits for input
    # borrow test_render's pause-menu driver without building a full Checker
    chk = R.Checker.__new__(R.Checker)
    chk.rom = rom
    R.Checker.set_size(chk, size, black, low, solid)
    F = T.LAYOUT["fields"]
    w = gm.load()                      # the rules model at the level start
    pilot = autopilot.Autopilot()
    times, pics = [], []
    for _ in range(steps):
        if w.dead or w.level_done:
            break
        if w.p.hp < 100:
            # top up HP and max HP in the ROM's game state and in the model
            for name in ("hp", "maxhp"):
                rom.e.poke(T.STATE_ADDR + F[name][0], 200)
            w.p.hp = w.p.maxhp = 200
        if w.dialog is not None:
            # a dialog is open: page through it, untimed
            for _ in range(T.pages(w, w.g["strings"].index(w.dialog))):
                rom.press(("a",))
            w.resume()
            continue
        if w.password is not None:
            # a door code is asked for: enter the right one
            for b in T.code_inputs(w.password[0]):
                rom.press(b)
            w.resume(w.password[0])
            continue
        act = pilot.choose(w)          # the autopilot's next action name
        # plain moves are one button; the rest are button 2 + a direction
        buttons = {"fwd": ("up",), "back": ("down",), "left": ("left",), "right": ("right",), "use": ("a",)}.get(act)
        if buttons is None:
            buttons = {"pass": ("2", "down"), "wnext": ("2", "right"), "wprev": ("2", "left"), "heal": ("2", "up")}[act]
            rom.press(("2",))
            rom.press(buttons[1:], held=("2",))    # the direction with 2 held
            rom.run(2)
        else:
            moved = (w.x, w.z, w.facing)
            rom.wait_idle()
            seq = rom.byte(T.ENG_TURN_SEQ)  # bumped by the engine when a step is handled
            views = rom.byte(VAR_VIEWS)
            f0 = rom.nframe
            dp = None
            # hold the button frame by frame until the step is handled
            for _ in range(3000):
                rom.run(1, buttons)
                if dp is None and rom.byte(VAR_VIEWS) != views:
                    dp = rom.nframe - f0        # press -> picture finished
                if rom.byte(T.ENG_TURN_SEQ) != seq:
                    break
            dt = rom.nframe - f0                # press -> step handled
            if dp is None:
                dp = dt
            rom.wait_idle()
            rom.run(2)
        # the same action in the model
        {"fwd": lambda: w.move(False), "back": lambda: w.move(True), "left": lambda: w.turn(-1),
         "right": lambda: w.turn(1), "use": w.use, "pass": w.pass_turn,
         "wnext": lambda: w.cycle_weapon(1), "wprev": lambda: w.cycle_weapon(-1),
         "heal": w.use_health_item}[act]()
        if act in ("fwd", "back", "left", "right") and (w.x, w.z, w.facing) != moved:
            times.append(dt)
            pics.append(dp)
    return np.array(pics), np.array(times)


def main():
    """Time every view setting and print the medians and 90th percentiles in seconds."""
    rom_path = sys.argv[1]
    steps = int(sys.argv[2]) if len(sys.argv) > 2 else 150
    # (size, black, low, solid): both drawing styles at full detail, then
    # each detail/wall combination drawn outward
    modes = [("large", 0, 0, 0), ("large", 1, 0, 0), ("small", 0, 0, 0), ("small", 1, 0, 0)]
    modes += [(s, 0, low, solid) for s in ("large", "small") for low, solid in ((1, 0), (0, 1), (1, 1))]
    for size, black, low, solid in modes:
        p, t = run(rom_path, size, black, steps, low, solid)
        s = size + ["", " at once"][black] + (" low" if low else "") + (" solid" if solid else "")
        print(f"{s:18s} {len(t)} moves/turns: picture median {np.median(p) / 60:.2f} s, 90% under "
              f"{np.percentile(p, 90) / 60:.2f} s; whole step median {np.median(t) / 60:.2f} s, "
              f"90% under {np.percentile(t, 90) / 60:.2f} s")


if __name__ == "__main__":
    main()
