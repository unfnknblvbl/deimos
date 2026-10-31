# Deimos for the Mega Drive / Genesis (version 0.1.0)

Doom RPG (id Software / Fountainhead, 2005, for mobile phones) on the
Sega Mega Drive / Genesis. This version contains **the whole game**: the
UAC Base, the Junction, Sectors 1 to 7, the destroyed Junction and the
Reactor, with the story, the vendors, the items and the ending.

Made by AI. Directed by unfnknblvbl.

## Playing

`out/deimos.md` runs in any Mega Drive emulator (Genesis Plus GX,
BlastEm, Kega Fusion...) and on a flash cart (such as the Mega EverDrive
Pro). It is 3.3 MB, with 8 KB of battery-backed save RAM, and should work
on NTSC and PAL consoles (it was tested as NTSC, in Genesis Plus GX).

**A 6-button pad is best.** With a 3-button pad the title screen says
"SIX BUTTON CONTROLLER PREFERRED / PRESS A+B+C+START TO BEGIN": hold all
four to go on. A 3-button pad then has fixed buttons: A use, B pass the
turn, C + Up medkit, C + Left / Right weapons, C + Down notebook, hold C
for full detail. (In an emulator, set the controller to "6 button" for
the full layout.)

| Button | default |
|---|---|
| D-pad Up / Down | step forward / back (each step is a turn: monsters move after it) |
| D-pad Left / Right | turn |
| A | use / attack whatever is in front: doors, terminals, people, monsters, barrels, crates |
| B | pass the turn |
| C | use a medkit (smallest first) |
| X / Z | previous / next weapon |
| Y | notebook (the notes the story gives you) |
| Mode (hold) | full detail, in smooth movement |
| Start | pause menu: resume, map, items, notebook, save, options, cheats, quit to title |

Every button except Start can be changed on the **options** screen (from
the title or the pause menu): go to an action and press the button you
want for it; the action that had that button gets yours. DEFAULTS puts
them all back.

**Movement** (options): **STEP** jumps from cell to cell, like the phone
game, and always draws in full detail. **SMOOTH** glides to the next cell
and turns smoothly (18 frames, 0.3 s), drawn in low detail (4-pixel-wide
columns) so the 68000 can keep up; hold **Mode** to see the full-detail
picture whenever you stop. **Draw view** (options): in step movement the
new view is drawn from the middle out, a tile column at a time, as the
Master System does (**OUTWARD**, the default), or all at once.

**Saving**: the pause menu saves the game; the title screen's first item continues it
loads it, even after the console has been switched off. The options are
saved too. The game also saves by itself at the start of every map; if
you die, A (TRY AGAIN) puts you back at the start of the map.

**The whole game.** The maps follow each other as in the original: the
UAC Base, the Junction (the hub: its doors lead to the sectors), Sectors 1
to 7, the destroyed Junction and the Reactor, where the game ends. Finishing
a sector shows its statistics (A goes on). Things work as in the phone game:

- **The Reactor's portal**: once opened, it brings monsters through every
  few turns, one at a time, until both power couplings beside it are
  destroyed; then the portal goes dark and something comes through.

- **Vendors** sell medkits, ammunition and upgrades for credits: A buys
  the line under the cursor, B leaves.
- **Items** (pause menu, ITEMS): the **Berserker** doubles your damage for
  a while (the screen turns red), the **collar** gives you a dog that
  fights for you and takes the hits.
- **Secrets**: some walls slide away when you use them (a message says so,
  +5 XP).
- The doors, keys, terminals, door codes, the people and their stories,
  the Pain Elementals' lost souls and the invisible walls of the original.

Menus: Up/Down and A (or Start); B goes back. Dialogs: A for the next
page. Door codes: Up/Down change the digit, Right adds a digit, Left
deletes, A enters, B cancels.

## How it works

The Master System version had every view drawn in advance (3 MB for one
sector). Here the 68000 draws the view **live**, every step, the way the
original phone game did:

- **Walls**: the original's projection (camera 16 units behind the cell
  centre, eye 36 units up, 90 degree view, textures repeating every 64
  units, one-sided walls, door frames shifted like the original). The
  walls that can be seen from each cell and facing are worked out by the
  build (about 10 per view), so only those are projected.
- **Wall decals** (screens, signs, blood) are baked into copies of the
  wall textures when the build makes the ROM, so they cost nothing.
- **Colours**: three 16-colour palettes for the view, chosen for each
  enclosed area of each map when the ROM is built (`tools/md_areas.c`:
  the textures and sprites seen from the area, weighted by how much of the
  screen they fill); each 8x8 tile of the view uses the palette that suits
  its texture or sprite. The weapon and the floor/ceiling shading share the
  fourth palette, rewritten band by band down the screen during the
  picture (the H interrupt, `src/crt0.s`), which also runs the queued
  DMA transfers (`src/dma.s`).
- **Floor and ceiling**: the view leaves them transparent, and the plane
  behind it holds the level's floor and ceiling colours fading darker
  towards the horizon, dithered between the Mega Drive's colour steps
  (`src/backdrop.c`). It costs nothing per picture.
- **Speed**: the walls are drawn by "compiled scalers" - one small routine
  per wall height (`tools/gen_scalers.py`), so a pixel costs one
  instruction. The view is 208x128 pixels in the middle of the screen,
  with a black border round it: the 256x160 picture it used to be, at 80%
  of the size (the same 90 degree view), which draws about a fifth
  faster. A step takes under 0.2 seconds to draw; the picture is drawn
  off screen and swapped in at once. In smooth movement, steps and turns
  also get in-between pictures (`render_between` in `src/render.s`, "word
  scalers" in `tools/gen_scalers.py`), and the camera can face any angle.
- **Things** (monsters, items, props) are drawn as billboards with a
  per-column depth test; **your weapon** is shown with hardware sprites,
  with its firing frame.
- **Status bar**: the original's face (it changes with health and when you
  are hurt), health, armour, weapon and ammo, level and XP, credits, keys,
  medkits.
- **Sound**: the original game's own sound effects - the phone version's
  little General MIDI clips (the weapons, explosions, the player's death,
  items and secrets, the menus) - played as a Mega Drive game would, on
  the YM2612's FM voices and the PSG's noise channel
  (`tools/build_sfx.py`): a gunshot is the noise channel's crack over an
  FM body falling in pitch, the dog and the death are FM distortion
  guitar, the plasma weapon an FM synth drum. Where the phone game is silent
  (doors, pick-ups, monsters) the Mega Drive game has effects written in the same
  instruments. An effect borrows FM voices 5 and 6 and the noise from the
  music and gives them back (`src/music.s`).
- **Music**: the original game's tunes (title, fresh game, sector complete)
  on the YM2612: `tools/build_music.py` turns the MIDI files into a stream
  with FM instruments for the guitars, bass and organ, an FM kick and
  toms, and the snare, hi-hats and cymbals on the PSG's noise channel
  when no sound effect needs it; `src/music.s` plays it from the frame
  interrupt.

- **The maps are data**: everything that depends on a map (walls,
  visibility, the rules' level tables, the texts, the colours) is in one
  **campaign block** at the end of the ROM (`tools/md_campaign.py`), with
  the wall textures and sprites in two shared banks (each picture once,
  however many maps use it), and the weapon pictures. A small directory
  in the cartridge header's notes field (`$1C8`: `"DEIMOSMD"`, the block's
  address, the end of the space it may use, `$400000`) says where it is.
  So the **Deimos Editor** (`../editor/`) makes the ROM without a
  compiler: the game code (its engine, `sg16.eng`) plus a new block made
  from your DoomRPG.jar, or from the maps of a project (`.deimos`) you
  edited in it. For the same maps it gives exactly the same bytes as
  `build.sh`. The ROM carries no copy of the maps for the editor: all the
  space goes to the maps themselves.
- **The game's own texts come from the jar too**: the messages the rules
  show, the menus' and the vendor's words, the weapons' names and the
  maps' titles are not in the code. `../tools/game_text.py` makes each
  from the jar's string tables (`base.str`, `entities.str`) by a recipe,
  into the campaign block's **texts table** (the block header's bytes
  24..31 give its place and size); the code names a text by its index
  (`TX_*`, `build/gen/texts.h` and `texts.inc`: `gtext(TX_...)` in C, the
  `TXT` and `SAYT` macros in `src/rules.s`). The editor makes the same
  table (`editor/src/gametext.cpp`). `../tools/jar_text_scan.py` checks
  that none of the jar's texts is in Deimos's own files.
- **68000 assembler**: the renderer (`src/render.s`)
  and the game rules (`src/rules.s`) are written in assembler, for speed;
  menus, screens and saving are C.

The game rules follow the original's code (read from the GPL-3.0
DoomRPG-RE project), and the tests check them against a Python model of
the original rules (`tools/game_model.py`) after every action, byte for
byte. `tools/md_campaign_lockstep.py` plays **the whole game**, all ten
maps to THE END (about 24,600 moves, driven by `tools/campaign_pilot.py`),
and the ROM's state matches the model's after every one of them.

## Building

You need Python 3 with Pillow and numpy, and a 68000 cross compiler.
`build.sh` checks for them first (`tools/deps.sh`): it works out your Linux
distribution (Arch / CachyOS / Manjaro, Debian / Ubuntu, Fedora, openSUSE),
says what is missing, and after one "yes" installs it, with the package
manager where the distribution has it and otherwise by building it from
source into `~/.local/share/deimos-tools` (no root needed for that part).
The 68000 compiler is always built from source (binutils 2.43.1 and gcc
13.3.0, the versions the reference ROM was made with): 10-30 minutes, once.
`DEIMOS_YES=1 ./build.sh` installs without asking.

**The ROM.**

```sh
./build.sh
```

It takes a few seconds and writes `out/deimos.md`. Everything is built
from the original game's files in `assets/jar` (from
your `DoomRPG.jar`).

**Tests (optional).** The lockstep test needs the Genesis Plus GX libretro
core:

```sh
git clone https://github.com/libretro/Genesis-Plus-GX
make -C Genesis-Plus-GX -f Makefile.libretro
export MD_CORE=$PWD/Genesis-Plus-GX/genesis_plus_gx_libretro.so
python3 tools/md_campaign_lockstep.py out/deimos.md      # the whole game (a few minutes)
python3 tools/md_campaign_demo.py demo.mp4                  # the 0.0.6 demo video (needs ffmpeg)
python3 tools/md_sfx_test.py sfx.wav --alone                # every sound effect, recorded from the ROM
```

`md_campaign_lockstep.py` takes a number of moves to stop after
(`... out/deimos.md 3000`), and `--frames DIR` saves every frame.

## Files

```
src/crt0.s        start-up: vectors, cartridge header, hardware init
src/vdp.c         video chip: set-up, DMA, frame interrupt (picture flip,
                  damage flash, sound), 3- and 6-button joypads
src/render.s      the 3D view (68000 assembler);  src/area.c  its palettes per area
src/dma.s         the DMA queue's runner (the interrupts send the picture)
src/rules.s       the game rules (68000 assembler)
src/ui.c          title, menus, options, input, messages, status bar,
                  dialogs, codes, vendors, items, the sector and end
                  screens, smooth movement
src/save.c        button mapping, settings and saved game (battery RAM)
src/weapon.c      the weapon sprites;  src/sound.c  the sound effects' queue
src/music.s       the FM driver (music and sound effects);  src/backdrop.c  the shaded floor and ceiling
src/pack.c        finds the campaign block and points the tables at a map
tools/            everything the build generates from the original files,
                  and the tests
```

Version numbers are 0.y.z: y goes up when a version is ready to show,
z counts the source packages.

## Licence and credits

The code (src/, tools/) is GPL-2.0-or-later. It was written from the
behaviour of the original and from reading the GPL-3.0 DoomRPG-RE project;
no DoomRPG-RE code is included. The game data (level, textures, sprites,
text, font, faces, logo) comes from the user's `DoomRPG.jar` (id Software /
Fountainhead Entertainment / JAMDAT, 2005) and is not covered by that
licence.

- Toolchain: GNU binutils and GCC.
- Test emulator: [Genesis Plus GX](https://github.com/libretro/Genesis-Plus-GX).
- Mega Drive hardware documentation: [Plutiedev](https://plutiedev.com), SpritesMind.
