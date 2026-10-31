# Doom RPG for the Game Boy Color (version 0.1.0)

Doom RPG (id Software / Fountainhead, 2005, for mobile phones) on the Game
Boy Color: the whole game, with its 3D view (128 x 88, in colour: a palette
per 8 x 8 tile), the status bar with the face, the weapon, the menus and the
dialogs, and the Mega Drive game's music and sound effects on the Game Boy's four
sound channels.

Made by AI. Directed by unfnknblvbl.

## Playing

`out/deimos_gbc.gbc` (4 MB, MBC5, 128 KB battery RAM) is tested in Gambatte
(RetroArch's core); it should also run in SameBoy, mGBA and BGB, and on a Game
Boy Color (or a Game Boy Advance) with a flash cartridge that has MBC5 and 128 KB of save RAM (an EverDrive GB X5/X7,
say). It needs a Game Boy Color: it does not run on the original Game Boy.

| Button | |
|---|---|
| D-pad Up / Down | step forward / back |
| D-pad Left / Right | turn |
| A | use / attack |
| B | pass the turn |
| Start | pause menu |
| Select + A | medkit |
| Select + B | notebook |
| Select + Left / Right | previous / next weapon |
| Select + Start | the SNES's Select |

A step takes the Game Boy Color about 0.7 seconds (its CPU runs at
8 MHz); the game is turn by turn, so nothing is missed. The view is drawn
from the middle out, a column of tiles at a time, so its middle shows
after about a third of that (options: DRAW VIEW OUTWARD, the default, or
AT ONCE).

## How it is made

The game is the SNES port's code (`snes/src/ui.c`, `save.c` and the C
rules in `core/`), compiled for the Game Boy's CPU with GBDK-2020 (SDCC
4.5), on a Game Boy machine layer (`src/`):

- the rules are bigger than a ROM bank: `tools/split_core.py` splits them
  over three banks (`tools/check_asm.py` and `tools/check_calls.py` stop
  the build on SDCC's known miscompilations and on calls into another bank)
- the level and the 3D view's variables live in the cartridge RAM's banks
  (the work RAM is 8 KB); `tools/view_ram.py` moves `view/view.c`'s
  variables there
- `src/vdraw.c` draws the view's columns into a picture in the cartridge
  RAM and turns it into tiles (the pixel loops in assembler); a tile takes
  one of three colour families each map's colours are grouped in
- `src/gfx.c` puts it all on the screen (the view drawn twice over, one
  shown while the other fills; the boxes on the window), the video memory
  written in the vertical blank only
- `src/snd.c` plays the Mega Drive game's music and effects in the vertical blank
  interrupt

The cartridge holds the SNES build's data as they are (from its resources'
bank on, made from the user's own `DoomRPG.jar`) and the Game Boy's own
resources, made from them (`tools/gb_res.py`: the title's logo, the faces,
the font, the weapons at half size, the maps' colours, the sound); the
editor makes the same bytes.

Tests: `./build.sh test` with `SCRIPT=` a script from
`32x/tools/make_script.py` builds a cartridge that plays it and compares
the game state after every move: the whole game (25,578 moves) has no
differences in Gambatte. The 3D view is checked pixel for pixel against
the build machine's renderer (`test/view_host`).

## Building

`./build.sh` (needs the megadrive/ folder, `MEGADRIVE_DIR=`, and the SNES build's
`snes/out/deimos.sfc`, `SNES_ROM=`), with GBDK-2020 4.5's release
(`GBDK=`; its SDCC 4.5.1).

Licence: GPL-2.0-or-later.
