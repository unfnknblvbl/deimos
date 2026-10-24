#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The Mega Drive port's map pilot (tools/campaign_pilot.py, the same file in
both repositories) driving this port's model: a one-map "campaign" around
game_model.World, with what the Master System's rules don't have (thin
walls, vendors, items) answered as absent. Used by test_lockstep.py
--pilot to play a whole map that has no hand-made route (e.g. the UAC Base).

How it works: campaign_pilot.py expects a game_model.Campaign (several
maps, stores, inventory items) and World objects with a few methods this
port's model lacks. This file patches game_model.Campaign to OneMap before
importing campaign_pilot, so the pilot builds a OneMap instead, and adapt()
gives each World harmless stand-ins for the missing methods. Importers then
use the pilot through the module attribute `cp` (sms_pilot.cp).

Licence: GPL-2.0-or-later.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import game_model as gm  # noqa: E402


def adapt(w):
    """Give World w the methods and fields the pilot asks for.

    Adds, only if w lacks static_at: static_at(x, z) (the static map cell
    from the 32-wide w.static list), thin_at (no thin walls here), store
    (no vendor), completed (0 maps done), use_item (always fails) and
    store_close (does nothing). Returns w itself, modified in place.
    """
    if not hasattr(w, "static_at"):
        w.static_at = lambda x, z: w.static[z * 32 + x]
        w.thin_at = lambda x, z: ()
        w.store = None
        w.completed = 0
        w.use_item = lambda k: False
        w.store_close = lambda: None
    return w


class OneMap:
    """The pilot's view of a campaign: one map, done when the map is.

    Stands in for the Mega Drive model's game_model.Campaign. Fields:
    world (the adapted World), index (current map, always 0), info (one
    map description dict), finished (set once the level is done) and
    stats (True: the pilot may show its end-of-map statistics).
    """

    def __init__(self, w, name="map"):
        """Wrap World w (adapted for the pilot) as a campaign named name."""
        self.world = adapt(w)
        self.index = 0
        self.info = [{"name": name, "title": name, "id": 0}]
        self.finished = False
        self.stats = True

    def advance(self):
        """Move on after the map ends: True once, the first time the level is done."""
        if self.world.level_done and not self.finished:
            self.finished = True
            return True
        return False

    def map_of(self, name):
        """No other maps to look up in a one-map campaign: always None."""
        return None


# the shim: campaign_pilot builds game_model.Campaign, so it must be patched
# before the import below runs campaign_pilot's module code
gm.Campaign = OneMap
import campaign_pilot as cp  # noqa: E402,F401  (after the shim)
