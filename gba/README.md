# Doom RPG for the Game Boy Advance (version 0.1.0)

Doom RPG (id Software / Fountainhead, 2005, for mobile phones) on the Game
Boy Advance: the whole game, with its 3D view (160 x 110, the SNES's
picture at 1.25 times, the floor and ceiling shaded line by line), smooth
steps and turns, the status bar with the face, the weapon, the menus and
the dialogs, and the SNES version's music and sound effects in stereo.

Made by AI. Directed by unfnknblvbl.

## Playing

`out/deimos_gba.gba` (about 4 MB, 32 KB battery RAM) is tested in mGBA
(RetroArch's mGBA core); it should run in other GBA emulators and on a Game
Boy Advance (or a DS / DS Lite in GBA mode) with a flash cartridge that has
battery RAM (SRAM) for the saves.

| Button | |
|---|---|
| D-pad Up / Down | step forward / back |
| D-pad Left / Right | turn |
| A | use / attack |
| B | pass the turn |
| L / R | previous / next weapon |
| Start | pause menu |
| Select + A | medkit |
| Select + B | notebook |
| Select + Start | the SNES's Select |

The pause menu's options put any action on any of these buttons.

## How it is made

The game is the SNES port's code (`snes/src/glue.c`, `campaign.c`,
`save.c` and the C rules in `core/`) with the Game Boy Color's menus and
boxes (`gbc/src/ui.c`, made for a small screen), compiled for the GBA's
ARM7 with GCC, on a GBA machine layer (`src/`):

- `src/gfx.c`: the screen in Mode 4 (240 x 160, a byte a pixel), made in
  the work RAM and copied onto the page not shown; the 3D view from
  `view/view.c` (its free camera: the SNES SuperFX build's C, a step or a
  turn shown in four pictures), drawn by `src/view_draw.c` and shown 1.25
  times; colour 0 of each line the floor's or ceiling's shade (H-blank
  DMA into the palette, as the SNES's HDMA)
- `src/sound.c`: the SNES's sound driver and as much of its DSP as it
  uses, in C (the 3DS port's), mixed in the vertical blank for the next
  frame and played by Direct Sound (A left, B right) at 18,157 Hz
- `src/plat.c`: the cartridge, the saves, the pad, the start; `src/div.s`
  the divisions; the busiest loops are ARM code in the 32 KB fast RAM

The cartridge holds the code (its first 512 KB), then the SNES build's
cartridge image as it is (made from the user's own `DoomRPG.jar`, the title
word ADVANCE), which the GBA reads at its SNES addresses; the editor makes
the same bytes.

Tests: `./build.sh test` with `SCRIPT=` a script from
`32x/tools/make_script.py` builds a cartridge that plays it and compares
the game state after every move (`tools/gba_lockstep.py`): the whole game
(25,578 moves) has no differences in mGBA. The 3D view is the same, pixel
for pixel, as the build machine's renderer's (`test/view_host.c` with
`VIEW_FX`).

## Building

`./build.sh` (needs the megadrive/ folder, `MEGADRIVE_DIR=`, and the SNES build's
`snes/out/`), with GCC 13.3 for `arm-none-eabi` (`ARM=` its prefix), built
from the GNU sources like the other ports' compilers (C only, no C
library: `src/libc.c` has the few functions the game uses); devkitARM's
`arm-none-eabi-gcc` should do as well.

Licence: GPL-2.0-or-later.
