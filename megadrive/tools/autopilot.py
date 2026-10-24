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
"""
from collections import deque

import game_model as gm

DIRS = gm.DIRS

# Sector 1's solution, in order. The pilot stands next to each cell, faces it
# and presses "use" the given number of times (dialogs in between are
# answered by the caller).
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
    """True if the player could stand on cell (x, z) of world w (a
    game_model.World): on the map, not a wall or blocking prop, and no
    closed line that is locked or a plain wall (eType 9). Closed doors
    count as passable: walking into one opens it.
    """
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
    """Breadth-first search over passable cells from `start` to the nearest
    cell in `goals`. Returns the path as a list of cells (without start,
    ending at the goal), or None if no goal can be reached.
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
    """The turn ("left" or "right") that brings facing `cur` closer to
    `want` (0 N, 1 E, 2 S, 3 W), or None if already there. An about-turn
    turns left.
    """
    if cur == want:
        return None
    return "right" if (want - cur) % 4 == 1 else "left"


class Autopilot:
    """Chooses the next action for the player of a game_model.World, working
    through `goals` (copies of WALKTHROUGH entries, the use counts going
    down as it goes). Actions are the lockstep's names: fwd back left
    right use pass wnext wprev heal.
    """
    def __init__(self, goals=WALKTHROUGH):
        """`goals`: the list of (cell, uses, note) to work through; `stuck` counts failed path searches, `cycles` weapon changes in a row."""
        self.goals = [list(g) for g in goals]
        self.stuck = 0
        self.cycles = 0

    def done(self):
        """True once every goal has been done."""
        return not self.goals

    def enemy_ahead(self, w, maxd=4):
        """The distance (1..maxd) of the first monster straight ahead with nothing in between, or None."""
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
        # heal when low
        """The next action for world w: heal when low on health, else _choose's
        pick; more than 12 weapon changes in a row are cut short with a "pass".
        """
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

        Returns the set of cells holding credits (eSub 22, 23) on the grid;
        the caller searches for the nearest.
        """
        cells = set()
        for e in w.ents:
            if e["def"] is not None and e["flags"] & gm.F_LINKED and w.edef(e)["eType"] == gm.ET_PICKUP \
                    and w.edef(e)["eSub"] in (22, 23):
                cells.add((e["x"], e["z"]))
        return cells

    def _choose(self, w):
        """The decision proper: switch weapon when out of ammo; fight a monster
        ahead (switching to a gun if it has one); turn to face an adjacent
        monster; collect credits when the goal needs them; else walk to the
        current goal and use it (popping it when its uses are done; dropping
        it after 4 failed searches).
        """
        wi = gm.WEAPONS[w.weapon]
        if wi[6] and w.ammo[wi[5]] < wi[6]:
            return "wnext"               # out of ammo for this one
        # fight: anything adjacent first, or in range of a gun
        d = self.enemy_ahead(w)
        if d is not None:
            if w.weapon in (0, 1) and any(w.ammo[gm.WEAPONS[k][5]] >= gm.WEAPONS[k][6]
                                          for k in range(2, 9) if w.weapons & (1 << k)):
                return "wnext"
            if w.weapon == 1:
                return "wprev"
            t = w.select_target()
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
        """One action along `path`: turn towards the next cell, open (use) a
        door there or chop a jammed one with the axe, put out a fire with the
        extinguisher, break a destructible, wait for a person or a monster in
        the way, else step forward.
        """
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

    max_tries: deaths allowed before giving up (returns None, or with
    `partial` the actions of the furthest progress). seed: the detours'
    random generator. (Defined after the __main__ block; it is meant to be
    imported.)
    """
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
            if fails >= 25 and len(stack) > 1:  # this checkpoint keeps failing: go back one further
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
        if rnd.random() < 0.3 * min(1, stack[-1][3]):  # detours only after a death at this checkpoint
            a = rnd.choice(["pass", "back", "back", "back", "left", "right", "heal", "fwd"])
        else:
            a = ap.choose(w)
        actmap[a](w)
        acts.append((a, None))
        since += 1
        calm = not any(abs(w.ents[i]["x"] - w.x) + abs(w.ents[i]["z"] - w.z) <= 3 for i in w.active)  # no active monster within 3 cells
        # new checkpoint: a goal done, or 12 actions on while calm and at least half health
        if not w.dead and ((len(ap.goals) < before) or (since >= 12 and calm and w.p.hp * 2 >= w.p.maxhp)):
            stack.append((copy.deepcopy(w), copy.deepcopy(ap), len(acts), 0))
            since = 0
            if len(ap.goals) < best[0]:
                best = (len(ap.goals), list(acts))
            if verbose and len(ap.goals) < before:
                print(f"  goal done after {len(acts)} actions (retries {tries}), hp {w.p.hp}/{w.p.maxhp}, "
                      f"level {w.level}, {len(ap.goals)} goals left", flush=True)
    return acts
