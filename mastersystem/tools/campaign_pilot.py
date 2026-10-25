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
caller), 'collar' (use the collar item) and 'berserk' (ACTIONS has
both; choose() only returns 'collar').
Licence: GPL-2.0-or-later.

This is the Mega Drive port's file, copied unchanged. Here it runs through
sms_pilot.py, which turns this port's game_model.World into a one-map
"campaign" (c.world, c.index, c.info, c.advance) and stubs the methods the
Master System's rules lack (static_at, thin_at, store, use_item...). run()
and Campaign are only meaningful with the Mega Drive model.

How it chooses: each step it refreshes small caches, deals with danger
(switch to a weapon with ammo, attack a monster ahead or beside), then
picks a goal with pick_goal(): the nearest event it could still trigger,
pickup it wants, or unvisited cell, else the exit. A "situation" is a
summary of the progress state; a use or door that failed is not retried
until the situation changes.
"""
from collections import deque, Counter

import game_model as gm

DIRS = gm.DIRS                  # (dx, dz) per facing N, E, S, W
PICKUP_TYPES = (gm.ET_PICKUP, gm.ET_INV, gm.ET_WEAPON, gm.ET_AMMO, gm.ET_AMMO2)


def turn_towards(cur, want):
    """The turn ("left"/"right") from facing cur towards facing want, or None if already facing it."""
    if cur == want:
        return None
    return "right" if (want - cur) % 4 == 1 else "left"


class Pilot:
    """The exploring player; choose(c) returns the next action for campaign c."""

    def __init__(self, uses=2):
        """uses: how many times one event is tried in one situation."""
        self.uses = Counter()          # (map, event, situation) -> times used
        self.max_uses = uses
        self.blocked_doors = {}        # (map, line) -> the situation it failed in
        self.visited = set()           # (map, x, z)
        self.cycles = 0                # weapon switches in a row
        self.idle = 0                  # steps with nothing changing
        self.last_sig = None           # last step's (position, situation) signature
        self.map = None                # the map index the counters belong to
        self.wander = 0                # steps left of random goals (after being stuck)
        self.door_try = None           # (map, line, situation) of the door being opened
        self.humans, self.lines_at = set(), {}   # per-step caches (refresh)
        self.avoid = set()             # cells bfs must avoid (routing round a blocker)

    # ---------------------------------------------------------- the map
    def passable(self, w, x, z, doors=True):
        """Can the pilot walk into cell (x, z)? Not off the map, a wall, a
        static blocker, a person, an avoided cell, or a shut line that is
        locked, a plain wall (eType 9), failed in this situation, or any
        shut line at all when doors is False."""
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

    # step (dx, dz) -> (the edge it leaves through, the edge it enters by)
    EDGE = {(1, 0): ("E", "W"), (-1, 0): ("W", "E"), (0, 1): ("S", "N"), (0, -1): ("N", "S")}

    def edge_ok(self, w, c, n):
        """No invisible thin wall between neighbours c and n."""
        ec, en = self.EDGE[(n[0] - c[0], n[1] - c[1])]
        return not any(e == ec for _t, e in w.thin_at(*c)) and \
            not any(e == en for _t, e in w.thin_at(*n))

    def bfs(self, w, goals, start=None):
        """Shortest path from start (default the player's cell) to a cell in goals, start excluded.

        Returns the cells to step through, or None.
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
        """Every cell the pilot can walk to from the player's cell."""
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
        """What makes trying the same thing again worthwhile."""
        return (tuple(w.event_state), w.credits, w.keys, sum(map(sum, w.cmd_alive)), w.p.armor // 10, w.level,
                w.weapons,
                tuple(l["locked"] for l in w.lines))

    def door_situation(self, w):
        """The situation plus which lines are open (not used)."""
        return self.situation(w), tuple(l["open"] for l in w.lines)

    # ---------------------------------------------------------- events
    def would_run(self, w, ev, flags=0x500):
        """The commands Game_runEvent would execute (without running them).

        ev is an event index; flags the trigger flags (default 0x500: a use,
        EVF_USE, with bit 0x400 set as well). A command runs if it is still
        alive, matches the event's state (bits 16-24 of a2: one bit per
        state above 0, none for state 0), matches the key held (bits
        12-15, from the strongest key) and shares a bit with flags.
        """
        e = w.g["events"][ev]
        if (e["flags"] & 1) and (flags & gm.EVF_MOVE):
            return []
        st = w.event_state[ev]
        n7 = 0 if st == 0 else (0x10000 << (st - 1))   # the state's bit in a2
        # the key held, as a flag bit: keys bit 3, 2, 0, 1 -> 0x8000..0x1000
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

        An exit is an event with a CHANGEMAP command (op 2). In the Junction
        the wanted one leads to next_sector(); elsewhere it is the one whose
        argument has bit 31 set (the exit that completes the map).
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
        """Index of the first "level*" map not yet completed, or None."""
        for i, m in enumerate(c.info):
            if m["name"].startswith("level") and not w.completed & (1 << (m["id"] - 1)):
                return i
        return None

    # ---------------------------------------------------------- choosing
    def refresh(self, w):
        """Per-step caches (the model's lookups walk whole lists)."""
        self.humans = {(e["x"], e["z"]) for e in w.ents if e["def"] is not None and e["flags"] & gm.F_LINKED
                       and w.etype(e) == gm.ET_HUMAN}
        self.lines_at = {}
        for i, l in enumerate(w.lines):
            if not l["open"]:
                self.lines_at.setdefault((l["x"], l["z"]), i)

    def choose(self, c):
        """The next action for campaign c: close a store, heal below a third
        of max HP, else _choose's pick (breaking endless weapon cycling)."""
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
        """True if the weapon in hand has the ammo for one attack."""
        wi = gm.WEAPONS[w.weapon]
        return not (wi[6] and w.ammo[wi[5]] < wi[6])

    def enemy_ahead(self, w, maxd=4):
        """Distance (1..maxd) to the first monster straight ahead, or None
        (the look stops at thin walls, shut lines and impassable cells)."""
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

    def _choose(self, c, w):
        """The action logic: weapons and fighting first, then the goal from
        pick_goal(): walk to it, or stand beside it, face it and use it."""
        if self.door_try and self.door_try[0] == c.index and w.lines[self.door_try[1]]["open"]:
            self.door_try = None          # it opened
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
            for i in w.entities_at(w.x + dx, w.z + dz):
                if w.etype(w.ents[i]) == gm.ET_ENEMY:
                    t = turn_towards(w.facing, DIRS.index((dx, dz)))
                    if t:
                        return t
                    if w.select_target() == ("ent", i) and self.usable_weapon(w):
                        return "use"
        # count steps where nothing changes (to notice being stuck)
        sig = (w.x, w.z, w.facing, self.situation(w), len(w.active))
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
            return "use"
        path = self.bfs(w, set(s for s in spots if self.passable(w, *s, doors=False)))
        return self.step_along(c, w, path) if path else "pass"

    def pick_goal(self, c, w):
        """The next goal: ("use", cell, None) or ("walk", cell, None), or None.

        Candidates are usable events (each tried at most max_uses times per
        situation), wanted pickups and unvisited cells (cost +2), by walking
        distance; the wanted exit only when nothing else is left. After 30
        idle steps it picks random candidates for 20 steps.
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
            r = random.Random(len(w.messages) + w.x * 7 + w.z)
            pool = cands or exits
            if pool:
                return r.choice(pool)[1]
        if cands:
            return min(cands, key=lambda t: t[0])[1]
        if exits:
            return min(exits, key=lambda t: t[0])[1]
        return None

    def wants(self, w, d):
        """Is pickup def d worth walking to? Armour and health only when not
        full, inventory items and ammo below 99, anything else always."""
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
        """Walking distance in steps from the player to every reachable cell."""
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
        """The action that moves towards path[0]: turn, open (or chop) a door,
        put out fire, break a crate, go round or back off from a person or
        monster, or step forward. A door that will not open is marked
        blocked for this situation."""
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


# action name -> what it does to the model (as the ROM's buttons do)
ACTIONS = {"fwd": lambda w: w.move(False), "back": lambda w: w.move(True), "left": lambda w: w.turn(-1),
           "right": lambda w: w.turn(1), "use": lambda w: w.use(), "pass": lambda w: w.pass_turn(),
           "wnext": lambda w: w.cycle_weapon(1), "wprev": lambda w: w.cycle_weapon(-1),
           "heal": lambda w: w.use_health_item(), "close": lambda w: w.store_close(),
           "berserk": lambda w: w.use_item(3), "collar": lambda w: w.use_item(4)}


def run(steps=200000, god=True, verbose=True, start_map=0):
    """Plays the campaign on the model. -> (campaign, [actions]).

    Mega Drive model only (it needs game_model.Campaign). god keeps HP at
    200; actions are (name, extra) with extra the code for "code"."""
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
