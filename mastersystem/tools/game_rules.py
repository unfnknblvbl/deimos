#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Rule tables of the original Doom RPG, re-expressed as plain data (from
reading the J2ME decompile and the DoomRPG-RE reimplementation; this is
our own transcription of the numbers, not copied code). Shared by the
Python reference model (game_model.py) and the ROM table generator
(gen_cdata.py, which writes them as assembler tables), so the two can't
drift apart.

Indices used throughout: a weapon index is a row of WEAPONS; an ammo type
is an index into AMMO_NAMES; a monster class is a row of MONSTERS (each
class has three ranks, e.g. Zombie Private/Lieutenant/Captain, which share
the class's attacks and damage multipliers).
"""

# Player and monster weapons: (name, strMin, strMax, rangeMin, rangeMax,
# ammoType, ammoUsage, armorShare, numShots)
#   armorShare/256 of the damage goes to armour (the rest to health)
#   numShots = how many hit rolls one attack makes (chaingun/plasma 3)
#   strMin..strMax is the damage roll range; rangeMin/rangeMax are the
#   weapon's range numbers as in the original tables; ammoUsage is ammo
#   spent per attack (0: needs no ammo)
WEAPONS = [
    ("Axe",            3,  12, 0,  70, 0,  0,  25, 1),   # 0
    ("Fire Ext",       1,   2, 0, 100, 0,  1, 204, 1),   # 1
    ("Pistol",         6,   7, 5,  80, 1,  1, 102, 1),   # 2
    ("Shotgun",        6,  10, 2,  80, 2,  1, 128, 1),   # 3
    ("Chaingun",       3,   6, 3,  90, 1,  3, 102, 3),   # 4
    ("Super Shotgun", 12,  18, 1,  90, 2,  2,  51, 1),   # 5
    ("Plasma Gun",     6,   8, 4,  90, 4,  3, 230, 3),   # 6
    ("Rocket Lnchr",  15,  36, 8,  70, 3,  1, 128, 1),   # 7
    ("BFG",           60, 105, 8, 100, 4, 15,  76, 1),   # 8
    ("Hell Hound",     5,   9, 0,  75, 5,  0,  25, 1),   # 9
    ("Cerberus",       8,  12, 0,  75, 5,  0,  25, 1),   # 10
    ("Demon Wolf",    15,  18, 0,  75, 5,  0,  25, 1),   # 11
    ("Melee 1",        3,   5, 0,  90, 0,  0, 128, 0),   # 12  (monster attacks: numShots
    ("Melee 2",        4,   7, 0,  80, 0,  0, 128, 0),   # 13   come from MONSTERS' shots)
    ("Melee 3",        5,  15, 0,  70, 0,  0, 128, 0),   # 14
    ("Missile",        4,  10, 3,  85, 0,  0, 128, 0),   # 15
    ("Boss Missile",  10,  20, 3,  75, 0,  0, 128, 0),   # 16
    ("Rocket Missile", 15, 30, 2,  80, 0,  0, 128, 0),   # 17
    ("Raise",          0,   0, 0,   0, 0,  0,   0, 0),   # 18
]
W_AXE, W_EXT, W_PISTOL = 0, 1, 2
W_RAISE = 18                    # the Archvile's "attack": raise a corpse
# ammo type names, indexed by WEAPONS' ammoType ("Dog": the hound weapons 9-11)
AMMO_NAMES = ["Halon", "Bullets", "Shells", "Rockets", "Cells", "Dog"]

# Monster classes (eSubType): base (hp, armor, defense, strength, agility,
# accuracy), attack weapons (attack1, attack2: indices into WEAPONS) and
# shots per attack. The base stats are scaled per def by its parm
# (game_model.setup_enemy), which is how the three ranks differ.
MONSTERS = [
    ((5, 4, 13, 13, 13, 13),     (2, 3),   1),   # 0 Zombie Pvt/Lt/Cpt
    ((7, 3, 12, 14, 12, 12),     (12, 13), 1),   # 1 Hellhound/Cerberus/Demon Wolf
    ((10, 5, 14, 7, 13, 12),     (4, 4),   3),   # 2 Troop/Commando/Assassin
    ((9, 4, 13, 14, 13, 13),     (15, 12), 1),   # 3 Impling/Imp/Imp Lord
    ((6, 3, 14, 6, 15, 15),      (13, 14), 3),   # 4 Phantom/Lost Soul/Nightmare
    ((10, 5, 14, 16, 12, 13),    (13, 12), 1),   # 5 Bull Demon/Pinky/Belphegor
    ((13, 7, 12, 10, 13, 15),    (15, 13), 3),   # 6 Malwrath/Cacodemon/Wretched
    ((15, 8, 14, 12, 12, 12),    (15, 14), 1),   # 7 Beholder/Rahovart/Pain Elemental
    ((13, 8, 16, 14, 15, 15),    (7, 12),  1),   # 8 Ghoul/Fiend/Revenant
    ((20, 10, 16, 14, 15, 15),   (7, 3),   1),   # 9 Behemoth/Mancubus/Druj
    ((18, 9, 15, 17, 14, 16),    (15, 15), 1),   # 10 Infernis/Archvile/Apollyon
    ((25, 15, 16, 15, 15, 15),   (16, 17), 1),   # 11 Ogre/Hell Knight/Baron
    ((600, 400, 45, 30, 35, 40), (7, 17),  1),   # 12 Cyberdemon (values clamp to 255)
    ((400, 250, 30, 35, 40, 55), (12, 13), 3),   # 13 Kronos
]

# Weapon damage multiplier vs monster class, /256 (CombatEntity_calcWeaponDamage):
# returns the multiplier for (monster class, weapon index, ammo type).
def weapon_damage_mult(mclass, weapon):
    """Damage multiplier, in 1/256, for weapon (a WEAPONS index) against mclass.

    mclass is a MONSTERS index, or None when the target is the player (always
    256, x1). Each class has a short list of special cases checked in order:
    ("axe", None, m) applies to the axe, ("ammo", t, m) to weapons using
    ammo type t. Anything else is x1. Zombies (class 0) take x3 from the axe
    and 204/256 from everything else.
    """
    ammo = WEAPONS[weapon][5]
    axe = weapon == 0
    if mclass is None:            # target is the player
        return 256
    if mclass == 0:               # zombies: weak to the axe, resist the rest
        return 768 if axe else 204
    table = {
        1: [("ammo", 2, 409)],
        2: [("ammo", 4, 512), ("axe", None, 204)],
        3: [("ammo", 2, 512), ("ammo", 4, 165)],
        4: [("axe", None, 191), ("ammo", 0, 2550)],
        5: [("ammo", 2, 382), ("ammo", 3, 191)],
        6: [("axe", None, 768), ("ammo", 5, 140)],
        7: [("ammo", 5, 637), ("ammo", 1, 165)],
        8: [("axe", None, 768), ("ammo", 2, 140)],
        9: [("ammo", 3, 512), ("ammo", 5, 165)],
        10: [("ammo", 0, 1530), ("ammo", 1, 165)],
        11: [("ammo", 2, 512)],
        12: [("ammo", 3, 382)],
        13: [("ammo", 4, 382)],
    }[mclass]
    for kind, val, mult in table:
        if kind == "axe" and axe:
            return mult
        if kind == "ammo" and ammo == val:
            return mult
    return 256


# Player start (Player_reset), plus what the Entrance level would have given
# by the time the player reaches Sector 1 (axe, fire extinguisher, a few
# halon cans) - documented in the README as a port decision.
PLAYER_START = dict(hp=30, maxhp=30, armor=0, maxarmor=20, defense=16, strength=12,
                    agility=14, accuracy=16, level=1, xp=0, nextxp=80, credits=0,
                    weapons=(1 << 0) | (1 << 1) | (1 << 2), weapon=2,
                    ammo=[6, 8, 0, 0, 0, 0], inventory=[0, 0, 0, 0, 0], keys=0)
# a new game of the whole campaign (Player_reset): the pistol and 8 bullets
CAMPAIGN_START = dict(PLAYER_START, weapons=1 << 2, ammo=[0, 8, 0, 0, 0, 0])

# Event flag bits used when running tile events. A tile event fires when
# its own flags share a bit with the flags of what is happening (game_model
# run_tile): the low bits say how the player crossed the cell edge, the top
# four bits which way the player faces.
EVF_USE = 0x100          # SELECT on the cell ahead / TRIGGER / line removal
EVF_MODIFY = 0x200       # command is one-shot
EVF_MOVE = 0x400         # caused by movement/turning
FACING_FLAG = {0: 0x10000000, 1: 0x20000000, 3: 0x40000000, 2: 0x80000000}  # N E W S
LEAVE_FLAG = {1: 32, 3: 128, 0: 16, 2: 64}      # leaving a cell moving E,W,N,S
ARRIVE_FLAG = {1: 8, 3: 2, 0: 4, 2: 1}           # arriving moving E,W,N,S

# Trace masks (bit n = entity type n blocks). A trace walks the cells
# between two points; MASK_* say which entity types stop it.
MASK_PLAYER_MOVE = 0xF287
MASK_MONSTER_MOVE = 0xFF87      # (+ bit 8: the player's cell)
MASK_SHOT = 0x5687              # SELECT trace and monster attack trace (22151)
MASK_SHOW_KILL = 4098           # SHOW kills enemies/destructibles on the tile
MASK_RAISE_FREE = 16263         # cell must be free of these to raise a corpse
