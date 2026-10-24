# Doom RPG in 3D for the Nintendo 3DS (version 0.1.0)

Doom RPG (id Software / Fountainhead, 2005, for mobile phones) on the 3DS:
the whole game, with a real 3D view (the 3DS's GPU, stereoscopic with the
3D slider) on the top screen and the status bar, messages, boxes and an
automap on the bottom screen.

Made by AI. Directed by unfnknblvbl.

## Playing

`out/deimos.3dsx` runs from the Homebrew Launcher (put it in `sdmc:/3ds/`)
and in emulators (tested in Panda3DS). Saved games go to
`sdmc:/3ds/deimos/deimos.sav`.

| Button | |
|---|---|
| D-pad Up / Down | step forward / back |
| D-pad Left / Right | turn |
| A | use / attack |
| B | pass the turn |
| Y | medkit |
| X | notebook |
| L / ZL, R / ZR | previous / next weapon |
| Start | pause menu |

The automap on the bottom screen shows the walls you have seen (doors in
yellow); it turns with you.

## How it is made

The game is the SNES port's own code (`snes/src/ui.c`, `glue.c`,
`campaign.c`, `save.c` and the C rules in `core/`), on a 3DS machine layer
(`src/plat.c`): the SNES's picture chip as far as the UI uses it (drawn into
textures), the pad, the saves, and the SNES sound driver in C with a small
S-DSP (`src/sound3ds.c`). The 3D view is `src/r3d.c` (citro3d). The game's
data is the SNES build's cartridge layout (`out/deimos3ds.sfc`, in the
romfs), made from the user's own `DoomRPG.jar` through the Mega Drive game's tools.

Tests: a script mode (`EXTRA="-DPLAT_SCRIPT" ./build.sh`) replays a run of
`tools/snes_play.py --script` (the SNES game in step with the model) and
compares the game state after every move; the whole game (25,578 moves) has
no differences.

## Building

`./build.sh` (needs the megadrive/ folder for the game data: `MEGADRIVE_DIR=`). The
compilers: see TOOLCHAIN.md.

Licence: GPL-2.0-or-later.
