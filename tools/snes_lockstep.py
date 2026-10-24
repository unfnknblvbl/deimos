#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Lockstep test of the SNES build's rules: the ROM (on the snes9x core) and
The Mega Drive game's Python model (game_model's Campaign) play the campaign side by
side, driven by the campaign pilot. The ROM is the rules test build
(snes/src/test_rules.c): the test writes each action into its mailbox in
RAM and waits for it to be done; after every action the ROM's state S
(work RAM) is compared byte for byte with the model's.

Usage: snes_lockstep.py <deimos dir> <rom.sfc> <labels.lbl> [max actions] [--no-god]

The mailbox (_mbox in the label file, work RAM): "MBOX", +4 the command,
+5 its argument, +6 the sequence number the test writes last, +7 the
ROM's reply (0xEE when ready, then each finished command's sequence
number), +8 a text argument (a door code, NUL-terminated), +32 u16 the
frames the command took. Commands (CMDS): 1 move, 2 turn, 3 use, 4 pass,
5 next / previous weapon, 6 medkit, 7 use an item, 8 resume a dialog
(argument 1: with the code), 9 close the vendor, 11 change map. God mode
(the default) sets the health and maximum to 200 in both whenever it
falls under 100.
Licence: GPL-2.0-or-later.
"""
import sys
import time
from pathlib import Path

MD = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(MD / "tools"))
sys.path.insert(0, str(Path(__file__).parent))
import game_model as gm  # noqa: E402
import campaign_pilot as cp  # noqa: E402
import md_campaign_lockstep as L  # noqa: E402
from snes_harness import Snes, labels  # noqa: E402

# the pilot's actions -> (mailbox command, argument); 255 is -1
CMDS = {"fwd": (1, 0), "back": (1, 1), "left": (2, 255), "right": (2, 1), "use": (3, 0), "pass": (4, 0),
        "wnext": (5, 1), "wprev": (5, 255), "heal": (6, 0), "berserk": (7, 3), "collar": (7, 4),
        "close": (9, 0)}


class Rom:
    """The rules test ROM on snes9x, driven through its mailbox."""

    def __init__(self, rom, lbl):
        self.e = Snes(rom)
        lab = labels(lbl)
        self.S = lab["_S"]
        self.mb = lab["_mbox"]
        self.seq = 0
        for _ in range(1200):                   # (20 s at most to get ready)
            self.e.run_frames(1)
            if self.e.wram(self.mb, 4) == b"MBOX" and self.e.wram(self.mb + 7, 1)[0] == 0xEE:
                break
        else:
            raise SystemExit("the ROM never got ready")
        self.init_frames = self.e.frames_run
        self.frames = []

    def do(self, cmd, arg=0, text=b""):
        """a command posted; frames run until the ROM answers with its
        sequence number; the frames it took are recorded"""
        if text:
            for i, b in enumerate(text + b"\0"):
                self.e.poke(self.mb + 8 + i, b)
        self.e.poke(self.mb + 4, cmd)
        self.e.poke(self.mb + 5, arg & 255)
        self.seq = (self.seq + 1) & 255 or 1    # (never 0)
        self.e.poke(self.mb + 6, self.seq)
        for _ in range(3000):
            self.e.run_frames(1)
            if self.e.wram(self.mb + 7, 1)[0] == self.seq:
                f = self.e.wram(self.mb + 32, 2)
                self.frames.append(f[0] | (f[1] << 8))
                return
        raise RuntimeError(f"the ROM did not finish command {cmd}")

    def state(self, n):
        """the first n bytes of the ROM's S"""
        return self.e.wram(self.S, n)


def main():
    """Play until the end (or max actions, or a difference); 0 when all matched."""
    rom_path, lbl = sys.argv[2], sys.argv[3]
    steps = int(sys.argv[4]) if len(sys.argv) > 4 and sys.argv[4].isdigit() else 60000
    god = "--no-god" not in sys.argv
    rom = Rom(rom_path, lbl)
    F = L.LAYOUT["fields"]
    ncmp = F["end_compared"][0]               # (the bytes of S compared)
    c = gm.Campaign()
    pilot = cp.Pilot()
    log = []
    t0 = time.time()

    def check(step, act):
        """the ROM's S against the model's; on a difference, the last moves and the fields"""
        a, b = rom.state(ncmp), L.model_bytes(c)
        if a != b:
            print("\n".join(log[-12:]))
            print(f"MISMATCH after step {step} ({act}) in {c.info[c.index]['name']}:\n" + L.describe_diff(a, b))
            print("model messages:", c.world.messages[-12:])
            return False
        return True

    if not check(-1, "start"):
        return 1
    print(f"ready after {rom.init_frames} frames")
    for step in range(steps):
        w = c.world
        if w.dead:
            log.append("dead")
            break
        if god and w.p.hp < 100:
            rom.e.poke(rom.S + F["hp"][0], 200)
            rom.e.poke(rom.S + F["maxhp"][0], 200)
            w.p.hp = w.p.maxhp = 200
        if w.dialog is not None:
            rom.do(8, 0)
            w.resume()
            act = "dialog"
        elif w.password is not None:
            code = w.password[0]
            rom.do(8, 1, code.encode("latin1"))
            w.resume(code)
            act = f"code {code}"
        else:
            act = pilot.choose(c)
            rom.do(*CMDS[act])
            cp.ACTIONS[act](w)
        if c.advance():                         # (the model went to another map)
            if c.finished:
                fr = sorted(rom.frames)
                print(f"step {step}: THE END")
                print(f"OK: {step + 1} actions in lockstep, the whole campaign ({time.time() - t0:.0f} s); "
                      f"frames per action: median {fr[len(fr) // 2]}, 90% {fr[len(fr) * 9 // 10]}, max {fr[-1]}")
                return 0
            ex = rom.state(F["exit"][0] + 4)[F["exit"][0] + 3]  # (S.exit[3]: the next map)
            rom.do(11, ex)                      # (the ROM changes map too)
            print(f"step {step}: -> {c.info[c.index]['name']}", flush=True)
        w = c.world
        log.append(f"{step:5d} {act:8s} {c.info[c.index]['name']} pos=({w.x},{w.z},{w.facing}) hp={w.p.hp}")
        if not check(step, act):
            return 1
    fr = sorted(rom.frames)
    print(f"OK: {len(log)} actions in lockstep, in {c.info[c.index]['name']}; frames per action: median "
          f"{fr[len(fr) // 2]}, max {fr[-1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
