#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
A player for the whole campaign, driven by the Python model (no per-map
walkthrough): it explores every map, uses whatever can be used (terminals,
people, switches, doors), picks things up, fights what gets in the way, and
leaves by the exit that completes the sector once there is nothing new to
do. In the Junction it takes the passage to the first sector not yet done.
Door codes are typed in right (the model knows them). Used by the campaign
lockstep test and the demo recorder, so the ROM gets exactly the same
buttons.

Actions are the lockstep's: fwd back left right use pass wnext wprev heal,
plus 'close' for the vendor's menu (dialogs and codes are answered by the
caller).
Licence: GPL-2.0-or-later.
"""
from collections import deque, Counter

import game_model as gm

DIRS = gm.DIRS
PICKUP_TYPES = (gm.ET_PICKUP, gm.ET_INV, gm.ET_WEAPON, gm.ET_AMMO, gm.ET_AMMO2)  # entity types worth walking over


def turn_towards(cur, want):
    """The turn ("left" or "right") that brings facing `cur` closer to
    `want` (0 N, 1 E, 2 S, 3 W), or None if already there.
    """
    if cur == want:
        return None
    return "right" if (want - cur) % 4 == 1 else "left"


class Pilot:
    """Chooses the next action for the campaign's current map. Its memory:
      uses           how often each event was used in each situation (a
                     use that changed nothing is tried at most max_uses times
                     until the situation changes)
      blocked_doors  doors that would not open, and the situation then
      visited        cells it has stood on, per map (exploration goals)
      idle, wander   how long it has made no progress; when stuck, a while
                     of random goals
      door_try       the door it is trying to open
      humans, lines_at, avoid   per-step caches of blocking cells
    """
    def __init__(self, uses=2):
        """uses: how many times to try the same event in the same situation."""
        self.uses = Counter()          # (map, event, situation) -> times used
        self.max_uses = uses
        self.blocked_doors = {}        # (map, line) -> the situation it failed in
        self.visited = set()           # (map, x, z)
        self.cycles = 0
        self.idle = 0
        self.last_sig = None
        self.map = None
        self.wander = 0
        self.door_try = None
        self.humans, self.lines_at = set(), {}
        self.avoid = set()

    # ---------------------------------------------------------- the map
    def passable(self, w, x, z, doors=True):
        """True if the player can go to cell (x, z): on the map, not a wall, prop
        or person (or the cell to avoid), and, if there is a closed line, it
        is a door (only when `doors`) that is not locked, not a plain wall, and
        not one that failed to open in the current situation.
        """
        if not (0 <= x < 32 and 0 <= z < 32):
            return False
        if w.wall[z * 32 + x] or w.static_at(x, z):
            return False
        if (x, z) in self.humans or (x, z) in self.avoid:
            return False
        li = self.lines_at.get((x, z))
        if li is not None:
            l = w.lines[li]
            if not doors or l["locked"] or l["eType"] == 9:
                return False
            if (self.map, li) in self.blocked_doors and self.blocked_doors[(self.map, li)] == self.situation(w):
                return False
        return True

    # step direction -> (the edge of the cell left, the edge of the cell entered)
    EDGE = {(1, 0): ("E", "W"), (-1, 0): ("W", "E"), (0, 1): ("S", "N"), (0, -1): ("N", "S")}

    def edge_ok(self, w, c, n):
        """No invisible thin wall between neighbours c and n."""
        ec, en = self.EDGE[(n[0] - c[0], n[1] - c[1])]
        return not any(e == ec for _t, e in w.thin_at(*c)) and \
            not any(e == en for _t, e in w.thin_at(*n))

    def bfs(self, w, goals, start=None):
        """Breadth-first search from `start` (default: the player's cell) to the
        nearest cell of `goals` other than start, through passable cells and
        open edges. Returns the path (without start) or None.
        """
        start = start or (w.x, w.z)
        prev = {start: None}
        q = deque([start])
        while q:
            c = q.popleft()
            if c in goals and c != start:
                path = []
                while c != start:
                    path.append(c)
                    c = prev[c]
                return path[::-1]
            for dx, dz in DIRS:
                n = (c[0] + dx, c[1] + dz)
                if n not in prev and self.passable(w, *n) and self.edge_ok(w, c, n):
                    prev[n] = c
                    q.append(n)
        return None

    def reachable(self, w):
        """The set of cells reachable from the player's cell (including it)."""
        seen = {(w.x, w.z)}
        q = deque([(w.x, w.z)])
        while q:
            c = q.popleft()
            for dx, dz in DIRS:
                n = (c[0] + dx, c[1] + dz)
                if n not in seen and self.passable(w, *n) and self.edge_ok(w, c, n):
                    seen.add(n)
                    q.append(n)
        return seen

    def situation(self, w):
        """What makes trying the same thing again worthwhile.

        A tuple of the things events depend on: event states, credits, keys,
        the number of one-shot commands left, armour (in steps of 10), level,
        weapons, which lines are locked.
        """
        return (tuple(w.event_state), w.credits, w.keys, sum(map(sum, w.cmd_alive)), w.p.armor // 10, w.level,
                w.weapons,
                tuple(l["locked"] for l in w.lines))

    def door_situation(self, w):
        """situation() plus which lines are open. (Not used at present.)"""
        return self.situation(w), tuple(l["open"] for l in w.lines)

    # ---------------------------------------------------------- events
    def would_run(self, w, ev, flags=0x500):
        """The commands Game_runEvent would execute (without running them).

        A copy of the condition tests of game_model.World.run_event; flags
        0x500 is a "use" from the next cell.
        """
        e = w.g["events"][ev]
        if (e["flags"] & 1) and (flags & gm.EVF_MOVE):
            return []
        st = w.event_state[ev]
        n7 = 0 if st == 0 else (0x10000 << (st - 1))
        if w.keys & 8:
            flags |= 0x8000
        elif w.keys & 4:
            flags |= 0x4000
        elif w.keys & 1:
            flags |= 0x2000
        elif w.keys & 2:
            flags |= 0x1000
        n8 = flags & 0xF000
        out = []
        for k, c in enumerate(e["cmds"]):
            if not w.cmd_alive[ev][k]:
                continue
            a2 = c["a2"]
            if n7:
                if not a2 & n7:
                    continue
            elif a2 & 0x1FF0000:
                continue
            if (a2 & 0xF000) not in (0, n8):
                continue
            if not flags & a2:
                continue
            out.append(c)
        return out

    def exit_kind(self, c, w, ev):
        """None (not an exit), 'want' (the exit to take), 'no' (an exit to avoid).

        c: the Campaign; ev: an event of the current map. In the Junction the
        wanted exit leads to the next sector not yet done; elsewhere it is the
        one that completes the sector (CHANGEMAP's bit 31).
        """
        for cmd in w.g["events"][ev]["cmds"]:
            if cmd["op"] == 2:
                target = c.map_of(w.g["strings"][cmd["a1"] & 0xFF])
                name = c.info[c.index]["name"]
                if name == "junction":
                    return "want" if target == self.next_sector(c, w) else "no"
                return "want" if cmd["a1"] & 0x80000000 else "no"
        return None

    def next_sector(self, c, w):
        """The campaign index of the first sector map (level0x) not yet completed, or None."""
        for i, m in enumerate(c.info):
            if m["name"].startswith("level") and not w.completed & (1 << (m["id"] - 1)):
                return i
        return None

    # ---------------------------------------------------------- choosing
    def refresh(self, w):
        """Per-step caches (the model's lookups walk whole lists).

        humans: cells holding people; lines_at: cell -> first closed line.
        """
        self.humans = {(e["x"], e["z"]) for e in w.ents if e["def"] is not None and e["flags"] & gm.F_LINKED
                       and w.etype(e) == gm.ET_HUMAN}
        self.lines_at = {}
        for i, l in enumerate(w.lines):
            if not l["open"]:
                self.lines_at.setdefault((l["x"], l["z"]), i)

    def choose(self, c):
        """The next action for Campaign c: close an open vendor menu, heal below
        a third of health, else _choose's pick (weapon cycling going nowhere
        is cut short after 12 changes with a "pass").
        """
        w = c.world
        self.refresh(w)
        if self.map != c.index:
            self.map = c.index
            self.idle = 0
        self.visited.add((c.index, w.x, w.z))
        if w.store is not None:
            return "close"
        if w.p.hp < w.p.maxhp // 3 and any(w.inventory[:3]):
            return "heal"
        a = self._choose(c, w)
        self.cycles = self.cycles + 1 if a in ("wnext", "wprev") else 0
        if self.cycles > 12:
            self.cycles = 0
            return "pass"
        return a

    def usable_weapon(self, w):
        """True if the current weapon has enough ammo for one attack."""
        wi = gm.WEAPONS[w.weapon]
        return not (wi[6] and w.ammo[wi[5]] < wi[6])

    def enemy_ahead(self, w, maxd=4):
        """The distance (1..maxd) of the first monster straight ahead with nothing in between, or None."""
        fx, fz = DIRS[w.facing]
        x, z = w.x, w.z
        for d in range(1, maxd + 1):
            if not self.edge_ok(w, (x, z), (x + fx, z + fz)):
                return None
            x += fx
            z += fz
            if not self.passable(w, x, z, doors=False):
                return None
            for i in w.entities_at(x, z):
                if w.etype(w.ents[i]) == gm.ET_ENEMY:
                    return d
        return None

    def can_douse(self, w):
        """True if it has the fire extinguisher and halon."""
        return bool(w.weapons & 2) and w.ammo[0] >= 1

    def fire_fight(self, w):
        """A monster standing in a fire right ahead: shots stop at the fire,
        so put it out first (the extinguisher).

        Returns the action for that ("use" or a weapon change), or None.
        """
        fx, fz = DIRS[w.facing]
        if not self.edge_ok(w, (w.x, w.z), (w.x + fx, w.z + fz)) or not self.can_douse(w):
            return None
        here = [w.etype(w.ents[i]) for i in w.entities_at(w.x + fx, w.z + fz)]
        if gm.ET_FIRE not in here or gm.ET_ENEMY not in here:
            return None
        if w.weapon == 1:
            return "use"
        return "wprev" if w.weapon > 1 else "wnext"

    def _choose(self, c, w):
        """The decision proper, in order: put out a fire with a monster in it;
        change from an empty weapon; fight a monster ahead; with weapons put
        away, collar an adjacent dog; face and hit an adjacent monster; else
        go after pick_goal's goal (walk to a cell, or stand next to a cell and
        use it).
        """
        if self.door_try and self.door_try[0] == c.index and w.lines[self.door_try[1]]["open"]:
            self.door_try = None          # it opened
        fire_fight = self.fire_fight(w)
        if fire_fight:
            return fire_fight
        if w.weapons and not self.usable_weapon(w):
            return "wnext"
        d = self.enemy_ahead(w)
        if d is not None and w.weapons:
            if w.weapon in (0, 1) and any(w.ammo[gm.WEAPONS[k][5]] >= gm.WEAPONS[k][6]
                                          for k in range(2, 12) if w.weapons & (1 << k)):
                return "wnext"
            t = w.select_target()
            if t and t[0] == "ent" and (d == 1 or (d <= 3 and w.weapon not in (0, 1))):
                return "use"
        if not w.weapons and w.inventory[4]:      # weapons put away: a dog to fight for us
            for k, (dx, dz) in enumerate(DIRS):
                if not self.edge_ok(w, (w.x, w.z), (w.x + dx, w.z + dz)):
                    continue
                for i in w.entities_at(w.x + dx, w.z + dz):
                    d = w.edef(w.ents[i])
                    if d["eType"] == gm.ET_ENEMY and d["eSub"] == 1:
                        return turn_towards(w.facing, k) or "collar"
        for dx, dz in DIRS:      # face an adjacent monster
            if not w.weapons or w.line_at(w.x + dx, w.z + dz) is not None \
                    or not self.edge_ok(w, (w.x, w.z), (w.x + dx, w.z + dz)):
                continue
            here = w.entities_at(w.x + dx, w.z + dz)
            if any(w.etype(w.ents[i]) == gm.ET_FIRE for i in here) and not self.can_douse(w):
                continue                  # a monster in a fire, and no halon: not this one
            for i in here:
                if w.etype(w.ents[i]) == gm.ET_ENEMY:
                    t = turn_towards(w.facing, DIRS.index((dx, dz)))
                    if t:
                        return t
                    if w.select_target() == ("ent", i) and self.usable_weapon(w):
                        return "use"
        sig = (w.x, w.z, w.facing, self.situation(w), len(w.active))  # nothing changed since last step: idle
        self.idle = self.idle + 1 if sig == self.last_sig else 0
        self.last_sig = sig
        goal = self.pick_goal(c, w)
        if goal is None:
            return "pass"
        kind, cell, face = goal
        if kind == "walk":
            path = self.bfs(w, {cell})
            return self.step_along(c, w, path) if path else "pass"
        # use the cell `cell` from a neighbour
        spots = {(cell[0] - dx, cell[1] - dz): k for k, (dx, dz) in enumerate(DIRS)}
        if (w.x, w.z) in spots:
            t = turn_towards(w.facing, spots[(w.x, w.z)])
            if t:
                return t
            ev = w.find_event(*cell)
            if ev is not None:
                self.uses[(c.index, ev, self.situation(w))] += 1
            elif any(w.etype(w.ents[i]) == gm.ET_DESTRUCT for i in w.entities_at(*cell)) \
                    and not w.select_target():
                return "wnext"
            return "use"
        path = self.bfs(w, set(s for s in spots if self.passable(w, *s, doors=False)))
        return self.step_along(c, w, path) if path else "pass"

    def pick_goal(self, c, w):
        """The next goal: ("walk", cell, None) or ("use", cell, None), or None.

        Candidates, scored by walking distance (lowest wins): events that
        would run and have not been tried too often in this situation, items
        it wants, the Reactor's power couplings (first, -100), and cells not
        yet visited (+2). The exit to take is chosen only when nothing else is
        left. When stuck (idle > 30) it picks random candidates for 20 steps.
        """
        self.refresh(w)
        reach = self.reachable(w)
        sit = self.situation(w)
        dist = self.distances(w)
        cands = []
        exits = []
        for ev, e in enumerate(w.g["events"]):
            cell = (e["x"], e["z"])
            if not self.would_run(w, ev):
                continue
            if self.uses[(c.index, ev, sit)] >= self.max_uses:
                continue
            # usable from a reachable neighbour
            spots = [(cell[0] - dx, cell[1] - dz) for dx, dz in DIRS]
            ds = [dist[s] for s in spots if s in dist and self.passable(w, *s, doors=False)]
            if not ds:
                continue
            kind = self.exit_kind(c, w, ev)
            if kind == "no":
                continue
            if kind == "want":
                exits.append((min(ds), ("use", cell, None)))
                continue
            cands.append((min(ds), ("use", cell, None)))
        for i, e in enumerate(w.ents):
            if e["def"] is None or not e["flags"] & gm.F_LINKED:
                continue
            d = w.edef(e)
            if d["eType"] in PICKUP_TYPES and (e["x"], e["z"]) in dist:
                if self.wants(w, d):
                    cands.append((dist[(e["x"], e["z"])], ("walk", (e["x"], e["z"]), None)))
            # the Reactor's power couplings, once the portal makes them targets
            if (d["eType"], d["eSub"]) == (gm.ET_DESTRUCT, 4) and e["flags"] & gm.F_ALIVE and w.weapons:
                spots = [(e["x"] - dx, e["z"] - dz) for dx, dz in DIRS]
                ds = [dist[s] for s in spots if s in dist and self.passable(w, *s, doors=False)]
                if ds:
                    cands.append((min(ds) - 100, ("use", (e["x"], e["z"]), None)))
        occupied = {(e["x"], e["z"]) for e in w.ents if e["def"] is not None and e["flags"] & gm.F_LINKED
                    and w.etype(e) in (gm.ET_HUMAN, gm.ET_ENEMY, gm.ET_DESTRUCT)}
        for cell in reach:
            if (c.index,) + cell not in self.visited and cell in dist and cell not in occupied:
                cands.append((dist[cell] + 2, ("walk", cell, None)))
        if self.idle > 30:                 # stuck: try something else for a while
            self.idle = 0
            self.wander = 20
        if self.wander:
            self.wander -= 1
            import random
            r = random.Random(len(w.messages) + w.x * 7 + w.z)  # seeded from the state, so a run is repeatable
            pool = cands or exits
            if pool:
                return r.choice(pool)[1]
        if cands:
            return min(cands, key=lambda t: t[0])[1]
        if exits:
            return min(exits, key=lambda t: t[0])[1]
        return None

    def wants(self, w, d):
        """False for a pickup it cannot use now (health, armour, an item or ammo already full)."""
        et, es, parm = d["eType"], d["eSub"], d["parm"]
        if et == gm.ET_PICKUP and es == 21:
            return w.p.armor < w.p.maxarmor
        if et == gm.ET_PICKUP and es == 20:
            return w.p.hp < w.p.maxhp
        if et == gm.ET_INV:
            return w.inventory[es - 25] < 99
        if et in (gm.ET_AMMO, gm.ET_AMMO2):
            return w.ammo[es] < 99
        return True

    def distances(self, w):
        """Walking distance (in steps) from the player's cell to every reachable cell: {cell: steps}."""
        dist = {(w.x, w.z): 0}
        q = deque([(w.x, w.z)])
        while q:
            cx, cz = q.popleft()
            for dx, dz in DIRS:
                n = (cx + dx, cz + dz)
                if n not in dist and self.passable(w, *n) and self.edge_ok(w, (cx, cz), n):
                    dist[n] = dist[(cx, cz)] + 1
                    q.append(n)
        return dist

    def step_along(self, c, w, path):
        """One action along `path`: turn towards the next cell; at a door, use it
        (once per situation: a door that does not open is marked blocked) or
        chop a jammed one with the axe; put out a fire or walk through it;
        break a destructible; go round (or back away from) a person or monster
        in the way; else step forward.
        """
        nx, nz = path[0]
        want = DIRS.index((nx - w.x, nz - w.z))
        t = turn_towards(w.facing, want)
        if t:
            return t
        li = w.line_at(nx, nz)
        if li is not None:
            l = w.lines[li]
            if l["eType"] == gm.ET_DESTRUCT:
                if not w.weapons & 1:            # no axe (or weapons put away): not now
                    self.blocked_doors[(c.index, li)] = self.situation(w)
                    return "pass"
                if w.weapon != 0:
                    return "wnext"               # the axe for a jammed-shut door
                return "use"
            ev = w.find_event(nx, nz)
            sit = self.situation(w)
            if ev is None or not self.would_run(w, ev) or self.door_try == (c.index, li, sit) \
                    or self.exit_kind(c, w, ev) == "no":
                self.blocked_doors[(c.index, li)] = sit       # it did not open: not now
                return "pass"
            self.door_try = (c.index, li, sit)
            return "use"
        for i in w.entities_at(nx, nz):
            et = w.etype(w.ents[i])
            if et == gm.ET_FIRE:
                if w.weapon == 1:
                    return "use"
                if w.weapons & 2 and w.ammo[0] >= 1:
                    return "wnext"               # get the fire extinguisher out
                return "fwd"                     # no halon: through it
            if et == gm.ET_DESTRUCT:
                t = w.select_target()
                return "use" if t else "wnext"
            if et in (gm.ET_HUMAN, gm.ET_ENEMY):
                # in the way and nothing to fight it with: around it, or back off
                self.avoid = {(nx, nz)}
                alt = self.bfs(w, {path[-1]})
                self.avoid = set()
                if alt and alt[0] != (nx, nz):
                    want = DIRS.index((alt[0][0] - w.x, alt[0][1] - w.z))
                    return turn_towards(w.facing, want) or "fwd"
                bx, bz = w.x - DIRS[w.facing][0], w.z - DIRS[w.facing][1]
                return "back" if not w.blocked(w.x, w.z, bx, bz, gm.MASK_PLAYER_MOVE) else "pass"
        return "fwd"


# action name -> what it does to a game_model.World
ACTIONS = {"fwd": lambda w: w.move(False), "back": lambda w: w.move(True), "left": lambda w: w.turn(-1),
           "right": lambda w: w.turn(1), "use": lambda w: w.use(), "pass": lambda w: w.pass_turn(),
           "wnext": lambda w: w.cycle_weapon(1), "wprev": lambda w: w.cycle_weapon(-1),
           "heal": lambda w: w.use_health_item(), "close": lambda w: w.store_close(),
           "berserk": lambda w: w.use_item(3), "collar": lambda w: w.use_item(4)}


def run(steps=200000, god=True, verbose=True, start_map=0):
    """Plays the campaign on the model. -> (campaign, [actions]).

    steps: the most actions to take; god: keep the player's health at 200
    (it changes the model's state, so a ROM played alongside must not use
    it); start_map: the campaign index to begin on. Dialogs are recorded
    as ("dialog", None) and codes as ("code", code).
    """
    c = gm.Campaign(start_map=start_map)
    p = Pilot()
    acts = []
    last = None
    for step in range(steps):
        w = c.world
        if c.finished or w.dead:
            break
        if god and w.p.hp < 100:
            w.p.hp = w.p.maxhp = 200
        if w.dialog is not None:
            acts.append(("dialog", None))
            w.resume()
        elif w.password is not None:
            acts.append(("code", w.password[0]))
            w.resume(w.password[0])
        else:
            a = p.choose(c)
            acts.append((a, None))
            ACTIONS[a](w)
        if c.advance():
            if verbose:
                print(f"step {step}: -> {'THE END' if c.finished else c.info[c.index]['name']}"
                      f"{' stats ' + str(c.stats) if c.stats else ''}  level {w.level} credits {w.credits}",
                      flush=True)
        if verbose and step % 2000 == 0 and step:
            print(f"  step {step} map {c.info[c.index]['name']} pos {(w.x, w.z)} xp lvl {w.level} "
                  f"msgs {w.messages[-2:]}", flush=True)
    return c, acts


if __name__ == "__main__":
    import sys
    c, acts = run(int(sys.argv[1]) if len(sys.argv) > 1 else 100000,
                  start_map=int(sys.argv[2]) if len(sys.argv) > 2 else 0)
    print("finished" if c.finished else f"stopped in {c.info[c.index]['name']}", len(acts), "actions")
