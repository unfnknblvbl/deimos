#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Reference model of Doom RPG's game rules for Sector 1, in plain Python.

This is the spec the Z80 engine (rom/game/*.asm) implements, and the
oracle the lockstep test (tools/test_lockstep.py) compares the ROM against:
both run the same 16-bit xorshift RNG and must make the same calls to it in
the same order, so after every player action the ROM's RAM must equal this
model's state byte for byte.

Rules were re-expressed from reading the original J2ME decompile and the
GPL DoomRPG-RE reimplementation (Combat.c, CombatEntity.c, Entity.c,
Player.c, Game.c, DoomCanvas.c). It is a reimplementation of behaviour, not
a translation of their code. Port decisions (things the SMS version does
differently on purpose) are marked "PORT:".

Used by test_lockstep.py and every tool that plays the ROM (the model
answers dialogs and door codes and checks the state), and by the pilots
(autopilot.py, campaign_pilot.py through sms_pilot.py). Data comes from
analysis/gamedata.json (gamedata.py) and the rule tables in game_rules.py.

Words used here: an "entity" is a dynamic thing on the map (monster,
person, pickup, fire, barrel, crate); its "def" is its entity
definition; a "line" is a switchable wall (door, secret wall, jammed
door); an "event" is a script on one cell, a list of commands run when
the player steps, turns, or uses the cell; a "trace" walks the cells
along a row or column and lists what blocks it. Cells are 32 x 32,
facing 0..3 is N E S W. Fixed point: damage is in 1/256 HP until the
end of calc_damage; multipliers are in 1/256.

Run as a script it loads the level and prints the starting state.
"""
import json
from pathlib import Path

from game_rules import (WEAPONS, MONSTERS, weapon_damage_mult, PLAYER_START, CAMPAIGN_START, EVF_USE, EVF_MODIFY,
                        EVF_MOVE, FACING_FLAG, LEAVE_FLAG, ARRIVE_FLAG, MASK_PLAYER_MOVE,
                        MASK_MONSTER_MOVE, MASK_SHOT, MASK_SHOW_KILL, MASK_RAISE_FREE, W_RAISE)

DIRS = [(0, -1), (1, 0), (0, 1), (-1, 0)]          # facing 0=N 1=E 2=S 3=W
# entity types (a def's eType)
ET_LINE, ET_ENEMY, ET_HUMAN, ET_PICKUP, ET_INV, ET_WEAPON, ET_AMMO = 0, 1, 2, 3, 4, 5, 6
ET_PLAYERMARK, ET_WALLMARK, ET_FIRE, ET_FIRE2, ET_DESTRUCT, ET_AMMO2 = 8, 9, 10, 11, 12, 16

# entity flag bits (RAM, one byte per entity)
F_ALIVE = 0x01      # targetable (original info 0x40000)
F_HIDDEN = 0x02     # sprite hidden (0x10000)
F_LINKED = 0x04     # on the map grid (0x4000000)
F_ACTIVE = 0x08     # monster in the active (thinking) list (0x80000)
F_ATK2 = 0x10       # monster uses its second attack (0x800000)
F_DROPPED = 0x20    # already dropped its item (0x100000)
F_CORPSE = 0x40     # dead monster with a corpse (sprite 0x1000000)
F_RAISING = 0x80    # corpse targeted by an Archvile (0x2000000)


class XorShift16:
    """x ^= x<<7; x ^= x>>9; x ^= x<<8 (period 65535). One byte = low byte
    of the next state. Cheap on a Z80, and the same sequence in both."""

    def __init__(self, seed=0x1234):
        """Start from seed (16 bits; 0 becomes 1, which xorshift needs). calls counts the bytes drawn.
        """
        self.x = seed & 0xFFFF or 1
        self.calls = 0

    def byte(self):
        """Advance the state once and return its low byte (0..255)."""
        x = self.x
        x ^= (x << 7) & 0xFFFF
        x ^= x >> 9
        x ^= (x << 8) & 0xFFFF
        self.x = x
        self.calls += 1
        return x & 0xFF

    def int32(self):
        """Four bytes drawn in turn, assembled little-endian into a 32-bit value."""
        b = [self.byte() for _ in range(4)]
        return b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)


def clamp8(v):
    """v limited to 0..255 (stats are bytes in the ROM)."""
    return 0 if v < 0 else 255 if v > 255 else v


class Combatant:
    """A fighter's stats (the player or one monster), each 0..255.

    mclass is the monster class (a MONSTERS index), None for the player.
    """
    __slots__ = ("hp", "maxhp", "armor", "maxarmor", "defense", "strength", "agility", "accuracy", "mclass")

    def __init__(self, hp, armor, defense, strength, agility, accuracy, mclass=None):
        """Set every stat (max and current HP and armour equal), clamped to a byte."""
        self.maxhp = self.hp = clamp8(hp)
        self.maxarmor = self.armor = clamp8(armor)
        self.defense, self.strength = clamp8(defense), clamp8(strength)
        self.agility, self.accuracy = clamp8(agility), clamp8(accuracy)
        self.mclass = mclass

    def exp(self):
        """XP for killing this fighter (CombatEntity's formula, rounded up by +49 / 50)."""
        return (((self.defense + self.strength) * 5) + ((self.agility + self.accuracy) * 3)
                + ((self.maxhp + self.maxarmor) * 5) + 49) // 50


# ---------------------------------------------------------------- combat math
def setup_enemy(c, parm, rng):
    """CombatEntity_setupEnemy: every stat scaled by a random factor in
    [0.75, 1.25) x parm/256 (armour and defence share one factor)."""
    i2 = (parm << 8) // 1024           # parm / 4 (in 1/256): the +-25% spread
    i3 = (i2 << 9) >> 8                # twice that: the random range

    def factor():
        """parm - spread + random(0..2 spread): one RNG byte, in 1/256 (parm 256 = x1)."""
        return (parm + ((i3 * rng.byte()) >> 8)) - i2

    f = factor()
    c.maxhp = c.hp = clamp8(((c.maxhp << 8) * f) >> 16)
    f = factor()
    c.maxarmor = c.armor = clamp8(((c.maxarmor << 8) * f) >> 16)
    c.defense = clamp8(((c.defense << 8) * f) >> 16)
    c.strength = clamp8(((c.strength << 8) * factor()) >> 16)
    c.agility = clamp8(((c.agility << 8) * factor()) >> 16)
    c.accuracy = clamp8(((c.accuracy << 8) * factor()) >> 16)


def range_excess(tiles, range_min):
    """dist - Weapon_getRangeMinToDist in units of 4096 (one tile squared):
    distances are always straight along a row/column of whole tiles.

    Positive when the target is beyond the weapon's rangeMin (in tiles)."""
    return tiles * tiles - range_min * range_min - 1


HIT_MISS, HIT_HIT, HIT_CRIT = 0, 1, 2   # calc_hit results


def calc_hit(att, weapon, tgt, tiles, rng):
    """Roll one hit of weapon (WEAPONS index) by att on tgt, tiles away (0: adjacent/unknown).

    The chance ch is out of 256: accuracy over the target's agility, plus a
    bonus from the weapon's rangeMax column. Beyond rangeMin a melee weapon
    (rangeMin 0) misses; other non-shotgun weapons lose 76/256 of the chance
    per distance step (steps at squared distances 1, 4, 9...). One RNG byte r:
    r < ch hits, and r < ch / 20 is a critical. Returns HIT_MISS, HIT_HIT or
    HIT_CRIT.
    """
    w = WEAPONS[weapon]
    agi = tgt.agility if tgt.agility else 1
    # accuracy / agility in 8.8, halved, plus rangeMax * 256 / 200 (1.28 x rangeMax)
    ch = (((att.accuracy << 16) // (agi << 8)) * 128 >> 8) + ((w[4] << 16) // 51200)
    ex = range_excess(tiles, w[3]) if tiles else -1
    if ex > 0 and w[3] == 0:
        return HIT_MISS                     # melee weapon, target not adjacent
    if w[5] != 2 and ex > 0:                # shells (shotguns) don't fall off
        dec = (ch * 76) >> 8
        k = 2
        d = 1
        while d < ex:                       # one step off per square number passed
            ch -= dec
            d = k * k
            k += 1
    r = rng.byte()
    if r < ch:
        return HIT_CRIT if r < ((ch << 8) // 5120) else HIT_HIT
    return HIT_MISS


def calc_damage(att, weapon, tgt, mult, tiles, rng):
    """-> (health_damage, armor_damage). mult = 256, 512 (crit) or 768.

    The roll is strMin..strMax (one RNG byte), times strength / defence,
    times mult and the class multiplier, less 76/256 per distance step
    past rangeMin; clamped to 1..999 HP, then split by the weapon's
    armorShare and rounded. All in 1/256 HP, wrapped to 32 bits like
    the Z80 code."""
    w = WEAPONS[weapon]
    r = rng.byte()
    dfn = tgt.defense if tgt.defense else 1
    ratio = ((att.strength << 16) // (dfn << 8)) & 0xFFFFFFFF
    a = (w[1] << 8) + ((r * ((w[2] - w[1]) << 8)) >> 8)   # base damage in 1/256 HP
    d = (a * ratio) & 0xFFFFFFFF
    d = (d >> 8)
    d = ((d * mult) & 0xFFFFFFFF) >> 8
    d = ((d * weapon_damage_mult(tgt.mclass, weapon)) & 0xFFFFFFFF) >> 8
    ex = range_excess(tiles, w[3]) if tiles else -1
    dec = (d * 76) >> 8
    k = 2
    dist = 1
    while dist < ex:
        d -= dec
        dist = k * k
        k += 1
    d = max(256, min(d, 255744))          # 1 .. 999 HP
    arm = (d * w[7]) >> 8
    return ((d - arm) + 128) >> 8, (arm + 128) >> 8


# ---------------------------------------------------------------- the world
class World:
    """The game state of one level and every rule that changes it.

    Built from gamedata.json (gamedata.py). The public actions (turn, move,
    use, pass_turn, cycle_weapon, use_health_item, resume) are what a button
    press does; each runs the player's part and then, unless a dialog or door
    code is waiting, the monsters' turn. Fields that the lockstep test
    compares are listed in test_lockstep.model_bytes: player position and
    stats, ents (per entity: def, x, z, flags F_*, c (Combatant or None),
    move), lines (open, locked, alive), event_state, cmd_alive, notes...
    Entities past first_drop are the 8 drop slots, reused in turn.
    """
    def __init__(self, gamedata, seed=0x1234):
        """Set up the level from the gamedata dict and an RNG seed.

        Monsters get their stats rolled in map order (the RNG order must match
        the ROM), the start cell's event runs, and visible monsters wake up.
        """
        g = gamedata
        self.g = g
        self.rng = XorShift16(seed)
        self.defs = g["defs"]
        self.wall = g["wall"]
        self.static = g["static_block"]
        p = dict(CAMPAIGN_START if g.get("new_game") else PLAYER_START)   # (the UAC Base: a new game's kit)
        self.p = Combatant(p["hp"], p["armor"], p["defense"], p["strength"], p["agility"], p["accuracy"])
        self.p.maxarmor = p["maxarmor"]
        self.level, self.xp, self.nextxp = p["level"], p["xp"], p["nextxp"]
        self.credits, self.weapons, self.weapon = p["credits"], p["weapons"], p["weapon"]
        self.ammo, self.inventory, self.keys = list(p["ammo"]), list(p["inventory"]), p["keys"]
        self.x, self.z, self.facing = g["spawn"]
        self.notes, self.status_msg, self.messages = [], None, []
        self.dead = False
        self.level_done = False
        self.map_given = False

        # entities: list of dicts; ents beyond the map's are 8 drop slots
        # (hidden ones start off the grid: SHOW events link them later)
        self.ents = []
        for e in g["entities"]:
            d = self.defs[e["def"]]
            ent = {"def": e["def"], "x": e["x"], "z": e["z"], "flags": 0, "c": None, "move": 0}
            if not e["hidden"]:
                ent["flags"] |= F_LINKED
            self.ents.append(ent)
        self.first_drop = len(self.ents)
        for _ in range(8):
            self.ents.append({"def": None, "x": 0, "z": 0, "flags": 0, "c": None, "move": 0})
        self.drop_index = 0

        self.lines = []
        for l in g["lines"]:
            self.lines.append({"x": l["x"], "z": l["z"], "open": False, "locked": l["locked"],
                               "eType": l["eType"], "eSub": l["eSub"], "name": l["name"],
                               "alive": l["eType"] == ET_DESTRUCT})
        self.event_state = [e["state"] for e in g["events"]]
        self.cmd_alive = [[True] * len(e["cmds"]) for e in g["events"]]
        self.event_at = {(e["x"], e["z"]): i for i, e in enumerate(g["events"])}
        self.active = []          # active monsters, activation order
        self.attackers = []       # this turn's attack queue (LIFO)
        self.dialog = None        # the dialog's text while a dialog is open
        self.password = None      # (code, prompt text) while a door code is asked for
        self.wait_resume = None   # (not used)
        self.pending_explosions = []   # cells waiting to explode, in order

        # Entity_initspawn, in map order (rng order matters)
        for ent in self.ents[:self.first_drop]:
            d = self.defs[ent["def"]]
            if d["eType"] == ET_ENEMY:
                base, _atk, _shots = MONSTERS[d["eSub"]]
                c = Combatant(*base, mclass=d["eSub"])
                setup_enemy(c, d["parm"], self.rng)
                ent["c"] = c
                if self.rng.byte() & 1:    # which attack it starts with
                    ent["flags"] |= F_ATK2
                ent["flags"] |= F_ALIVE
            elif d["eType"] in (ET_FIRE, ET_DESTRUCT):
                ent["flags"] |= F_ALIVE
        self.msg = []
        # the start cell's events, as if arrived from every side, facing the start way
        self.run_tile(self.x, self.z, 0x40F | FACING_FLAG[self.facing])
        self.activate_visible()

    # -------------------------------------------------- small helpers
    def edef(self, ent):
        """The def dict of entity ent (None for an empty drop slot)."""
        return self.defs[ent["def"]] if ent["def"] is not None else None

    def etype(self, ent):
        """The eType of entity ent."""
        return self.edef(ent)["eType"]

    def say(self, text):
        """Add a message line (the model's log; the ROM shows these on the message line)."""
        self.messages.append(text)

    def entities_at(self, x, z):
        """Indices of the entities on the grid (linked) in cell (x, z), in entity order."""
        return [i for i, e in enumerate(self.ents)
                if e["flags"] & F_LINKED and e["def"] is not None and e["x"] == x and e["z"] == z]

    def line_at(self, x, z):
        """Index of the first shut switchable line in cell (x, z), or None."""
        for i, l in enumerate(self.lines):
            if l["x"] == x and l["z"] == z and not l["open"]:
                return i
        return None

    def trace(self, sx, sz, dx, dz, mask, self_idx=None):
        """Game_trace along a row/column. Returns list of ('wall'|'player'|
        ('line', i)|('static', etype)|('ent', i)).

        Walks from (sx, sz) to (dx, dz), both included. mask bit n blocks
        entity type n (also line and static blocker types); bit 8 the
        player. A wall or the player ends the trace; at most 8 hits are
        returned. self_idx (the tracer itself) is ignored."""
        out = []
        if sx == dx and sz == dz:
            cells = [(sx, sz)]
        elif sx == dx:
            st = 1 if dz > sz else -1
            cells = [(sx, z) for z in range(sz, dz + st, st)]
        else:
            st = 1 if dx > sx else -1
            cells = [(x, sz) for x in range(sx, dx + st, st)]
        for (x, z) in cells:
            if not (0 <= x < 32 and 0 <= z < 32) or self.wall[z * 32 + x]:
                out.append("wall")
                return out
            if (mask & 0x100) and (x, z) == (self.x, self.z):
                out.append("player")
                return out
            li = self.line_at(x, z)
            if li is not None and mask & (1 << self.lines[li]["eType"]):
                out.append(("line", li))
            st = self.static[z * 32 + x]
            if st and mask & (1 << st):
                out.append(("static", st))
            for i in self.entities_at(x, z):
                if i != self_idx and mask & (1 << self.etype(self.ents[i])):
                    out.append(("ent", i))
            if len(out) >= 8:
                return out[:8]
        return out

    def blocked(self, sx, sz, dx, dz, mask, self_idx=None):
        """True if trace() from (sx, sz) to (dx, dz) finds anything that mask blocks."""
        return len(self.trace(sx, sz, dx, dz, mask, self_idx)) > 0

    # -------------------------------------------------- player stats
    def add_health(self, n):
        """Add n HP (may be negative), kept within 0..maxhp; reports a gain."""
        before = self.p.hp
        self.p.hp = max(0, min(self.p.hp + n, self.p.maxhp))
        if self.p.hp > before:
            self.say(f"Gained {self.p.hp - before} health")

    def add_armor(self, n):
        """Add n armour, up to maxarmor; reports a gain."""
        before = self.p.armor
        self.p.armor = min(self.p.armor + n, self.p.maxarmor)
        if self.p.armor > before:
            self.say(f"Gained {self.p.armor - before} armor")

    def add_ammo(self, t, n):
        """Add n of ammo type t, up to 99. False (nothing added) if already at 99."""
        if self.ammo[t] == 99:
            return False
        self.ammo[t] = min(99, self.ammo[t] + n)
        return True

    def add_xp(self, n):
        """Add n XP; each time it reaches nextxp, the excess carries over and the player levels up.
        """
        self.say(f"Gained {n} XP!")
        self.xp += n
        while self.xp >= self.nextxp:
            self.xp -= self.nextxp
            self.level_up()

    def level_up(self):
        """Next level: nextxp = level * 20 + 60, max HP and max armour +3..5
        (capped at 99; HP refilled), each other stat +1..2 (capped at 99), using
        the RNG in that order.
        """
        self.level += 1
        self.nextxp = self.level * 20 + 60
        c = self.p
        n = 3 + (self.rng.byte() % 3)
        n = min(n, 99 - c.maxhp) if c.maxhp + n > 99 else n
        c.maxhp += n
        c.hp = c.maxhp
        n = 3 + (self.rng.byte() % 3)
        n = min(n, 99 - c.maxarmor) if c.maxarmor + n > 99 else n
        c.maxarmor += n
        for attr in ("defense", "strength", "agility", "accuracy"):
            n = 1 + (self.rng.byte() % 2)
            v = getattr(c, attr)
            if v + n > 99:
                n = 99 - v
            setattr(c, attr, v + n)
        self.say(f"Level up! Level {self.level}")

    def player_pain(self, dmg, arm):
        """Hurt the player: dmg to health, arm to armour (armour that runs out
        passes the rest to health). Warns when health falls below about 10%
        (26/256) or 30% (78/256) or armour is used up; sets dead at 0 HP.
        """
        self.say(f"{dmg + arm} damage!")
        if dmg + arm:
            a = self.p.armor
            if a < arm:
                dmg += arm - a
                self.p.armor = 0
            else:
                self.p.armor = a - arm
            h1 = (self.p.hp << 16) // (self.p.maxhp << 8)
            h2 = ((self.p.hp - dmg) << 16) // (self.p.maxhp << 8)
            if h2 > 0:
                if h1 > 26 >= h2:
                    self.say("Near Death!")
                elif h1 > 78 >= h2:
                    self.say("Low Health!")
                elif a > 0 and self.p.armor == 0:
                    self.say("Armor Gone!")
            self.p.hp = max(0, self.p.hp - dmg)
            if self.p.hp <= 0:
                self.dead = True

    # -------------------------------------------------- monsters
    def activate(self, i):
        """Wake monster i: mark it active and add it to the end of the active list."""
        e = self.ents[i]
        if not e["flags"] & F_ACTIVE:
            e["flags"] |= F_ACTIVE
            self.active.append(i)

    def deactivate(self, i):
        """Put monster i back to sleep (out of the active list)."""
        e = self.ents[i]
        if e["flags"] & F_ACTIVE:
            e["flags"] &= ~F_ACTIVE
            self.active.remove(i)

    def visible(self, x, z):
        """PORT: the original activates a monster the first time the renderer
        draws it. Our views are pre-rendered, so 'drawn' becomes: within 5
        cells ahead, inside the 90-degree cone, with a straight grid line of
        sight not crossing a wall or closed door.

        (The views have been drawn live since 0.2, but this rule stays: the
        ROM's rules code does the same test, independent of the renderer.)"""
        fx, fz = DIRS[self.facing]
        rx, rz = -fz, fx
        dx, dz = x - self.x, z - self.z
        depth = dx * fx + dz * fz
        lat = dx * rx + dz * rz
        if depth < 1 or depth > 5 or abs(lat) > depth:
            return False
        steps = max(abs(dx), abs(dz))
        for s in range(1, steps):
            # nearest grid cell on the line, halves rounded up (integer-only,
            # so the Z80 code computes exactly the same cells)
            cx = self.x + (2 * dx * s + steps) // (2 * steps)
            cz = self.z + (2 * dz * s + steps) // (2 * steps)
            if self.wall[cz * 32 + cx] or self.line_at(cx, cz) is not None:
                return False
        return True

    def activate_visible(self):
        """Wake every living, linked, sleeping monster that visible() says the player can see."""
        for i, e in enumerate(self.ents[:self.first_drop]):
            if (e["flags"] & (F_LINKED | F_ALIVE | F_ACTIVE)) == (F_LINKED | F_ALIVE) and \
                    self.etype(e) == ET_ENEMY and self.visible(e["x"], e["z"]):
                self.activate(i)

    def monster_died(self, i, overkill=False):
        """Monster i dies: XP to the player, out of the active list and off the
        grid, a corpse unless overkill (gibbed), then maybe a drop.
        """
        e = self.ents[i]
        if not e["flags"] & F_ALIVE:
            return
        e["flags"] &= ~F_ALIVE
        self.add_xp(e["c"].exp())
        self.deactivate(i)
        e["flags"] &= ~F_LINKED
        if not overkill:
            e["flags"] |= F_CORPSE
        self.spawn_drop(i)

    def spawn_drop(self, i):
        """Maybe drop an item where monster i died (Entity_spawnDropItem).

        One 32-bit roll: nothing for some classes (1, 12, 13), once per monster,
        or when the low byte is below 51 (20%). Otherwise the other bits pick
        health, armour, credits (more likely for higher-parm defs) or, by
        monster class, ammo or medkits. The item takes the next of the 8 drop
        slots (overwriting the oldest).
        """
        e = self.ents[i]
        d = self.edef(e)
        rnd = self.rng.int32()
        es = d["eSub"]
        # never twice; never for hounds (1), Cyberdemon (12), Kronos (13); else 80%
        if e["flags"] & F_DROPPED or es in (1, 12, 13) or (rnd & 0xFF) < 51:
            return
        e["flags"] |= F_DROPPED
        n = rnd >> 8
        n2 = n >> 1
        n5, n6 = n2 & 0xFF, (n2 >> 8) & 0xFF
        dt = None
        if n & 1:                           # health, armour or credits
            if n5 < 115:
                dt = (3, 20)
            elif n5 < 230:
                dt = (3, 21)
            else:
                n7 = {294: 3, 467: 8, 640: 13}.get(d["parm"], 0)
                dt = (3, 23) if n5 < 230 + n7 else (3, 22)
        else:                               # by class: ammo, medkits, credits
            b3, b4 = n5 < 76, n6 < 15
            dt = {0: ((16 if b4 else 6), (2 if b3 else 1)),
                  2: ((16 if (b4 or b3) else 6), 1),
                  6: (4, 26 if b4 else 25),
                  7: (3, 23),
                  8: ((16 if b4 else 6), 3),
                  9: ((16 if b4 else 6), 4),
                  10: ((4, 26 if b4 else 25) if b3 else (3, 23)),
                  11: (16, 4 if b3 else 2)}.get(es)
        if dt is None:
            return
        di = next(k for k, dd in enumerate(self.defs) if (dd["eType"], dd["eSub"]) == dt)
        slot = self.ents[self.first_drop + self.drop_index]
        self.drop_index = (self.drop_index + 1) % 8
        slot.update({"def": di, "x": e["x"], "z": e["z"], "flags": F_LINKED})

    # -------------------------------------------------- monster AI
    def monster_mask(self, ent):
        """The trace mask monster ent moves with: some classes may pass certain blockers (bits 10, 11).
        """
        m = MASK_MONSTER_MOVE
        es = self.edef(ent)["eSub"]
        # floating classes pass over some blockers: entity types 10 and 11
        # (fire) for 4 and 13, type 11 for 6 and 7, type 10 for 10
        if es in (4, 13):
            m &= ~3072
        elif es in (6, 7):
            m &= ~2048
        elif es == 10:
            m &= ~1024
        return m

    def can_step(self, ent, i, sx, sz, nx, nz, mask):
        """True if entity i may move from (sx, sz) to (nx, nz) under mask."""
        return not self.blocked(sx, sz, nx, nz, mask, i)

    def calc_path(self, i, sx, sz, dx, dz, mask):
        """Score for reaching (dx, dz) from (sx, sz) by up to two greedy steps
        (each to the free neighbour nearest the goal). 0 or 1 if reached after
        one or two steps, else 2 + the squared distance left. Lower is better.
        """
        if (sx, sz) == (dx, dz):
            return 0
        vx = vz = None
        for k in range(2):
            best = 999999
            stx = stz = 0
            for (ox, oz) in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                ok = self.can_step(self.ents[i], i, sx, sz, sx + ox, sz + oz, mask)
                nd = (sx + ox - dx) ** 2 + (sz + oz - dz) ** 2
                if ok and nd < best:
                    best, stx, stz = nd, ox, oz
            sx += stx
            sz += stz
            vx, vz = sx, sz
            if (sx, sz) == (dx, dz):
                return k            # (sic) reached after 1 step -> 0, after 2 -> 1
        return 2 + (vx - dx) ** 2 + (vz - dz) ** 2

    def ai_move(self, i, dx, dz):
        """Entity_aiGoal_MOVE: one step toward (dx,dz) by a 2-step greedy look-ahead."""
        e = self.ents[i]
        sx, sz = e["x"], e["z"]
        mask = self.monster_mask(e)
        best = 9999
        order = []
        # each free neighbour, scored by calc_path; equal best ones are
        # collected and one is picked at random (code: 0 N, 1 S, 2 E, 3 W)
        for (ox, oz, code) in ((1, 0, 2), (-1, 0, 3), (0, 1, 1), (0, -1, 0)):
            if not self.can_step(e, i, sx, sz, sx + ox, sz + oz, mask):
                continue
            pd = self.calc_path(i, sx + ox, sz + oz, dx, dz, mask)
            if code == 2:
                if pd < 9999:
                    best, order = pd, [code]
                elif pd == 9999:
                    order.append(code)
            elif pd < best:
                best, order = pd, [code]
            elif pd == best:
                order.append(code)
        if not order:
            return False
        pick = order[(self.rng.byte() & 3) % len(order)]
        ox, oz = {0: (0, -1), 1: (0, 1), 2: (1, 0), 3: (-1, 0)}[pick]
        e["x"], e["z"] = sx + ox, sz + oz
        return True

    def queue_attack(self, i, target=None):
        """Add monster i (and its target: None the player, or a tuple) to the front of this turn's attack list, once.
        """
        if i not in [a for a, _t in self.attackers]:
            self.attackers.insert(0, (i, target))

    def attack_weapon(self, e):
        """The WEAPONS index monster ent attacks with (its first or second attack, by F_ATK2)."""
        _base, atk, _shots = MONSTERS[self.edef(e)["eSub"]]
        return atk[1] if e["flags"] & F_ATK2 else atk[0]

    def ai_think(self, i):
        """Monster i's decision for this turn (Entity_aiThink).

        An Archvile (class 10) may first try to raise a nearby corpse, and
        otherwise switches attack each turn. If the player is in a straight line
        within reach and the trace is clear (or the first thing is a crate or
        barrel, which it then attacks), it attacks, though monsters with longer
        reach sometimes (39/256) move instead. Otherwise it steps towards the
        cell in front of the player. Returns "moved" if it stepped, else None.
        """
        e = self.ents[i]
        d = self.edef(e)
        px, pz = self.x, self.z
        if d["eSub"] == 10:                 # Archvile: raise a corpse 84/256 of the time
            if self.rng.byte() < 84:
                for j in [k for k in range(self.first_drop) if self.ents[k]["flags"] & F_CORPSE]:
                    c = self.ents[j]
                    dd = ((c["x"] - e["x"]) ** 2 + (c["z"] - e["z"]) ** 2) * 4096
                    # within 2 cells (squared 4 tiles), not on its own cell
                    if dd == 0 or dd > 0x4000 or c["flags"] & F_RAISING or \
                            self.blocked(c["x"], c["z"], c["x"], c["z"], MASK_RAISE_FREE):
                        continue
                    c["flags"] |= F_RAISING
                    self.queue_attack(i, ("raise", j))
                    return
            e["flags"] ^= F_ATK2
        w = self.attack_weapon(e)
        rng_min = WEAPONS[w][3]
        reach = 1 + rng_min                 # attack distance in cells
        d2 = (e["x"] - px) ** 2 + (e["z"] - pz) ** 2
        aligned = e["x"] == px or e["z"] == pz
        can_hit, target = False, None
        if aligned and d2 <= reach * reach:
            tr = self.trace(e["x"], e["z"], px, pz, MASK_SHOT, i)
            if tr:
                first = tr[0]
                if first[0] == "ent" and self.etype(self.ents[first[1]]) == ET_DESTRUCT:
                    target, can_hit = ("ent", first[1]), True
            else:
                can_hit = True
        half = reach // 2
        if not can_hit or (half != 0 and self.rng.byte() >= 217):
            # move: aim for the cell in front of the player, sometimes
            # (38/256, when close) one further back towards the monster
            fx, fz = DIRS[self.facing]
            tx, tz = px + fx, pz + fz
            if d2 <= half * half and aligned and self.rng.byte() < 38:
                tx += -1 if e["x"] < px else 1 if e["x"] > px else 0
                tz += -1 if e["z"] < pz else 1 if e["z"] > pz else 0
            if self.ai_move(i, tx, tz):
                e["move"] = 0
                return "moved"
            if can_hit:
                self.queue_attack(i, target)
        else:
            self.queue_attack(i, target)
        return None

    def ai_after_step(self, i):
        """Entity_aiMoveToGoal, called each time a monster finishes a step.

        Fast classes (4, 13) take up to 3 steps a turn, the others 1; some
        classes (1, 4, 5, 13) attack straight after their last step if next
        to the player with a clear trace. Returns True if it stepped again.
        """
        e = self.ents[i]
        es = self.edef(e)["eSub"]
        steps = 3 if es in (4, 13) else 1
        can_lunge = es in (5, 4, 1, 13)
        adj = (e["x"] == self.x or e["z"] == self.z) and (e["x"] - self.x) ** 2 + (e["z"] - self.z) ** 2 <= 1
        e["move"] += 1
        if e["move"] < steps:
            if adj:
                e["move"] = steps
            elif self.ai_move(i, self.x, self.z):
                return True
        if e["move"] != steps or not can_lunge or not adj:
            return False
        tr = self.trace(e["x"], e["z"], self.x, self.z, MASK_SHOT, i)
        if tr:
            return False
        self.queue_attack(i, None)
        return False

    def monster_turn(self):
        """The monsters' turn: every active monster thinks; those that moved take
        their extra steps (ai_after_step) round by round; then the queued
        attacks run, most recently queued first, stopping if the player dies;
        then newly visible monsters wake.
        """
        self.attackers = []
        movers = [i for i in list(self.active) if self.ai_think(i) == "moved"]
        while movers:
            again = [i for i in self.active if i in movers and self.ai_after_step(i)]
            movers = again
        for i, target in list(self.attackers):
            if self.dead:
                break
            self.monster_attack(i, target)
        self.attackers = []
        self.activate_visible()

    def monster_attack(self, i, target):
        """Monster i carries out its queued attack on target: raise a corpse,
        hit the player (one roll per shot; a critical on the first shot doubles
        every shot), or break the crate/barrel (target ("ent", j)).
        """
        e = self.ents[i]
        if not e["flags"] & F_ALIVE:
            return
        d = self.edef(e)
        if target and target[0] == "raise":
            self.say(f"{d['name']} casts raise")
            j = target[1]
            c = self.ents[j]
            if not self.blocked(c["x"], c["z"], c["x"], c["z"], MASK_RAISE_FREE):
                c["flags"] = (c["flags"] & ~(F_CORPSE | F_RAISING)) | F_ALIVE | F_LINKED
                c["c"].hp, c["c"].armor = c["c"].maxhp, c["c"].maxarmor
                self.activate(j)
                self.say(f"{self.edef(c)['name']} is revived!")
            else:
                c["flags"] &= ~F_RAISING
                self.say("Raise failed!")
            return
        self.say(f"{d['name']} attacks")
        w = self.attack_weapon(e)
        shots = MONSTERS[d["eSub"]][2]
        tot = tarm = 0
        crit = False
        for s in range(shots):
            if target is None:
                # distance 0: monster attacks never lose accuracy with range
                h = calc_hit(e["c"], w, self.p, 0, self.rng)
                if h:
                    if h == HIT_CRIT and s == 0:
                        crit = True
                    dm, am = calc_damage(e["c"], w, self.p, 512 if crit else 256, 0, self.rng)
                    tot += dm
                    tarm += am
            else:
                tot += 1                    # an object: any shot breaks it
        if target is None:
            if tot + tarm == 0:
                self.say("Dodged!")
            else:
                self.player_pain(tot, tarm)
        else:
            self.destroy(target[1])

    # -------------------------------------------------- destruction
    def destroy(self, j):
        """Destroy entity j (fire, barrel, crate) if it is still alive.

        Fire gives 2 XP; a barrel (DESTRUCT eSub 1) explodes; a crate (eSub 2)
        rolls one RNG byte for a trap (explosion) or an item that takes the
        crate's place. Then any pending explosions run.
        """
        e = self.ents[j]
        d = self.edef(e)
        if not e["flags"] & F_ALIVE:
            return
        e["flags"] &= ~(F_ALIVE | F_LINKED)
        if d["eType"] == ET_FIRE:
            self.say("Fire cleared!")
            self.add_xp(2)
        elif d["eType"] == ET_DESTRUCT and d["eSub"] == 1:
            self.pending_explosions.append((e["x"], e["z"]))
        elif d["eType"] == ET_DESTRUCT and d["eSub"] == 2:
            r = self.rng.byte()
            dt = None
            if r < 2:
                self.say("Trapped!")
                self.pending_explosions.append((e["x"], e["z"]))
            elif r < 4:
                dt = (3, 23)
            elif r < 12:
                dt = (3, 22)
            elif r < 24:
                dt = (4, 25)
            elif r < 150:
                dt = (3, 21)
            elif r < 213:                   # ammo of a random type 0..4
                dt = (6, self.rng.byte() % 5)
            if dt:
                di = next((k for k, dd in enumerate(self.defs) if (dd["eType"], dd["eSub"]) == dt), None)
                if di is not None:
                    e["def"] = di
                    e["flags"] = F_LINKED
        self.run_explosions()

    def run_explosions(self):
        """Run queued explosions in order: 5..28 damage (to health and armour
        alike) on the four neighbours, half that on the diagonals. Explosions
        they cause are queued and run in turn.
        """
        while self.pending_explosions:
            x, z = self.pending_explosions.pop(0)
            r = (self.rng.int32() & 0xFF) // 11    # 0..23
            dmg = r + 5
            for (ox, oz) in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                self.hurt_at(x + ox, z + oz, dmg, dmg)
            if dmg >> 1:
                for (ox, oz) in ((-1, 1), (1, 1), (1, -1), (-1, -1)):
                    self.hurt_at(x + ox, z + oz, dmg >> 1, dmg >> 1)

    def hurt_at(self, x, z, dmg, arm):
        """Explosion damage on cell (x, z): the player, or the first thing there
        a shot would hit. A door or static blocker shields the cell; a monster
        is hurt (and may die); other things are destroyed, except fire and
        DESTRUCT eSub 3/4.
        """
        if (x, z) == (self.x, self.z):
            self.player_pain(dmg, arm)
            return
        if not (0 <= x < 32 and 0 <= z < 32) or self.wall[z * 32 + x]:
            return
        li = self.line_at(x, z)
        if li is not None and MASK_SHOT & (1 << self.lines[li]["eType"]):
            return                          # first thing on the tile is a door: not hurt
        st = self.static[z * 32 + x]
        if st and MASK_SHOT & (1 << st):
            return
        for j in self.entities_at(x, z):
            e = self.ents[j]
            if not MASK_SHOT & (1 << self.etype(e)):
                continue
            if not e["flags"] & F_ALIVE:
                return                      # first matching entity isn't targetable
            d = self.edef(e)
            if d["eType"] == ET_ENEMY:
                self.monster_pain(j, dmg, arm)
                if e["c"].hp <= 0:
                    self.say(f"{d['name']} died!")
                    self.monster_died(j)
            elif d["eType"] != ET_FIRE and not (d["eType"] == ET_DESTRUCT and d["eSub"] in (3, 4)):
                self.destroy(j)
            return

    def monster_pain(self, j, dmg, arm):
        """Monster j takes dmg to health and arm to armour (armour that runs out passes the rest on).
        """
        c = self.ents[j]["c"]
        if c.armor < arm:
            dmg += arm - c.armor
            c.armor = 0
        else:
            c.armor -= arm
        c.hp = max(0, c.hp - dmg)

    # -------------------------------------------------- player actions
    def select_target(self):
        """What "use" would attack: the first thing a shot trace finds up to 8
        cells ahead. Returns ("ent", i), ("line", i) or None.

        A jammed door (DESTRUCT line) only with the axe; a crate only with a
        weapon in its parm bit mask; fire only with the extinguisher; a monster
        standing in an open door's cell behind the line is the target.
        """
        fx, fz = DIRS[self.facing]
        tr = self.trace(self.x, self.z, self.x + 8 * fx, self.z + 8 * fz, MASK_SHOT)
        for k, t in enumerate(tr):
            if t == "wall" or t == "player":
                return None
            if t[0] == "static":
                return None
            if t[0] == "line":
                l = self.lines[t[1]]
                if l["eType"] == ET_DESTRUCT:
                    return ("line", t[1]) if (1 << self.weapon) & 1 else None   # the axe only
                if l["eType"] != ET_LINE:
                    return None
                # a plain line: a monster in the same cell behind it is the target
                for t2 in tr[k + 1:]:
                    if t2[0] == "ent" and self.etype(self.ents[t2[1]]) == ET_ENEMY and \
                            (self.ents[t2[1]]["x"], self.ents[t2[1]]["z"]) == (l["x"], l["z"]):
                        return ("ent", t2[1])
                return None
            e = self.ents[t[1]]
            if (e["x"], e["z"]) == (self.x, self.z):   # things under the player
                continue
            et = self.etype(e)
            if et == ET_HUMAN:
                return None
            if et == ET_DESTRUCT:
                return ("ent", t[1]) if (1 << self.weapon) & self.edef(e)["parm"] else None
            if et == ET_FIRE:
                return ("ent", t[1]) if self.weapon == 1 else None
            if et == ET_ENEMY:
                return ("ent", t[1])
            return None
        return None

    def target_dist(self, tx, tz):
        """Grid distance (in cells) from the player to (tx, tz)."""
        return abs(tx - self.x) + abs(tz - self.z)

    def player_attack(self, target):
        """Attack target (from select_target) with the weapon in hand.

        False (no turn used) if there is not enough ammo; else spends the ammo,
        rolls every shot (a critical on the first shot doubles the rest), hurts
        or kills a monster (overkill when adjacent and the damage passes
        about 40% (256/637) of its max HP below zero) or breaks the object, and returns
        True.
        """
        w = self.weapon
        wi = WEAPONS[w]
        if wi[6] > 0 and self.ammo[wi[5]] < wi[6]:
            self.say("Not enough ammo!")
            return False
        self.ammo[wi[5]] -= wi[6]
        kind, j = target
        if kind == "line":
            tx, tz = self.lines[j]["x"], self.lines[j]["z"]
        else:
            tx, tz = self.ents[j]["x"], self.ents[j]["z"]
        tiles = self.target_dist(tx, tz)
        shots = wi[8]                       # numShots: hit rolls per attack
        enemy = kind == "ent" and self.etype(self.ents[j]) == ET_ENEMY
        tot = tarm = 0
        crit = False
        hit = HIT_MISS
        for s in range(shots):
            if enemy:
                c = self.ents[j]["c"]
                hit = calc_hit(self.p, w, c, tiles, self.rng)
                if hit:
                    if hit == HIT_CRIT and s == 0:
                        crit = True
                    dm, am = calc_damage(self.p, w, c, 512 if crit else 256, tiles, self.rng)
                    tot += dm
                    tarm += am
            else:
                hit = self.calc_hit_object(target, tiles)
        if enemy:
            e = self.ents[j]
            d = self.edef(e)
            if tot + tarm == 0:
                self.say("Missed!" if w != 1 else "No effect!")
            else:
                c = e["c"]
                over = tiles == 1 and (c.hp + c.armor - tot - tarm) <= -(((c.maxhp << 16) // 637) >> 8)
                self.say(("Crit! " if crit else "") + f"{tot + tarm} damage!")
                self.monster_pain(j, tot, tarm)
                if c.hp <= 0:
                    self.say(f"{d['name']} died!")
                    self.monster_died(j, overkill=over)
        else:
            if hit:
                if kind == "line":
                    self.line_destroyed(j)
                else:
                    self.destroy(j)
            elif w == 1:
                self.say("No effect!")
            else:
                self.say("Missed!")
        return True

    def calc_hit_object(self, target, tiles):
        """Hit roll for an object: against a dummy whose agility is 80% of the
        player's accuracy. Then the weapon must suit it: the axe for a jammed
        door, the extinguisher for fire, anything but the extinguisher for the
        rest. Returns 1 hit, 0 miss.
        """
        dummy = Combatant(0, 0, 1, 1, 1, 1)
        # agility = accuracy * 204/256: about an 80% base chance
        dummy.agility = (self.p.accuracy << 8) * 204 >> 16
        if calc_hit(self.p, self.weapon, dummy, tiles, self.rng) == HIT_MISS:
            return 0
        kind, j = target
        if kind == "line":
            return 1 if self.weapon == 0 else 0
        d = self.edef(self.ents[j])
        if d["eType"] == ET_FIRE:
            return 1 if self.weapon == 1 else 0
        return 0 if self.weapon == 1 else 1

    def line_destroyed(self, li):
        """A jammed door is chopped through: it opens for good, 1 XP, and its cell's use event runs.
        """
        l = self.lines[li]
        l["alive"] = False
        l["open"] = True                    # the line entity is removed either way
        self.say("Door cleared!")
        self.add_xp(1)
        self.run_tile(l["x"], l["z"], EVF_USE)

    def touch_tile(self, touched):
        """Touch the things in the player's cell: all of them after a move (touched), else only fire.
        """
        for j in self.entities_at(self.x, self.z):
            e = self.ents[j]
            d = self.edef(e)
            if not touched and d["eType"] not in (ET_FIRE, ET_FIRE2):
                continue
            self.touch(j)

    def touch(self, j):
        """Player touches entity j: pick up credits, armour, health, keys,
        inventory items, ammo or weapons (each refused when already full), or
        get burnt by fire.
        """
        e = self.ents[j]
        d = self.edef(e)
        et, es, parm = d["eType"], d["eSub"], d["parm"]
        if et == ET_PICKUP:
            # eSub 20 health, 21 armour, 22/23 credits, 24 key (parm: key bit)
            if es in (22, 23):
                self.credits += parm
            elif es == 21:
                if self.p.armor >= self.p.maxarmor:
                    self.say("Armor at maximum")
                    return
                self.add_armor(parm)
            elif es == 20:
                if self.p.hp >= self.p.maxhp:
                    self.say("Health at maximum")
                    return
                self.add_health(parm)
            elif es == 24:
                self.keys |= 1 << parm
            self.say(f"Got {d['name']}")
            e["flags"] &= ~F_LINKED
        elif et == ET_INV:
            k = es - 25                     # inventory slot: eSub 25.. -> 0..
            # (slots 0-2 medkits and Soul Sphere, 3 berserk, 4 dog collar)
            if self.inventory[k] == 99:
                self.say(f"Can't hold more {d['name']}s")
                return
            self.inventory[k] = min(99, self.inventory[k] + 1)
            self.say(f"Got {d['name']}")
            e["flags"] &= ~F_LINKED
        elif et in (ET_AMMO, ET_AMMO2):
            if not self.add_ammo(es, parm):
                self.say("Ammo at maximum")
                return
            self.say(f"Got {d['name']}")
            e["flags"] &= ~F_LINKED
        elif et == ET_WEAPON:
            # eSub is the weapon index; a new weapon is taken in hand
            new = not self.weapons & (1 << es)
            if new:
                self.weapon = es
            self.weapons |= 1 << es
            if WEAPONS[es][6]:
                self.add_ammo(WEAPONS[es][5], parm)
            self.say(f"Got {d['name']}")
            e["flags"] &= ~F_LINKED
        elif et == ET_FIRE:
            self.player_pain(1, 2)
            self.say("It burns!")
        elif et == ET_FIRE2:
            self.player_pain(10, 10)
            self.say("It really burns!!")

    def end_player_turn(self):
        """Game_advanceTurn only flags the monsters' turn; it runs once no
        dialog/password is open (the original's playing state waits for the
        dialog to close, and the dialog resumes its event first)."""
        self.monsters_pending = True
        self.settle()

    def modal(self):
        """True while a dialog or a door-code box is open."""
        return self.dialog is not None or self.password is not None

    def settle(self):
        """Run the pending monsters' turn once nothing modal is open (and wake visible monsters first).
        """
        if self.modal() or not getattr(self, "monsters_pending", False):
            return
        self.monsters_pending = False
        self.activate_visible()
        if not self.dead:
            self.monster_turn()

    # public actions -------------------------------------------------
    def turn(self, delta):
        """Turn left (-1) or right (+1). Free: runs the cell's facing events but gives no monster turn.
        """
        self.facing = (self.facing + delta) % 4
        self.run_tile(self.x, self.z, FACING_FLAG[self.facing] | EVF_MOVE)
        self.activate_visible()

    def move(self, back=False):
        """Step forward, or back if back. False if blocked or a leave event aborts.

        Runs the leave event of the old cell and the arrive event of the new one,
        touches what is there, then ends the turn.
        """
        d = (self.facing + 2) % 4 if back else self.facing
        dx, dz = DIRS[d]
        nx, nz = self.x + dx, self.z + dz
        if self.blocked(self.x, self.z, nx, nz, MASK_PLAYER_MOVE):
            return False
        self.abort = False                  # (nothing in this model sets it)
        self.run_tile(self.x, self.z, LEAVE_FLAG[d] | EVF_MOVE)
        if self.abort:
            return False
        self.x, self.z = nx, nz
        self.run_tile(nx, nz, ARRIVE_FLAG[d] | FACING_FLAG[self.facing] | EVF_MOVE)
        self.touch_tile(True)
        self.end_player_turn()
        return True

    def use(self):
        """Button 1: run the use event of the cell ahead; if there is none (or
        nothing ran), attack what select_target finds. A dialog or code box
        opened by the event postpones the turn (skip_turn).
        """
        fx, fz = DIRS[self.facing]
        self.skip_turn = False
        # the cell ahead's events, as a use (EVF_USE | 0x400)
        if self.run_tile(self.x + fx, self.z + fz, 0x500):
            if not self.skip_turn:
                self.touch_tile(False)
                self.end_player_turn()
            return
        t = self.select_target()
        if t is None:
            self.say("Nothing to use")
            return
        if self.player_attack(t):
            self.touch_tile(False)
            self.end_player_turn()

    def pass_turn(self):
        """Do nothing for one turn (button 2 + Down)."""
        self.say("Turn passed.")
        self.touch_tile(False)
        self.end_player_turn()

    def cycle_weapon(self, step):
        """Switch to the next (step 1) or previous (-1) weapon owned that needs no ammo or has some.
        """
        cur = self.weapon
        for k in range(1, 12):              # the 12 weapon slots, wrapping round
            w = (cur + step * k) % 12
            if self.weapons & (1 << w) and (WEAPONS[w][6] == 0 or self.ammo[WEAPONS[w][5]] > 0):
                self.weapon = w
                return

    def use_health_item(self):
        """PORT: no inventory menu - button 2 + Up uses the smallest medkit you
        carry (Sm Medkit, else Lg Medkit, else Soul Sphere)."""
        for k, es in ((0, 25), (1, 26), (2, 27)):
            if self.inventory[k]:
                d = next(dd for dd in self.defs if (dd["eType"], dd["eSub"]) == (4, es))
                if es == 27:
                    self.add_health(200)
                    self.add_armor(200)
                else:
                    self.add_health(d["parm"])
                self.inventory[k] -= 1
                self.end_player_turn()
                return True
        self.say("No health items")
        return False

    # -------------------------------------------------- events
    def run_tile(self, x, z, flags):
        """Run the event of cell (x, z) with flags, if it has one. Returns whether any command ran.
        """
        ev = self.event_at.get((x, z))
        if ev is None:
            return False
        return self.run_event(ev, 0, flags)

    def run_event(self, ev, start, flags):
        """Game_runEvent: run event ev's commands from index start.

        A command runs if it is still alive, belongs to the event's current
        state (bits 16-24 of a2), matches the key held (bits 12-15; the
        strongest key is added to flags) and shares a bit with flags (how the
        event was triggered). Commands with EVF_MODIFY are used up once they
        succeed. A command that opens a dialog or code box, or stops the event,
        sets suspend: the rest waits for resume(). Returns whether anything
        ran.
        """
        e = self.g["events"][ev]
        if (e["flags"] & 1) and (flags & EVF_MOVE):   # event flag 1: not by moving
            return False
        st = self.event_state[ev]
        n7 = 0 if st == 0 else (0x10000 << (st - 1))   # the state's bit in a2
        # the strongest key held, as a flag bit (keys bit 3, 2, 0, 1)
        if self.keys & 8:
            flags |= 0x8000
        elif self.keys & 4:
            flags |= 0x4000
        elif self.keys & 1:
            flags |= 0x2000
        elif self.keys & 2:
            flags |= 0x1000
        n8 = flags & 0xF000                 # the key bit (or none)
        ran = False
        cmds = e["cmds"]
        for k in range(start, len(cmds)):
            if not self.cmd_alive[ev][k]:
                continue
            a2 = cmds[k]["a2"]
            if n7:                          # state above 0: the command must name it
                if not a2 & n7:
                    continue
            elif a2 & 0x1FF0000:            # state 0: only commands for no state
                continue
            if (a2 & 0xF000) not in (0, n8):   # needs a different key
                continue
            if not flags & a2:              # not triggered this way
                continue
            self.suspend = None
            if self.exec_cmd(ev, k, cmds[k], flags):
                ran = True
                if a2 & EVF_MODIFY:
                    self.cmd_alive[ev][k] = False
            if self.suspend is not None:
                # remember where to continue (resume) and stop here
                self.suspend = (ev, k, flags, self.suspend)
                self.pending = self.suspend
                break
        return ran

    def find_event(self, x, z):
        """The event index at cell (x, z), or None."""
        return self.event_at.get((x, z))

    def exec_cmd(self, ev, k, c, flags):
        """Execute one event command c (index k of event ev). Returns True if it
        counted as run (False: a locked door, a door already in that state, or
        a key that is held). Sets self.suspend to "dialog", "password" or
        "stop" to pause or end the event.

        Arguments packed in a1: cells as x | z << 8, strings and entity
        indices as they are; see each op below.
        """
        op, a1 = c["op"], c["a1"]
        s = self.g["strings"]
        if op == 2:                       # CHANGEMAP
            if c["a1"] & 0x80000000:      # "show stats" exit: Sector 1 completed
                self.level_done = True
            else:                         # PORT: the way back to Junction (not in this port)
                self.say("The way back is sealed")
                self.suspend = "stop"
        elif op == 4:                     # MESSAGE
            self.say(s[a1])
        elif op == 5:                     # PAIN: a1 damage to health
            self.say("Pain!")
            self.player_pain(a1, 0)
        elif op == 7:                     # SHOW
            if a1 == 0xFF:
                return True
            e = self.ents[a1]
            # it appears: the first two things in its cell are killed or broken
            for j in self.entities_at(e["x"], e["z"])[:2]:
                if MASK_SHOW_KILL & (1 << self.etype(self.ents[j])):
                    if self.etype(self.ents[j]) == ET_ENEMY:
                        self.monster_died(j)
                    else:
                        self.destroy(j)
            e["flags"] |= F_LINKED
        elif op in (8, 26):               # DIALOG
            self.dialog = s[a1]
            self.suspend = "dialog"
            self.skip_turn = True
        elif op == 9:                     # GIVEMAP
            self.map_given = True
        elif op == 10:                    # PASSWORD
            self.password = (s[a1 & 0xFF], s[(a1 >> 8) & 0xFF])   # (code, prompt)
            self.suspend = "password"
            self.skip_turn = True
        elif op in (11, 19, 20):          # CHANGE/NEXT/PREV STATE
            tgt = self.find_event(a1 & 0xFF, (a1 >> 8) & 0xFF)   # the event at cell (x, z)
            if tgt is not None:
                st = self.event_state[tgt]
                if op == 11:
                    st = (a1 >> 16) & 0xFF
                elif op == 19 and st < 9:
                    st += 1
                elif op == 20 and st > 0:
                    st -= 1
                self.event_state[tgt] = st & 15
        elif op in (12, 13, 14):          # LOCK / UNLOCK / TOGGLE
            l = self.lines[a1]
            l["locked"] = True if op == 12 else False if op == 13 else not l["locked"]
        elif op in (6, 15, 16, 17):       # door lines: MOVELINE/MOVELINE2 toggle, OPENLINE, CLOSELINE
            l = self.lines[a1]
            if l["locked"]:
                return False
            if op == 15 and l["open"]:
                return False
            if op == 16 and not l["open"]:
                return False
            l["open"] = not l["open"]
        elif op == 18:                    # HIDE
            for j in self.entities_at(a1 & 0xFF, (a1 >> 8) & 0xFF):
                if self.etype(self.ents[j]) != ET_ENEMY:
                    self.ents[j]["flags"] = (self.ents[j]["flags"] | F_HIDDEN) & ~F_LINKED
        elif op in (21, 22):              # INC/DEC STAT
            # a1: stat (0 health, 1 armour, 2 credits, 3 XP), amount << 8
            n = (a1 >> 8) & 0xFF
            n = n if op == 21 else -n
            stat = a1 & 0xFF
            if stat == 0:
                self.add_health(n)
            elif stat == 1:
                self.add_armor(n)
            elif stat == 2:
                self.credits += n
            elif stat == 3:
                self.add_xp(n)
        elif op == 23:                    # REQSTAT
            # a1: stat, amount needed << 8, string shown if short << 16
            stat, need = a1 & 0xFF, (a1 >> 8) & 0xFF
            have = [self.p.hp, self.p.armor, self.credits, self.xp][stat]
            if have < need:
                self.say(["Insufficient health!", "Insufficient armor!", "Insufficient funds!",
                          "Insufficient XP!"][stat])
                self.suspend = "stop"
                txt = s[a1 >> 16]
                if txt:
                    self.dialog = txt
                    self.skip_turn = True
        elif op == 24:                    # FORCEMESSAGE: the status line text
            self.status_msg = s[a1] or None
        elif op == 37:                    # WAIT: PORT - no real-time pause; continue
            pass
        elif op == 40:                    # NOTE: a string id into the notebook
            self.notes.append(a1)
        elif op == 41:                    # CHECK_KEY
            need = {0: 1, 1: 2, 2: 4, 3: 8}[a1]   # key number -> keys bit
            if not self.keys & need:
                self.say(["Need Green Key", "Need Yellow Key", "Need Blue Key", "Need Red Key"][a1])
                self.suspend = "stop"
                return True
            return False
        # 27 SAVEGAME, 35 PARTICLES, 36 REFRESHVIEW: nothing to do here
        return True

    def resume(self, password=None):
        """Called when a dialog/password closes: continues the event that
        opened it with the next command (Game_runEvent(..., index+1, ...)).

        password: the code typed (None or empty: cancelled). A wrong or
        cancelled code ends the event, and returns before settle(), so
        the pending monsters' turn waits for the next action."""
        p = getattr(self, "pending", None)
        self.pending = None
        self.dialog = None
        if p is None:
            self.settle()
            return
        ev, k, flags, why = p
        if why == "dialog":
            self.run_event(ev, k + 1, flags)
        elif why == "password":
            want = self.password[0]
            self.password = None
            if password != want:
                if password:
                    self.say("Invalid code!")
                return
            self.say("Correct code!")
            self.run_event(ev, k + 1, flags)
        self.settle()


def load(path=Path(__file__).resolve().parent.parent / "analysis" / "gamedata.json", seed=0x1234):
    """A World for the gamedata at path (default analysis/gamedata.json)."""
    return World(json.loads(Path(path).read_text()), seed)


if __name__ == "__main__":
    w = load()
    print("spawn", w.x, w.z, w.facing, "rng calls", w.rng.calls, "messages", w.messages)
    print("active monsters", [w.edef(w.ents[i])["name"] for i in w.active])
    print("player", vars(w.p) if hasattr(w.p, '__dict__') else [getattr(w.p, s) for s in w.p.__slots__])
