#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The lockstep test: the test cartridge (./build.sh test, SCRIPT= a
32x/tools/make_script.py script) run in mGBA until its script is done;
the game state compared after every move (src/plat.c's script mode).

  gba_lockstep.py out/deimos_gba_test.gba obj/gba.syms [max minutes] [screenshot.png]

"Lockstep": the SNES game was run beside the portable rules model and its
state recorded after every move; this cartridge replays the same presses
and checks that its own state matches at each point. It reports the
script mode's counters (script_states, script_bad, script_first_bad,
script_done) read from the GBA's memory by symbol.
Licence: GPL-2.0-or-later.
"""
import sys
import time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from gba_harness import GBA  # noqa: E402


def main():
    """Run the test until the script ends or the time limit; print the progress and the result."""
    g = GBA(sys.argv[1], sys.argv[2])
    limit = float(sys.argv[3]) * 60 if len(sys.argv) > 3 else 3600   # seconds of real time (default 1 hour)
    t0 = time.time()
    last = -1
    while time.time() - t0 < limit:
        g.run_frames(600)           # 10 seconds of play between looks
        st, bad, done = g.u32("script_states"), g.u32("script_bad"), g.u32("script_done")
        if st // 1000 != last:      # a line every 1000 states
            last = st // 1000
            print(f"{g.frames_run} frames: {st} states, {bad} bad" +
                  (f" (first {g.u32('script_first_bad')})" if bad else ""), flush=True)
        if done:
            break
    st, bad = g.u32("script_states"), g.u32("script_bad")
    print(f"done: {st} states, {bad} bad, {g.frames_run} frames ({g.frames_run / 3600:.1f} minutes of play)")
    # (an optional 4th argument: a screenshot of the last frame)
    if len(sys.argv) > 4:
        g.screenshot().save(sys.argv[4])


if __name__ == "__main__":
    main()
