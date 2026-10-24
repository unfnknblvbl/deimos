#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The campaign's maps: parses every map of the original game and compiles its
game-logic data (tools/gamedata.py) into analysis/maps/<name>.json (and the
parsed geometry into analysis/maps/<name>.bsp.json), plus
analysis/campaign.json: the maps in ROM order with their original ids.

The original (J2ME) map ids, from each .bsp's header: 1 intro, 2 junction,
3..9 Sectors 1..7 (level01..07), 10 junction_destroyed, 11 reactor (the
last map: its exit to /endgame.bsp ends the game). items.bsp (a test map)
and endgame.bsp (the epilogue's cast call) are not used.

Usage: build_campaign.py <jar dir> [out dir = analysis]
Licence: GPL-2.0-or-later.
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bsp_parser  # noqa: E402
import gamedata  # noqa: E402

# ROM order: the map index the ROM and the model use
MAPS = ["intro", "junction", "level01", "level02", "level03", "level04", "level05", "level06", "level07",
        "junction_destroyed", "reactor"]
TITLES = {"intro": "UAC Base", "junction": "Junction", "junction_destroyed": "Junction", "reactor": "Reactor",  # map name -> the title shown on screen (end-of-sector statistics)
          **{f"level0{i}": f"Sector {i}" for i in range(1, 8)}}


def main():
    """build_campaign.py <jar dir> [out dir]: for each map of MAPS (or the
    space-separated names in the MD_MAPS environment variable) parses
    <name>.bsp (bsp_parser), saves the geometry as maps/<name>.bsp.json,
    compiles the game data with gamedata.build into maps/<name>.json, and
    finally writes campaign.json: {"maps": [{"name", "title", "id"}]}, in
    ROM order (id = the original's map number from the .bsp header).
    """
    import os
    maps = os.environ.get("MD_MAPS", "").split() or MAPS     # (MD_MAPS="intro": the UAC Base demo)
    jar = Path(sys.argv[1])
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("analysis")
    (out / "maps").mkdir(parents=True, exist_ok=True)
    camp = {"maps": []}
    for name in maps:
        level = bsp_parser.parse_bsp(jar / f"{name}.bsp")
        bsp_json = out / "maps" / f"{name}.bsp.json"
        bsp_json.write_text(json.dumps(level))
        g = gamedata.build(bsp_json, jar / "entities.db", jar / "entities.str", jar / f"{name}.str",
                           out / "maps", out / "maps" / f"{name}.json")
        camp["maps"].append({"name": name, "title": TITLES[name], "id": g["map_id"]})
    (out / "campaign.json").write_text(json.dumps(camp, indent=1))


if __name__ == "__main__":
    main()
