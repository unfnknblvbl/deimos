#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""Usage: python3 md_demo_video.py OUT.mp4   (needs ffmpeg and the GPGX core, see md_harness.py)
The Mega Drive game 0.0.3 demo video: 3-button warning, menus and remapping,
step play, smooth play, holding Mode, saving and continuing after a restart."""
import subprocess, sys, wave
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import md_lockstep as L
import game_model as gm
import autopilot
from PIL import Image, ImageDraw, ImageFont

# The script runs at import: it starts ffmpeg at once and films the ROM as
# it goes. It was written for version 0.0.3 (Sector 1 alone: it plays with
# game_model.load() and the Sector 1 autopilot); the menu positions it
# steps through are that version's, so it may not match the current ROM
# (md_campaign_demo.py is the current demo).
ROM = str(Path(__file__).resolve().parent.parent / 'out' / 'deimos.md')
OUT = sys.argv[1]
FONT = ImageFont.truetype('DejaVuSans.ttf', 18)
W, H, CAP = 640, 448, 48  # video size: the screen doubled, plus a caption strip
ff = subprocess.Popen(['ffmpeg', '-y', '-loglevel', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
                       '-s', f'{W}x{H + CAP}', '-r', '59.92', '-i', '-', '-c:v', 'libx264', '-pix_fmt', 'yuv420p',
                       '-crf', '18', OUT + '.video.mp4'], stdin=subprocess.PIPE)
# caption: the text under the picture; audio: the sound so far; rate: its sample rate
caption = ''
audio = bytearray()
rate = None


class Rec(L.Rom):
    """The lockstep's Rom, writing every frame (doubled, with the caption and the buttons held) to ffmpeg."""
    def run(self, n, buttons=()):
        """Runs n frames holding `buttons`, filming each."""
        buttons = tuple(L.BUTTONS[b] for b in buttons)
        for _ in range(n):
            self.e.run_frames(1, buttons)
            im = self.e.screenshot().resize((W, H), Image.NEAREST)
            fr = Image.new('RGB', (W, H + CAP), (16, 16, 24))
            fr.paste(im, (0, 0))
            d = ImageDraw.Draw(fr)
            d.text((70 + (W - 70) // 2, H + CAP // 2), caption, font=FONT, fill=(255, 220, 120), anchor='mm')
            if buttons:
                d.text((8, H + CAP // 2), ' '.join(b.upper() for b in buttons), font=FONT,
                       fill=(140, 200, 255), anchor='lm')
            ff.stdin.write(fr.tobytes())
            self.nframe += 1


def boot(sram=None, pad=6):
    """Starts a fresh emulator on the ROM (a 3- or 6-button pad), restoring
    the battery RAM `sram` if given (a console switched on again), and
    records its sound into `audio`. Returns the Rec.
    """
    global rate
    r = Rec(ROM)
    r.e.set_pad(pad)
    if sram is not None:
        r.e.set_sram(sram)
    r.e.audio = audio
    rate = r.e.sample_rate()
    return r


def play(r, w, pilot, n, held=()):
    """Lets the Sector 1 autopilot play n actions in the ROM (r) and the model
    (w), holding `held` throughout. Health below 22 is refilled in both
    (to keep the demo going). Dialogs and codes are answered at a reading
    pace. Asserts that the ROM's state still matches the model's at the end.
    """
    for _ in range(n):
        if w.dead or w.level_done:
            break
        if w.p.hp < 22:                       # keep the demo alive (the ROM and the model alike)
            r.e.poke(L.STATE_ADDR + L.LAYOUT['fields']['hp'][0], w.p.maxhp)
            w.p.hp = w.p.maxhp
        if w.dialog is not None:
            sid = w.g['strings'].index(w.dialog)
            for _ in range(L.pages(w, sid)):
                r.run(40, held)
                r.press(('a',), held)
            w.resume()
            continue
        if w.password is not None:
            for b in L.code_inputs(w.password[0]):
                r.run(8, held)
                r.press(b, held)
            w.resume(w.password[0])
            continue
        act = pilot.choose(w)
        b = {'fwd': 'up', 'back': 'down', 'left': 'left', 'right': 'right', 'use': 'a', 'pass': 'b',
             'wnext': 'z', 'wprev': 'x', 'heal': 'c'}[act]
        r.press((b,), held)
        {'fwd': lambda: w.move(False), 'back': lambda: w.move(True), 'left': lambda: w.turn(-1),
         'right': lambda: w.turn(1), 'use': w.use, 'pass': w.pass_turn, 'wnext': lambda: w.cycle_weapon(1),
         'wprev': lambda: w.cycle_weapon(-1), 'heal': w.use_health_item}[act]()
        r.run(6, held)
    assert r.state() == L.model_bytes(w), 'out of step with the model'


def say(text, r, frames=0):
    """Sets the caption, then lets `frames` frames go by."""
    global caption
    caption = text
    if frames:
        r.run(frames)


def tap(r, b, wait=20):
    """Presses button b once, then lets `wait` frames go by."""
    r.press((b,))
    r.run(wait)


# ---- 1. a 3-button pad
r = boot(pad=3)
say('A 3-button pad is plugged in', r, 150)
r.run(3, ('start',))
say('A 3-button pad is plugged in: the game will not start', r, 200)
r.run(3, ('a',))
r.run(120)
r.e.set_pad(6)
say('A 6-button pad is plugged in', r, 120)
tap(r, 'start', 40)
# ---- 2. options and remapping  (rows: 0 movement, 1-7 actions, 8 defaults, 9 done)
def mode(r):
    """The ROM's current UI mode (eng_mode: 0 playing, 6 pause menu, 7 options, 8 title)."""
    return r.byte(L.ENG_MODE)
say('Title menu: Options', r)
tap(r, 'down', 30)
tap(r, 'a', 60)
assert mode(r) == 7
say('Options: every action except Start can go on any button', r, 150)
tap(r, 'down', 30)
tap(r, 'down', 40)
say('Remap: Y on PASS TURN (NOTEBOOK gets B in exchange)', r, 30)
tap(r, 'y', 150)
say('DEFAULTS puts them all back', r, 30)
for _ in range(6):
    tap(r, 'down', 12)
tap(r, 'a', 120)
say('Movement stays STEP for now: DONE', r)
tap(r, 'down', 30)
tap(r, 'a', 40)
assert mode(r) == 8
# ---- 3. step play
say('New game', r)
tap(r, 'up', 30)
tap(r, 'a', 10)
r.wait_idle()
assert mode(r) == 0
w = gm.load()
pilot = autopilot.Autopilot()
say('STEP movement (full detail)', r)
play(r, w, pilot, 45)
r.run(60)
# ---- 4. smooth via the pause menu
say('Start: pause menu', r)
tap(r, 'start', 60)
tap(r, 'down', 15)
tap(r, 'down', 15)
tap(r, 'down', 30)
tap(r, 'a', 60)
assert mode(r) == 7
say('Options: Movement SMOOTH', r)
tap(r, 'a', 80)
tap(r, 'start', 40)
assert mode(r) == 6
say('Resume', r)
for _ in range(3):
    tap(r, 'up', 15)
tap(r, 'a', 40)
assert mode(r) == 0
say('SMOOTH movement (low detail)', r)
play(r, w, pilot, 45)
r.run(30)
say('Standing still, holding Mode: full detail', r)
r.run(90, ('mode',))
r.run(60)
say('Holding Mode while moving: sharp at every stop', r)
play(r, w, pilot, 20, held=('mode',))
say('SMOOTH movement (low detail)', r)
play(r, w, pilot, 25)
r.run(30)
# ---- 5. save, power off, continue
say('Pause menu: Save game', r)
tap(r, 'start', 40)
tap(r, 'down', 15)
tap(r, 'down', 30)
tap(r, 'a', 120)
saved_state = r.state()
sram = r.e.sram()
say('Power off ...', r, 30)
# 45 frames of black and silence: the console off
for _ in range(45):
    ff.stdin.write(Image.new('RGB', (W, H + CAP), (0, 0, 0)).tobytes())
    audio.extend(bytes(int(rate / 59.92) * 4))
r = boot(sram)
say('... and on again: Continue', r, 90)
tap(r, 'start', 40)
tap(r, 'a', 60)
assert r.state() == saved_state, 'the saved game did not come back'
say('Continued - still smooth: settings are saved too', r)
play(r, w, pilot, 25)
r.run(90)
ff.stdin.close()
ff.wait()
with wave.open(OUT + '.wav', 'wb') as f:
    f.setnchannels(2)
    f.setsampwidth(2)
    f.setframerate(int(round(rate)))
    f.writeframes(bytes(audio))
subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', OUT + '.video.mp4', '-i', OUT + '.wav', '-c:v', 'copy',
                '-c:a', 'aac', '-b:a', '128k', '-shortest', OUT], check=True)
print('done', r.nframe)
