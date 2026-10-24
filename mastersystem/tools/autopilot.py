#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
A simple player for Sector 1, driven by the Python model's view of the
world: walks to a list of goals (things to "use": terminals, people, door
panels), opening doors on the way, fighting what gets in front of it and
healing when low. Used by the lockstep test and the video recorder; the
ROM gets exactly the same button presses.

The goal list is the sector's intended solution: the terminal sequence that
unlocks the main corridor, the civilian who sells the north door code, the
e-mail with the security-room code, then the exit.

Actions are the names the tests map to buttons: fwd back left right use
pass wnext wprev heal. The pilot only reads the model (game_model.World);
the caller applies each action to the model and presses the same buttons
on the ROM. plan_walkthrough() (at the end of the file, after the script
block) searches for a whole surviving action list, for test_lockstep.py
--plan. Run as a script it plays the walkthrough on the model alone and
prints progress.
"""
from collections import deque

import game_model as gm

DIRS = gm.DIRS                  # (dx, dz) per facing N, E, S, W

# (cell to use, how many times, note)
WALKTHROUGH = [
    ((6, 21), 1, "terminal: unlocks the east door of the south rooms"),
    ((14, 21), 1, "terminal"),
    ((10, 9), 1, "terminal"),
    ((14, 9), 1, "terminal"),
    ((10, 21), 1, "terminal"),
    ((6, 9), 1, "terminal: opens the west wing"),
    ((1, 19), 1, "Dr. Guerard"),
    ((3, 21), 3, "the civilian"),
    ((1, 19), 1, "Dr. Guerard"),
    ((3, 21), 2, "the civilian, now with 10 credits"),
    ((4, 4), 1, "north door code 225"),
    ((9, 5), 4, "e-mail terminal: birthday 10/10"),
    ((16, 5), 1, "security door override: 1010"),
    ((26, 2), 1, "exit"),
]


def passable(w, x, z):
    """Can the player walk into cell (x, z)? Not off the map, a wall, a static
    blocker, or a shut line that is locked or a plain wall (eType 9).
    Shut unlocked doors count as passable: the pilot opens them."""
    if not (0 <= x < 32 and 0 <= z < 32):
        return False
    if w.wall[z * 32 + x] or w.static[z * 32 + x]:
        return False
    for l in w.lines:
        if (l["x"], l["z"]) == (x, z) and not l["open"]:
            if l["locked"] or l["eType"] == 9:
                return False
    return True


def bfs(w, start, goals):
    """Shortest 4-way path from cell start to any cell in goals over passable cells.

    Returns the cells to step through (start excluded, the goal last),
    [] if start is a goal, or None if no goal is reachable.
    """
    prev = {start: None}
    q = deque([start])
    while q:
        c = q.popleft()
        if c in goals:
            path = []
            while c != start:
                path.append(c)
                c = prev[c]
            return path[::-1]
        for dx, dz in DIRS:
            n = (c[0] + dx, c[1] + dz)
            if n not in prev and passable(w, *n):
                prev[n] = c
                q.append(n)
    return None


def turn_towards(cur, want):
    """The turn ("left"/"right") from facing cur towards facing want, or None if already facing it."""
    if cur == want:
        return None
    return "right" if (want - cur) % 4 == 1 else "left"


class Autopilot:
    """The walkthrough player: call choose(w) for each action."""

    def __init__(self, goals=WALKTHROUGH):
        """Start with a copy of goals ([cell, times to use, note]); counters for being stuck and weapon cycling."""
        self.goals = [list(g) for g in goals]
        self.stuck = 0
        self.cycles = 0

    def done(self):
        """True when every goal has been used."""
        return not self.goals

    def enemy_ahead(self, w, maxd=4):
        """Distance (1..maxd) to the first monster straight ahead, or None.

        The look stops at anything impassable or any line cell.
        """
        fx, fz = DIRS[w.facing]
        x, z = w.x, w.z
        for d in range(1, maxd + 1):
            x += fx
            z += fz
            if not passable(w, x, z) or w.line_at(x, z) is not None:
                return None
            for i in w.entities_at(x, z):
                if w.etype(w.ents[i]) == gm.ET_ENEMY:
                    return d
        return None

    def choose(self, w):
        """The next action for model w: heal when low, else _choose's pick,
        breaking out of endless weapon cycling with a "pass"."""
        # heal when low
        if w.p.hp < 12 and any(w.inventory[:3]):
            return "heal"
        a = self._choose(w)
        self.cycles = self.cycles + 1 if a in ("wnext", "wprev") else 0
        if self.cycles > 12:              # weapon cycling going nowhere
            self.cycles = 0
            return "pass"
        return a

    def credits_goal(self, w):
        """nearest reachable credit pickup, while we need 10 for the civilian

        Returns the set of cells holding a credit pickup still in the world
        (pickup eSub 22 or 23); the caller's bfs finds the nearest."""
        cells = set()
        for e in w.ents:
            if e["def"] is not None and e["flags"] & gm.F_LINKED and w.edef(e)["eType"] == gm.ET_PICKUP \
                    and w.edef(e)["eSub"] in (22, 23):
                cells.add((e["x"], e["z"]))
        return cells

    def _choose(self, w):
        """The action logic: switch weapon when out of ammo, fight what is
        ahead or adjacent, else walk to (or use) the current goal. A goal
        that cannot be reached for 4 tries is dropped."""
        wi = gm.WEAPONS[w.weapon]
        # WEAPONS columns: 5 ammo type, 6 ammo used per attack
        if wi[6] and w.ammo[wi[5]] < wi[6]:
            return "wnext"               # out of ammo for this one
        # fight: anything adjacent first, or in range of a gun
        d = self.enemy_ahead(w)
        if d is not None:
            # axe or extinguisher in hand but a gun with ammo: switch
            if w.weapon in (0, 1) and any(w.ammo[gm.WEAPONS[k][5]] >= gm.WEAPONS[k][6]
                                          for k in range(2, 9) if w.weapons & (1 << k)):
                return "wnext"
            if w.weapon == 1:
                return "wprev"
            t = w.select_target()        # what "use" would hit now
            if t and t[0] == "ent" and (d == 1 or (d <= 3 and w.weapon not in (0, 1))):
                return "use"
        for dx, dz in DIRS:      # turn to face an adjacent monster we can hit
            if w.line_at(w.x + dx, w.z + dz) is not None:
                continue
            for i in w.entities_at(w.x + dx, w.z + dz):
                if w.etype(w.ents[i]) == gm.ET_ENEMY:
                    t = turn_towards(w.facing, DIRS.index((dx, dz)))
                    if t:
                        return t
        if not self.goals:
            return "pass"
        (tx, tz), times, note = self.goals[0]
        if "10 credits" in note and w.credits < 10:
            cells = self.credits_goal(w)
            path = bfs(w, (w.x, w.z), cells) if cells else None
            if path:
                return self.step_along(w, path)
        # stand next to the goal, facing it
        spots = {(tx - dx, tz - dz): k for k, (dx, dz) in enumerate(DIRS)}
        if (w.x, w.z) in spots:
            t = turn_towards(w.facing, spots[(w.x, w.z)])
            if t:
                return t
            self.goals[0][1] -= 1
            if self.goals[0][1] <= 0:
                self.goals.pop(0)
            return "use"
        path = bfs(w, (w.x, w.z), set(c for c in spots if passable(w, *c)))
        if not path:
            self.stuck += 1
            if self.stuck > 3:
                self.goals.pop(0)
                self.stuck = 0
            return "pass"
        return self.step_along(w, path)

    def step_along(self, w, path):
        """The action that moves towards path[0]: turn, open or break what is
        in the way, wait for a person or monster, or step forward."""
        nx, nz = path[0]
        want = DIRS.index((nx - w.x, nz - w.z))
        t = turn_towards(w.facing, want)
        if t:
            return t
        li = w.line_at(nx, nz)
        if li is not None:
            if w.lines[li]["eType"] == gm.ET_DESTRUCT and w.weapon != 0:
                return "wprev"           # jammed door: get the axe out
            return "use"                 # open the door / chop the jammed one
        for i in w.entities_at(nx, nz):
            et = w.etype(w.ents[i])
            if et in (gm.ET_DESTRUCT, gm.ET_FIRE):
                if et == gm.ET_FIRE and w.weapon != 1 and w.weapons & 2:
                    return "wprev" if w.weapon > 1 else "wnext"
                return "use"
            if et == gm.ET_HUMAN or et == gm.ET_ENEMY:
                return "pass"
        return "fwd"


if __name__ == "__main__":
    # model-only dry run of the walkthrough
    import sys
    w = gm.load()
    ap = Autopilot()
    acts = {"fwd": lambda: w.move(False), "back": lambda: w.move(True), "left": lambda: w.turn(-1),
            "right": lambda: w.turn(1), "use": w.use, "pass": w.pass_turn,
            "wnext": lambda: w.cycle_weapon(1), "wprev": lambda: w.cycle_weapon(-1), "heal": w.use_health_item}
    for step in range(int(sys.argv[1]) if len(sys.argv) > 1 else 3000):
        if w.dead or w.level_done:
            break
        if w.dialog is not None:
            w.resume()
            continue
        if w.password is not None:
            w.resume(w.password[0])
            continue
        g = ap.goals[0][2] if ap.goals else "-"
        a = ap.choose(w)
        acts[a]()
        if step % 50 == 0 or a == "use":
            print(step, a, (w.x, w.z, w.facing), "hp", w.p.hp, "lvl", w.level, "cr", w.credits, "goal:", g,
                  w.messages[-1:] if w.messages else "")
    print("dead" if w.dead else "DONE" if w.level_done else "unfinished", "level", w.level, "hp", w.p.hp)


def plan_walkthrough(max_tries=3000, seed=1, verbose=True, partial=False):
    """Finds a list of actions that gets through the sector alive, by running
    the autopilot on the model with occasional random detours, keeping a
    stack of checkpoints (deep copies of the world) and going back to one
    whenever the player dies. Returns [(action, extra)]: extra is the string
    id for 'dialog' and the code for 'code'.

    max_tries: deaths (or over-long runs) allowed before giving up, then
    None, or with partial the actions that reached the most goals. seed
    seeds the detours; verbose prints each goal reached. A checkpoint that
    has failed 25 times is dropped for the one before it. Detours get more
    likely the more often the current checkpoint has failed."""
    import copy
    import random
    rnd = random.Random(seed)
    w = gm.load()
    ap = Autopilot()
    acts = []
    stack = [(copy.deepcopy(w), copy.deepcopy(ap), 0, 0)]   # world, pilot, n actions, failures
    tries = 0
    since = 0
    best = (len(ap.goals), [])      # fewest goals left, actions to get there
    actmap = {"fwd": lambda w: w.move(False), "back": lambda w: w.move(True), "left": lambda w: w.turn(-1),
              "right": lambda w: w.turn(1), "use": lambda w: w.use(), "pass": lambda w: w.pass_turn(),
              "wnext": lambda w: w.cycle_weapon(1), "wprev": lambda w: w.cycle_weapon(-1),
              "heal": lambda w: w.use_health_item()}
    while not w.level_done:
        if w.dead or len(acts) > 8000:
            tries += 1
            if tries > max_tries:
                return best[1] if partial else None
            cw, cap, n, fails = stack[-1]
            if fails >= 25 and len(stack) > 1:
                stack.pop()
                cw, cap, n, fails = stack[-1]
            stack[-1] = (cw, cap, n, fails + 1)
            w, ap = copy.deepcopy(cw), copy.deepcopy(cap)
            del acts[n:]
            since = 0
            continue
        if w.dialog is not None:
            acts.append(("dialog", w.g["strings"].index(w.dialog)))
            w.resume()
            continue
        if w.password is not None:
            acts.append(("code", w.password[0]))
            w.resume(w.password[0])
            continue
        before = len(ap.goals)
        if rnd.random() < 0.3 * min(1, stack[-1][3]):
            a = rnd.choice(["pass", "back", "back", "back", "left", "right", "heal", "fwd"])
        else:
            a = ap.choose(w)
        actmap[a](w)
        acts.append((a, None))
        since += 1
        # checkpoint after a goal, or after 12 quiet actions with HP at half or more
        calm = not any(abs(w.ents[i]["x"] - w.x) + abs(w.ents[i]["z"] - w.z) <= 3 for i in w.active)
        if not w.dead and ((len(ap.goals) < before) or (since >= 12 and calm and w.p.hp * 2 >= w.p.maxhp)):
            stack.append((copy.deepcopy(w), copy.deepcopy(ap), len(acts), 0))
            since = 0
            if len(ap.goals) < best[0]:
                best = (len(ap.goals), list(acts))
            if verbose and len(ap.goals) < before:
                print(f"  goal done after {len(acts)} actions (retries {tries}), hp {w.p.hp}/{w.p.maxhp}, "
                      f"level {w.level}, {len(ap.goals)} goals left", flush=True)
    return acts
