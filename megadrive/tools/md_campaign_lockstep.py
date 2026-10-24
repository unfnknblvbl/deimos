#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The Mega Drive game campaign lockstep test: the ROM and the Python model
(game_model.Campaign) play the whole game side by side, driven by the
campaign pilot (tools/campaign_pilot.py); after every action the ROM's
game state S is compared byte for byte with the model's. Map changes go
through the ROM's own screens (the sector's stats: A).

Usage: md_campaign_lockstep.py <rom> [max actions] [--no-god] [--frames DIR]
  --god (the default) keeps the player's health up in both, so the pilot
  gets through every map.
Exit code 0 = every state matched.
Licence: GPL-2.0-or-later.
"""
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import game_model as gm  # noqa: E402
import campaign_pilot as cp  # noqa: E402
from md_lockstep import Rom, LAYOUT, ENDIAN, STATE_ADDR, ENG_MODE, describe_diff, code_inputs, BOX_LINES  # noqa
from key_text import md_text  # noqa: E402

UI_DONE, UI_END = 4, 11  # values of the ROM's eng_mode: the sector's statistics screen, THE END
# pilot action -> the ROM's button presses (default 6-button layout); the
# Berserker and the Dog Collar go through the pause menu's ITEMS list
BUTTONS = {"fwd": [("up",)], "back": [("down",)], "left": [("left",)], "right": [("right",)],
           "use": [("a",)], "pass": [("b",)], "wnext": [("z",)], "wprev": [("x",)], "heal": [("c",)],
           "close": [("b",)],
           "berserk": [("start",), ("down",), ("a",), ("a",)],
           "collar": [("start",), ("down",), ("a",), ("down",), ("a",)]}


def map_param(c, w, a1):
    """CHANGEMAP's argument as the ROM keeps it: the target's map index in
    the low byte (255: the end).

    c: the Campaign; w: its World; a1: the argument as the model keeps it
    (the low byte is the target's string id).
    """
    t = c.map_of(w.g["strings"][a1 & 0xFF])
    return (a1 & ~0xFF) | (255 if t is None else t)


def model_bytes(c):
    """The state of Campaign c's current map, laid out as the ROM's state
    block S up to end_compared (build/gen/layout.json), for a byte-for-
    byte comparison. Unfilled bytes (padding) stay 0.
    """
    w = c.world
    F = LAYOUT["fields"]
    buf = bytearray(F["end_compared"][0])

    def put(name, vals):
        """Writes `vals` into field `name`: an int as a big-endian number of the field's size (masked to fit), or a byte sequence."""
        off, size = F[name]
        if isinstance(vals, int):
            vals = (vals & ((1 << (8 * size)) - 1)).to_bytes(size, ENDIAN)
        vals = bytes(vals)
        assert len(vals) <= size, name
        buf[off:off + len(vals)] = vals

    p = w.p
    put("rng", w.rng.x)
    put("px", w.x), put("pz", w.z), put("facing", w.facing)
    put("hp", p.hp), put("maxhp", p.maxhp), put("armor", p.armor), put("maxarmor", p.maxarmor)
    put("defense", p.defense), put("strength", p.strength), put("agility", p.agility), put("accuracy", p.accuracy)
    put("level", w.level), put("xp", w.xp), put("nextxp", w.nextxp), put("credits", w.credits)
    put("weapons", w.weapons), put("weapon", w.weapon), put("ammo", w.ammo), put("inventory", w.inventory)
    put("keys", w.keys), put("dead", int(w.dead)), put("map", c.index)
    put("map_given", int(w.map_given)), put("drop_index", w.drop_index)
    put("berserk", w.berserk), put("store", 255 if w.store is None else w.store)
    put("secret_turn", int(w.secret_turn))
    put("chg_set", int(w.change is not None)), put("exit_set", int(w.exit is not None))
    put("completed", w.completed), put("disabled_weapons", w.disabled_weapons)
    put("chg", 0 if w.change is None else map_param(c, w, w.change))
    put("exit", 0 if w.exit is None else map_param(c, w, w.exit))
    put("floor_rgb", w.floor_rgb), put("ceil_rgb", w.ceil_rgb)
    put("n_active", len(w.active))
    put("active", b"".join(i.to_bytes(2, ENDIAN) for i in w.active))
    put("e_def", [255 if e["def"] is None else e["def"] for e in w.ents])
    put("e_x", [e["x"] for e in w.ents]), put("e_z", [e["z"] for e in w.ents])
    put("e_flags", [e["flags"] for e in w.ents]), put("e_move", [e["move"] & 255 for e in w.ents])
    mons = [e["c"] for e in w.ents[:w.first_drop] if e["c"] is not None]
    for name, attr in (("m_hp", "hp"), ("m_maxhp", "maxhp"), ("m_armor", "armor"), ("m_maxarmor", "maxarmor"),
                       ("m_def", "defense"), ("m_str", "strength"), ("m_agi", "agility"), ("m_acc", "accuracy")):
        put(name, [getattr(cc, attr) for cc in mons])
    put("l_flags", [(1 if l["open"] else 0) | (2 if l["locked"] else 0) | (4 if l["alive"] else 0) for l in w.lines])
    put("ev_state", w.event_state)
    bits = bytearray(F["cmd_dead"][1])
    k = 0
    for alive in w.cmd_alive:
        for a in alive:
            if not a:
                bits[k >> 3] |= 1 << (k & 7)
            k += 1
    put("cmd_dead", bits)
    # the hidden-prop bitmap: one bit per cell (z*32 + x)
    hid = bytearray(128)
    for (x, z) in w.static_hidden:
        cc = z * 32 + x
        hid[cc >> 3] |= 1 << (cc & 7)
    put("static_hidden", hid)
    put("n_notes", len(w.notes)), put("notes", w.notes)
    put("portal_mon", 0xFFFF if w.portal_mon is None else w.portal_mon), put("portal", int(w.portal))
    put("portal_cnt", w.portal_cnt & 255), put("couplings", w.couplings), put("god", int(w.god))
    return bytes(buf)


ROM = [None]  # the Rom under test, for the crash report in __main__


def pages(text):
    """How many dialog pages `text` takes in the ROM ('|' ends a line, 13 lines a page): A presses to close it.
    (The lines are counted as the ROM has them: key_text.md_text may have re-wrapped a text that names keys.)"""
    return max(1, -(-len(md_text(text).split("|")) // BOX_LINES))


def main():
    """Runs the test (arguments in the module docstring). Starts a new game
    in the ROM and a game_model.Campaign, then lets the pilot play up to
    `max actions`: answers dialogs and codes, presses each action's
    buttons in the ROM and applies it to the model, follows map changes
    (pressing A on the sector's statistics screen) and compares the state
    after every action. Returns the exit code: 0 if every state matched
    (and, at the end of the game, the ROM shows THE END).

    Environment: LOCKSTEP_VERBOSE prints each action; LOCKSTEP_SHOTS =
    "DIR:map" saves a screenshot after every action in that map.
    """
    rom_path = sys.argv[1]
    steps = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 40000
    god = "--no-god" not in sys.argv
    frames = sys.argv[sys.argv.index("--frames") + 1] if "--frames" in sys.argv else None
    if frames:
        os.makedirs(frames, exist_ok=True)
    rom = Rom(rom_path, frames)
    ROM[0] = rom
    c = gm.Campaign()
    pilot = cp.Pilot()
    rom.run(30)
    rom.press(("start",))
    rom.press(("a",))                          # new game
    if not rom.wait_idle():
        print("ROM never reached the game")
        return 1
    F = LAYOUT["fields"]

    def check(step, act):
        """Compares the ROM's state with the model's; on a mismatch prints the recent log and the differing fields and returns False."""
        a, b = rom.state(), model_bytes(c)
        if a != b:
            w = c.world
            print("\n".join(log[-12:]))
            print(f"MISMATCH after step {step} ({act}) in {c.info[c.index]['name']}:\n" + describe_diff(a, b))
            print("model messages:", w.messages[-10:])
            return False
        return True

    log = []
    if not check(-1, "start"):
        return 1
    for step in range(steps):
        w = c.world
        if w.dead:
            log.append("dead")
            break
        if god and w.p.hp < 100:
            for name in ("hp", "maxhp"):
                rom.e.poke(STATE_ADDR + F[name][0], 200)
            w.p.hp = w.p.maxhp = 200
        if w.dialog is not None:
            for _ in range(pages(w.dialog)):
                rom.press(("a",))
            w.resume()
            act = "dialog"
        elif w.password is not None:
            code = w.password[0]
            for b in code_inputs(code):
                rom.press(b)
            w.resume(code)
            act = f"code {code}"
        else:
            act = pilot.choose(c)
            if os.environ.get("LOCKSTEP_VERBOSE"):
                print(step, act, (w.x, w.z, w.facing), flush=True)
            for b in BUTTONS[act]:
                rom.press(b)
            cp.ACTIONS[act](w)
        moved = c.advance()
        if moved:
            if c.stats:                         # the sector's screen: A goes on (none at the end)
                if c.finished:
                    log.append(f"{step} THE END")
                    rom.run(10)
                    print("\n".join(log[-5:]))
                    ok = rom.byte(ENG_MODE) == UI_END
                    print("OK: the end reached" if ok else "ROM not at the end screen")
                    return 0 if ok else 1
                rom.press(("a",))
            elif c.finished:
                print("the end without stats?")
                return 1
            log.append(f"{step} -> {c.info[c.index]['name']}")
            print(f"step {step}: {c.info[c.index]['name']}", flush=True)
        w = c.world
        log.append(f"{step:5d} {act:8s} {c.info[c.index]['name']} pos=({w.x},{w.z},{w.facing}) hp={w.p.hp} "
                   f"frames={rom.nframe}")
        if not check(step, act):
            return 1
        shots = os.environ.get("LOCKSTEP_SHOTS")       # "DIR:map": a picture after every action there
        if shots and c.info[c.index]["name"] == shots.split(":")[1]:
            os.makedirs(shots.split(":")[0], exist_ok=True)
            rom.e.screenshot().save(f"{shots.split(':')[0]}/a{step:06d}.png")
    print("\n".join(log[-5:]))
    print(f"OK: {len(log)} actions in lockstep, {rom.nframe} frames, in {c.info[c.index]['name']}")
    return 0


def where(rom):
    """(the tests) where a busy ROM is: the PC and the word on the stack at frames

    Runs 30 frames and collects the distinct (PC, stacked word) pairs the
    ROM's frame interrupt saved at vblank_pc.
    """
    import struct
    from md_lockstep import sym
    seen = set()
    for _ in range(30):
        rom.run(1)
        seen.add(struct.unpack(">II", rom.e.read_block(sym("vblank_pc"), 8)))
    return sorted(seen)


if __name__ == "__main__":
    try:
        sys.exit(main())
    # the ROM stopped taking input (Rom.press gave up): print where it is stuck,
    # save a screenshot and dump the ROM's crash_info
    except RuntimeError as e:
        print(e)
        print("PCs:", [(hex(a), hex(b)) for a, b in where(ROM[0])])
        ROM[0].e.screenshot().save("/tmp/lockstep_hang.png")
        import subprocess
        nm = subprocess.run(["m68k-elf-nm", "build/deimos.elf"], capture_output=True, text=True).stdout
        tab = {l.split()[2]: int(l.split()[0], 16) for l in nm.splitlines() if len(l.split()) == 3}
        sym = tab.__getitem__
        r = ROM[0]
        for k in range(6):
            r.run(1, ("up",) if k < 3 else ())
            print({n: r.e.read_block(sym(n), 2).hex() for n in ("eng_idle", "eng_turn_seq", "crash_info")})
        print(r.e.read_block(sym("crash_info"), 22).hex())
        sys.exit(1)
