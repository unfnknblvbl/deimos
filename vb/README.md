# Doom RPG for the Virtual Boy (version 0.1.0)

Doom RPG (id Software / Fountainhead, 2005, for mobile phones) on the
Virtual Boy: the whole game, with its 3D view in stereo (256 x 176, the
SNES's picture size, in the Virtual Boy's four shades of red), smooth steps
and turns, the status bar with the face, the weapon, the menus and the
dialogs, and the Mega Drive game's music and sound effects on the VSU.

Made by AI. Directed by unfnknblvbl.

## Playing

`out/deimos_vb.vb` (8 MB, 8 KB battery RAM) is tested in Mednafen
(RetroArch's Beetle VB core) and written to the hardware's rules as Red
Viper (3DS) emulates them; it should run in other Virtual Boy emulators,
and on a Virtual Boy with a flash cartridge that has battery RAM for the
saves (not yet tried on the hardware).

| Button | |
|---|---|
| Left D-pad Up / Down | step forward / back |
| Left D-pad Left / Right | turn |
| A | use / attack |
| B | pass the turn |
| L / R | previous / next weapon |
| Right D-pad Up | medkit |
| Right D-pad Down | notebook |
| Start | pause menu |
| Select | the SNES's Select (no action until one is put on it) |

The pause menu's options put any action on A, B, L, R, Right D-pad Up or
Down, or Select; the game's texts name the buttons as they are set.

A step or a turn (four pictures) takes about a third of a second
(0.31-0.41 s in Mednafen with its accurate CPU emulation: each picture
15-40 ms for the view, 46-78 ms to make the two eyes and show them, which
waits for the next display frame).

## How it is made

The game is the SNES port's code (`snes/src/glue.c`, `campaign.c`,
`save.c` and the C rules in `core/`) with the Game Boy Color's menus and
boxes (`gbc/src/ui.c`; `tools/vb_ui.py` makes a copy with the Virtual
Boy's button names and the SNES's wider status bar), compiled
for the NEC V810 with GCC 4.9.4 (VUEngine's `v810-gcc`), on a Virtual Boy
machine layer (`src/`) - the GBA port's way:

- `src/gfx.c`: the VIP's own drawing is off (XPEN never set), and the CPU
  draws every pixel (2 bits a pixel, a column at a time). Which frame
  buffer pair the display shows with the drawing off is not relied on:
  the screen's middle 256 columns are made in a back buffer in the VIP's
  spare memory and copied into both pairs at the start of a display frame
  (about 5 ms for the view, before the display reaches the left eye).
  The 3D view is `view/view.c`'s 128 x 88 picture (its free camera, as on
  the GBA: a step or a turn shown in four pictures), drawn by
  `src/view_draw.c` and shown at twice the size; each colour becomes one
  of 13 levels, shown as a 2 x 2 pattern of two neighbouring shades.
  The weapon is the Mega Drive's frames at 1.5 times the SNES's size
  (nearest pixel), read from the SNES image as it is shown; the status bar
  under the view has the SNES's 25-column layout.
- Stereo: the view is drawn once, and each eye shifts each column by an
  amount from its depth (the nearest of its wall and the things drawn in
  it): 1 pixel between the eyes for what is near, up to 8 for the far
  distance, so the view is behind the screen and things stand out from the
  walls behind them. The status bar, the messages, the boxes and the
  weapon are at the screen, in front.
- `src/sound.c`: the Game Boy Color's sound driver on the VSU (its five
  waves written once at the start, with every channel stopped, as the
  hardware requires; never again), in the
  timer's interrupt (60 times a second): the songs' five voices on the
  five wave channels (each Mega Drive instrument's FM tone as a wave,
  `tools/vb_sound.py`), the drums on the noise channel.
- `src/plat.c`: the cartridge, the saves, the pad, the interrupts, the
  start; `src/crt0.s` the start and the vectors.

The cartridge holds the code (its first 512 KB), then the SNES build's
cartridge image as it is (made from the user's own `DoomRPG.jar`, with
this version's title screen), which the Virtual Boy reads at its SNES
addresses, then the header and the vectors at its end
(`tools/vb_rom.py`). The work RAM is 64 KB: the level's data (`glue.c`'s), the title's picture
and the weapon's live in the VIP's memory the game does not show.

## Tests

- `./build.sh test` with `SCRIPT=` a script from `32x/tools/make_script.py`
  builds a cartridge that plays it and compares the game state after
  every move (`tools/vb_lockstep.py`): the whole game (25,578 moves) has
  no differences in Mednafen.
- `tools/vb_view_test.py <campaign.bin>` builds the view test cartridge
  (`./build.sh viewtest`) and compares 88 views (every map's start facing
  each way, and free-camera views between cells and facings) with the
  build machine's renderer (`test/view_host.c` with `VIEW_FX`, through
  `tools/vb_view_ref.c`): the picture, and each eye's screen worked out
  from it. They are the same, pixel for pixel.
- `tools/vb_harness.py` drives the Beetle VB libretro core headless
  (`VB_CORE=`, else `emu/beetle-vb/mednafen_vb_libretro.so`).

## Building

`./build.sh` (needs the megadrive/ folder, `MEGADRIVE_DIR=`, and the SNES
build's `snes/out/`), with VUEngine's `v810-gcc` (binutils 2.27, GCC
4.9.4; `V810=` its prefix, default `v810`): `../tools/deps.sh` finds it, or
builds it into `~/.local/share/deimos-tools/v810` (C only, no C library:
`gba/src/libc.c` has the few functions the game uses).

Licence: GPL-2.0-or-later.
