#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Lockstep test: plays the ROM (on the Gearsystem core) and the Python
reference model (game_model.py) side by side with the same inputs, and
after every action compares the ROM's game state - read straight out of
RAM - with the model's, byte for byte. Both use the same RNG, so any
difference in rules, or in the order random numbers are drawn, shows up
as a mismatch at the action that caused it.

Usage: python3 test_lockstep.py <rom> [steps] [seed] [--frames DIR] [--audio FILE.wav]
  seed picks the random action sequence (not the game's RNG).
Exit code 0 = all states matched.

More options:
  --god             keep the player's HP at 200 (ROM and model) so a run
                    can reach the end of the map
  --autopilot       autopilot.py chooses the actions (Sector 1 route)
  --pilot           the generic map pilot (sms_pilot.py) chooses them
  --plan FILE       replay the actions in a pickled plan
  --render          also compare every picture with live_data.Model
                    (test_render.Checker), cycling the view settings
  --gamedata FILE   the model loads this project/gamedata (editor maps)
  --endshot FILE    save a screenshot of the end
Environment (debugging): LOCKSTEP_WATCH=lo-hi (hex) watches RAM writes
and prints them on a mismatch; LOCKSTEP_KEEP keeps the first watch;
LOCKSTEP_DBG=dir adds a syms module that names engine RAM variables.

Run by build.sh step 5 (300 random actions, seed 1) on both ROMs. The
"game state" is the RAM block at STATE_ADDR whose layout gen_cdata.py
wrote to game/gen/layout.json; model_bytes() builds the same bytes from
the model. The Rom class here is also how the other tools drive the ROM.
"""
import contextlib
import json
import os
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import game_model as gm
from gearsystem_harness import Gearsystem, DEFAULT_CORE
from key_text import sms_text

ROOT = Path(__file__).resolve().parent.parent
# the game state's field layout: {"fields": {name: [offset, size]}, ...}
LAYOUT = json.loads((ROOT / "game" / "gen" / "layout.json").read_text())
STATE_ADDR = 0xC400                 # the game state in RAM
# engine variables in RAM: a counter bumped each time an action has been
# handled, 1 while waiting for input, and the UI mode (UI_* values)
ENG_TURN_SEQ, ENG_IDLE, ENG_MODE = 0xC006, 0xC007, 0xC008
UI_PLAY, UI_DIALOG, UI_PASSWORD, UI_DEAD, UI_DONE = 0, 1, 2, 3, 4
BOX_LINES = 13                      # text lines per dialog page


def model_bytes(w):
    """The model World w's state as the ROM's game state bytes, up to end_compared.

    l_flags packs each line as bit 0 open, bit 1 locked, bit 2 alive;
    cmd_dead sets a bit for each event command that has been used up;
    entity defs of None become 255.
    """
    F = LAYOUT["fields"]
    buf = bytearray(F["end_compared"][0])

    def put(name, vals):
        """Write field name: an int (little-endian, the field's size) or a byte sequence."""
        off, size = F[name]
        if isinstance(vals, int):
            vals = vals.to_bytes(size, "little")
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
    put("keys", w.keys), put("dead", int(w.dead)), put("level_done", int(w.level_done))
    put("map_given", int(w.map_given)), put("drop_index", w.drop_index)
    put("n_active", len(w.active)), put("active", w.active)
    put("e_def", [255 if e["def"] is None else e["def"] for e in w.ents])
    put("e_x", [e["x"] for e in w.ents]), put("e_z", [e["z"] for e in w.ents])
    put("e_flags", [e["flags"] for e in w.ents]), put("e_move", [e["move"] & 255 for e in w.ents])
    # monster slots in entity order (only map entities have combat stats)
    mons = [e["c"] for e in w.ents[:w.first_drop] if e["c"] is not None]
    for name, attr in (("m_hp", "hp"), ("m_maxhp", "maxhp"), ("m_armor", "armor"), ("m_maxarmor", "maxarmor"),
                       ("m_def", "defense"), ("m_str", "strength"), ("m_agi", "agility"), ("m_acc", "accuracy")):
        put(name, [getattr(c, attr) for c in mons])
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
    put("n_notes", len(w.notes)), put("notes", w.notes)
    return bytes(buf)


def describe_diff(a, b):
    """Text naming each state field where ROM bytes a and model bytes b differ (first 6 bytes each)."""
    F = LAYOUT["fields"]
    out = []
    for name, (off, size) in F.items():
        if size and a[off:off + size] != b[off:off + size]:
            diffs = [(i, a[off + i], b[off + i]) for i in range(size) if a[off + i] != b[off + i]]
            out.append(f"  {name}: " + ", ".join(f"[{i}] rom={x} model={y}" for i, x, y in diffs[:6]))
    return "\n".join(out)


class Rom:
    """The ROM running on the Gearsystem core, with the helpers the tests need."""

    def __init__(self, path, frames_dir=None):
        """Load the ROM at path (the core's start-up chatter is silenced).

        frames_dir: if set, every frame is saved there as fNNNNNN.png.
        nframe counts the frames run.
        """
        with open(os.devnull, "w") as dn, contextlib.redirect_stdout(dn):
            self.e = Gearsystem(DEFAULT_CORE, path)
        self.frames_dir = frames_dir
        self.nframe = 0

    def run(self, n, buttons=()):
        """Run n frames with buttons held, one frame at a time."""
        for _ in range(n):
            self.e.run_frames(1, buttons)
            if self.frames_dir:
                self.e.screenshot().save(f"{self.frames_dir}/f{self.nframe:06d}.png")
            self.nframe += 1

    def ram(self):
        """The 8 KB of work RAM."""
        return self.e.ram()

    def byte(self, a):
        """The RAM byte at CPU address a."""
        return self.e.ram()[a - 0xC000]

    def wait_idle(self, seq_before=None, limit=2000):
        """Run until the engine waits for input (and, with seq_before, has
        bumped the action counter past it). False after limit frames."""
        for _ in range(limit):
            r = self.ram()
            if r[ENG_IDLE - 0xC000] == 1 and (seq_before is None or r[ENG_TURN_SEQ - 0xC000] != seq_before):
                return True
            self.run(1)
        return False

    def press(self, buttons, held=()):
        """hold the buttons until the game has handled them, then release and
        let it see the release (it only polls between actions)"""
        if not self.wait_idle():
            raise RuntimeError("ROM never became idle")
        seq = self.byte(ENG_TURN_SEQ)
        for _ in range(3000):
            self.run(1, tuple(held) + tuple(buttons))
            if self.byte(ENG_TURN_SEQ) != seq:
                break
        else:
            r = self.ram()
            raise RuntimeError(f"ROM did not handle {buttons}: idle {r[ENG_IDLE - 0xC000]} "
                               f"mode {r[ENG_MODE - 0xC000]} " + str({k: hex(v) for k, v in self.e.regs().items()}))
        self.wait_idle()
        self.run(2, tuple(held))

    def state(self):
        """The ROM's game state bytes, as compared with model_bytes()."""
        n = LAYOUT["fields"]["end_compared"][0]
        r = self.ram()
        return bytes(r[STATE_ADDR - 0xC000:STATE_ADDR - 0xC000 + n])


GG = [False]                        # True when the ROM under test is the Game Gear's (main())


def pages(w, sid):
    """How many dialog pages string sid takes ("|" separates lines, BOX_LINES per page), its lines
    counted as the ROM has them (key_text.sms_text may have re-wrapped a text that names keys)."""
    return max(1, -(-len(sms_text(w.g["strings"][sid], GG[0]).split("|")) // BOX_LINES))


def code_inputs(code):
    """button presses for the ROM's door-code entry

    Each digit starts at 0: Up until it is right, then Right for the next
    digit; button 1 enters the code. Returns a list of button tuples."""
    seq = []
    digit = 0
    for k, ch in enumerate(code):
        d = int(ch)
        while digit != d:
            seq.append(("up",))
            digit = (digit + 1) % 10
        if k < len(code) - 1:
            seq.append(("right",))
            digit = 0
    seq.append(("a",))
    return seq


def choose(w, rnd):
    """A plausible player: fights what is in front, opens doors, explores.

    w is the model, rnd the seeded random.Random. Returns an action name:
    fwd back left right use pass wnext wprev heal."""
    fx, fz = gm.DIRS[w.facing]
    ahead = (w.x + fx, w.z + fz)
    if w.p.hp < 12 and any(w.inventory[:3]) and rnd.random() < 0.7:
        return "heal"
    for i in w.entities_at(*ahead):
        if w.etype(w.ents[i]) == gm.ET_ENEMY and w.weapon in (0, 1) and rnd.random() < 0.7:
            return "wnext"
        if w.etype(w.ents[i]) in (gm.ET_ENEMY, gm.ET_DESTRUCT, gm.ET_FIRE) and rnd.random() < 0.8:
            return "use"
    if w.event_at.get(ahead) is not None and rnd.random() < 0.5:
        return "use"
    r = rnd.random()
    if r < 0.50:
        return "fwd"
    if r < 0.60:
        return "back"
    if r < 0.72:
        return "left"
    if r < 0.84:
        return "right"
    if r < 0.90:
        return "use"
    if r < 0.93:
        return "pass"
    if r < 0.96:
        return rnd.choice(["wnext", "wprev"])
    return "heal"


def main():
    """Run the lockstep test (options in the module docstring); returns the exit code."""
    rom_path = sys.argv[1]
    GG[0] = rom_path.endswith(".gg")    # (its texts: pages())
    steps = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    frames_dir = None
    if "--frames" in sys.argv:
        frames_dir = sys.argv[sys.argv.index("--frames") + 1]
        os.makedirs(frames_dir, exist_ok=True)
    rnd = random.Random(seed)
    plan = None
    if "--plan" in sys.argv:
        import pickle
        plan = [a for a, _x in pickle.load(open(sys.argv[sys.argv.index("--plan") + 1], "rb"))
                if a not in ("dialog", "code")]
    rom = Rom(rom_path, frames_dir)
    if "--audio" in sys.argv:        # record the sound to a .wav (s16 stereo)
        import atexit
        import wave
        wav_path = sys.argv[sys.argv.index("--audio") + 1]
        rom.e.audio = bytearray()

        def save_audio():
            """At exit: write the recorded sound as a 16-bit stereo WAV."""
            with wave.open(wav_path, "wb") as f:
                f.setnchannels(2)
                f.setsampwidth(2)
                f.setframerate(int(round(rom.e.sample_rate())))
                f.writeframes(bytes(rom.e.audio))
        atexit.register(save_audio)
    if "--gamedata" in sys.argv:     # a map made with the editor: its project.json
        w = gm.load(sys.argv[sys.argv.index("--gamedata") + 1])
    else:
        w = gm.load()
    rom.run(30)                        # the title screen
    rom.run(3, ("a",))                 # button 1: start
    rom.run(2)
    if not rom.wait_idle():
        print("ROM never reached the game")
        return 1
    if "--god" in sys.argv:
        # test set-up: a tough player in both, so a scripted run reaches the
        # end of the sector and exercises every script on the way
        F = LAYOUT["fields"]
        for name in ("hp", "maxhp"):
            rom.e.poke(STATE_ADDR + F[name][0], 200)
        w.p.hp = w.p.maxhp = 200
    pilot = None
    if "--autopilot" in sys.argv:
        import autopilot
        pilot = autopilot.Autopilot()
    if "--pilot" in sys.argv:              # the generic map pilot (any map: tools/sms_pilot.py)
        import sms_pilot
        one_map = sms_pilot.OneMap(w)
        cpilot = sms_pilot.cp.Pilot()

        class _P:
            """Adapter: the autopilot interface (choose(w)) over the campaign pilot."""

            def choose(self, _w):
                """The campaign pilot's next action for the one-map campaign."""
                return cpilot.choose(one_map)
        pilot = _P()
    # the states must already match at the start of the level
    ok = rom.state() == model_bytes(w)
    if not ok:
        print("MISMATCH right after start\n" + describe_diff(rom.state(), model_bytes(w)))
        return 1
    render = None
    if "--render" in sys.argv:
        # also check every picture against the renderer's model, switching the
        # view size (pause menu) every 25 actions, the large view's update
        # mode (outward, black, across) every 50, the detail every 100 and the walls every 200
        import test_render
        render = test_render.Checker(rom, rom_path)
    log = []
    for step in range(steps):
        if render is not None and step % 25 == 0 and w.dialog is None and w.password is None \
                and not (w.dead or w.level_done):
            render.set_size(["large", "small"][(step // 25) % 2], (step // 50) % 2, (step // 100) % 2, (step // 200) % 2)
        if w.dead or w.level_done:
            log.append("end: " + ("dead" if w.dead else "sector complete"))
            break
        if "--god" in sys.argv and w.p.hp < 100:
            F = LAYOUT["fields"]
            for name in ("hp", "maxhp"):
                rom.e.poke(STATE_ADDR + F[name][0], 200)
            w.p.hp = w.p.maxhp = 200
        if w.dialog is not None:
            # a dialog is open: page through it in the ROM, close it in the model
            sid = w.g["strings"].index(w.dialog)
            for _ in range(pages(w, sid)):
                rom.press(("a",))
            w.resume()
            act = f"close dialog {sid}"
        elif w.password is not None:
            want = w.password[0]
            # random play enters a wrong code 30% of the time (tests the refusal)
            right = plan is not None or pilot is not None or rnd.random() < 0.7
            code = want if right else "999"
            for b in code_inputs(code):
                rom.press(b)
            w.resume(code)
            act = f"door code {code}"
        else:
            if pilot is not None:
                act = pilot.choose(w)
            elif plan is not None:
                if not plan:
                    break
                act = plan.pop(0)
            else:
                act = choose(w, rnd)
            # action -> pad buttons ("2" first: hold button 2, then the direction)
            buttons = {"fwd": ("up",), "back": ("down",), "left": ("left",), "right": ("right",),
                       "use": ("a",), "pass": ("2", "down"), "wnext": ("2", "right"), "wprev": ("2", "left"),
                       "heal": ("2", "up")}[act]
            if buttons[0] == "2":
                rom.press(("2",))                 # button 2 alone does nothing
                rom.press(buttons[1:], held=("2",))
                rom.run(2)
            else:
                rom.press(buttons)
            # the same action in the model
            {"fwd": lambda: w.move(False), "back": lambda: w.move(True), "left": lambda: w.turn(-1),
             "right": lambda: w.turn(1), "use": w.use, "pass": w.pass_turn,
             "wnext": lambda: w.cycle_weapon(1), "wprev": lambda: w.cycle_weapon(-1),
             "heal": w.use_health_item}[act]()
        a, b = rom.state(), model_bytes(w)     # ROM state, model state
        if os.environ.get("LOCKSTEP_WATCH") and a == b and (step == 0 or not os.environ.get("LOCKSTEP_KEEP")):
            lo, hi = (int(v, 16) for v in os.environ["LOCKSTEP_WATCH"].split("-"))
            rom.e.watch(lo, hi)
        log.append(f"{step:4d} {act:16s} pos=({w.x},{w.z},{w.facing}) hp={w.p.hp} active={len(w.active)} "
                   f"frames={rom.nframe}")
        if render is not None and a == b and w.dialog is None and w.password is None \
                and not (w.dead or w.level_done):
            msg = render.check()
            if msg:
                print("\n".join(log[-5:]))
                print(f"PICTURE MISMATCH after step {step} ({act}): {msg}")
                return 1
        if a != b:
            print("\n".join(log[-15:]))
            print(f"MISMATCH after step {step} ({act}):\n" + describe_diff(a, b))
            if os.environ.get("LOCKSTEP_WATCH"):      # debugging: who wrote the watched RAM in this step
                print("writes (pc, offset, value):", [(hex(p), o, v) for p, o, v in rom.e.watch_log()])
                print("before the first:", " ".join(f"{p:04x}" for p in rom.e.watch_trace()))
                dbg = os.environ.get("LOCKSTEP_DBG")
                if dbg:
                    sys.path.insert(0, dbg)
                    import syms
                    S = syms.enum_syms()
                    r = rom.ram()
                    print("rom", {k: r[S[k] - 0xC000] for k in ("sel_kind", "sel_idx", "hit_n", "n_pend_expl")},
                          list(r[S["hit_kind"] - 0xC000:S["hit_kind"] - 0xC000 + 8]),
                          list(r[S["hit_val"] - 0xC000:S["hit_val"] - 0xC000 + 8]))
                    fx, fz = gm.DIRS[w.facing]
                    print("model trace ahead:", w.trace(w.x, w.z, w.x + 8 * fx, w.z + 8 * fz, gm.MASK_SHOT),
                          "weapon", w.weapon, "pos", (w.x, w.z, w.facing))
                    def cell_list(x, z):
                        """The ROM's linked list of entities in cell (x, z) (1-based links, 0 ends)."""
                        n, out = r[S["cell_head"] - 0xC000 + z * 32 + x], []
                        while n and len(out) < 20:
                            out.append(n - 1)
                            n = r[S["ent_next"] - 0xC000 + n - 1]
                        return out
                    bad = []
                    for z in range(32):
                        for x in range(32):
                            want = [i for i in w.entities_at(x, z)]
                            if cell_list(x, z) != want:
                                bad.append(((x, z), cell_list(x, z), want))
                    print("cell lists that differ:", bad[:10])
            print("model messages:", w.messages[-10:])
            return 1
    print("\n".join(log[-10:]))
    # monsters no longer alive
    kills = sum(1 for e in w.ents[:w.first_drop] if e["c"] is not None and not e["flags"] & gm.F_ALIVE)
    if "--endshot" in sys.argv:            # (a picture of how it ended)
        rom.run(120)
        rom.e.screenshot().save(sys.argv[sys.argv.index("--endshot") + 1])
    print(f"OK: {len(log)} actions in lockstep, {rom.nframe} frames; kills {kills}, level {w.level}, "
          f"hp {w.p.hp}, rng calls {w.rng.calls}")
    if render is not None:
        print(render.summary())
    return 0


if __name__ == "__main__":
    sys.exit(main())
