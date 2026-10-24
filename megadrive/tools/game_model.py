#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Reference model of Doom RPG's game rules for Sector 1, in plain Python.
(It began with Sector 1; `Campaign` at the end now plays every map.)

This is the spec the game rules in the ROM implement (here, the 68000
assembler of src/rules.s; the file is shared with the Master System
version, whose Z80 engine is rom/game/*.asm), and the oracle the lockstep
tests (tools/md_lockstep.py, tools/md_campaign_lockstep.py) compare the
ROM against: both run the same 16-bit xorshift RNG and must make the same
calls to it in the same order, so after every player action the ROM's RAM
must equal this model's state byte for byte.

Main parts:
  XorShift16, Combatant          the RNG and the fighting stats
  setup_enemy, calc_hit,         combat maths (fixed point, as the original)
  calc_damage
  World                          one map in play: the player's actions
                                 (move, turn, use, pass_turn, ...), the
                                 monsters' turn, the tile events
  STORES, STORE_ITEMS            the vendors' price lists
  Campaign                       the whole game, map after map
Its input is a map's game data (tools/gamedata.py's JSON). Its users are
the autopilots (autopilot.py, campaign_pilot.py) and the lockstep tests.

Terms: a "cell" or "tile" is one square of the 32 x 32 map grid; a "line"
is a door or wall entity sitting in a cell; a "tile event" is the list of
script commands attached to a cell, run when the player arrives, leaves,
turns or uses it; "lockstep" means the ROM and the model play the same
inputs and are compared after every action.

Rules were re-expressed from reading the original J2ME decompile and the
GPL DoomRPG-RE reimplementation (Combat.c, CombatEntity.c, Entity.c,
Player.c, Game.c, DoomCanvas.c). It is a reimplementation of behaviour, not
a translation of their code. Port decisions (things the SMS version does
differently on purpose) are marked "PORT:".
"""
import json
from pathlib import Path

from game_rules import (WEAPONS, MONSTERS, weapon_damage_mult, PLAYER_START, CAMPAIGN_START, EVF_USE, EVF_MODIFY,
                        EVF_MOVE, FACING_FLAG, LEAVE_FLAG, ARRIVE_FLAG, MASK_PLAYER_MOVE,
                        MASK_MONSTER_MOVE, MASK_SHOT, MASK_SHOW_KILL, MASK_RAISE_FREE, W_RAISE)

# the Reactor's portal: the cell of its line (whose event runs when it closes)
# and the cells it sends monsters to (Game_updateSpawnPortals: x 1056, 992,
# 1120 at y 992) - fixed in the original's code, not in the map
PORTAL_CELL = (16, 14)
PORTAL_SPAWN_X, PORTAL_SPAWN_Z = (16, 15, 17), 15
DIRS = [(0, -1), (1, 0), (0, 1), (-1, 0)]          # facing 0=N 1=E 2=S 3=W
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
    """16-bit xorshift random number generator shared with the ROM.

    x ^= x<<7; x ^= x>>9; x ^= x<<8 (all 16-bit, period 65535). One byte
    is the low byte of the next state. It was chosen because it is cheap on
    an 8-bit CPU; the Mega Drive's src/rules.s runs the same sequence, so
    the model and the ROM draw identical numbers as long as they make the
    same calls in the same order. `calls` counts bytes drawn (for debugging
    lockstep mismatches).
    """

    def __init__(self, seed=0x1234):
        """Seeds the generator. A seed of 0 would lock xorshift at 0 for ever,
        so it becomes 1. The default 0x1234 is the ROM's seed.
        """
        self.x = seed & 0xFFFF or 1
        self.calls = 0

    def byte(self):
        """Advances the state once and returns its low byte (0..255).
        Side effect: increments `calls`.
        """
        x = self.x
        x ^= (x << 7) & 0xFFFF
        x ^= x >> 9
        x ^= (x << 8) & 0xFFFF
        self.x = x
        self.calls += 1
        return x & 0xFF

    def int32(self):
        """Four bytes, least significant first, as one unsigned 32-bit number
        (the original's NextInt, as the ROM builds it).
        """
        b = [self.byte() for _ in range(4)]
        return b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)


def clamp8(v):
    """Clamps v to 0..255: the stats are bytes in the ROM's RAM."""
    return 0 if v < 0 else 255 if v > 255 else v


class Combatant:
    """The fighting stats of the player or one monster (the original's
    CombatEntity). Every field is a byte (0..255): hp/maxhp health,
    armor/maxarmor armour, defense, strength, agility, accuracy.
    `mclass` is the monster class (MONSTERS index, the entity's eSub) or
    None for the player; it picks the weapon damage multiplier.
    """
    __slots__ = ("hp", "maxhp", "armor", "maxarmor", "defense", "strength", "agility", "accuracy", "mclass")

    def __init__(self, hp, armor, defense, strength, agility, accuracy, mclass=None):
        """Sets every stat from the arguments, clamped to a byte; hp and armor
        start full (equal to their maximums).
        """
        self.maxhp = self.hp = clamp8(hp)
        self.maxarmor = self.armor = clamp8(armor)
        self.defense, self.strength = clamp8(defense), clamp8(strength)
        self.agility, self.accuracy = clamp8(agility), clamp8(accuracy)
        self.mclass = mclass

    def exp(self):
        """XP the player earns for killing this combatant
        (CombatEntity_getExp): a weighted sum of its stats, /50 rounded up.
        """
        return (((self.defense + self.strength) * 5) + ((self.agility + self.accuracy) * 3)
                + ((self.maxhp + self.maxarmor) * 5) + 49) // 50


# ---------------------------------------------------------------- combat math
def setup_enemy(c, parm, rng):
    """CombatEntity_setupEnemy: every stat scaled by a random factor in
    [0.75, 1.25) x parm/256 (armour and defence share one factor).

    c: the monster's Combatant, loaded with its class's base stats.
    parm: the entity definition's parm (256 = 1.0; bigger for the tougher
    variants of a class). rng: draws one byte per factor (5 bytes in all).
    Changes c in place.
    """
    # i2 = parm/4 (the low end's offset), i3 = parm/2 (the factor's random span)
    i2 = (parm << 8) // 1024
    i3 = (i2 << 9) >> 8

    def factor():
        """One random scale factor, 8.8 fixed point (256 = 1.0): parm - parm/4
        plus up to parm/2 times a random byte/256.
        """
        return (parm + ((i3 * rng.byte()) >> 8)) - i2

    f = factor()
    # stat * factor: (stat << 8) * f is 16.16, >> 16 back to an integer
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

    tiles: distance to the target in tiles; range_min: the weapon's
    rangeMin. A result > 0 means the target is beyond the weapon's full-
    power range; the hit chance and damage then fall off per extra tile.
    """
    return tiles * tiles - range_min * range_min - 1


# results of calc_hit
HIT_MISS, HIT_HIT, HIT_CRIT = 0, 1, 2


def calc_hit(att, weapon, tgt, tiles, rng):
    """CombatEntity_calcHit: rolls whether `att` hits `tgt` with `weapon`.

    att, tgt: Combatants. weapon: WEAPONS index. tiles: distance in tiles
    (0 = a monster attacking the player, which skips the range test).
    Draws one rng byte. Returns HIT_MISS, HIT_HIT or HIT_CRIT.
    The chance `ch` is out of 256: attacker accuracy / target agility
    (scaled by 128/256) plus the weapon's rangeMax/200.
    """
    w = WEAPONS[weapon]
    agi = tgt.agility if tgt.agility else 1
    # accuracy/agility as 8.8 fixed point, halved, plus rangeMax * 256/200
    ch = (((att.accuracy << 16) // (agi << 8)) * 128 >> 8) + ((w[4] << 16) // 51200)
    ex = range_excess(tiles, w[3]) if tiles else -1
    if ex > 0 and w[3] == 0:
        return HIT_MISS                     # melee weapon, target not adjacent
    if w[5] != 2 and ex > 0:                # shells (shotguns) don't fall off
        # beyond rangeMin the chance drops by 76/256 (about 30%) of itself for
        # each further square distance step (1, 4, 9, ... tiles squared)
        dec = (ch * 76) >> 8
        k = 2
        d = 1
        while d < ex:
            ch -= dec
            d = k * k
            k += 1
    r = rng.byte()
    if r < ch:
        return HIT_CRIT if r < ((ch << 8) // 5120) else HIT_HIT  # crit: the lowest ch/20 of the roll
    return HIT_MISS


def calc_damage(att, weapon, tgt, mult, tiles, rng):
    """CombatEntity_calcDamage: rolls the damage of one hit.

    att, tgt: Combatants; weapon: WEAPONS index; mult: 256 normal, 512
    (critical hit) or 768 (berserker; 1536 with a crit); tiles: distance
    (0 for monster attacks). Draws one rng byte.
    Returns (health_damage, armor_damage): the damage is split by the
    weapon's armorShare/256 and both parts are rounded to integers.
    Internally everything is 8.8 fixed point (256 = 1 point of damage).
    """
    w = WEAPONS[weapon]
    r = rng.byte()
    dfn = tgt.defense if tgt.defense else 1
    # strength/defence as 8.8 fixed point
    ratio = ((att.strength << 16) // (dfn << 8)) & 0xFFFFFFFF
    # base damage: strMin..strMax, picked by the random byte (8.8 fixed point)
    a = (w[1] << 8) + ((r * ((w[2] - w[1]) << 8)) >> 8)
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
    # at least 1 point, at most 999 (255744 = 999 * 256)
    d = max(256, min(d, 255744))
    arm = (d * w[7]) >> 8
    return ((d - arm) + 128) >> 8, (arm + 128) >> 8


# ---------------------------------------------------------------- the world
# what carries over from one map to the next (Player_t)
CARRY = ("level", "xp", "nextxp", "credits", "weapons", "weapon", "ammo", "inventory", "keys", "completed",
         "disabled_weapons", "god")


class World:
    """One map in play. `carry` is the World of the map before (the player and
    the random numbers go on), `spawn` an (x, z, facing) other than the map's
    own; `start` the player a new game starts with.

    The map is a 32 x 32 grid of cells (x east, z south; index z*32 + x).
    State the ROM must match byte for byte (see md_lockstep.py):
      p            the player's Combatant
      ents         entity dicts: {"def": index into defs, "x", "z",
                   "flags": F_* bits, "c": Combatant (monsters), "move": steps
                   taken this turn}; the map's own entities, then 8 drop slots
                   (first_drop..) for items dropped by monsters
      lines        door/wall "line" entities: {"x", "z", "open", "locked",
                   "eType", "eSub", "alive", "tex", "flags"}
      event_state  per tile event, its current state (0..15)
      cmd_alive    per event command, False once a one-shot command has run
      active       monsters taking turns, in activation order
      rng          the shared XorShift16
    Modal state: `dialog` (text of an open dialog), `password` (prompt and
    expected code), `store` (open vendor); the event that opened them is
    saved in `pending` and continued by resume().
    """

    def __init__(self, gamedata, seed=0x1234, carry=None, spawn=None, start=PLAYER_START, single_map=True):
        """Builds the map's starting state from `gamedata` (one map's JSON from
        tools/gamedata.py). Rolls every monster's stats in map order (the order
        the rng is used in matters), runs the spawn cell's arrival event and
        wakes the monsters in view. `single_map` is True for the old
        Sector-1-only game (its exit ends the game) and False under Campaign.
        """
        g = gamedata
        self.g = g
        self.defs = g["defs"]
        self.wall = g["wall"]
        self.static = g["static_block"]
        # invisible thin walls on cell edges (Game_trace's eType 14/15 test):
        # cell -> [(type, edge 'W' / 'E' / 'N' / 'S')]
        self.thin = {}
        for t in g.get("thin", []):
            if t["axis"] == "x":
                edge = "W" if t["pos"] == t["x"] * 64 else "E"
            else:
                edge = "N" if t["pos"] == t["z"] * 64 else "S"
            self.thin.setdefault((t["x"], t["z"]), []).append((t["type"], edge))
        self.static_hidden = set()    # cells whose props HIDE / CHANGESPRITE took away
        if carry is None:
            self.rng = XorShift16(seed)
            p = dict(start)
            self.p = Combatant(p["hp"], p["armor"], p["defense"], p["strength"], p["agility"], p["accuracy"])
            self.p.maxarmor = p["maxarmor"]
            self.level, self.xp, self.nextxp = p["level"], p["xp"], p["nextxp"]
            self.credits, self.weapons, self.weapon = p["credits"], p["weapons"], p["weapon"]
            self.ammo, self.inventory, self.keys = list(p["ammo"]), list(p["inventory"]), p["keys"]
            self.completed = 0            # bit (map id - 1) per completed sector
            self.disabled_weapons = 0     # weapons put away by ENABLEWEAPONS 0
            self.god = False              # nothing hurts the player (after the Reactor's Cyberdemon)
        else:
            self.rng = carry.rng
            c = carry.p
            self.p = Combatant(0, 0, 0, 0, 0, 0)
            for a in Combatant.__slots__:
                setattr(self.p, a, getattr(c, a))
            for a in CARRY:
                v = getattr(carry, a)
                setattr(self, a, list(v) if isinstance(v, list) else v)
        self.x, self.z, self.facing = spawn if spawn is not None else g["spawn"]
        self.notes, self.status_msg, self.messages = [], None, []
        self.dead = False
        self.level_done = False
        self.map_given = False
        self.floor_rgb = tuple(g["floor_rgb"])
        self.ceil_rgb = tuple(g["ceiling_rgb"])
        self.berserk = 0              # berserker turns left
        self.store = None             # the vendor (0..3) while its menu is open
        self.change = None            # CHANGEMAP's argument, until the exit door opens
        self.exit = None              # ... then: where to go (the Campaign moves on)
        self.secret_turn = False      # a secret wall found this action (+5 XP once)
        self.single_map = single_map  # Sector 1 on its own (the exit ends the game)

        # entities: list of dicts; ents beyond the map's are 8 drop slots
        self.ents = []
        for e in g["entities"]:
            d = self.defs[e["def"]]
            ent = {"def": e["def"], "x": e["x"], "z": e["z"], "flags": 0, "c": None, "move": 0}
            # hidden entities wait off the grid until an event SHOWs them
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
                               "alive": l["eType"] == ET_DESTRUCT, "tex": l.get("tex", 0),
                               "flags": l.get("flags", 4)})
        self.event_state = [e["state"] for e in g["events"]]
        self.cmd_alive = [[True] * len(e["cmds"]) for e in g["events"]]
        self.event_at = {(e["x"], e["z"]): i for i, e in enumerate(g["events"])}
        self.active = []          # active monsters, activation order
        self.attackers = []       # this turn's attack queue (LIFO)
        self.dialog = None        # (string id, resume info) while a dialog is open
        self.password = None
        self.wait_resume = None
        self.pending_explosions = []
        # the Reactor's portal (EV_ACTIVE_PORTAL, Game_updateSpawnPortals)
        self.portal = False           # spawning monsters
        self.portal_cnt = 0           # turns to wait (spawnCount; goes below 0)
        self.portal_mon = None        # the monster it sent last (it waits while that one lives)
        self.couplings = 0            # power couplings destroyed (powerCouplingDeaths)

        # Entity_initspawn, in map order (rng order matters)
        for ent in self.ents[:self.first_drop]:
            d = self.defs[ent["def"]]
            if d["eType"] == ET_ENEMY:
                base, _atk, _shots = MONSTERS[d["eSub"]]
                c = Combatant(*base, mclass=d["eSub"])
                setup_enemy(c, d["parm"], self.rng)
                ent["c"] = c
                # each monster uses its first or second attack, chosen at random
                if self.rng.byte() & 1:
                    ent["flags"] |= F_ATK2
                ent["flags"] |= F_ALIVE
            elif d["eType"] == ET_FIRE or (d["eType"] == ET_DESTRUCT and d["eSub"] != 4):
                ent["flags"] |= F_ALIVE       # (power couplings: once the portal opens)
        self.msg = []
        if self.disabled_weapons:             # Player_setup
            self.restore_weapons()
        # the spawn cell's event, as if arriving from every direction (0x40F = EVF_MOVE
        # plus the four arrival bits) and facing the start direction
        self.run_tile(self.x, self.z, 0x40F | FACING_FLAG[self.facing])
        self.activate_visible()

    # -------------------------------------------------- small helpers
    def edef(self, ent):
        """The entity definition (dict from gamedata "defs") of `ent`, or None for an empty drop slot."""
        return self.defs[ent["def"]] if ent["def"] is not None else None

    def etype(self, ent):
        """The entity type (ET_*) of `ent`."""
        return self.edef(ent)["eType"]

    def say(self, text):
        """Adds a message to the message log (the model's equivalent of the HUD text; not compared with the ROM)."""
        self.messages.append(text)

    def entities_at(self, x, z):
        """Indices of the entities on the grid (F_LINKED) at cell (x, z), in entity order."""
        return [i for i, e in enumerate(self.ents)
                if e["flags"] & F_LINKED and e["def"] is not None and e["x"] == x and e["z"] == z]

    def line_at(self, x, z):
        """Index of the closed line (door or wall entity) at cell (x, z), or None."""
        for i, l in enumerate(self.lines):
            if l["x"] == x and l["z"] == z and not l["open"]:
                return i
        return None

    def trace(self, sx, sz, dx, dz, mask, self_idx=None):
        """Game_trace along a row/column. Returns list of ('wall'|'player'|
        ('line', i)|('static', etype)|('ent', i)).

        Walks every cell from (sx, sz) to (dx, dz), both included, which must
        share a row or a column. `mask` has bit n set for each entity type n
        that counts as a hit; bit 8 (0x100) makes the player's cell a hit.
        `self_idx` is the entity doing the trace (it does not hit itself).
        Stops at a wall, the player, or an invisible thin wall crossed on the
        way; returns at most 8 hits, nearest first.
        """
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
            st = self.static_at(x, z)
            if st and mask & (1 << st):
                out.append(("static", st))
            for i in self.entities_at(x, z):
                if i != self_idx and mask & (1 << self.etype(self.ents[i])):
                    out.append(("ent", i))
            for typ, edge in self.thin_at(x, z):      # (linked at load: after what moved in)
                if mask & (1 << typ) and self.crosses(edge, x, z, sx, sz, dx, dz):
                    out.append(("static", typ))
                    return out[:8]
            if len(out) >= 8:
                return out[:8]
        return out

    def static_at(self, x, z):
        """The blocking prop type at (x, z) (a static entity type, 0 = none), unless HIDE took it away."""
        return 0 if (x, z) in self.static_hidden else self.static[z * 32 + x]

    def thin_at(self, x, z):
        """The invisible thin walls on the edges of cell (x, z): [(type, edge)], unless hidden."""
        return () if (x, z) in self.static_hidden else self.thin.get((x, z), ())

    @staticmethod
    def crosses(edge, x, z, sx, sz, dx, dz):
        """Does a trace from (sx, sz) to (dx, dz), passing cell (x, z), cross
        that cell's `edge` (centre to centre, as Game_trace measures)?"""
        if sz == dz and sx != dx and edge in "WE":
            if dx > sx:
                return (edge == "W" and x > sx) or (edge == "E" and x < dx)
            return (edge == "E" and x < sx) or (edge == "W" and x > dx)
        if sx == dx and sz != dz and edge in "NS":
            if dz > sz:
                return (edge == "N" and z > sz) or (edge == "S" and z < dz)
            return (edge == "S" and z < sz) or (edge == "N" and z > dz)
        return False

    def blocked(self, sx, sz, dx, dz, mask, self_idx=None):
        """True if a trace (see trace) from (sx, sz) to (dx, dz) hits anything in `mask`."""
        return len(self.trace(sx, sz, dx, dz, mask, self_idx)) > 0

    # -------------------------------------------------- player stats
    def add_health(self, n):
        """Heals by n (or hurts, if n < 0), keeping hp within 0..maxhp; reports a gain."""
        before = self.p.hp
        self.p.hp = max(0, min(self.p.hp + n, self.p.maxhp))
        if self.p.hp > before:
            self.say(f"Gained {self.p.hp - before} health")

    def add_armor(self, n):
        """Adds n armour, up to maxarmor; reports a gain."""
        before = self.p.armor
        self.p.armor = min(self.p.armor + n, self.p.maxarmor)
        if self.p.armor > before:
            self.say(f"Gained {self.p.armor - before} armor")

    def add_ammo(self, t, n):
        """Adds n of ammo type t (AMMO_NAMES index), up to 99.
        Returns False (and adds nothing) if that ammo was already at 99.
        """
        if self.ammo[t] == 99:
            return False
        self.ammo[t] = min(99, self.ammo[t] + n)
        return True

    def add_xp(self, n):
        """Gives n XP; every time xp reaches nextxp the player levels up (xp carries over)."""
        self.say(f"Gained {n} XP!")
        self.xp += n
        while self.xp >= self.nextxp:
            self.xp -= self.nextxp
            self.level_up()

    def level_up(self):
        """Player_levelUp: next level, nextxp = level*20 + 60, health refilled.
        Draws 6 rng bytes: +3..5 max health, +3..5 max armour, +1..2 to each of
        defence, strength, agility and accuracy; every stat is capped at 99.
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
        """Player_pain: the player takes `dmg` health and `arm` armour damage.

        Nothing happens in god mode. With a dog out (weapons 9..11) the dog's
        health (ammo type 5) takes all of it, and the dog goes when it reaches 0.
        Otherwise armour absorbs `arm` (any excess goes to health), warnings are
        given when health crosses about 10% and 30%, and the player dies at 0.
        """
        if self.god:                      # Player_pain
            return
        if self.weapon in (9, 10, 11) and self.ammo[5] > 0:      # the dog takes it all
            self.ammo[5] = max(0, self.ammo[5] - (dmg + arm))
            self.say(f"Dog took {dmg + arm} damage!")
            if self.ammo[5] == 0:
                self.weapons &= ~0xE00
                self.select_next_weapon()
                self.say("Dog died!")
            return
        self.say(f"{dmg + arm} damage!")
        if dmg + arm:
            a = self.p.armor
            if a < arm:
                dmg += arm - a
                self.p.armor = 0
            else:
                self.p.armor = a - arm
            # health before (h1) and after (h2) as a fraction of maxhp, x256: 26 = ~10%, 78 = ~30%
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
        """Puts monster i in the active list (it takes turns from now on), once."""
        e = self.ents[i]
        if not e["flags"] & F_ACTIVE:
            e["flags"] |= F_ACTIVE
            self.active.append(i)

    def deactivate(self, i):
        """Takes monster i out of the active list."""
        e = self.ents[i]
        if e["flags"] & F_ACTIVE:
            e["flags"] &= ~F_ACTIVE
            self.active.remove(i)

    def visible(self, x, z):
        """PORT: the original activates a monster the first time the renderer
        draws it. Our views are pre-rendered, so 'drawn' becomes: within 5
        cells ahead, inside the 90-degree cone, with a straight grid line of
        sight not crossing a wall or closed door.

        (x, z): the cell to test. Returns True if it counts as seen from the
        player's cell and facing. (On the Mega Drive the view is drawn live,
        but the ROM keeps this rule so the two agree.)
        """
        fx, fz = DIRS[self.facing]
        rx, rz = -fz, fx  # the player's right-hand direction
        dx, dz = x - self.x, z - self.z
        depth = dx * fx + dz * fz  # cells ahead
        lat = dx * rx + dz * rz  # cells to the side (negative = left)
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
        """Activates every living, linked, inactive monster of the map that visible() says the player can see."""
        for i, e in enumerate(self.ents[:self.first_drop]):
            if (e["flags"] & (F_LINKED | F_ALIVE | F_ACTIVE)) == (F_LINKED | F_ALIVE) and \
                    self.etype(e) == ET_ENEMY and self.visible(e["x"], e["z"]):
                self.activate(i)

    def monster_died(self, i, overkill=False):
        """Monster i dies: XP for the player, it leaves the active list and the
        grid, leaves a corpse (unless `overkill` or it explodes), may drop an
        item (spawn_drop), and runs the special deaths: the Cyberdemon opens
        the Reactor's exit and turns on god mode, Kronos runs a fixed event,
        a Pain Elemental class (eSub 7) releases its two lost souls.
        """
        e = self.ents[i]
        if not e["flags"] & F_ALIVE:
            return
        e["flags"] &= ~F_ALIVE
        self.add_xp(e["c"].exp())
        self.deactivate(i)
        e["flags"] &= ~F_LINKED
        es = self.edef(e)["eSub"]
        if not overkill and es != 8:          # eSub 8 (Ghoul, Fiend, Revenant) explodes
            e["flags"] |= F_CORPSE
        self.spawn_drop(i)
        if es == 12:                          # the Cyberdemon: the Reactor's exit opens
            self.run_tile(18, 19, EVF_USE)     # (cell fixed in the original's code)
            if e["flags"] & F_CORPSE:          # its body bursts; from now on nothing hurts
                e["flags"] = (e["flags"] & ~F_CORPSE) | F_HIDDEN
                self.god = True
        elif es == 13:                        # Kronos (Game_executeTile(1984, 0): no event
            self.run_tile(31, 0, EVF_USE)      # there in the maps we have)
        if es == 7 and not e["flags"] & F_HIDDEN and "spawns" in self.g["entities"][i]:
            # Beholder / Rahovart / Pain Elemental: the next two map things (lost
            # souls) appear, on its cell and beside it (once: F_HIDDEN marks it)
            e["flags"] |= F_HIDDEN
            s1, s2 = self.g["entities"][i]["spawns"]
            x, z = e["x"], e["z"]
            if s1 != 0xFFFF:
                if not self.cell_taken(x, z):
                    self.place(s1, x, z)
            if s2 != 0xFFFF:
                for (ox, oz) in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                    nx, nz = x + ox, z + oz
                    if not self.cell_taken(nx, nz) and (nx, nz) != (self.x, self.z):
                        self.place(s2, nx, nz)
                        break

    def cell_taken(self, x, z):
        """Game_findMapEntityXYFlag(x, z, 0xFF87) != NULL; PORT: walls and
        cells off the map count as taken too (the original could put a lost
        soul inside a wall).
        """
        if not (0 <= x < 32 and 0 <= z < 32) or self.wall[z * 32 + x]:
            return True
        return bool(self.trace(x, z, x, z, MASK_MONSTER_MOVE & ~0x100))

    def place(self, j, x, z):
        """Puts entity j on the grid at (x, z) (links it)."""
        e = self.ents[j]
        e["x"], e["z"] = x, z
        e["flags"] |= F_LINKED

    def spawn_drop(self, i):
        """Entity_spawnDropItem: monster i may drop an item where it died.

        Draws one 32-bit random number (always, even when nothing drops). No
        drop if it already dropped, for dogs (1), the Cyberdemon (12), Kronos
        (13), or 51/256 of the time. Otherwise the low bit picks a pickup
        (health, armour, credits) or a class-specific ammo/item, and the
        result goes into the next of the 8 drop slots (round robin, so the
        ninth drop reuses the first slot).
        """
        e = self.ents[i]
        d = self.edef(e)
        rnd = self.rng.int32()
        es = d["eSub"]
        if e["flags"] & F_DROPPED or es in (1, 12, 13) or (rnd & 0xFF) < 51:
            return
        e["flags"] |= F_DROPPED
        # n bit 0: pickup or class item; n5, n6: two more random bytes
        n = rnd >> 8
        n2 = n >> 1
        n5, n6 = n2 & 0xFF, (n2 >> 8) & 0xFF
        dt = None
        if n & 1:
            if n5 < 115:
                dt = (3, 20)
            elif n5 < 230:
                dt = (3, 21)
            else:
                n7 = {294: 3, 467: 8, 640: 13}.get(d["parm"], 0)  # tougher variants (larger parm) drop big credits more often
                dt = (3, 23) if n5 < 230 + n7 else (3, 22)
        else:
            b3, b4 = n5 < 76, n6 < 15
            # monster class -> (eType, eSub) of the drop; 16 and 6 are both ammo types
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
        """The trace mask for monster `ent`'s moves: flying classes ignore some
        blockers (4 and 13 ignore types 10 and 11 (fires), 6 and 7 type 11,
        10 type 10).
        """
        m = MASK_MONSTER_MOVE
        es = self.edef(ent)["eSub"]
        if es in (4, 13):
            m &= ~3072
        elif es in (6, 7):
            m &= ~2048
        elif es == 10:
            m &= ~1024
        return m

    def can_step(self, ent, i, sx, sz, nx, nz, mask):
        """True if monster i can step from (sx, sz) to the next cell (nx, nz) with `mask`. (`ent` is unused.)"""
        return not self.blocked(sx, sz, nx, nz, mask, i)

    def calc_path(self, i, sx, sz, dx, dz, mask):
        """Entity_calcPath: how good a cell is as a step towards (dx, dz).

        From (sx, sz) it takes up to two greedy steps (each to the free
        neighbour nearest the goal). Returns 0 or 1 if the goal is reached in
        one or two steps, else 2 + the squared distance left. Lower is better.
        """
        if (sx, sz) == (dx, dz):
            return 0
        vx = vz = None
        for k in range(2):
            best = 999999
            stx = stz = 0
            # the first free neighbour wins ties (east, west, south, north)
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
        """Entity_aiGoal_MOVE: one step toward (dx,dz) by a 2-step greedy look-ahead.

        Scores each free neighbour with calc_path, keeps the best (ties all
        kept) and picks one with an rng byte. Moves monster i there and
        returns True, or returns False if it cannot move.
        """
        e = self.ents[i]
        sx, sz = e["x"], e["z"]
        mask = self.monster_mask(e)
        best = 9999
        order = []
        for (ox, oz, code) in ((1, 0, 2), (-1, 0, 3), (0, 1, 1), (0, -1, 0)):
            if not self.can_step(e, i, sx, sz, sx + ox, sz + oz, mask):
                continue
            pd = self.calc_path(i, sx + ox, sz + oz, dx, dz, mask)
            # the east step (code 2) is tried first: it is kept unless it is unreachable
            # (9999), and joins the list only on an exact 9999 tie (as the original does)
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
        pick = order[(self.rng.byte() & 3) % len(order)]  # random choice among the equally good steps
        ox, oz = {0: (0, -1), 1: (0, 1), 2: (1, 0), 3: (-1, 0)}[pick]
        e["x"], e["z"] = sx + ox, sz + oz
        return True

    def queue_attack(self, i, target=None):
        """Puts monster i in this turn's attack queue (at the front: the queue
        runs last in, first out), once. target: None = the player, ("ent", j)
        = a destructible in the way, ("raise", j) = an Archvile raising corpse j.
        """
        if i not in [a for a, _t in self.attackers]:
            self.attackers.insert(0, (i, target))

    def attack_weapon(self, e):
        """The WEAPONS index of the attack monster `e` uses now (its first or second, by F_ATK2)."""
        _base, atk, _shots = MONSTERS[self.edef(e)["eSub"]]
        return atk[1] if e["flags"] & F_ATK2 else atk[0]

    def ai_think(self, i):
        """Entity_aiThink for active monster i: decides to attack or move.

        An Archvile (eSub 10) may first pick a corpse within reach to raise,
        otherwise it alternates its attacks. A monster lined up with the
        player and in range (nothing in the way, or a destructible it attacks
        instead) attacks; otherwise, or now and then (rng >= 217) even when it
        could, it moves towards the cell in front of the player. Returns
        "moved" if it stepped (it may step again in ai_after_step), else None.
        """
        e = self.ents[i]
        d = self.edef(e)
        px, pz = self.x, self.z
        if d["eSub"] == 10:
            # 84/256 of the time: look for a corpse to raise
            if self.rng.byte() < 84:
                for j in [k for k in range(self.first_drop) if self.ents[k]["flags"] & F_CORPSE]:
                    c = self.ents[j]
                    dd = ((c["x"] - e["x"]) ** 2 + (c["z"] - e["z"]) ** 2) * 4096  # squared distance in world units (64 per cell)
                    # skip: on its own cell, more than 2 cells away (0x4000 = 4 * 4096), taken, or its cell is not free
                    if dd == 0 or dd > 0x4000 or c["flags"] & F_RAISING or \
                            self.blocked(c["x"], c["z"], c["x"], c["z"], MASK_RAISE_FREE):
                        continue
                    c["flags"] |= F_RAISING
                    self.queue_attack(i, ("raise", j))
                    return
            e["flags"] ^= F_ATK2
        w = self.attack_weapon(e)
        rng_min = WEAPONS[w][3]
        reach = 1 + rng_min  # attack reach in cells
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

        Fast monsters (classes 4 and 13) take up to 3 steps a turn, the rest
        one. Returns True if monster i stepped again (call it again). After its
        last step a lunging class (1, 4, 5, 13) next to the player with a
        clear line queues an attack.
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

    def update_portal(self):
        """Game_updateSpawnPortals: while the Reactor's portal is open, every
        few turns it brings one of the map's living monsters that are not
        yet fighting (in the original, its inactive list) in front of it,
        and waits while that one lives. PORT: the original tests cells 14
        and 18 but places on 15 and 17, and stops for good when all three
        are taken; here the cell it tests is the cell it uses, and it tries
        again later.

        Called at the start of every monster turn. Uses the rng: one 32-bit
        number to pick the monster, one byte for the next wait (1..4 turns).
        """
        if not self.portal:
            return
        m = self.portal_mon
        if m is not None and self.ents[m]["flags"] & F_ALIVE:
            return
        self.portal_cnt -= 1
        if self.portal_cnt + 1 > 0:
            return
        pool = [i for i, e in enumerate(self.ents[:self.first_drop])
                if self.etype(e) == ET_ENEMY and
                (e["flags"] & (F_ALIVE | F_LINKED | F_ACTIVE | F_HIDDEN)) == (F_ALIVE | F_LINKED)]
        if not pool:
            self.portal = False
            return
        j = pool[(self.rng.int32() & 4095) % len(pool)]
        self.portal_mon = None
        for cx in PORTAL_SPAWN_X:
            if not self.cell_taken(cx, PORTAL_SPAWN_Z) and (cx, PORTAL_SPAWN_Z) != (self.x, self.z):
                e = self.ents[j]
                e["x"], e["z"] = cx, PORTAL_SPAWN_Z
                self.portal_mon = j
                self.say(f"Portal spawns {self.edef(e)['name']}!")
                break
        self.portal_cnt = 1 + (self.rng.byte() & 3)

    def monster_turn(self):
        """The monsters' turn: the portal, then every active monster thinks
        (ai_think), the movers keep stepping until all are done, then the
        queued attacks run (stopping if the player dies), and newly visible
        monsters wake up.
        """
        self.update_portal()
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
        """Monster i carries out its queued attack on `target` (see queue_attack).

        A raise brings corpse j back with full health, if its cell is free.
        An attack on the player rolls each shot (calc_hit, calc_damage; a crit
        on the first shot doubles every shot), then calls player_pain. An
        attack on a destructible just destroys it.
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
                h = calc_hit(e["c"], w, self.p, 0, self.rng)
                if h:
                    if h == HIT_CRIT and s == 0:
                        crit = True
                    dm, am = calc_damage(e["c"], w, self.p, 512 if crit else 256, 0, self.rng)
                    tot += dm
                    tarm += am
            else:
                tot += 1
        if target is None:
            if tot + tarm == 0:
                self.say("Dodged!")
            else:
                self.player_pain(tot, tarm)
        else:
            self.destroy(target[1])

    # -------------------------------------------------- destruction
    def destroy(self, j):
        """Destroys object entity j (a fire, a barrel, a crate, a power coupling).

        eSub 1 (barrel) explodes; eSub 2 (crate) rolls one rng byte for its
        contents (a trap explosion, credits, armour, health or ammo: the crate
        becomes that pickup); eSub 4 (power coupling): the second one closes
        the Reactor's portal. Pending explosions then run.
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
            elif r < 213:
                dt = (6, self.rng.byte() % 5)
            if dt:
                di = next((k for k, dd in enumerate(self.defs) if (dd["eType"], dd["eSub"]) == dt), None)
                if di is not None:
                    e["def"] = di
                    e["flags"] = F_LINKED
        elif d["eType"] == ET_DESTRUCT and d["eSub"] == 4:     # a power coupling
            self.couplings += 1
            if self.couplings == 2:           # the portal closes (its line's event)
                self.portal = False
                self.run_tile(PORTAL_CELL[0], PORTAL_CELL[1], EVF_USE)
        self.run_explosions()

    def run_explosions(self):
        """Runs queued explosions in order: 5..28 damage (from a 32-bit random
        number) to the four neighbouring cells, half that to the four diagonal
        ones. An explosion can queue more (barrels next to barrels).
        """
        while self.pending_explosions:
            x, z = self.pending_explosions.pop(0)
            r = (self.rng.int32() & 0xFF) // 11
            dmg = r + 5
            for (ox, oz) in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                self.hurt_at(x + ox, z + oz, dmg, dmg)
            if dmg >> 1:
                for (ox, oz) in ((-1, 1), (1, 1), (1, -1), (-1, -1)):
                    self.hurt_at(x + ox, z + oz, dmg >> 1, dmg >> 1)

    def hurt_at(self, x, z, dmg, arm):
        """Explosion damage at cell (x, z): hurts the player if there, else the
        first thing that a shot would hit there (a door or a prop protects the
        cell): a monster takes the damage (and may die), most other objects are
        destroyed; fires and power couplings are not.
        """
        if (x, z) == (self.x, self.z):
            self.player_pain(dmg, arm)
            return
        if not (0 <= x < 32 and 0 <= z < 32) or self.wall[z * 32 + x]:
            return
        li = self.line_at(x, z)
        if li is not None and MASK_SHOT & (1 << self.lines[li]["eType"]):
            return                          # first thing on the tile is a door: not hurt
        st = self.static_at(x, z)
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
        """Monster j takes dmg health and arm armour damage (armour absorbs arm, any excess goes to health)."""
        c = self.ents[j]["c"]
        if c.armor < arm:
            dmg += arm - c.armor
            c.armor = 0
        else:
            c.armor -= arm
        c.hp = max(0, c.hp - dmg)

    # -------------------------------------------------- player actions
    def select_target(self):
        """Player_selectTarget: what pressing "use" would attack.

        Traces up to 8 cells ahead with MASK_SHOT. Returns ("ent", i) or
        ("line", i) for the first thing the current weapon can hit, or None:
        a jammed door (destructible line) only with the axe, a destructible
        whose parm bit for the weapon is set, a fire only with the
        extinguisher (weapon 1), any monster; a monster standing in an open
        doorway line is also found. Walls, props and people stop the search.
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
                    return ("line", t[1]) if (1 << self.weapon) & 1 else None  # only weapon 0 (the axe)
                if l["eType"] != ET_LINE:
                    return None
                for t2 in tr[k + 1:]:
                    if t2[0] == "ent" and self.etype(self.ents[t2[1]]) == ET_ENEMY and \
                            (self.ents[t2[1]]["x"], self.ents[t2[1]]["z"]) == (l["x"], l["z"]):
                        return ("ent", t2[1])
                return None
            e = self.ents[t[1]]
            if (e["x"], e["z"]) == (self.x, self.z):
                continue
            et = self.etype(e)
            if et == ET_HUMAN:
                return None
            if et == ET_DESTRUCT:
                return ("ent", t[1]) if (1 << self.weapon) & self.edef(e)["parm"] else None  # parm: bit per weapon that can break it
            if et == ET_FIRE:
                return ("ent", t[1]) if self.weapon == 1 else None
            if et == ET_ENEMY:
                return ("ent", t[1])
            return None
        return None

    def target_dist(self, tx, tz):
        """Distance in cells to (tx, tz), measured along the grid (targets are always in a straight line)."""
        return abs(tx - self.x) + abs(tz - self.z)

    def player_attack(self, target):
        """Player_fireWeapon at `target` (from select_target).

        Returns False with no turn spent if the weapon is put away or there is
        not enough ammo; otherwise spends the ammo, rolls each shot (monsters:
        calc_hit and calc_damage; objects: calc_hit_object), applies the
        damage, and returns True. A kill from an adjacent cell that leaves the
        monster far below 0 health is an overkill (no corpse).
        """
        w = self.weapon
        wi = WEAPONS[w]
        if self.disabled_weapons and not self.weapons & (1 << w):
            return False                        # Player_fireWeapon: weapons put away
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
        shots = wi[8]
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
                    n = 768 if self.berserk else 256  # berserker: triple damage
                    dm, am = calc_damage(self.p, w, c, 2 * n if crit else n, tiles, self.rng)
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
                over = tiles == 1 and (c.hp + c.armor - tot - tarm) <= -(((c.maxhp << 16) // 637) >> 8)  # overkill: below -maxhp*256/637 (about 40%)
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
        """Whether a shot at an object hits (returns 1) or not (0). Rolls
        calc_hit against a dummy with agility = 80% of the player's accuracy
        (one rng byte), then the weapon must suit the object: the axe for a
        jammed door, the extinguisher only for fires, anything else for the rest.
        """
        dummy = Combatant(0, 0, 1, 1, 1, 1)
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
        """A jammed door (line li) is chopped down: it opens for good, +1 XP, and its cell's event runs as if used."""
        l = self.lines[li]
        l["alive"] = False
        l["open"] = True                    # the line entity is removed either way
        self.say("Door cleared!")
        self.add_xp(1)
        self.run_tile(l["x"], l["z"], EVF_USE)

    def touch_tile(self, touched):
        """Touches the entities on the player's cell: all of them if `touched`
        (the player just stepped there), else only fires (which burn every
        turn the player stands in them).
        """
        for j in self.entities_at(self.x, self.z):
            e = self.ents[j]
            d = self.edef(e)
            if not touched and d["eType"] not in (ET_FIRE, ET_FIRE2):
                continue
            self.touch(j)

    def touch(self, j):
        """The player touches entity j: picks up credits, armour, health, keys,
        inventory items, ammo or weapons (each unlinked once taken; refused
        when already full), or gets burnt by a fire (1 health + 2 armour) or a
        big fire (10 + 10).
        """
        e = self.ents[j]
        d = self.edef(e)
        et, es, parm = d["eType"], d["eSub"], d["parm"]
        if et == ET_PICKUP:
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
                self.keys |= 1 << parm  # key bits: 1 green, 2 yellow, 4 blue, 8 red
            self.say(f"Got {d['name']}")
            e["flags"] &= ~F_LINKED
        elif et == ET_INV:
            k = es - 25  # inventory slot: 0 small medkit, 1 large, 2 soul sphere, 3 berserker, 4 dog collar
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
            new = not (self.weapons | self.disabled_weapons) & (1 << es)
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
        dialog/password/store is open (the original's playing state waits for
        the dialog to close, and the dialog resumes its event first).

        Also counts down the berserker.
        """
        if self.berserk:
            self.berserk -= 1
            if not self.berserk:
                self.say("Berserker expired!")
        self.monsters_pending = True
        self.settle()

    def modal(self):
        """True while a dialog, a code prompt or a vendor's menu is open (no monster turn until it closes)."""
        return self.dialog is not None or self.password is not None or self.store is not None

    def settle(self):
        """Runs the pending monster turn, if one is pending and nothing modal is open."""
        if self.modal() or not getattr(self, "monsters_pending", False):
            return
        self.monsters_pending = False
        self.activate_visible()
        if not self.dead:
            self.monster_turn()

    # public actions -------------------------------------------------
    def turn(self, delta):
        """Player action: turn by `delta` quarter turns (-1 left, +1 right).
        Runs the cell's facing event; no monster turn.
        """
        self.secret_turn = False
        self.facing = (self.facing + delta) % 4
        self.run_tile(self.x, self.z, FACING_FLAG[self.facing] | EVF_MOVE)
        self.activate_visible()

    def move(self, back=False):
        """Player action: one step forward (or back). Returns False if blocked
        (nothing happens) or a leave event stopped it (self.abort). Otherwise
        runs the leave event on the old cell and the arrival event on the new
        one, touches what is there and ends the turn.
        """
        self.secret_turn = False
        d = (self.facing + 2) % 4 if back else self.facing
        dx, dz = DIRS[d]
        nx, nz = self.x + dx, self.z + dz
        if self.blocked(self.x, self.z, nx, nz, MASK_PLAYER_MOVE):
            return False
        self.abort = False
        self.run_tile(self.x, self.z, LEAVE_FLAG[d] | EVF_MOVE)
        if self.abort:
            return False
        self.x, self.z = nx, nz
        self.run_tile(nx, nz, ARRIVE_FLAG[d] | FACING_FLAG[self.facing] | EVF_MOVE)
        self.touch_tile(True)
        self.end_player_turn()
        return True

    def use(self):
        """Player action: "use" the cell ahead. Its event runs first (0x500 =
        EVF_USE | EVF_MOVE); if nothing ran, attacks select_target's target.
        A turn passes unless the event opened something modal (skip_turn) or
        there was nothing to use.
        """
        self.secret_turn = False
        fx, fz = DIRS[self.facing]
        self.skip_turn = False
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
        """Player action: do nothing; the monsters take their turn."""
        self.secret_turn = False
        self.say("Turn passed.")
        self.touch_tile(False)
        self.end_player_turn()

    def cycle_weapon(self, step):
        """Player action: the next (step +1) or previous (-1) owned weapon that
        has ammo, among the 12 player weapons (wrapping round). Takes no turn.
        """
        self.secret_turn = False
        cur = self.weapon
        for k in range(1, 12):
            w = (cur + step * k) % 12
            if self.weapons & (1 << w) and (WEAPONS[w][6] == 0 or self.ammo[WEAPONS[w][5]] > 0):
                self.weapon = w
                return

    # weapons ------------------------------------------------------
    def select_next_weapon(self):
        """Player_selectNextWeapon: the next usable weapon up, else from the bottom."""
        cur = self.weapon
        for i in list(range(cur + 1, 12)) + (list(range(0, cur)) if cur != 0 else []):
            if self.weapons & (1 << i) and (WEAPONS[i][6] == 0 or self.ammo[WEAPONS[i][5]] > 0):
                self.weapon = i
                return

    def restore_weapons(self):
        """Player_restoreWeapons (ENABLEWEAPONS 1, a new map): the weapons put
        away come back (a dog already with you is not doubled).

        0xE00 = bits 9..11, the three dog "weapons".
        """
        if self.weapons & 0xE00 and self.disabled_weapons & 0xE00:
            self.disabled_weapons &= ~0xE00
        self.weapons |= self.disabled_weapons
        self.disabled_weapons = 0
        if not self.weapons & (1 << self.weapon):
            self.select_next_weapon()

    # items ----------------------------------------------------------
    def use_item(self, k):
        """Player_useItem for the Berserker (3) and the Dog Collar (4); the
        medkits and the soul sphere go through use_health_item.

        k: inventory slot. The Berserker gives 31 turns of triple damage. The
        Dog Collar must be used on a dog (class 1) right in front: the dog
        becomes a weapon (9..11 by its variant) whose "ammo" (type 5) is its
        health x5. Returns True if the item was used.
        """
        self.secret_turn = False
        if not self.inventory[k]:
            self.say("You have none")
            return False
        if k == 3:
            self.say("Berserker activated!")
            self.berserk += 31
            self.inventory[k] -= 1
            self.end_player_turn()
            return True
        if k == 4:
            fx, fz = DIRS[self.facing]
            tr = [t for t in self.trace(self.x, self.z, self.x + fx, self.z + fz, MASK_SHOT)
                  if t != "wall" and t != "player" and t[0] in ("ent", "line")]
            if not tr:
                self.say("No dog within range")
                return False
            if tr[0][0] != "ent" or self.etype(self.ents[tr[0][1]]) != ET_ENEMY:
                self.say("Bad target for Dog Collar")
                return False
            j = tr[0][1]
            e = self.ents[j]
            d = self.edef(e)
            if d["eSub"] != 1:
                self.say(f"Dog Collar won't fit {d['name']}")
                return False
            self.inventory[k] -= 1
            self.deactivate(j)                       # the dog leaves the map...
            e["flags"] &= ~F_LINKED
            w = 9 if d["parm"] == 294 else 10 if d["parm"] == 467 else 11
            self.weapons |= 1 << w                   # ... and fights for you
            self.weapon = w
            self.ammo[5] = min(99, (e["c"].hp * 5) & 0xFF)
            self.dialog = f"You captured|the {d['name']}!|"
            self.pending = None
            self.skip_turn = True
            self.monsters_pending = True             # the turn passes when the dialog closes
            if self.berserk:
                self.berserk -= 1
                if not self.berserk:
                    self.say("Berserker expired!")
            return True
        return False

    # the vendor -----------------------------------------------------
    def store_items(self):
        """The vendor's list: (name tag, price) per line (vendingMenuTable),
        without the 'Back' line and the dividers.
        """
        return [(t, p) for t, p in STORES[self.store] if p > 0]

    def store_buy(self, tag):
        """Buy one STORE_ITEMS[tag]: -> 'bought', 'credits' or 'full'.

        Items add 1 to the inventory, ammo 10 (3 for rockets), stats +1.
        """
        price = dict(self.store_items())[tag]
        kind, what = STORE_ITEMS[tag][1], STORE_ITEMS[tag][2]
        if kind == 1:
            have = self.inventory[what - 25]
        elif kind == 2:
            have = self.ammo[what]
        else:
            have = getattr(self.p, ("defense", "agility", "strength", "accuracy")[what - 1])
        if price > self.credits:
            return "credits"
        if have == 99:
            return "full"
        self.credits -= price
        if kind == 1:
            self.inventory[what - 25] = min(99, self.inventory[what - 25] + 1)
        elif kind == 2:
            self.add_ammo(what, 3 if what == 3 else 10)
        else:
            a = ("defense", "agility", "strength", "accuracy")[what - 1]
            setattr(self.p, a, getattr(self.p, a) + 1)
        return "bought"

    def store_close(self):
        """Closes the vendor's menu and runs the pending monster turn."""
        self.secret_turn = False
        self.store = None
        self.settle()

    def use_health_item(self):
        """PORT: no inventory menu - button 2 + Up uses the smallest medkit you
        carry (Sm Medkit, else Lg Medkit, else Soul Sphere).

        A soul sphere gives up to 200 health and 200 armour (capped at the
        maximums). Returns True and ends the turn if one was used. (The string
        after the first line of this method repeats this; it is not a
        docstring, since it is not the first statement.)
        """
        self.secret_turn = False
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
        """Runs the tile event at (x, z), if there is one, with `flags` (what
        happened: EVF_*, arrival/leave/facing bits). Returns True if any of
        its commands ran.
        """
        ev = self.event_at.get((x, z))
        if ev is None:
            return False
        return self.run_event(ev, 0, flags)

    def run_event(self, ev, start, flags):
        """Game_runEvent: runs event ev's commands from index `start`.

        Each command's a2 holds the conditions it runs on: state bits
        0x10000 << (state-1) (or none, for state 0), a key bit in 0xF000 that
        must match the best key held, and the event bits in `flags`. One-shot
        commands (EVF_MODIFY) are disabled after they run. A command that opens
        something modal (exec_cmd sets self.suspend) stops the event; its
        place is saved in self.pending for resume(). Returns True if any
        command ran. Events with flag bit 0 ignore movement.
        """
        e = self.g["events"][ev]
        if (e["flags"] & 1) and (flags & EVF_MOVE):
            return False
        st = self.event_state[ev]
        n7 = 0 if st == 0 else (0x10000 << (st - 1))  # the state's condition bit
        # the best key held, as a condition bit: red 0x8000, blue 0x4000, green 0x2000, yellow 0x1000
        if self.keys & 8:
            flags |= 0x8000
        elif self.keys & 4:
            flags |= 0x4000
        elif self.keys & 1:
            flags |= 0x2000
        elif self.keys & 2:
            flags |= 0x1000
        n8 = flags & 0xF000
        ran = False
        cmds = e["cmds"]
        for k in range(start, len(cmds)):
            if not self.cmd_alive[ev][k]:
                continue
            a2 = cmds[k]["a2"]
            if n7:
                if not a2 & n7:
                    continue
            elif a2 & 0x1FF0000:  # state 0: commands for other states are skipped
                continue
            if (a2 & 0xF000) not in (0, n8):
                continue
            if not flags & a2:
                continue
            self.suspend = None
            if self.exec_cmd(ev, k, cmds[k], flags):
                ran = True
                if a2 & EVF_MODIFY:
                    self.cmd_alive[ev][k] = False
            if self.suspend is not None:
                self.suspend = (ev, k, flags, self.suspend)
                self.pending = self.suspend
                break
        return ran

    def find_event(self, x, z):
        """Index of the tile event at (x, z), or None."""
        return self.event_at.get((x, z))

    def exec_cmd(self, ev, k, c, flags):
        """Executes command k (dict c: "op", "a1", "a2") of event ev.

        op is the original's event command number (comments name them);
        a1 its argument, often packed: cell x in bits 0..7 and z in 8..15.
        Returns True if the command "ran" (counts for one-shot commands and the
        event's result). May set self.suspend ("dialog", "password", "stop")
        to stop the event, and self.skip_turn when no monster turn follows.
        """
        op, a1 = c["op"], c["a1"]
        s = self.g["strings"]
        if op == 1:                       # GOTO x, z, angle
            self.x, self.z = a1 & 0xFF, (a1 >> 8) & 0xFF
            self.facing = {64: 0, 0: 1, 192: 2, 128: 3}.get((a1 >> 16) & 0xFF, self.facing)  # angle 0..255: 64 north, 0 east, 192 south, 128 west
        elif op == 2:                     # CHANGEMAP: when the exit door opens
            if self.single_map:           # the one-sector game: the stats exit ends it
                if c["a1"] & 0x80000000:  # bit 31: the exit that completes the sector
                    self.level_done = True
                else:
                    self.say("The way back is sealed")
                    self.suspend = "stop"
            else:
                self.change = a1
        elif op == 3:                     # TRIGGER: another cell's event, as if used
            self.skip_turn = False
            self.run_tile(a1 & 0xFF, (a1 >> 8) & 0xFF, EVF_USE)
            self.suspend = None           # (a dialog in there resumes that event; this one goes on)
        elif op == 4:  # MESSAGE
            self.say(s[a1])
        elif op == 5:  # PAIN
            self.say("Pain!")
            self.player_pain(a1, 0)
        elif op == 7:                     # SHOW
            if a1 == 0xFFFF:              # (a thing that is only drawn)
                return True
            e = self.ents[a1]
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
        elif op == 9:  # GIVEMAP (the automap)
            self.map_given = True
        elif op in (30, 31):              # FLOOR / CEILING COLOUR (rgb888)
            rgb = ((a1 >> 16) & 0xFF, (a1 >> 8) & 0xFF, a1 & 0xFF)
            if op == 30:
                self.floor_rgb = rgb
            else:
                self.ceil_rgb = rgb
        elif op == 32:                    # ENABLEWEAPONS
            if a1:
                self.restore_weapons()
            else:
                self.disabled_weapons, self.weapons, self.weapon = self.weapons, 0, 0
        elif op == 33:                    # OPENSTORE: the vendor's menu (modal)
            self.store = a1 if 0 <= a1 < 4 else 0
        elif op == 34:                    # CHANGESPRITE: the things there turn to scenery
            for j in self.entities_at(a1 & 31, (a1 >> 5) & 31):
                if self.etype(self.ents[j]) != ET_ENEMY:
                    self.ents[j]["flags"] &= ~F_LINKED
            self.static_hidden.add((a1 & 31, (a1 >> 5) & 31))
        elif op == 39:                    # CHECK_COMPLETED_LEVEL
            if not self.completed >> max(0, (a1 >> 16) - 1):  # no sector from that one up done yet: the dialog says why not
                self.dialog = s[a1 & 0xFFFF]
                self.suspend = "dialog"
                self.skip_turn = True
        elif op == 10:                    # PASSWORD
            self.password = (s[a1 & 0xFF], s[(a1 >> 8) & 0xFF])
            self.suspend = "password"
            self.skip_turn = True
        elif op in (11, 19, 20):          # CHANGE/NEXT/PREV STATE
            tgt = self.find_event(a1 & 0xFF, (a1 >> 8) & 0xFF)
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
        elif op in (6, 15, 16, 17):       # door lines
            l = self.lines[a1]
            if l["locked"]:
                return False
            if op == 15 and l["open"]:
                return False
            if op == 16 and not l["open"]:
                return False
            l["open"] = not l["open"]
            if not l["flags"] & 4 and not self.secret_turn:   # a sliding wall: a secret
                self.secret_turn = True
                self.say("Found Secret!")
                self.add_xp(5)
            if l["open"] and l["tex"] == 7 and self.change is not None:   # the exit door
                self.exit = self.change
        elif op == 18:                    # HIDE (props too: their blocking goes)
            for j in self.entities_at(a1 & 0xFF, (a1 >> 8) & 0xFF):
                if self.etype(self.ents[j]) != ET_ENEMY:
                    self.ents[j]["flags"] = (self.ents[j]["flags"] | F_HIDDEN) & ~F_LINKED
            self.static_hidden.add((a1 & 0xFF, (a1 >> 8) & 0xFF))
        elif op in (21, 22):              # INC/DEC STAT
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
        elif op == 24:  # STATUS (bar message)
            self.status_msg = s[a1] or None
        elif op == 37:                    # WAIT: PORT - no real-time pause; continue
            pass
        elif op == 38:                    # PORTAL: opens; the power couplings become targets
            self.portal = True
            self.portal_mon = None
            self.portal_cnt = self.rng.byte() % 3
            n = 0
            for e in self.ents[:self.first_drop]:
                d = self.edef(e)
                if (d["eType"], d["eSub"]) == (ET_DESTRUCT, 4) and n < 2:
                    e["flags"] |= F_ALIVE
                    n += 1
        elif op == 40:  # NOTEBOOK: a note for the notebook
            self.notes.append(a1)
        elif op == 41:                    # CHECK_KEY
            need = {0: 1, 1: 2, 2: 4, 3: 8}[a1]  # key number -> key bit
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

        `password` is the code the player entered (for a code prompt); a wrong
        code ends the event there. Then runs the pending monster turn.
        """
        self.secret_turn = False
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


# the vendors (Menu.c vendingMenuTable): (name tag, price); price -1 = a divider
STORES = [
    [(0, -1), (1, -1), (2, 8), (3, 35), (6, -1), (7, 5), (8, 5), (9, 10), (11, 10), (10, 15), (12, -1),
     (13, 15), (14, 15), (15, 20), (16, 20)],
    [(0, -1), (1, -1), (2, 6), (6, -1), (8, 3), (7, 4), (9, 6), (12, -1), (13, 15), (14, 15), (15, 20),
     (16, 20)],
    [(0, -1), (1, -1), (2, 6), (3, 25), (4, 50), (6, -1), (7, 3), (8, 4), (11, 6), (9, 8), (10, 12),
     (12, -1), (13, 15), (14, 15), (15, 20), (16, 20)],
    [(0, -1), (1, -1), (2, 4), (3, 15), (4, 30), (5, 40), (6, -1), (7, 2), (8, 2), (9, 4), (11, 5),
     (10, 8), (12, -1), (13, 8), (14, 8), (15, 10), (16, 10)],
]
# name tag -> (name, kind 1 item / 2 ammo / 3 stat, what)
STORE_ITEMS = {2: ("Sm Medkit", 1, 25), 3: ("Lg Medkit", 1, 26), 4: ("Soul Sphere", 1, 27),
               5: ("Berserker", 1, 28), 7: ("10x Hal. Cans", 2, 0), 8: ("10x Bullets", 2, 1),
               9: ("10x Shells", 2, 2), 10: ("10x Cells", 2, 4), 11: ("3x Rockets", 2, 3),
               13: ("+1 Accuracy", 3, 4), 14: ("+1 Agility", 3, 2), 15: ("+1 Strength", 3, 3),
               16: ("+1 Defense", 3, 1)}


class Campaign:
    """The whole game: the maps of analysis/campaign.json in turn. `world` is
    the map in play; after every player action call `advance()`, which moves
    to the next map once an exit door has opened (Game_changeMap).

    `info` is the campaign's map list ({"name", "id", "title", ...}),
    `data` caches each map's game data, `index` is the map in play.
    """

    def __init__(self, root=Path(__file__).resolve().parent.parent / "analysis", seed=0x1234, start_map=0):
        """Loads the map list from `root` (the analysis directory) and starts a new game on map `start_map`."""
        self.root = Path(root)
        self.info = json.loads((self.root / "campaign.json").read_text())["maps"]
        self.data = {}
        self.finished = False
        self.stats = None             # (map title, kills, monsters) of the sector just completed
        self.index = start_map
        self.world = World(self.gamedata(start_map), seed, start=CAMPAIGN_START, single_map=False)
        self.entry = None             # (carry snapshot, spawn) to restart the map from (continue)

    def gamedata(self, i):
        """Map i's game data (analysis/maps/<name>.json), loaded once and cached."""
        if i not in self.data:
            self.data[i] = json.loads((self.root / "maps" / f"{self.info[i]['name']}.json").read_text())
        return self.data[i]

    def map_of(self, path):
        """The campaign index of the map whose file is `path` (such as "/junction.bsp"), or None."""
        name = path.strip("/").rsplit(".", 1)[0]
        return next((i for i, m in enumerate(self.info) if m["name"] == name), None)

    def advance(self):
        """-> True if the map changed (or the game ended).

        The exit's CHANGEMAP argument: bits 0..7 string id of the target map's
        file name, bits 8..30 the spawn cell and angle (0 = the map's own
        spawn), bit 31 set when the exit completes the sector (records it in
        `completed` and the statistics in `stats`).
        """
        w = self.world
        if w.exit is None or w.dead:
            return False
        param = w.exit
        target = self.map_of(w.g["strings"][param & 0xFF])
        # drop bit 31, then arithmetic shift: sp = bits 8..30 (sign from bit 30)
        sp = (param << 1) & 0xFFFFFFFF
        if sp & 0x80000000:
            sp -= 1 << 32
        sp >>= 9
        spawn = None if sp == 0 else (sp & 31, (sp >> 5) & 31,
                                      {64: 0, 0: 1, 192: 2, 128: 3}.get((sp >> 10) & 255, 0))
        self.stats = None
        if param & 0x80000000:        # Player_addLevelStats(true)
            mid = self.info[self.index]["id"]
            if mid != 2:
                w.completed |= 1 << (mid - 1)
            mons = [e for e in w.ents[:w.first_drop] if w.etype(e) == ET_ENEMY]
            self.stats = (self.info[self.index]["title"], sum(1 for e in mons if not e["flags"] & F_ALIVE), len(mons))
        if target is None:            # /endgame.bsp (the Reactor's exit): the end
            self.finished = True
            return True
        self.index = target
        self.world = World(self.gamedata(target), carry=w, spawn=spawn, single_map=False)
        return True


def load(path=Path(__file__).resolve().parent.parent / "analysis" / "gamedata.json", seed=0x1234):
    """The old single-map game (Sector 1) from analysis/gamedata.json, as a World."""
    return World(json.loads(Path(path).read_text()), seed)


if __name__ == "__main__":
    w = load()
    print("spawn", w.x, w.z, w.facing, "rng calls", w.rng.calls, "messages", w.messages)
    print("active monsters", [w.edef(w.ents[i])["name"] for i in w.active])
    print("player", vars(w.p) if hasattr(w.p, '__dict__') else [getattr(w.p, s) for s in w.p.__slots__])
