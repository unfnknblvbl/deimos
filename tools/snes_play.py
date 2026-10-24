#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The SNES game played through its pad, in step with the Mega Drive game's model: the
campaign pilot chooses each action, the test presses the buttons for it
(the title menu, dialogs, door codes, the vendor, the pause menu's items),
waits until the game is idle again, and compares the game state S in work
RAM with the model's, byte for byte, after every action. With --video,
the first seconds are recorded (every frame, scaled up, with a caption).

Usage: snes_play.py <deimos dir> <deimos.sfc> <deimos.lbl> [max actions]
                    [--video OUT.mp4 SECONDS CAPTION] [--no-god] [--script OUT.txt]
--script writes the run for the 3DS port's script mode (3ds/src/plat.c,
PLAT_SCRIPT): a line per press ("k <button>"), per poke ("w <offset>
<value>") and per compared state ("c <bytes> <crc32>").
(32x/tools/make_script.py packs that text into a binary script for the
other ports' test builds; on the Game Boy Color gbc/src/pad.c plays it.)

"Lockstep": the game and the model take the same actions and their
states must stay equal. The test knows when the game is ready for the
next press through three variables the SNES ui.c exports (the label file
gives their addresses): eng_turn_seq (incremented per handled press),
eng_idle (1 while waiting for a press) and eng_mode (the screen state).
Licence: GPL-2.0-or-later.
"""
import subprocess
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

# ui.c's screen states (eng_mode)
UI_PLAY, UI_DIALOG, UI_PASSWORD, UI_DEAD, UI_DONE, UI_END, UI_STORE, UI_PAUSE = range(8)
UI_ITEMS, UI_TITLE = 9, 11
# the pilot's actions -> the SNES button that does them
KEYS = {"fwd": "up", "back": "down", "left": "left", "right": "right", "use": "a", "pass": "b",
        "heal": "y", "wprev": "l", "wnext": "r", "close": "b"}


class Done(Exception):
    """the video's length reached"""
    pass


class Game:
    """The SNES game on snes9x, played through its pad; with video =
    (out, seconds, caption), every frame piped to ffmpeg."""

    def __init__(self, rom, lbl, video=None):
        self.e = Snes(rom)
        lab = labels(lbl)
        self.S = lab["_S"]
        self.seq_a, self.idle_a, self.mode_a = lab["_eng_turn_seq"], lab["_eng_idle"], lab["_eng_mode"]
        self.frames = 0
        self.video = video
        self.ff = None
        if video:
            from PIL import ImageFont
            self.font = ImageFont.truetype("DejaVuSans.ttf", 19)
            out, secs, self.caption = video
            self.out = out
            self.max_frames = int(secs * 60.1)
            self.e.audio = bytearray()
            self.W, self.H, self.CAP = 768, 672, 44
            self.ff = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt",
                                        "rgb24", "-s", f"{self.W}x{self.H + self.CAP}", "-r", "60.1", "-i", "-",
                                        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18", out + ".v.mp4"],
                                       stdin=subprocess.PIPE)

    def run(self, n, buttons=()):
        """n frames with buttons held (each frame recorded when making a video)"""
        for _ in range(n):
            self.e.run_frames(1, buttons)
            self.frames += 1
            if self.ff:
                from PIL import Image, ImageDraw
                im = self.e.screenshot().convert("RGB").resize((self.W, self.H), Image.NEAREST)
                fr = Image.new("RGB", (self.W, self.H + self.CAP), (16, 16, 24))
                fr.paste(im, (0, 0))
                ImageDraw.Draw(fr).text((self.W // 2, self.H + self.CAP // 2), self.caption, font=self.font,
                                        fill=(255, 220, 120), anchor="mm")
                self.ff.stdin.write(fr.tobytes())
                if self.frames >= self.max_frames:
                    raise Done()

    def byte(self, a):
        """a work RAM byte"""
        return self.e.wram(a, 1)[0]

    def wait_idle(self, limit=3000):
        """frames until the game waits for a press (eng_idle)"""
        for _ in range(limit):
            if self.byte(self.idle_a):
                return
            self.run(1)
        raise RuntimeError("the game did not get idle")

    script = None                                   # (the --script file, when written)

    def press(self, key):
        """one button, then wait until the game has handled it and is idle"""
        if self.script:
            self.script.write(f"k {key}\n")
        self.wait_idle()
        self.run(2)                                 # (the game sees the button released first)
        seq = self.byte(self.seq_a)
        self.run(4, (key,))
        for _ in range(4000):
            self.run(1)
            if self.byte(self.seq_a) != seq and self.byte(self.idle_a):
                return
        raise RuntimeError(f"no answer to {key} (mode {self.mode()}, idle {self.byte(self.idle_a)}, "
                           f"seq {self.byte(self.seq_a)} was {seq})")

    def mode(self):
        """the game's screen state (UI_*)"""
        return self.byte(self.mode_a)

    def state(self, n):
        """the first n bytes of the game's S"""
        return self.e.wram(self.S, n)

    def close(self):
        """the video finished: the sound written, both muxed into out"""
        if self.ff:
            import wave
            self.ff.stdin.close()
            self.ff.wait()
            with wave.open(self.out + ".wav", "wb") as w:
                w.setnchannels(2)
                w.setsampwidth(2)
                w.setframerate(int(round(self.e.sample_rate())))
                w.writeframes(bytes(self.e.audio))
            subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", self.out + ".v.mp4", "-i", self.out + ".wav",
                            "-c:v", "copy", "-c:a", "aac", "-b:a", "160k", "-shortest", self.out], check=True)
            Path(self.out + ".v.mp4").unlink()
            Path(self.out + ".wav").unlink()


def main():
    """Play the campaign through the pad; 0 when every state matched."""
    rom, lbl = sys.argv[2], sys.argv[3]
    steps = int(sys.argv[4]) if len(sys.argv) > 4 and sys.argv[4].isdigit() else 60000
    god = "--no-god" not in sys.argv
    video = None
    if "--video" in sys.argv:
        k = sys.argv.index("--video")
        video = (sys.argv[k + 1], float(sys.argv[k + 2]), sys.argv[k + 3])
    g = Game(rom, lbl, video)
    if "--script" in sys.argv:
        g.script = open(sys.argv[sys.argv.index("--script") + 1], "w")
    F = L.LAYOUT["fields"]
    ncmp = F["end_compared"][0]
    c = gm.Campaign()
    pilot = cp.Pilot()
    log = []
    t0 = time.time()

    def check(step, act):
        """the game's S against the model's (and the script's "c" line)"""
        a, b = g.state(ncmp), L.model_bytes(c)
        if g.script:
            import zlib
            g.script.write(f"c {ncmp} {zlib.crc32(bytes(b)) & 0xFFFFFFFF:08x}\n")
        if a != b:
            print("\n".join(log[-12:]))
            print(f"MISMATCH after step {step} ({act}) in {c.info[c.index]['name']}:\n" + L.describe_diff(a, b))
            return False
        return True

    try:
        g.run(120)                                  # the title
        g.wait_idle()
        g.press("start")                            # -> the menu: NEW GAME (no saved game)
        g.press("a")
        while g.mode() != UI_PLAY:
            g.run(1)
        g.wait_idle()
        if not check(-1, "start"):
            return 1
        print(f"playing after {g.frames} frames", flush=True)
        for step in range(steps):
            w = c.world
            if w.dead:
                log.append("dead")
                break
            if god and w.p.hp < 100:
                full = 99 if video else 200                 # (the video: a status bar that fits)
                g.e.poke(g.S + F["hp"][0], full)
                g.e.poke(g.S + F["maxhp"][0], full)
                if g.script:
                    g.script.write(f"w {F['hp'][0]} {full}\nw {F['maxhp'][0]} {full}\n")
                w.p.hp = w.p.maxhp = full
            if w.dialog is not None:
                for _ in range(L.pages(w.dialog)):  # every page (another dialog may follow)
                    g.press("a")
                w.resume()
                act = "dialog"
            elif w.password is not None:
                code = w.password[0]
                # each digit: UP that many times from 0, RIGHT to add it; A at the end
                for k, ch in enumerate(code):
                    for _ in range(int(ch)):
                        g.press("up")
                    if k + 1 < len(code):
                        g.press("right")
                g.press("a")
                w.resume(code)
                act = f"code {code}"
            else:
                act = pilot.choose(c)
                if act in ("berserk", "collar"):  # (pause menu -> ITEMS -> the item)
                    g.press("start")
                    g.press("down")
                    g.press("a")
                    if act == "collar":
                        g.press("down")
                    g.press("a")
                else:
                    g.press(KEYS[act])
                cp.ACTIONS[act](w)
            if c.advance():
                if c.finished:
                    # (the model's last map change marks the reactor completed: the
                    # game's end has no map change, so the state is not compared here)
                    print(f"step {step}: THE END ({g.frames} frames, {time.time() - t0:.0f} s)")
                    print(f"OK: {step + 1} actions through the pad, in lockstep, the whole campaign")
                    return 0
                if g.mode() == UI_DONE:
                    g.press("a")
                while g.mode() != UI_PLAY and g.mode() not in (UI_DIALOG, UI_PASSWORD, UI_STORE):
                    g.run(1)
                g.wait_idle()
                print(f"step {step}: -> {c.info[c.index]['name']} ({g.frames} frames)", flush=True)
            w = c.world
            log.append(f"{step:5d} {act:8s} {c.info[c.index]['name']} pos=({w.x},{w.z},{w.facing}) hp={w.p.hp} "
                       f"ui={g.mode()}")
            if not check(step, act):
                return 1
        print(f"OK: {len(log)} actions in lockstep, in {c.info[c.index]['name']} ({g.frames} frames)")
        return 0
    except Done:
        print(f"video: {g.frames} frames")
        return 0
    finally:
        g.close()
        if g.script:
            g.script.close()


if __name__ == "__main__":
    sys.exit(main())
