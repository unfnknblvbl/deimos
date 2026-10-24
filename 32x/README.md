# Doom RPG for the Sega 32X (version 0.1.0)

Doom RPG (id Software / Fountainhead, 2005, for mobile phones) on the 32X:
the whole game, with a full-width 3D view (320 x 176) drawn by the 32X's
two SH-2s, smooth steps and turns, and the SNES version's music and sound
effects through the 32X's PWM.

Made by AI. Directed by unfnknblvbl.

## Playing

`out/deimos32x.32x` runs in PicoDrive (RetroArch's PicoDrive core, or
PicoDrive itself). It needs no 32X BIOS files. It does not run on real
hardware or in emulators that boot with Sega's 32X BIOS: the cartridge has
no Sega security code. Use a 6-button pad.

| Button | |
|---|---|
| D-pad Up / Down | step forward / back |
| D-pad Left / Right | turn |
| A | use / attack |
| B | pass the turn |
| C | medkit |
| X / Z | previous / next weapon |
| Y | notebook |
| Start | pause menu |

Saved games go to the cartridge's battery RAM (8 KB).

## How it is made

The game runs on the SH-2s: the SNES port's own code (`snes/src/ui.c`,
`glue.c`, `campaign.c`, `save.c` and the C rules in `core/`) on a 32X
machine layer (`sh2/plat.c`: the SNES's picture chip as far as the UI uses
it, drawn into the 32X's frame buffer, whose palette is the SNES's CGRAM).
The 3D view is `sh2/r32x.c` (a column renderer: walls by their 1/depth,
things far to near); the master SH-2 works out the walls and draws the left
half, the slave the right half between making the sound (`sh2/sound32x.c`:
the SNES sound driver in C, its samples decoded as they play, through the
PWM's interrupt). The 68000 (`m68k/`) starts the 32X, reads the pad, counts
the frames and keeps the battery RAM, through the COMM registers.

The game's data is the SNES build's cartridge layout, at $80000 in the
cartridge, made from the user's own `DoomRPG.jar` (title word "32X").

Tests: `./build.sh test` with `SCRIPT=` a script from
`tools/make_script.py` (a `tools/snes_play.py --script` run of the SNES game
in step with the model) builds a cartridge that plays it and compares the
game state after every move; the whole game (25,578 moves) has no
differences in PicoDrive.

## Building

`./build.sh` (needs the megadrive/ folder: `MEGADRIVE_DIR=`). The compilers are
GCC 13.3 for `sh-elf` (`--with-cpu=m2`, C only) and `m68k-elf`, built from
the GNU sources (the Mega Drive game's `tools/build_toolchain.sh`, with
`--target=sh-elf` for the SH-2s); `SH2=` and `M68K=` give their prefixes.

Licence: GPL-2.0-or-later.
