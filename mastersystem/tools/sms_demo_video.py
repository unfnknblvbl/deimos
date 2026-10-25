#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Demo video of the live-render build: the autopilot plays the whole of
level01 on the Gearsystem core (health topped up, as in the lockstep
test), switching the view through the pause menu along the way:
large + sweep, small, large + black, small, large + sweep. Every frame,
with the sound and a caption bar.

Usage: sms_demo_video.py rom/deimos.sms out.mp4   (needs ffmpeg)

How the ROM and model stay together: test_lockstep.Rom runs the ROM on the
Gearsystem libretro core (tools/gearsystem_harness.py); game_model.World is
the Python re-implementation of the rules. Each action is pressed on the
ROM and then applied to the model, so the model always knows when a dialog
or a door code is up and what the right answer is.

Unlike console_video.py, this asserts after every action that the ROM's game
state bytes equal the model's (the lockstep check), and it runs until the
level is done or 3000 actions, not for a set time. "Sweep" (drawn outward)
and "black" (drawn at once) are the pause menu's two ways of drawing the view.
Licence: GPL-2.0-or-later.
"""
import subprocess
import sys
import wave
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).parent))
import test_lockstep as T
import test_render as R
import game_model as gm
import autopilot

W, H, CAP = 768, 576, 48       # 256x192 scaled x3, plus a 48-pixel caption bar
FONT = ImageFont.truetype("DejaVuSans.ttf", 20)
# (from action number, view size, black, caption): the view settings to show
PLAN = [(0, "large", 0, "Large view (160x96), drawn outward from the centre"),
        (110, "small", 0, "Small view (128x80), drawn outward"),
        (230, "large", 1, "Large view, all at once (black while drawing)"),
        (350, "small", 1, "Small view, all at once (drawn out of sight)"),
        (460, "large", 0, "Large view, outward")]


def main():
    """Record the demo video (arguments from sys.argv); asserts if ROM and model diverge."""
    rom_path, out = sys.argv[1], Path(sys.argv[2])
    # the video encoder: reads raw RGB frames on its standard input
    ff = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
                           "-s", f"{W}x{H + CAP}", "-r", "59.92", "-i", "-", "-c:v", "libx264", "-pix_fmt",
                           "yuv420p", "-crf", "18", str(out) + ".video.mp4"], stdin=subprocess.PIPE)
    state = {"caption": ""}

    class Rec(T.Rom):
        """A test_lockstep.Rom that writes every emulated frame to the video."""

        def run(self, n, buttons=()):
            """Run n frames with buttons held, sending each (scaled, captioned) to ffmpeg."""
            for _ in range(n):
                self.e.run_frames(1, buttons)
                im = self.e.screenshot().convert("RGB").resize((W, H), Image.NEAREST)
                fr = Image.new("RGB", (W, H + CAP), (16, 16, 24))
                fr.paste(im, (0, 0))
                ImageDraw.Draw(fr).text((W // 2, H + CAP // 2), state["caption"], font=FONT,
                                        fill=(255, 220, 120), anchor="mm")
                ff.stdin.write(fr.tobytes())
                self.nframe += 1

    rom = Rec(rom_path)
    rom.e.audio = bytearray()          # the harness appends the sound samples here
    state["caption"] = "Doom RPG for the Master System 0.1.6-exp: views drawn live"
    rom.run(120)                       # 2 s of the title screen
    rom.run(3, ("a",))                 # button 1: start the game
    rom.run(2)
    rom.wait_idle()                    # until the engine waits for input
    # borrow test_render's pause-menu driver without building a full Checker
    chk = R.Checker.__new__(R.Checker)
    chk.rom = rom
    F = T.LAYOUT["fields"]
    w = gm.load()
    pilot = autopilot.Autopilot()
    plan = list(PLAN)
    step = 0
    while not (w.dead or w.level_done) and step < 3000:
        # time for the next view setting (never while a dialog or code entry is up)
        if plan and step >= plan[0][0] and w.dialog is None and w.password is None:
            _, size, black, cap = plan.pop(0)
            state["caption"] = "Pause menu: " + cap.split(" (")[0].split(",")[0]
            R.Checker.set_size(chk, size, black)   # through the pause menu, on camera
            state["caption"] = cap
        if w.p.hp < 100:
            # keep the player alive: HP and max HP to 200 in ROM and model
            for name in ("hp", "maxhp"):
                rom.e.poke(T.STATE_ADDR + F[name][0], 200)
            w.p.hp = w.p.maxhp = 200
        if w.dialog is not None:
            # page through the dialog, slowly enough to read
            for _ in range(T.pages(w, w.g["strings"].index(w.dialog))):
                rom.run(50)
                rom.press(("a",))
            w.resume()
            continue
        if w.password is not None:
            # type the right door code
            for b in T.code_inputs(w.password[0]):
                rom.run(6)
                rom.press(b)
            w.resume(w.password[0])
            continue
        act = pilot.choose(w)
        buttons = {"fwd": ("up",), "back": ("down",), "left": ("left",), "right": ("right",), "use": ("a",),
                   "pass": ("2", "down"), "wnext": ("2", "right"), "wprev": ("2", "left"), "heal": ("2", "up")}[act]
        if buttons[0] == "2":
            rom.press(("2",))
            rom.press(buttons[1:], held=("2",))
            rom.run(2)
        else:
            rom.press(buttons)
        # the same action in the model
        {"fwd": lambda: w.move(False), "back": lambda: w.move(True), "left": lambda: w.turn(-1),
         "right": lambda: w.turn(1), "use": w.use, "pass": w.pass_turn,
         "wnext": lambda: w.cycle_weapon(1), "wprev": lambda: w.cycle_weapon(-1),
         "heal": w.use_health_item}[act]()
        # lockstep: the ROM's game state must be byte for byte the model's
        assert rom.state() == T.model_bytes(w), f"out of step with the rules model at step {step}"
        step += 1
    state["caption"] = "level01 complete"
    rom.run(240)
    ff.stdin.close()                   # finish the video stream
    ff.wait()
    # the sound: 16-bit stereo at the core's sample rate, then mux with ffmpeg
    wav = str(out) + ".wav"
    with wave.open(wav, "wb") as f:
        f.setnchannels(2)
        f.setsampwidth(2)
        f.setframerate(int(round(rom.e.sample_rate())))
        f.writeframes(bytes(rom.e.audio))
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(out) + ".video.mp4", "-i", wav, "-c:v", "copy",
                    "-c:a", "aac", "-b:a", "128k", "-shortest", str(out)], check=True)
    Path(str(out) + ".video.mp4").unlink()
    Path(wav).unlink()
    print(f"{out}: {step} actions, {rom.nframe} frames ({rom.nframe / 59.92 / 60:.1f} min)")


if __name__ == "__main__":
    main()
