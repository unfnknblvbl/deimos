#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
A short gameplay video of a Master System or Game Gear ROM: the title
screen, then the map pilot (tools/sms_pilot.py) plays on the Gearsystem
core, its health kept up (as in the lockstep test), with the model in step
so it answers dialogs and door codes. Every frame and the sound, scaled
up, with a caption bar.

Usage: console_video.py ROM OUT.mp4 [seconds = 75] [caption]   (needs ffmpeg)

How the ROM and model stay together: test_lockstep.Rom runs the ROM on the
Gearsystem libretro core (tools/gearsystem_harness.py); game_model.World is
the Python re-implementation of the rules. Each action is pressed on the
ROM and then applied to the model, so the model always knows when a dialog
or a door code is up and what the right answer is.

Output: frames are piped raw to one ffmpeg (H.264, OUT.mp4.video.mp4), the
emulator's audio is collected into OUT.mp4.wav, and a second ffmpeg run
muxes the two into OUT.mp4 and the temporary files are deleted.
Licence: GPL-2.0-or-later.
"""
import subprocess
import sys
import wave
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).parent))
import test_lockstep as T  # noqa: E402
import game_model as gm  # noqa: E402
import sms_pilot  # noqa: E402

CAP = 44                        # caption bar height under the picture, pixels
FONT = ImageFont.truetype("DejaVuSans.ttf", 19)
# pilot action -> pad buttons; "2" first means hold button 2, then press the rest
BUTTONS = {"fwd": ("up",), "back": ("down",), "left": ("left",), "right": ("right",),
           "use": ("a",), "pass": ("2", "down"), "wnext": ("2", "right"), "wprev": ("2", "left"),
           "heal": ("2", "up")}


def main():
    """Record the video described in the module docstring (arguments from sys.argv)."""
    rom_path, out = sys.argv[1], Path(sys.argv[2])
    seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 75
    caption = sys.argv[4] if len(sys.argv) > 4 else ""
    gg = rom_path.endswith(".gg")
    scale = 4 if gg else 3             # Game Gear 160x144 x4, Master System 256x192 x3
    W, H = (160 * scale, 144 * scale) if gg else (256 * scale, 192 * scale)
    max_frames = int(seconds * 59.92)  # the NTSC frame rate the core runs at
    tmp = str(out) + ".video.mp4"
    # the video encoder: reads raw RGB frames on its standard input
    ff = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
                           "-s", f"{W}x{H + CAP}", "-r", "59.92", "-i", "-", "-c:v", "libx264", "-pix_fmt",
                           "yuv420p", "-crf", "18", tmp], stdin=subprocess.PIPE)
    state = {"caption": caption}       # a dict so Rec.run sees later changes

    class Done(Exception):
        """Raised by Rec.run when the video has reached max_frames."""
        pass

    class Rec(T.Rom):
        """A test_lockstep.Rom that writes every emulated frame to the video."""

        def run(self, n, buttons=()):
            """Run n frames with buttons held, sending each (scaled, captioned) to ffmpeg.

            Raises Done once max_frames frames have been written.
            """
            for _ in range(n):
                self.e.run_frames(1, buttons)
                im = self.e.screenshot().convert("RGB").resize((W, H), Image.NEAREST)
                fr = Image.new("RGB", (W, H + CAP), (16, 16, 24))
                fr.paste(im, (0, 0))
                ImageDraw.Draw(fr).text((W // 2, H + CAP // 2), state["caption"], font=FONT,
                                        fill=(255, 220, 120), anchor="mm")
                ff.stdin.write(fr.tobytes())
                self.nframe += 1
                if self.nframe >= max_frames:
                    raise Done()

    rom = Rec(rom_path)
    rom.e.audio = bytearray()          # the harness appends the sound samples here
    try:
        rom.run(150)                          # the title
        rom.run(3, ("a",))                    # button 1: start the game
        rom.run(2)
        rom.wait_idle()                       # until the engine waits for input
        w = gm.load()                         # the rules model at the level start
        F = T.LAYOUT["fields"]
        one = sms_pilot.OneMap(w)             # the model as a one-map campaign
        pilot = sms_pilot.cp.Pilot()          # campaign_pilot's map pilot
        while not (w.dead or w.level_done):
            if w.p.hp < 100:
                # keep the player alive: HP and max HP to 200 in ROM and model
                for name in ("hp", "maxhp"):
                    rom.e.poke(T.STATE_ADDR + F[name][0], 200)
                w.p.hp = w.p.maxhp = 200
            if w.dialog is not None:
                sid = w.g["strings"].index(w.dialog)
                for _ in range(T.pages(w, sid)):
                    rom.run(70)               # time to read it
                    rom.press(("a",))
                w.resume()
            elif w.password is not None:
                code = w.password[0]          # type the right door code
                for b in T.code_inputs(code):
                    rom.press(b)
                w.resume(code)
            else:
                act = pilot.choose(one)
                b = BUTTONS[act]
                if b[0] == "2":
                    rom.press(("2",))
                    rom.press(b[1:], held=("2",))
                    rom.run(2)
                else:
                    rom.press(b)
                # the same action in the model
                {"fwd": lambda: w.move(False), "back": lambda: w.move(True), "left": lambda: w.turn(-1),
                 "right": lambda: w.turn(1), "use": w.use, "pass": w.pass_turn,
                 "wnext": lambda: w.cycle_weapon(1), "wprev": lambda: w.cycle_weapon(-1),
                 "heal": w.use_health_item}[act]()
                rom.run(8)                    # a short pause, as a player would
        rom.run(240)                          # 4 s on the last picture
    except Done:
        pass
    ff.stdin.close()                          # finish the video stream
    ff.wait()
    # the sound: 16-bit stereo at the core's sample rate
    wav = str(out) + ".wav"
    with wave.open(wav, "wb") as f:
        f.setnchannels(2)
        f.setsampwidth(2)
        f.setframerate(int(round(rom.e.sample_rate())))
        f.writeframes(bytes(rom.e.audio))
    # mux: copy the video, encode the sound as AAC
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", tmp, "-i", wav, "-c:v", "copy", "-c:a", "aac",
                    "-b:a", "128k", "-shortest", str(out)], check=True)
    Path(tmp).unlink()
    Path(wav).unlink()
    print(f"{out}: {rom.nframe} frames")


if __name__ == "__main__":
    main()
