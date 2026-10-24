#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Lockstep test of the portable core (core/rules.c) on the build machine:
the C rules (test/libcore.so) and the Mega Drive game's Python model (game_model's
Campaign) play the whole campaign side by side, driven by the campaign
pilot; after every action the C state S is compared byte for byte with
the model's (the Mega Drive game's md_campaign_lockstep.model_bytes: the same layout).

Usage: lockstep_host.py <deimos dir> [max actions = 60000] [--no-god]
Licence: GPL-2.0-or-later.
"""
import ctypes
import json
import sys
from pathlib import Path

MD = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(MD / "tools"))
import game_model as gm  # noqa: E402
import campaign_pilot as cp  # noqa: E402
import md_campaign_lockstep as L  # noqa: E402


def main():
    """Play until the end (or max actions, or a difference); 0 when all matched."""
    steps = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 60000
    god = "--no-god" not in sys.argv
    lib = ctypes.CDLL(str(Path(__file__).parent / "libcore.so"))   # (make libcore.so first)
    camp = json.loads((MD / "analysis" / "campaign.json").read_text())["maps"]
    names = ",".join(m["name"] for m in camp)
    if lib.host_load(str(MD / "build" / "gen" / "maps").encode(), names.encode()) != len(camp):
        print("can't read the maps (build the Mega Drive game first)")
        return 1
    lib.host_state.restype = ctypes.POINTER(ctypes.c_ubyte)
    lib.host_messages.restype = ctypes.c_char_p
    n = lib.host_state_size()                       # (not used: ncmp bytes are compared)
    F = L.LAYOUT["fields"]
    ncmp = F["end_compared"][0]
    st = lib.host_state()                           # (S itself: pokes change the C state)

    def state():
        """the compared part of the C state"""
        return bytes(st[:ncmp])

    c = gm.Campaign()
    pilot = cp.Pilot()
    lib.game_init()
    log = []

    def check(step, act):
        """the C state against the model's; on a difference, what led to it"""
        a, b = state(), L.model_bytes(c)
        if a != b:
            print("\n".join(log[-12:]))
            print(f"MISMATCH after step {step} ({act}) in {c.info[c.index]['name']}:\n" + L.describe_diff(a, b))
            print("model messages:", c.world.messages[-12:])
            print("C messages:", lib.host_messages().decode(errors="replace").split("\n")[-14:])
            return False
        return True

    if not check(-1, "start"):
        return 1
    for step in range(steps):
        w = c.world
        if w.dead:
            log.append("dead")
            break
        if god and w.p.hp < 100:                    # (god mode: the health back to 200 in both)
            st[F["hp"][0]] = 200
            st[F["maxhp"][0]] = 200
            w.p.hp = w.p.maxhp = 200
        if w.dialog is not None:
            lib.resume(None)
            w.resume()
            act = "dialog"
        elif w.password is not None:
            code = w.password[0]
            lib.resume(code.encode("latin1"))
            w.resume(code)
            act = f"code {code}"
        else:
            act = pilot.choose(c)
            # the action as the core's call
            {"fwd": lambda: lib.act_move(0), "back": lambda: lib.act_move(1),
             "left": lambda: lib.act_turn(ctypes.c_byte(-1)), "right": lambda: lib.act_turn(ctypes.c_byte(1)),
             "use": lib.act_use, "pass": lib.act_pass,
             "wnext": lambda: lib.act_cycle_weapon(ctypes.c_byte(1)),
             "wprev": lambda: lib.act_cycle_weapon(ctypes.c_byte(-1)),
             "heal": lib.act_health_item, "close": lib.store_close,
             "berserk": lambda: lib.use_item(3), "collar": lambda: lib.use_item(4)}[act]()
            cp.ACTIONS[act](w)
        lib.host_messages()                         # (this turn's messages dropped)
        if c.advance():
            if c.finished:
                print(f"step {step}: THE END")
                print(f"OK: {step + 1} actions in lockstep, the whole campaign")
                return 0
            ex = st[F["exit"][0] + 3]            # the target map's index (low byte)
            lib.map_change(ex)
            print(f"step {step}: -> {c.info[c.index]['name']}", flush=True)
        w = c.world
        log.append(f"{step:5d} {act:8s} {c.info[c.index]['name']} pos=({w.x},{w.z},{w.facing}) hp={w.p.hp}")
        if not check(step, act):
            return 1
    print(f"OK: {len(log)} actions in lockstep, in {c.info[c.index]['name']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
