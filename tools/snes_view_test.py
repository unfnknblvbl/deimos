#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The SNES's 3D view against the build machine's (test/view_host, the same
view/view.c; --fx: the SuperFX build's, against test/view_host_fx): views of the campaign's maps drawn by the ROM (the view test
build, snes/src/test_view.c) and by view_host must have the same frame
buffer, byte for byte.

Usage: snes_view_test.py [--fx] [--view W H] <campaign.bin> <view_test.sfc> <view_test.lbl> <view_host> [n views per map] [shots dir]

--view: the view's size (view.h's VIEW_W, VIEW_H; default 128 88, the
SNES game's 104 72 with test/view_host_snes): the frame buffer compared
is view.h's FB_BYTES (12288 at 128 x 88, else tiled: W x H).

The ROM is driven through its mailbox as in snes_lockstep.py: command 11
changes map, 13 draws a view (text: x, z, facing, couplings) and returns
the frames it took. The positions are random (seed 1), so runs repeat.
Licence: GPL-2.0-or-later.
"""
import random
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from snes_harness import Snes, labels  # noqa: E402
from campaign_block import Block  # noqa: E402

FB = 0x10000                    # the frame buffer: work RAM $7F0000
FB_BYTES = 12288                # its size (view.h; --view changes it)


class Rom:
    """The view test ROM on snes9x, driven through its mailbox."""

    def __init__(self, rom, lbl):
        self.e = Snes(rom)
        lab = labels(lbl)
        self.mb = lab["_mbox"]
        self.seq = 0
        for _ in range(1200):
            self.e.run_frames(1)
            if self.e.wram(self.mb, 4) == b"MBOX" and self.e.wram(self.mb + 7, 1)[0] == 0xEE:
                break
        else:
            raise SystemExit("the ROM never got ready")

    def do(self, cmd, arg=0, text=b""):
        """a command posted; returns the frames it took"""
        for i, b in enumerate(text):
            self.e.poke(self.mb + 8 + i, b)
        self.e.poke(self.mb + 4, cmd)
        self.e.poke(self.mb + 5, arg)
        self.seq = (self.seq + 1) & 255 or 1
        self.e.poke(self.mb + 6, self.seq)
        for _ in range(600):
            self.e.run_frames(1)
            if self.e.wram(self.mb + 7, 1)[0] == self.seq:
                f = self.e.wram(self.mb + 32, 2)
                return f[0] | f[1] << 8
        raise RuntimeError("the ROM did not finish")


def walkable(blk, m):
    """cells of map m to look from: those whose first PVS index word is not
    0 (the cell has something visible), not a check of where the player
    may stand"""
    # the level blob's wall map: tools/gen_cdata.py: 1024 bytes after the header? use pvs: any view with walls
    t = blk.map(m).t
    cells = []
    for c in range(1024):
        if t["pvs_index"][c * 4]:
            cells.append((c & 31, c >> 5))
    return cells


def main():
    """n random views per map, ROM against view_host; 1 if any differ."""
    global FB_BYTES
    fx = "--fx" in sys.argv
    if fx:
        sys.argv.remove("--fx")
    if "--view" in sys.argv:                    # (another size: the tiled frame buffer, W x H bytes)
        k = sys.argv.index("--view")
        w, h = int(sys.argv[k + 1]), int(sys.argv[k + 2])
        FB_BYTES = 12288 if (w, h) == (128, 88) else w * h
        del sys.argv[k:k + 3]
    camp, rom_path, lbl, host = sys.argv[1:5]
    n = int(sys.argv[5]) if len(sys.argv) > 5 else 6
    shots = Path(sys.argv[6]) if len(sys.argv) > 6 else None
    blk = Block(camp)
    rom = Rom(rom_path, lbl)
    rnd = random.Random(1)
    bad = 0
    frames = []
    tmp = Path(tempfile.mkdtemp())
    for m in range(blk.n_maps):
        if m:
            rom.do(11, m)
        cells = walkable(blk, m)
        for k in range(n):
            x, z = rnd.choice(cells)
            f = rnd.randrange(4)
            cp = 2 if m == blk.n_maps - 1 and k % 2 else 0   # (the last map: every other view with couplings 2)
            fr = rom.do(13, 0, bytes([x, z, f, cp]))
            frames.append(fr)
            a = rom.e.sram()[0x200:0x200 + FB_BYTES] if fx else rom.e.wram(FB, FB_BYTES)  # (SuperFX: in its RAM from $200)
            out = tmp / "v.bin"
            subprocess.run([host, camp, str(m), str(out), str(x), str(z), str(f), str(cp)], check=True,
                           capture_output=True)
            b = out.read_bytes()
            if a != b:
                diff = sum(1 for i in range(FB_BYTES) if a[i] != b[i])
                print(f"map {m} ({x},{z},{f}): {diff} bytes differ")
                bad += 1
            if shots:
                shots.mkdir(parents=True, exist_ok=True)
                rom.e.run_frames(2)
                rom.e.screenshot().save(shots / f"m{m:02d}_{k}.png")
    fr = sorted(frames)
    print(f"{len(frames)} views, {bad} different; frames per picture: median {fr[len(fr) // 2]}, max {fr[-1]}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
