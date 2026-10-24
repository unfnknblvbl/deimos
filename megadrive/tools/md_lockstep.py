#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The Mega Drive game lockstep test: plays the ROM (on the Genesis Plus GX core) and the Python
reference model (game_model.py) side by side with the same inputs, and
after every action compares the ROM's game state - read straight out of
RAM - with the model's, byte for byte. Both use the same RNG, so any
difference in rules, or in the order random numbers are drawn, shows up
as a mismatch at the action that caused it.

Usage: python3 md_lockstep.py <rom> [steps] [seed] [--smooth] [--frames DIR] [--audio FILE.wav]
  --smooth sets smooth movement on the options screen first.
  seed picks the random action sequence (not the game's RNG).
Exit code 0 = all states matched.

It needs a build: build/deimos.map (symbol addresses) and
build/gen/layout.json (the state's layout, from gen_cdata.py). It plays a
single map from analysis/gamedata.json (game_model.load), as when the ROM
held Sector 1 alone; tools/md_campaign_lockstep.py is the test of the
whole campaign and reuses this file's Rom class and helpers.
"""
import contextlib
import json
import os
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import game_model as gm
from md_harness import MegaDrive
from key_text import md_text
import re

# the state layout gen_cdata.py wrote, and the linker map of the last build
# (for the addresses of the ROM's symbols)
ROOT = Path(__file__).resolve().parent.parent
LAYOUT = json.loads((ROOT / "build" / "gen" / "layout.json").read_text())
ENDIAN = LAYOUT.get("endian", "big")
_MAP = (ROOT / "build" / "deimos.map").read_text()


def sym(name):
    """The address of global symbol `name` in build/deimos.map (the linker's map file). Raises if it is not there."""
    m = re.search(r"0x0*([0-9a-f]+)\s+" + name + r"\n", _MAP)
    return int(m.group(1), 16)


STATE_ADDR = sym("S")  # the game state block in RAM
ENG_TURN_SEQ, ENG_IDLE, ENG_MODE = sym("eng_turn_seq"), sym("eng_idle"), sym("eng_mode")  # action counter, 'waiting for input' flag, UI mode
UI_PLAY, UI_DIALOG, UI_PASSWORD, UI_DEAD, UI_DONE = 0, 1, 2, 3, 4
BOX_LINES = 13  # text lines per dialog page ('|' separates lines)


def model_bytes(w):
    """The model World w's state, laid out as the ROM's state block S (up to
    end_compared), so the two can be compared byte for byte. Fields the
    model does not fill stay 0.
    """
    F = LAYOUT["fields"]
    buf = bytearray(F["end_compared"][0])

    def put(name, vals):
        """Writes `vals` (an int, as a number of the field's size, or a byte sequence) into field `name` of buf."""
        off, size = F[name]
        if isinstance(vals, int):
            vals = vals.to_bytes(size, ENDIAN)
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
    put("keys", w.keys), put("dead", int(w.dead)), put("level_done", int(w.level_done))  # the current layout (gen_cdata.state_layout) has no level_done field
    put("map_given", int(w.map_given)), put("drop_index", w.drop_index)
    put("n_active", len(w.active)), put("active", w.active)
    put("e_def", [255 if e["def"] is None else e["def"] for e in w.ents])
    put("e_x", [e["x"] for e in w.ents]), put("e_z", [e["z"] for e in w.ents])
    put("e_flags", [e["flags"] for e in w.ents]), put("e_move", [e["move"] & 255 for e in w.ents])
    mons = [e["c"] for e in w.ents[:w.first_drop] if e["c"] is not None]
    for name, attr in (("m_hp", "hp"), ("m_maxhp", "maxhp"), ("m_armor", "armor"), ("m_maxarmor", "maxarmor"),
                       ("m_def", "defense"), ("m_str", "strength"), ("m_agi", "agility"), ("m_acc", "accuracy")):
        put(name, [getattr(c, attr) for c in mons])
    put("l_flags", [(1 if l["open"] else 0) | (2 if l["locked"] else 0) | (4 if l["alive"] else 0) for l in w.lines])  # line flags: 1 open, 2 locked, 4 alive
    put("ev_state", w.event_state)
    # one bit per event command (all events' commands in order): 1 = a one-shot that has run
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
    """A text listing of each state field where a (the ROM) and b (the model) differ, with up to 6 differing bytes each."""
    F = LAYOUT["fields"]
    out = []
    for name, (off, size) in F.items():
        if size and a[off:off + size] != b[off:off + size]:
            diffs = [(i, a[off + i], b[off + i]) for i in range(size) if a[off + i] != b[off + i]]
            out.append(f"  {name}: " + ", ".join(f"[{i}] rom={x} model={y}" for i, x, y in diffs[:6]))
    return "\n".join(out)


BUTTONS = {b: b for b in ("a", "b", "c", "x", "y", "z", "mode", "up", "down", "left", "right", "start")}  # lockstep button name -> md_harness button name (the same here)


class Rom:
    """The ROM under test in a MegaDrive emulator, with helpers to press
    buttons the way a player would and to read the game state. frames_dir:
    save every frame there as PNG (for videos). nframe counts frames run.
    """
    def __init__(self, path, frames_dir=None):
        """Starts the emulator on the ROM at `path` (the core's own console output is silenced)."""
        with open(os.devnull, "w") as dn, contextlib.redirect_stdout(dn):
            self.e = MegaDrive(path)
        self.frames_dir = frames_dir
        self.nframe = 0

    def run(self, n, buttons=()):
        """Runs n frames, one at a time, holding `buttons`; saves each frame if frames_dir is set."""
        buttons = tuple(BUTTONS[b] for b in buttons)
        for _ in range(n):
            self.e.run_frames(1, buttons)
            if self.frames_dir:
                self.e.screenshot().save(f"{self.frames_dir}/f{self.nframe:06d}.png")
            self.nframe += 1

    def byte(self, a):
        """One byte of work RAM at 68000 address a."""
        return self.e.read(a, 1)[0]

    def wait_idle(self, seq_before=None, limit=4000):
        """Runs frames until the game waits for input (eng_idle = 1) and, if
        seq_before is given, eng_turn_seq has moved on from it. Returns False
        after `limit` frames without that.
        """
        for _ in range(limit):
            if self.byte(ENG_IDLE) == 1 and (seq_before is None or self.byte(ENG_TURN_SEQ) != seq_before):
                return True
            self.run(1)
        return False

    def press(self, buttons, held=()):
        """hold the buttons until the game has handled them, then release and
        let it see the release (it only polls between actions)

        `held` are buttons held down throughout (such as Mode). The game
        counts each handled action in eng_turn_seq; raises RuntimeError if the
        ROM does not take the press within 3000 frames.
        """
        if not self.wait_idle():
            raise RuntimeError("ROM never became idle")
        seq = self.byte(ENG_TURN_SEQ)
        for _ in range(3000):
            self.run(1, tuple(held) + tuple(buttons))
            if self.byte(ENG_TURN_SEQ) != seq:
                break
        else:
            raise RuntimeError(f"ROM did not handle {buttons}: idle {self.byte(ENG_IDLE)} "
                               f"mode {self.byte(ENG_MODE)}")
        self.wait_idle()
        self.run(2, tuple(held))

    def state(self):
        """The ROM's state block S, up to end_compared (the part compared with the model)."""
        n = LAYOUT["fields"]["end_compared"][0]
        return self.e.read_block(STATE_ADDR, n)


def pages(w, sid):
    """How many dialog pages string sid takes in the ROM (13 lines a page, rounded up; at least 1): A presses to close it.
    (The lines are counted as the ROM has them: key_text.md_text may have re-wrapped a text that names keys.)"""
    return max(1, -(-len(md_text(w.g["strings"][sid]).split("|")) // BOX_LINES))


def code_inputs(code):
    """button presses for the ROM's door-code entry

    Each digit starts at 0: Up raises it, Right moves to the next digit,
    A enters the code. Returns a list of button tuples.
    """
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

    w: the model's World; rnd: the random generator for the actions. Returns
    an action name. (Used when neither --plan nor --autopilot is given.)
    """
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
    """Runs the test (arguments in the module docstring; also --plan FILE: a
    pickled action list, --autopilot, --god, --gamedata FILE). Starts a
    new game in the ROM and the model, then does `steps` actions in both,
    comparing the state after each one. Prints the last actions and
    either OK or the first mismatch. Returns the exit code (0 = all matched).
    """
    rom_path = sys.argv[1]
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
            """Writes the recorded sound to wav_path when the program exits."""
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
    rom.run(30)
    rom.press(("start",))                     # PRESS START -> the title menu (no save: NEW GAME, OPTIONS)
    if "--smooth" in sys.argv:
        rom.press(("down",))
        rom.press(("a",))                     # options
        rom.press(("a",))                     # movement: smooth
        rom.press(("start",))                 # done
        rom.press(("up",))
    rom.press(("a",))                         # new game
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
    ok = rom.state() == model_bytes(w)
    if not ok:
        print("MISMATCH right after start\n" + describe_diff(rom.state(), model_bytes(w)))
        return 1
    log = []
    for step in range(steps):
        if w.dead or w.level_done:
            log.append("end: " + ("dead" if w.dead else "sector complete"))
            break
        if "--god" in sys.argv and w.p.hp < 100:
            F = LAYOUT["fields"]
            for name in ("hp", "maxhp"):
                rom.e.poke(STATE_ADDR + F[name][0], 200)
            w.p.hp = w.p.maxhp = 200
        if w.dialog is not None:
            sid = w.g["strings"].index(w.dialog)
            for _ in range(pages(w, sid)):
                rom.press(("a",))
            w.resume()
            act = f"close dialog {sid}"
        elif w.password is not None:
            want = w.password[0]
            # random play gets 30% of codes wrong on purpose, to test the wrong-code path
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
            buttons = {"fwd": ("up",), "back": ("down",), "left": ("left",), "right": ("right",),
                       "use": ("a",), "pass": ("b",), "wnext": ("z",), "wprev": ("x",),
                       "heal": ("c",)}[act]
            if os.environ.get("LOCKSTEP_VERBOSE"):
                print(step, act, (w.x, w.z, w.facing), flush=True)
            rom.press(buttons)
            {"fwd": lambda: w.move(False), "back": lambda: w.move(True), "left": lambda: w.turn(-1),
             "right": lambda: w.turn(1), "use": w.use, "pass": w.pass_turn,
             "wnext": lambda: w.cycle_weapon(1), "wprev": lambda: w.cycle_weapon(-1),
             "heal": w.use_health_item}[act]()
        a, b = rom.state(), model_bytes(w)
        log.append(f"{step:4d} {act:16s} pos=({w.x},{w.z},{w.facing}) hp={w.p.hp} active={len(w.active)} "
                   f"frames={rom.nframe}")
        if a != b:
            print("\n".join(log[-15:]))
            print(f"MISMATCH after step {step} ({act}):\n" + describe_diff(a, b))
            print("model messages:", w.messages[-10:])
            # debug output: the ROM's messages and its per-cell entity lists (cell_head,
            # ent_next: 1-based linked lists), checked against the model's
            try:  # the ROM's message queue (ui.c msgq, a static: from the ELF)
                import subprocess
                nm = subprocess.run(["m68k-elf-nm", str(ROOT / "build" / "deimos.elf")],
                                    capture_output=True, text=True).stdout
                q = int(next(l.split()[0] for l in nm.splitlines() if l.endswith(" msgq")), 16)
                raw = rom.e.read_block(q, 32 * 8)
                ch = int(next(l.split()[0] for l in nm.splitlines() if l.endswith(" cell_head")), 16)
                en = int(next(l.split()[0] for l in nm.splitlines() if l.endswith(" ent_next")), 16)
                heads = rom.e.read_block(ch, 1024)
                nxt = rom.e.read_block(en, 160)
                bad = 0
                for c in range(1024):
                    lst, v = [], heads[c]
                    while v and len(lst) < 200:
                        lst.append(v - 1)
                        v = nxt[v - 1]
                    want = w.entities_at(c & 31, c >> 5)
                    if lst != want and bad < 10:
                        bad += 1
                        print("cell", (c & 31, c >> 5), "rom", lst, "model", want)
                print("rom msgq:", [raw[i:i + 32].split(b"\0")[0].decode("latin1") for i in range(0, len(raw), 32)])
            except Exception as ex:  # (no toolchain on the path)
                print("rom msgq: ?", ex)
            return 1
    print("\n".join(log[-10:]))
    kills = sum(1 for e in w.ents[:w.first_drop] if e["c"] is not None and not e["flags"] & gm.F_ALIVE)
    print(f"OK: {len(log)} actions in lockstep, {rom.nframe} frames; kills {kills}, level {w.level}, "
          f"hp {w.p.hp}, rng calls {w.rng.calls}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
