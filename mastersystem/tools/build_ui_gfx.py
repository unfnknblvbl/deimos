#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
Builds the non-view graphics: status bar, fonts, title screen.

Both fonts come from Doom RPG's own font sheet (a.png in the jar: 16 x 9
cells of 9x12 pixels, '!' first, white glyphs with a black outline). On the
SMS the text sits on black, so only the white core is kept, squeezed into
8x8 tiles: capitals/digits lose one repeated row to become 7 rows tall,
leaving a blank top row so lines of text don't touch; lowercase keeps its
x-height and descenders are lifted by one pixel.

Outputs (into <out_dir>):
  hud_tiles.bin    status-bar tiles, SMS planar: tile slots 0..71 go to VRAM
                   tiles 56..127, slots 72..79 to tiles 496..503 (the unused
                   bottom rows of the name table)
  font_index.bin   u16 x 64: ASCII 32..95 -> VRAM tile number (HUD font)
  hud_palette.bin  32 bytes CRAM: bank 0 = face colours, bank 1 = text
  hud_static.bin   name table rows 19..23 (5 x 32 x 2 bytes): face, blanks
  dlg_font.bin     95 tiles (ASCII 32..126) for dialog boxes: loaded into the
                   idle view tile set while a box is open
  title_map.bin    full title name table (24 x 32 x 2), tiles relative to 128
  title_tiles.bin  title tiles, loaded at VRAM tile 128
  *_gg.bin         the same for the Game Gear (hud_tiles, font_index,
                   hud_static, title_map, title_tiles): a 24x24 face beside
                   the status text (static rows 16..20: the message line,
                   the face at rows 17..19, columns 6..8), the title laid out
                   for its 20 x 18 screen (columns 6..25, rows 3..20)

Usage: build_ui_gfx.py <jar_dir> <out_dir>   (build.sh step 3)
The Master System status bar shows a 40x40 face (5 x 5 tiles, name table
rows 19..23, columns 1..5); the title shows the 48x48 portrait (6 x 6 tiles)
centred above four lines of text. The editor's C++ port must produce the
same bytes, so the colour choices use portable_img's integer steps.
A name table entry here is two bytes: tile number low 8 bits, then bit 0
tile bit 8 and bit 3 the palette bank (0 face colours, 1 text colours).
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))
from sms_gfx import TileSet, rgb_to_sms, sms_cram_byte, sms_to_rgb
from portable_img import load_rgb, resize_bilinear, greedy_palette

BLACK = (0, 0, 0)
# VRAM tile numbers the status bar's tiles go to, in slot order
HUD_SLOTS = list(range(56, 128)) + list(range(496, 504))
# the characters the status bar font has (font_index maps the rest to tile 0)
HUD_CHARS = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:!./-'?+,%()"
SPRITE_TILES = 56            # VRAM tiles 0..55 are the hardware sprite patterns


def load_font_sheet(jar_dir: Path):
    """Cut a.png into {character: 12x9 bool array of its white core}, '!'..'~' plus space.

    a.png is a palette image; colour index 1 is the glyphs' white.
    """
    a = np.array(Image.open(jar_dir / "a.png"))
    glyphs = {}
    for i in range(16 * 9):
        ch = 33 + i
        if ch > 126:
            break
        r, c = divmod(i, 16)
        cell = a[r * 12:r * 12 + 12, c * 9:c * 9 + 9]
        glyphs[chr(ch)] = (cell == 1)          # white core
    glyphs[" "] = np.zeros((12, 9), bool)
    return glyphs


def squeeze_rows(rows):
    """Drop one row from an 8-row glyph: a row equal to its neighbour,
    preferring the middle."""
    order = [4, 3, 5, 2, 6, 1, 7, 0]
    for k in order:
        if k > 0 and np.array_equal(rows[k], rows[k - 1]):
            return np.delete(rows, k, axis=0)
    for k in order:
        if k < 7 and np.array_equal(rows[k], rows[k + 1]):
            return np.delete(rows, k, axis=0)
    return np.delete(rows, 4, axis=0)


def glyph_tile(g):
    """12x9 bool core -> 8x8 bool array (row 0 blank, columns 1..6 used).

    Glyphs inside sheet rows 2..9 (the cap height): those that reach row 3
    or above are squeezed from 8 rows to 7, the rest (lowercase, low
    punctuation) drop their top row. Glyphs with an ascender (row 1) or a
    descender (row 10) keep their last 7 rows (or fewer) ending at the
    bottom, which lifts descenders by one pixel.
    """
    out = np.zeros((8, 8), bool)
    ys = np.nonzero(g.any(axis=1))[0]
    if len(ys) == 0:
        return out
    core = g[:, 2:8]                           # columns 2..7 hold every glyph
    top, bot = ys.min(), ys.max()
    if top >= 2 and bot <= 9:
        body = core[2:10]                      # rows 2..9: caps height
        if top <= 3:                           # uses the full cap height: squeeze
            body = squeeze_rows(body)
            out[1:8, 1:7] = body
        else:                                  # lowercase / punctuation below the cap line
            out[1:8, 1:7] = body[1:8]
    else:                                      # ascender at 1 or descender at 10
        body = core[max(1, bot - 6):bot + 1]
        out[8 - len(body):8, 1:7] = body
    return out


def main():
    """Write every UI graphics file listed in the module docstring."""
    jar_dir, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    glyphs = load_font_sheet(jar_dir)

    # ---- palettes ----
    face = load_rgb(jar_dir / "doomrpg.png")       # the portrait, 48 x 48
    # 15 colours, in the SMS's 2-bit levels (tools/portable_img.greedy_palette)
    sms_px = [rgb_to_sms(*map(int, c)) for c in face.reshape(-1, 3)]
    keys = sorted(set(sms_px))                     # the distinct SMS colours used
    counts = [sms_px.count(k) for k in keys]
    code = lambda c: (c[0] << 4) | (c[1] << 2) | c[2]   # noqa: E731  (sort order: r, g, b)
    # pick 15 of them; distance: squared difference of the 2-bit channels
    chosen = greedy_palette([code(k) for k in keys], counts, 15,
                            dist=lambda a, b: ((a >> 4) - (b >> 4)) ** 2 + (((a >> 2) & 3) - ((b >> 2) & 3)) ** 2 +
                            ((a & 3) - (b & 3)) ** 2)
    face_pal = [BLACK]                             # colour 0 black, then the picks
    for c in chosen:
        c = sms_to_rgb(c >> 4, (c >> 2) & 3, c & 3)
        if c not in face_pal:
            face_pal.append(c)
    while len(face_pal) < 16:
        face_pal.append(BLACK)
    # the text palette (CRAM bank 1): fixed colours for the status bar text
    text_pal = [BLACK, (255, 255, 255), (255, 0, 0), (170, 0, 0), (0, 255, 0), (0, 170, 0),
                (255, 255, 0), (170, 170, 170), (85, 85, 85), (85, 85, 255), (255, 170, 0),
                (255, 85, 85), (0, 85, 0), (85, 0, 0), (170, 85, 0), (255, 255, 170)]
    (out_dir / "hud_palette.bin").write_bytes(bytes(sms_cram_byte(*rgb_to_sms(*c)) for c in face_pal + text_pal))

    def nearest(c, pal):
        """Index of the colour of pal closest to c (squared RGB distance, first wins)."""
        return min(range(len(pal)), key=lambda i: sum((a - b) ** 2 for a, b in zip(c, pal[i])))

    def face_tiles(size, tset):
        """Add the portrait, scaled to size x size, to tset; return its grid of tile indices."""
        im = face if size == face.shape[0] else resize_bilinear(face, size, size)
        n = size // 8
        grid = [[0] * n for _ in range(n)]
        for ty in range(n):
            for tx in range(n):
                pix = [nearest(tuple(int(v) for v in im[ty * 8 + y, tx * 8 + x]), face_pal)
                       for y in range(8) for x in range(8)]
                grid[ty][tx] = tset.add_tile(pix)
        return grid

    def entry(slot, bank):
        """Name table entry for HUD tile slot slot in palette bank bank."""
        t = HUD_SLOTS[slot]
        return bytes([t & 0xFF, ((t >> 8) & 1) | (bank << 3)])

    # the status bar set, once for the Master System and once for the Game Gear
    for gg in (False, True):
        sfx = "_gg" if gg else ""
        ts = TileSet()
        assert ts.add_tile([0] * 64) == 0              # solid black
        font_index = [0] * 64
        for ch in HUD_CHARS:
            g = glyph_tile(glyphs[ch])
            # glyph pixels use colour 1 of the text palette (white)
            font_index[ord(ch) - 32] = ts.add_tile([1 if v else 0 for v in g.ravel()])
        fsize = 24 if gg else 40                   # the face: 3x3 tiles on GG, 5x5 on SMS
        fc = face_tiles(fsize, ts)
        assert len(ts.tiles) <= len(HUD_SLOTS), f"too many HUD tiles: {len(ts.tiles)}"
        print(f"hud tiles{sfx}: {len(ts.tiles)} of {len(HUD_SLOTS)}")
        # always 80 tiles (72 + 8) so the engine's two copies have fixed sizes
        (out_dir / f"hud_tiles{sfx}.bin").write_bytes(ts.pack_tiles_binary().ljust(len(HUD_SLOTS) * 32, b"\0"))
        vram = [HUD_SLOTS[i] for i in range(len(ts.tiles))]  # tile index -> VRAM tile
        (out_dir / f"font_index{sfx}.bin").write_bytes(b"".join(vram[i].to_bytes(2, "little") for i in font_index))
        # (Game Gear: blanks in the text palette, whose colour 0 is black
        # from the first line of the message row; the face's palette comes
        # in a few lines later)
        blank = entry(0, 1 if gg else 0)
        rows = b""
        # five name table rows: SMS face at columns 1..5 of every row; GG
        # face at columns 6..8 of rows 1..3 (row 0 is the message line)
        for r in range(5):
            if not gg:
                rows += blank + b"".join(entry(fc[r][i], 0) for i in range(5)) + blank * 26
            elif 1 <= r <= 3:
                rows += blank * 6 + b"".join(entry(fc[r - 1][i], 0) for i in range(3)) + blank * 23
            else:
                rows += blank * 32
        (out_dir / f"hud_static{sfx}.bin").write_bytes(rows)

    # ---- dialog font: ASCII 32..126 ----
    dlg = TileSet()                                # (not used)
    data = bytearray()
    for code in range(32, 127):
        g = glyph_tile(glyphs[chr(code)])
        # a one-tile set per glyph: every character gets its own tile, no dedup
        t = TileSet()
        t.add_tile([1 if v else 0 for v in g.ravel()])
        data += t.pack_tiles_binary()
    (out_dir / "dlg_font.bin").write_bytes(bytes(data))

    # preview of both fonts
    prev = Image.new("L", (16 * 9, 6 * 9))
    for code in range(32, 127):
        g = glyph_tile(glyphs[chr(code)])
        i = code - 32
        prev.paste(Image.fromarray((g * 255).astype(np.uint8)), ((i % 16) * 9, (i // 16) * 9))
    prev.resize((prev.width * 4, prev.height * 4), Image.NEAREST).save(out_dir / "font_preview.png")

    # ---- title (Game Gear: laid out for its 20 x 18 screen) ----
    # the picture: Deimos's moon (art/title_icon.png, 48 x 48, drawn by
    # megadrive/tools/make_title_art.py), in the text palette (CRAM bank 1:
    # its greys suit the moon), so its tiles go on the map with bank 1
    icon = np.array(Image.open(Path(__file__).resolve().parent.parent / "art" / "title_icon.png").convert("RGB"))

    def icon_tiles(tset):
        """Add the 48 x 48 title picture to tset; return its 6 x 6 grid of tile indices."""
        grid = [[0] * 6 for _ in range(6)]
        for ty in range(6):
            for tx in range(6):
                pix = [nearest(tuple(int(v) for v in icon[ty * 8 + y, tx * 8 + x]), text_pal)
                       for y in range(8) for x in range(8)]
                grid[ty][tx] = tset.add_tile(pix)
        return grid

    version = (Path(__file__).resolve().parent.parent / "VERSION").read_text().strip().upper()
    for gg in (False, True):
        system = "SEGA GAME GEAR" if gg else "SEGA MASTER SYSTEM"
        title_ts = TileSet()
        assert title_ts.add_tile([0] * 64) == 0     # tile 0: solid black
        face48 = icon_tiles(title_ts)
        txt_tiles = {}
        for ch in sorted(set("DEIMOS A DOOM RPG ENGINE " + system + " PRESS BUTTON 1 PORT GPL VERSION " + version)):
            txt_tiles[ch] = title_ts.add_tile([1 if v else 0 for v in glyph_tile(glyphs[ch]).ravel()])
        # the name DEIMOS in orange (text palette colour 10)
        orange = text_pal.index((255, 170, 0))
        name_tiles = {ch: title_ts.add_tile([orange if v else 0 for v in glyph_tile(glyphs[ch]).ravel()])
                      for ch in sorted(set("DEIMOS"))}

        def tentry(i, bank):
            """Name table entry for title tile i (VRAM tile 128 + i), palette bank bank."""
            t = 128 + i
            return bytes([t & 0xFF, ((t >> 8) & 1) | (bank << 3)])

        tmap = bytearray()
        fy = 3 if gg else 4                        # the face's first tile row
        # a full 32 x 24 map: the 6x6 face at columns 13..18, black elsewhere
        for ty in range(24):
            for tx in range(32):
                if fy <= ty < fy + 6 and 13 <= tx <= 18:
                    tmap += tentry(face48[ty - fy][tx - 13], 1)   # (the picture: text palette)
                else:
                    tmap += tentry(0, 0)

        def put_text(ty, s, tiles=txt_tiles):
            """Write string s centred on name table row ty in the text palette
            (with `tiles`: the white letters, or name_tiles' orange ones)."""
            tx0 = (32 - len(s)) // 2
            for k, ch in enumerate(s):
                off = (ty * 32 + tx0 + k) * 2
                tmap[off:off + 2] = tentry(tiles[ch], 1)

        # title text: text palette (bank 1) colour 1 = white
        rows = (10, 11, 13, 15, 17) if gg else (12, 13, 15, 17, 19)
        for r, t in zip(rows, ("DEIMOS", "A DOOM RPG ENGINE", system, "VERSION " + version, "PRESS BUTTON 1")):
            put_text(r, t, name_tiles if t == "DEIMOS" else txt_tiles)
        sfx = "_gg" if gg else ""
        (out_dir / f"title_map{sfx}.bin").write_bytes(bytes(tmap))
        (out_dir / f"title_tiles{sfx}.bin").write_bytes(title_ts.pack_tiles_binary())
        print(f"title tiles{sfx}: {len(title_ts.tiles)}")
    print(f"dialog font {len(data)} B")


if __name__ == "__main__":
    main()
