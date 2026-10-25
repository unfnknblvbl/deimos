#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Checks the live renderer: plays the ROM on the Gearsystem core and, after
every step, compares the view on screen pixel for pixel with the bit-exact
Python model (tools/live_data.py), at both view sizes; also measures how
many frames each step took to draw.

Usage: test_render.py rom/deimos.sms [steps] [seed] [--size large|small|both] [--shots DIR]
Exit code 0 = every picture matched.
Licence: GPL-2.0-or-later.

The ROM's screen is read back as colour indices by matching each pixel to
the level palette. Where a hardware sprite (a monster or item drawn with
the VDP's sprites, not by the renderer) covers the view, those pixels are
not compared. Standalone, the model's data comes from running
live_data.main() on analysis/level01.json (level01); the Checker used by
test_lockstep.py --render reads it from the ROM itself (rom_level.py).
The engine variable addresses are read from rom/main.asm and
rom/deimos.sym, so they follow the engine's source.
"""
import contextlib
import io
import random
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))
import test_lockstep as T
import live_data as L
import rom_level

ROOT = Path(__file__).resolve().parent.parent
VIEW_POS = {"large": (48, 32), "small": (64, 40)}   # view's top-left pixel on the SMS screen
# engine variables in RAM: the view's cell x (then z and facing in the next
# two bytes), view size (1 small), drawing mode (1 at once), pause, low
# detail, solid walls
ENG_VIEW_X, ENG_VSIZE, ENG_VMODE, ENG_PAUSE, ENG_DETAIL, ENG_SOLID = 0xC009, 0xC00C, 0xC00D, 0xC00E, 0xC00F, 0xC005


def engine_vars():
    """addresses of the engine variables (rom/main.asm, .ENUM $C010)

    Parses the .ENUM block: each "name db/dw/dsb n" line takes 1, 2 or n
    bytes from $C010 on. Returns {name: address}."""
    src = (Path(__file__).resolve().parent.parent / "rom" / "main.asm").read_text()
    block = src[src.index(".ENUM $C010"):]
    block = block[:block.index(".ENDE")]
    addr, out = 0xC010, {}
    for line in block.splitlines()[1:]:
        p = line.split(";")[0].split()
        if len(p) < 2:
            continue
        out[p[0]] = addr
        addr += {"db": 1, "dw": 2}.get(p[1], int(p[2]) if p[1] == "dsb" else 0)
    return out


_V = engine_vars()
# hardware-sprite state: a flag when one is on screen, and its record's
# cols, rows, x, y (four bytes from spr_cols)
VAR_NEED_LAYOUT, VAR_SPR_SHOWN, SPR_COLS = _V["var_need_layout"], _V["var_spr_shown"], _V["spr_cols"]


def sym(name):
    """The address of label name in rom/deimos.sym (KeyError if absent)."""
    for line in (ROOT / "rom" / "deimos.sym").read_text().splitlines():
        p = line.split()
        if len(p) == 2 and p[1] == name:
            return int(p[0].split(":")[-1], 16)
    raise KeyError(name)


def load_data():
    """Build the renderer's data for level01 with live_data.main() and return it.

    live_data reads its arguments from sys.argv, so they are swapped in for
    the call; its output files go to /tmp/live_test_out and its printing is
    discarded.
    """
    a = ROOT / "analysis"
    sys.argv_saved = sys.argv
    argv = ["live_data.py", str(a / "level01.json"), str(a / "gamedata.json"), str(a / "level01_sms.json"),
            str(ROOT / "assets" / "jar"), "/tmp/live_test_out"]
    old = sys.argv
    sys.argv = argv
    with contextlib.redirect_stdout(io.StringIO()):
        D = L.main()
    sys.argv = old
    return D


def screen_view(rom, size, pal):
    """The view area of the ROM's screen as palette indices.

    Returns (H x W nearest palette index per pixel, H x W squared colour
    distance to it).
    """
    img = np.array(rom.e.screenshot().convert("RGB")).astype(int)
    W, H = L.SIZES[size]
    x0, y0 = VIEW_POS[size]
    v = img[y0:y0 + H, x0:x0 + W]
    d = ((v[:, :, None, :] - pal[None, None].astype(int)) ** 2).sum(-1)   # distance to each palette colour
    return d.argmin(-1).astype(np.uint8), d.min(-1)


def set_size(rom, size):
    """the pause menu: VIEW SIZE is the second line

    Opens the pause menu, moves to VIEW SIZE, toggles it and resumes, if
    the ROM is not already at size."""
    want = 1 if size == "small" else 0
    if rom.ram()[ENG_VSIZE - 0xC000] == want:
        return
    rom.press(("start",))
    rom.press(("down",))
    rom.press(("down",))
    rom.press(("a",))
    rom.press(("2",))
    assert rom.ram()[ENG_VSIZE - 0xC000] == want and rom.ram()[T.ENG_MODE - 0xC000] == 0


class Checker:
    """Compares the view on screen with the model after a step (used by
    test_lockstep.py --render)."""

    def __init__(self, rom, rom_path=None):
        """Check rom (a test_lockstep.Rom); rom_path is the ROM file to read the level data from."""
        self.rom = rom
        # the model draws from the ROM's own level data (so editor-made ROMs
        # are checked too); without a path, from a fresh Python build
        self.D = rom_level.load(rom_path) if rom_path else load_data()
        # one model per (size, detail, solid walls) setting
        self.models = {(s, d, w): L.Model(self.D, s, d, w) for s in L.SIZES for d in (0, 1) for w in (0, 1)}
        self.pal = self.D["palette"].astype(int)
        self.n = {}                    # pictures checked per setting
        self.size = "large"

    def set_size(self, size, black, detail=0, solid=0):
        # black: drawing the view, 0 outward (in place), 1 at once
        """the pause menu: resume, notebook, view size, drawing the view,
        detail, walls

        Sets the view size, black (drawing at once), detail and solid walls
        through the menu, pressing button 1 on a line (at most twice) until
        its RAM value is right. Asserts the menu closed with the values
        set."""
        rom = self.rom
        want = {ENG_VSIZE: 1 if size == "small" else 0, ENG_VMODE: black, ENG_DETAIL: detail, ENG_SOLID: solid}
        r = rom.ram()
        self.size = size
        if all(r[a - 0xC000] == v for a, v in want.items()):
            return
        rom.press(("start",))
        rom.press(("down",))                    # notebook
        for a in (ENG_VSIZE, ENG_VMODE, ENG_DETAIL, ENG_SOLID):   # menu lines 3..6
            rom.press(("down",))
            for _ in range(2):
                if rom.ram()[a - 0xC000] == want[a]:
                    break
                rom.press(("a",))
        rom.press(("2",))
        r = rom.ram()
        assert all(r[a - 0xC000] == v for a, v in want.items()) and r[T.ENG_MODE - 0xC000] == 0

    def check(self):
        """Compare the view on screen now with the model; None if they match.

        On a mismatch, saves model / screen / difference side by side to
        /tmp/render_mismatch.png and returns a message.
        """
        rom = self.rom
        rom.run(3)                     # let the picture settle
        r = rom.ram()
        size = "small" if r[ENG_VSIZE - 0xC000] else "large"
        detail = r[ENG_DETAIL - 0xC000]
        solid = r[ENG_SOLID - 0xC000]
        x, z, f = r[ENG_VIEW_X - 0xC000], r[ENG_VIEW_X - 0xC000 + 1], r[ENG_VIEW_X - 0xC000 + 2]
        F = T.LAYOUT["fields"]
        # the switchable lines' flags (open, locked, alive) from the game state
        lf0, nl = F["l_flags"][0] + T.STATE_ADDR, F["l_flags"][1]
        flags = [r[lf0 - 0xC000 + i] for i in range(nl)]
        want = self.models[size, detail, solid].render(x, z, f, flags)
        got, _ = screen_view(rom, size, self.pal)
        mask = got != want
        if r[VAR_SPR_SHOWN - 0xC000]:
            # skip the hardware sprite's rectangle (8x16 sprites; the SAT y
            # is one line above the first row drawn)
            cols, rows, sx, sy = (r[SPR_COLS - 0xC000 + k] for k in range(4))
            vx, vy = VIEW_POS[size]
            ys, xs = np.mgrid[0:mask.shape[0], 0:mask.shape[1]]
            mask &= ~((xs + vx >= sx) & (xs + vx < sx + cols * 8) & (ys + vy >= sy + 1) & (ys + vy < sy + 1 + rows * 16))
        key = size + (" low" if detail else "") + (" solid" if solid else "")
        self.n[key] = self.n.get(key, 0) + 1
        if mask.any():
            P = self.D["palette"]
            im = np.concatenate([P[want], P[got], np.where(mask[..., None], 255, 0).repeat(3, -1).astype(np.uint8)], 1)
            Image.fromarray(im.astype(np.uint8)).resize((im.shape[1] * 3, im.shape[0] * 3), Image.NEAREST).save(
                "/tmp/render_mismatch.png")
            return f"{size} view ({x},{z},{f}): {int(mask.sum())} pixels differ (/tmp/render_mismatch.png)"
        return None

    def summary(self):
        """One line: how many pictures were checked per setting."""
        return "pictures checked: " + ", ".join(f"{k} {v}" for k, v in self.n.items())


def main():
    """Play random moves, alternating view sizes, and compare each view; returns the exit code."""
    rom_path = sys.argv[1]
    steps = int(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else 40
    seed = int(sys.argv[3]) if len(sys.argv) > 3 and not sys.argv[3].startswith("--") else 1
    sizes = ["large", "small"]
    if "--size" in sys.argv:
        v = sys.argv[sys.argv.index("--size") + 1]
        sizes = sizes if v == "both" else [v]
    shots = Path(sys.argv[sys.argv.index("--shots") + 1]) if "--shots" in sys.argv else None
    if shots:
        shots.mkdir(parents=True, exist_ok=True)
    D = load_data()
    models = {s: L.Model(D, s) for s in L.SIZES}
    pal = D["palette"].astype(int)
    rom = T.Rom(rom_path)
    rom.run(30)                        # the title screen
    rom.run(3, ("a",))                 # button 1: start
    rom.run(2)
    rom.wait_idle()
    rnd = random.Random(seed)
    F = T.LAYOUT["fields"]
    lf0, nl = F["l_flags"][0] + T.STATE_ADDR, F["l_flags"][1]
    bad = 0
    times = {s: [] for s in sizes}
    for step in range(steps):
        size = sizes[step % len(sizes)]
        set_size(rom, size)
        # an action: mostly moves and turns
        b = rnd.choice(["up", "up", "up", "left", "right", "down", "a"])
        seq = rom.byte(T.ENG_TURN_SEQ)         # (not used)
        f0 = rom.nframe
        rom.press((b,))
        dt = rom.nframe - f0                   # frames the step took
        r = rom.ram()
        x, z, f = r[ENG_VIEW_X - 0xC000], r[ENG_VIEW_X - 0xC000 + 1], r[ENG_VIEW_X - 0xC000 + 2]
        if r[T.ENG_MODE - 0xC000] != 0:
            # a dialog or password box is up: close it
            for _ in range(20):
                if rom.ram()[T.ENG_MODE - 0xC000] == 0:
                    break
                # button 1 pages a dialog; button 2 cancels a door code (mode 2)
                rom.press(("a",)) if rom.ram()[T.ENG_MODE - 0xC000] != 2 else rom.press(("2",))
            continue
        rom.run(4)
        flags = [r[lf0 - 0xC000 + i] for i in range(nl)]
        want = models[size].render(x, z, f, flags)
        got, err = screen_view(rom, size, pal)
        times[size].append(dt)
        mask = got != want
        if r[VAR_SPR_SHOWN - 0xC000]:                   # the hardware sprite covers part of the view
            cols, rows, sx, sy = (r[SPR_COLS - 0xC000 + k] for k in range(4))
            vx, vy = VIEW_POS[size]
            ys, xs = np.mgrid[0:mask.shape[0], 0:mask.shape[1]]
            inside = (xs + vx >= sx) & (xs + vx < sx + cols * 8) & (ys + vy >= sy + 1) & (ys + vy < sy + 1 + rows * 16)
            mask &= ~inside
        diff = int(mask.sum())
        if diff:
            bad += 1
            print(f"step {step} {size} view ({x},{z},{f}) after {b}: {diff} pixels differ")
            if shots:
                P = D["palette"]
                im = np.concatenate([P[want], P[got], np.where(mask[..., None], 255, 0).repeat(3, -1).astype(np.uint8)], 1)
                Image.fromarray(im.astype(np.uint8)).resize((im.shape[1] * 3, im.shape[0] * 3), Image.NEAREST).save(
                    shots / f"diff_{step:03d}_{size}.png")
            if bad > 5:                        # stop after a few failures
                break
    for s in sizes:
        if times[s]:
            t = np.array(times[s])
            print(f"{s}: {len(t)} views, frames per step min {t.min()} median {int(np.median(t))} max {t.max()}")
    print("OK: every view matched the model" if not bad else f"FAIL: {bad} views differ")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
