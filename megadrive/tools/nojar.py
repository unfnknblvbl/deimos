#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The Mega Drive build without DoomRPG.jar (build.sh with DEIMOS_NOJAR=1):
the generated sources step 2 of build.sh makes from the jar, with every
part made from the jar left as zeros. The ROM it gives has no maps, no
pictures and no sounds, but its code and tables are where the full
build's are, so tools/make_engine.py makes the same engine from it (and
the SNES family's builds can use its build/gen as they use the full one's).

Written into <gen dir>:
  ui_data.c, ui_data.h      the font, faces, icons, palettes and title as
                            zeros, each array the size of its region in
                            ../editor/engines/sg16.eng (the sizes the jar's
                            pictures give: the code after them is placed by
                            them); TITLE_N_TILES from the title's size
  sfx_data.c, sfx.h, sfx.inc   every effect's stream as zeros of its
                            region's size (tools/build_sfx.py's names and order)
  music_data.s, music.h     the three songs as zeros of their regions' sizes
                            (with the instruments and the F-number table, which
                            are not from the jar)
  weapon_data.h             (tools/build_weapons.py's, which holds nothing
                            from the jar)
  campaign.bin, areas.bin, campaign.s   an empty campaign block (its
                            header, no maps: after the engine, where the
                            editor writes the maps)
  render_data.c, render_data.h   (tools/build_render_data.py's write_common)
The rules' sources (data.c, state.h, texts.h ...) come from gen_cdata.py run
on an empty campaign (build.sh), the scalers from gen_scalers.py, as in the
full build.

Usage: nojar.py <gen dir> <engines dir> <version>
Licence: GPL-2.0-or-later.
"""
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / "tools"))
from engine_file import regions  # noqa: E402  (../../tools/engine_file.py)


def zeros(r, name, width=1):
    """a list of zeros as long as region name (width bytes a value)"""
    n = r[name][1]
    if n % width:
        raise SystemExit(f"{name}: its region is {n} bytes, not a whole number of {width}-byte values")
    return [0] * (n // width)


def main():
    """Write the stand-ins (see the module docstring)."""
    gen, engines, version = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
    gen.mkdir(parents=True, exist_ok=True)
    r = regions(engines / "sg16.eng")
    import build_md_ui
    build_md_ui.write(gen, version, zeros(r, "ui_font"), zeros(r, "ui_faces"), zeros(r, "ui_icons"),
                      zeros(r, "ui_hud_pal", 2), zeros(r, "ui_dlg_pal", 2), zeros(r, "title_tiles"),
                      zeros(r, "title_map", 2), zeros(r, "title_pal", 2))
    import build_sfx
    build_sfx.write(gen, [(n, bytes(r["sfx_" + n.lower()][1])) for n in build_sfx.names()])
    import build_music
    build_music.write(gen, [bytes(r["song_" + name][1]) for name, _f, _l in build_music.SONGS])
    import build_weapons
    build_weapons.write_header(gen)
    import md_campaign
    md_campaign.write_common(gen)
    # an empty campaign block: its header only, no maps (md_campaign.py's layout)
    blk = bytearray(md_campaign.HEAD)
    blk[0:8] = b"MDCP" + struct.pack(">HH", md_campaign.VERSION, 0)
    struct.pack_into(">I", blk, 20, len(blk))      # (the block's size)
    (gen / "campaign.bin").write_bytes(bytes(blk))
    (gen / "areas.bin").write_bytes(bytes(16))
    md_campaign.write_campaign_s(gen)
    print("no-jar stand-ins written into", gen)


if __name__ == "__main__":
    main()
