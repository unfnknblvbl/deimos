#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The Mega Drive game 0.0.6 demo video: the whole game, in highlights.

  python3 tools/md_campaign_demo.py OUT.mp4        (needs ffmpeg and the GPGX core)

1. Plan: the campaign pilot (tools/campaign_pilot.py) plays the whole game
   on the Python model, as the lockstep test does (the player's health is
   kept up), and the plan notes every move and where the interesting
   moments are: each map's start and exit, the vendors, secrets, the items,
   the end.
2. Film: the ROM replays the plan move for move (its state is checked
   against the model after every move). Only the highlights are filmed, with
   a short pause after each move, like a person playing; the rest is played
   at full speed, off camera.

Licence: GPL-2.0-or-later.
"""
import subprocess
import sys
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import game_model as gm  # noqa: E402
import campaign_pilot as cp  # noqa: E402
import md_lockstep as L  # noqa: E402
from md_campaign_lockstep import model_bytes, BUTTONS, pages, UI_END  # noqa: E402
from PIL import Image, ImageDraw, ImageFont  # noqa: E402

ROM = str(Path(__file__).resolve().parent.parent / "out" / "deimos.md")  # the ROM filmed: build.sh's output
W, H, CAP = 640, 448, 48  # video size: the 320x224 screen doubled, plus a caption strip below
FPS = 59.92  # the NTSC Mega Drive's frame rate
PAUSE = 20            # frames after each filmed move (a person looks before the next one)
READ = 100            # frames to read a dialog page
UI_PLAY, UI_PAUSE, UI_OPTIONS, UI_TITLE = 0, 6, 7, 8  # values of the ROM's eng_mode


# ------------------------------------------------------------------ the plan
def god(w):
    """Keeps the model player alive as the lockstep test does: below 100 health, health and maximum become 200. Returns True if it changed them (the ROM must then be poked too)."""
    if w.p.hp < 100:
        w.p.hp = w.p.maxhp = 200
        return True
    return False


def plan():
    """-> (steps, events, titles). steps: ('dialog', pages) | ('code', code) |
    ('act', action) | ('buy', line); events: (step, kind, info).

    Plays the whole campaign on the model with the campaign pilot. At each
    vendor it buys the first line once, if it can afford it. Event kinds:
    "berserk" / "collar" (first use), "store", "secret", "exit" (info:
    (map, completed sector?)), "arrive" (info: map), "end". titles: each
    map's title. Raises SystemExit if the pilot dies.
    """
    c = gm.Campaign()
    pilot = cp.Pilot()
    steps, events, bought = [], [], set()
    titles = [m["title"] for m in c.info]
    first = {"berserk": True, "collar": True}
    while True:
        w = c.world
        if w.dead:
            raise SystemExit("the pilot died")
        god(w)
        s = len(steps)
        store0, nmsg = w.store, len(w.messages)
        if w.dialog is not None:
            steps.append(("dialog", pages(w.dialog)))
            w.resume()
        elif w.password is not None:
            steps.append(("code", w.password[0]))
            w.resume(w.password[0])
        elif (w.store is not None and (c.index, w.store) not in bought and w.store_items()
              and w.store_items()[0][1] <= w.credits):
            bought.add((c.index, w.store))
            steps.append(("buy", 0))
            w.store_buy(w.store_items()[0][0])
        else:
            a = pilot.choose(c)
            steps.append(("act", a))
            cp.ACTIONS[a](w)
            if a in first and first[a]:
                first[a] = False
                events.append((s, a, None))
        if store0 is None and w.store is not None:
            events.append((s, "store", None))
        if "Found Secret!" in w.messages[nmsg:]:
            events.append((s, "secret", None))
        here = c.index
        if c.advance():
            events.append((s, "end" if c.finished else "exit", (here, bool(c.stats))))
            if c.finished:
                return steps, events, titles
            events.append((s + 1, "arrive", c.index))


def windows(steps, events, titles):
    """-> {step: caption} of the filmed moves, and the step to switch back
    to STEP movement

    A window of moves round each interesting event (arrivals, exits, the
    first 3 secrets, up to 12 vendor moves, the items, the last fight),
    and the opening: about 45 moves of SMOOTH movement, then STEP from
    `switch` (the first real move after move 45) to move 75.
    """
    cap = {}

    def win(a, b, text):
        """Gives moves a..b-1 (clipped to the plan) the caption `text`; a later window overwrites an earlier one."""
        for i in range(max(0, a), min(len(steps), b)):
            cap[i] = text

    n_maps = len(titles)
    secrets = 0
    seen = set()
    for s, kind, info in events:
        if kind == "arrive":
            again = info in seen
            seen.add(info)
            win(s, s + (6 if again else 12), f"{'Back at ' if again else ''}{titles[info]} - map {info + 1} of {n_maps}")
        elif kind == "exit":
            win(s - (6 if info[0] in seen and not info[1] else 10), s + 1, f"Leaving {titles[info[0]]}")
    for s, kind, info in events:
        if kind == "secret" and secrets < 3:
            secrets += 1
            win(s - 3, s + 3, "A secret wall: Found Secret! (+5 XP)")
        elif kind == "store" and sum(1 for c in cap.values() if c.startswith("A vendor")) < 12:
            win(s - 4, s + 4, "A vendor: credits buy medkits, ammo and upgrades")
        elif kind == "berserk":
            win(s - 2, s + 8, "Items: the Berserker (double damage for a while)")
        elif kind == "collar":
            win(s - 2, s + 8, "Items: the Dog Collar - the dog fights for you")
        elif kind == "end":
            win(s - 30, s + 1, "The last fight ...")
    # the opening: SMOOTH movement, then back to STEP through the pause menu
    win(0, 75, "UAC Base: FM music, shaded floor and ceiling")
    switch = next(i for i in range(45, len(steps)) if steps[i][0] == "act" and steps[i][1] != "close")
    for i in range(0, switch):
        cap[i] = "SMOOTH movement" if i > 8 else cap[i]
    for i in range(switch, 75):
        cap[i] = "STEP movement (each step is a turn)"
    return cap, switch


# ------------------------------------------------------------------ filming
class Film:
    """The video being made: frames go to an ffmpeg process as raw RGB (each
    emulator frame doubled, with a caption and the buttons held below);
    the sound is collected in `audio` and muxed in at the end. `on` says
    whether frames are being filmed now; `rate` is the sample rate.
    """
    def __init__(self, out):
        """Starts ffmpeg writing the silent video to out + ".video.mp4"."""
        self.out = out
        self.ff = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
                                    "-s", f"{W}x{H + CAP}", "-r", str(FPS), "-i", "-", "-c:v", "libx264",
                                    "-pix_fmt", "yuv420p", "-crf", "20", out + ".video.mp4"],
                                   stdin=subprocess.PIPE)
        self.audio = bytearray()
        self.caption = ""
        self.on = False
        self.frames = 0
        self.rate = 0

    def frame(self, im, buttons):
        """Adds one frame: the emulator's image `im`, the caption, and the names of the buttons held."""
        fr = Image.new("RGB", (W, H + CAP), (16, 16, 24))
        fr.paste(im.resize((W, H), Image.NEAREST), (0, 0))
        d = ImageDraw.Draw(fr)
        d.text((70 + (W - 70) // 2, H + CAP // 2), self.caption, font=FONT, fill=(255, 220, 120), anchor="mm")
        if buttons:
            d.text((8, H + CAP // 2), " ".join(b.upper() for b in buttons), font=FONT, fill=(140, 200, 255),
                   anchor="lm")
        self.ff.stdin.write(fr.tobytes())
        self.frames += 1

    def card(self, text, n=60):
        """a title card between highlights (silent)

        n frames of `text` on black, with as much silence in the sound.
        """
        fr = Image.new("RGB", (W, H + CAP), (0, 0, 0))
        d = ImageDraw.Draw(fr)
        d.text((W // 2, (H + CAP) // 2), text, font=BIG, fill=(200, 200, 200), anchor="mm")
        for _ in range(n):
            self.ff.stdin.write(fr.tobytes())
            self.audio.extend(bytes(int(self.rate / FPS) * 4))
            self.frames += 1

    def close(self):
        """Ends the video, writes the sound to out + ".wav", and muxes both into `out` (AAC sound)."""
        self.ff.stdin.close()
        self.ff.wait()
        with wave.open(self.out + ".wav", "wb") as f:
            f.setnchannels(2)
            f.setsampwidth(2)
            f.setframerate(int(round(self.rate)))
            f.writeframes(bytes(self.audio))
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", self.out + ".video.mp4", "-i",
                        self.out + ".wav", "-c:v", "copy", "-c:a", "aac", "-b:a", "160k", "-shortest", self.out],
                       check=True)


# caption fonts (DejaVu Sans must be installed); FILM: the Film being made
FONT = ImageFont.truetype("DejaVuSans.ttf", 18)
BIG = ImageFont.truetype("DejaVuSans.ttf", 26)
FILM = None


class Rec(L.Rom):
    """The lockstep's Rom, filming every frame (and recording the sound) while FILM.on."""
    def run(self, n, buttons=()):
        """Runs n frames holding `buttons`, passing each to FILM when filming."""
        names = buttons
        buttons = tuple(L.BUTTONS[b] for b in buttons)
        self.e.audio = FILM.audio if FILM.on else None
        for _ in range(n):
            self.e.run_frames(1, buttons)
            if FILM.on:
                FILM.frame(self.e.screenshot(), names)
            self.nframe += 1


def boot(pad=6):
    """Starts a fresh emulator on the ROM with a 3- or 6-button pad. Returns the Rec."""
    r = Rec(ROM)
    r.e.set_pad(pad)
    FILM.rate = r.e.sample_rate()
    return r


def tap(r, b, wait=20):
    """Presses button b once, then lets `wait` frames go by (for the viewer)."""
    r.press((b,))
    r.run(wait)


def mode(r):
    """The ROM's current UI mode (eng_mode)."""
    return r.byte(L.ENG_MODE)


def main():
    """md_campaign_demo.py OUT.mp4: plans the game, then films it: the 3-button
    pad's title screen, the options (SMOOTH), and the whole game replayed
    move for move in the ROM, filming only the planned windows, checking
    the ROM's state against the model after every move (SystemExit on a
    mismatch).

    Environment: DEMO_PLAN = a file to cache the plan in (pickle);
    DEMO_NOFILM: film only the opening; DEMO_ONLY = "a:b caption": film
    only moves a..b-1 with that caption.
    """
    global FILM
    out = sys.argv[1]
    print("planning the campaign on the model ...", flush=True)
    import os
    import pickle
    cache = os.environ.get("DEMO_PLAN")
    if cache and os.path.exists(cache):
        steps, events, titles = pickle.load(open(cache, "rb"))
    else:
        steps, events, titles = plan()
        if cache:
            pickle.dump((steps, events, titles), open(cache, "wb"))
    cap, switch = windows(steps, events, titles)
    if os.environ.get("DEMO_NOFILM"):           # (tests) replay without filming
        cap = {i: "" for i in range(switch + 1)}
    only = os.environ.get("DEMO_ONLY")           # "a:b caption": film only moves a..b-1
    if only:
        rng, text = (only.split(" ", 1) + [""])[:2]
        a, b = (int(x) for x in rng.split(":"))
        cap = {i: text for i in range(a, b)}
        switch = -1
    print(f"plan: {len(steps)} moves, {len(cap)} filmed", flush=True)
    FILM = Film(out)
    FILM.on = True

    # ---- a 3-button pad
    if only:
        FILM.on = False
    r = boot(pad=3)
    FILM.caption = "A 3-button pad: the title asks for A+B+C+Start"
    r.run(200)
    r.run(4, ("a", "b", "c", "start"))
    FILM.caption = "A+B+C+Start: the game begins (6 buttons are better)"
    r.run(150)
    if not only:
        FILM.card("A 6-button pad for the rest of the game", 90)

    # ---- the title: SMOOTH movement, new game
    r = boot(pad=6)
    FILM.caption = "Mega Drive version 0.0.6: the whole game"
    r.run(150)
    tap(r, "start", 40)
    if not only:
        FILM.caption = "Options: movement SMOOTH"
        tap(r, "down", 30)
        tap(r, "a", 60)
        assert mode(r) == UI_OPTIONS
        tap(r, "a", 80)
        tap(r, "start", 40)
        assert mode(r) == UI_TITLE, mode(r)
    else:
        tap(r, "down", 30)
    FILM.caption = "New game"
    tap(r, "up", 30)
    r.press(("a",))
    r.wait_idle()

    # ---- the game, move by move
    c = gm.Campaign()
    F = L.LAYOUT["fields"]
    prev_on = not only
    last = max(cap) if only else len(steps)
    for i, (kind, arg) in enumerate(steps):
        if i > last:
            break
        w = c.world
        on = i in cap
        if on and not prev_on and not only:
            FILM.card(". . .", 45)
        FILM.on = on
        prev_on = on
        if on:
            FILM.caption = cap[i]
        if i == switch:                       # pause menu -> options -> STEP
            FILM.caption = "Pause menu: Options: movement STEP"
            tap(r, "start", 40)
            assert mode(r) == UI_PAUSE
            # down 4 lines to OPTIONS (resume, items, notebook, save game, options)
            for _ in range(4):
                tap(r, "down", 15)
            tap(r, "a", 50)
            assert mode(r) == UI_OPTIONS
            tap(r, "a", 60)
            tap(r, "start", 40)
            assert mode(r) == UI_PAUSE
            # back up to RESUME
            for _ in range(4):
                tap(r, "up", 12)
            tap(r, "a", 30)
            assert mode(r) == UI_PLAY
            FILM.caption = cap[i]
        if god(w):
            for name in ("hp", "maxhp"):
                r.e.poke(L.STATE_ADDR + F[name][0], 200)
        if kind == "dialog":
            for _ in range(arg):
                if on:
                    r.run(READ)
                r.press(("a",))
            w.resume()
        elif kind == "code":
            for b in L.code_inputs(arg):
                if on:
                    r.run(8)
                r.press(b)
            w.resume(arg)
        elif kind == "buy":
            if on:
                r.run(60)
            r.press(("a",))
            w.store_buy(w.store_items()[arg][0])
            if on:
                r.run(60)
        else:
            for b in BUTTONS[arg]:
                if on and len(BUTTONS[arg]) > 1:
                    r.run(25)
                r.press(b)
            cp.ACTIONS[arg](w)
        if on:
            r.run(PAUSE)
        here = c.index
        if c.advance():
            if c.stats:
                if on:
                    FILM.caption = f"{titles[here]} complete"
                    r.run(180)
                if c.finished:
                    FILM.on = True
                    FILM.caption = "THE END"
                    r.run(400)
                    assert mode(r) == UI_END
                    break
                r.press(("a",))
            print(f"move {i}: {titles[c.index]}", flush=True)
        if r.state() != model_bytes(c):
            print(L.describe_diff(r.state(), model_bytes(c)), "\nmodel messages:", c.world.messages[-8:],
                  "\nrecent moves:", steps[max(0, i - 15):i + 1], flush=True)
            raise SystemExit(f"move {i}: the ROM is out of step with the model")
    FILM.close()
    print("done:", FILM.frames, "frames,", round(FILM.frames / FPS), "s")


if __name__ == "__main__":
    main()
