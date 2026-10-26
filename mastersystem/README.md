# Deimos for the Master System and Game Gear (0.2.16: views drawn live, all assembler)

Part of **Deimos**: Doom RPG on ten retro consoles. Made by AI. Directed by unfnknblvbl. Licence: GPL-2.0-or-later (`../LICENSE`).

The Z80 draws every view as you play (0.1.5 stored 1,700 pre-drawn
pictures in 4 MB). The view comes in two sizes and sits in a border in the
style of the original Doom. The ROM is **512 KB**. The Deimos Editor (`../editor/`) makes the
same ROM from your DoomRPG.jar, or from a level you have edited in it (a `.deimos` project).

A port of **Doom RPG** (id Software / Fountainhead, 2005, J2ME) - its first sector, level01 - to
the **Sega Master System**, built from the level, textures and sprites in
the original `DoomRPG.jar`, with the original game's rules re-implemented:
turn-based combat, monster AI, items, drops and XP, doors, scripted events,
dialogs, door codes, notes and the status bar.

`rom/deimos.sms` is a 512 KB Sega-mapper ROM for any SMS emulator with
the standard mapper (Gearsystem, Emulicious, Kega Fusion, BlastEm,
Mednafen...) or a flash cart. The header carries the right checksum and
size, so console BIOSes accept it.

## The Game Gear

`rom/deimos.gg` is the same game for the **Sega Game Gear** (512 KB, any GG
emulator or flash cart). It is built from the same code (`wla-z80
-DGAMEGEAR`) with a layout for the 160x144 screen: the view fills its whole
width with no border (rows 0..11 of the screen), a black row, the message
line, then the status bar beside a small face (HP and armour, weapon and
ammo, level and XP, credits, medkits and keys). **Start** is the pause
button. Dialogs, door codes, the notebook and the pause menu are laid out
20 columns wide. The palettes are the Master System's, widened to the Game
Gear's 12-bit colours as they are written; the view's go in at every
vblank and one line interrupt swaps in the status bar's in the black row.
The lockstep test runs on it as on the SMS (the whole UAC Base matches the
model), and the editor makes Game Gear ROMs as it makes Master System ones.

## Playing

| Button | |
|---|---|
| Up / Down | step forward / back (a step is a turn: monsters move after it) |
| Left / Right | turn (free, no monster turn) |
| Button 1 | use / attack whatever is in front: doors, terminals, people, monsters, barrels, crates |
| Button 2 + Left / Right | previous / next weapon |
| Button 2 + Up | use a medkit (smallest first) |
| Button 2 + Down | pass the turn |
| Button 2 + Button 1 | notebook (notes the story gives you; also in the pause menu) |

Dialogs: button 1 for the next page. Door codes: Up/Down change the digit,
Right adds a digit, Left deletes, button 1 enters, button 2 cancels.

**Pause** (the console's pause button; on most flash carts in a Mega
Drive, Start) opens the pause menu: Up/Down choose, button 1 selects or
changes, button 2 or pause again resumes. It has Resume, the Notebook,
and four settings:

| Setting | | |
|---|---|---|
| View size | **Large** 160x96 (default) | **Small** 128x80 |
| Draw view | **Outward** (default): the new view is drawn over the old one from the middle outwards, in either size | **At once**: the new view appears when it is finished (large: the view is black meanwhile; small: drawn out of sight and swapped in) |
| Detail | **High** (default) | **Low**: walls and scenery are worked out for every other column and drawn two pixels wide (faster) |
| Walls | **Textured** (default) | **Solid**: each wall column in one colour, the texture's colour halfway up it (faster) |

The new view is drawn as soon as your own move is done, and
the monsters take their turn after it. In the large view at high detail
the picture is finished about 0.6 s after the button press; low detail
and solid walls together bring that to 0.4 s, and the small view at low
detail with solid walls to 0.3 s. See "Speed" below. The settings last
until the console is switched off.

The top line shows messages (the damage done, what was picked up, a new level),
the bottom shows health/armour, weapon and ammo, level and XP, credits and
keycards, medkits. The view flashes red when you're hit.

Sound: simple PSG "beeps and bloops" for each weapon, monster attacks,
hits and deaths, getting hurt, dodging, footsteps and bumping into walls,
pickups (items, credits, weapons, keycards), doors, explosions, locked
doors and wrong codes, level-ups, dialogs and the end of the sector. The
effects of one turn play one after another; pressing a button drops any
that are still waiting, so the sound keeps up with you.

level01 at level 1 with 30 HP is a hard fight, exactly as in the original
(where you normally arrive with a few levels from the Entrance). Use doors
and corridors so monsters come one at a time, keep the axe for jammed doors
and the fire extinguisher for fires, and don't stand next to a barrel you're
shooting.

### Known limits

- **Bars, railings and gates** standing free across an opening (not on a
  wall) are drawn as a flat picture that always faces you. Seen straight
  on, from either side, they look right; from beside them they are
  missing, and from a square diagonally away they appear face-on instead
  of at an angle. Drawing them at their true angle needs new renderer code.
- **Monsters, people and items** show only when straight ahead (1 to 4
  cells): one at a time, as a hardware sprite with its own 16 colours. Things
  off to the side are not drawn until you face them.

## How it's built

```
game/           the game logic, Z80 assembler (WLA-DX), included by rom/main.asm
  game.asm      constants, the engine variables it shares, its own RAM
  core.asm      the level blob, random numbers, cells, entity lists, the trace
  math32.asm    24/32/40-bit arithmetic for the damage formulas
  combat.asm    hit and damage rolls, stats, monsters, their AI and turn, explosions
  events.asm    the tile scripts: commands, conditions, stopping and resuming
  actions.asm   move, turn, use/attack, pickups, weapons, medkits, a fresh game
  ui.asm        input, messages, status bar, dialogs, codes, notebook, pause menu
  gen/          generated: limits and state addresses (defs.inc), rule tables
                (tables.inc), sound ids (sfx.inc), where each of the game's
                texts is (texts.inc), the state layout for the tests
rom/main.asm    the display engine (Z80, WLA-DX): reset, interrupts, VDP,
                sprites, dialog box, status bar, services at $0100
rom/render.asm  the live renderer
tools/          everything that turns the jar into ROM data, plus the tests
```

### The rules: one spec, two implementations, checked against each other

`tools/game_model.py` is an executable spec of the rules in Python, written
from reading the J2ME decompile and the GPL-3.0
[DoomRPG-RE](https://github.com/Erick194/DoomRPG-RE) reimplementation
(Combat.c, CombatEntity.c, Entity.c, Player.c, Game.c): hit chance with
range falloff, damage with strength/defence ratio, crits, armour share and
per-class weapon multipliers; monster stat scaling; the XP formula and
level-ups; drop tables; the AI (`aiThink`, the two-step greedy path search,
lunges, the Archvile raising corpses); barrel explosions and crate loot;
the tile-event byte code (state/key/facing/arrive/leave filters, one-shot
commands, dialogs and codes that pause a script and resume it).
`game/*.asm` implements the same thing in Z80 assembler, routine for
routine (the names match the model's).

Both use the same 16-bit xorshift RNG. `tools/test_lockstep.py` plays the
ROM on the Gearsystem emulator core and the model side by side with the
same button presses, and after **every** action compares the ROM's whole
game state - 1,433 bytes read out of RAM: player, every entity's position
and flags, every monster's stats, doors, event states, RNG - with the
model's, byte for byte. Any difference in a rule, or in the order random
numbers are drawn, fails the test at the action that caused it. Thousands
of actions over several random play-throughs (including deaths, dialogs,
door codes, explosions) match exactly.

That test has paid for itself more than once. Up to 0.2.11 the rules were
C (z88dk); the test caught a miscompile in sccz80's optimiser there. In
0.2.12 the game logic was rewritten in assembler (about 15 KB instead of
36 KB of compiled C, and several times faster), and the test - plus a
second one that plays the C and the assembler ROMs side by side and
compares every message, status bar, dialog box, sound effect and sprite
choice - found each slip in the rewrite.

Port decisions (marked `PORT:` in the model):
- Monsters wake when they come into view: in the original that's "drawn by
  the renderer"; here, within 5 cells inside the 90-degree view cone with a
  clear line of sight.
- Medkits are used with button 2 + Up instead of an inventory menu.
- The door beside the start leads back to Junction, which isn't in this
  port: it says so and stays shut. The exit in the north-east completes the
  sector.
- WAIT script commands don't pause.

### Pictures: the live renderer

`rom/render.asm` (its own bank, paged into slot 1 while it runs) draws
the view of the cell and facing you are at; `tools/live_data.py` builds
everything it reads and holds a bit-exact Python model of it.

1. **Walls.** The build splits the level's walls into pieces of at most 64
   units (so textures repeat every 64 units: the long walls that 0.1.5
   stretched are now right), shifts door frames like the original, bakes
   wall decals (screens, signs, blood) into copies of the textures, and
   works out which walls can be seen from each cell and facing (about 10).
   At run time each of those is culled (open doors, back faces), clipped
   and projected with the original engine's camera (16 units behind the
   cell centre, eye 36 units up, 90 degree view); every screen column keeps
   the nearest wall. All walls are axis-aligned or diagonal and textures
   run one texel per world unit, so the per-column maths needs no
   multiplication or division: heights step linearly (1/depth is linear
   on screen) and a table per view size turns a height into its rows and
   texture step.
2. **Scenery** (lamps, chairs, tables, cabinets, the exit sign) is drawn as
   billboards with a depth test, like the original.
3. **Strips of 8 columns** are filled into a chunky buffer (ceiling, wall
   texels, floor) by unrolled code, then turned into the console's planar
   tiles with lookup tables, two pixels per lookup, straight into video
   memory. Tile rows that are all ceiling or all floor use two shared solid
   tiles and cost nothing.
4. **Colours**: one 16-colour palette for the level (chosen in CIELAB, with
   the 0.1.5 2x2 dither pairs). Each texel is a colour *pair*; a pixel
   shows the first colour where x + y is even and the second where odd, so
   the dither costs nothing at draw time.
5. **The screen**: the view sits in a border of rock (the jar's own
   texture, in the style of the original Doom's shrunk view) with a
   bevelled frame. Large: tiles 128-367, drawn over the picture on screen.
   Small: two tile sets (128-287, 288-447), drawn out of sight and swapped
   below the view. The border uses the level palette, so the damage flash
   reddens the whole frame.

Monsters, people and items straight ahead (1-4 cells) are hardware sprites,
as in 0.1.5, with one set of pre-scaled art per view size.

**Speed** (Gearsystem, the autopilot's moves and turns over the start of
level01, `tools/time_steps.py`; medians, from the button press):

| | picture finished | whole step (monsters, status bar) |
|---|---|---|
| Large | 0.62 s | 0.63 s |
| Large, low detail | 0.43 s | 0.47 s |
| Large, solid walls | 0.52 s | 0.55 s |
| Large, low detail, solid walls | 0.38 s | 0.42 s |
| Small | 0.47 s | 0.48 s |
| Small, low detail | 0.33 s | 0.35 s |
| Small, solid walls | 0.40 s | 0.42 s |
| Small, low detail, solid walls | 0.30 s | 0.32 s |
| 0.2.11: large / small | 0.62 s / 0.47 s | 0.72 s / 0.55 s |
| 0.2.9: large / small | 0.75 s / 0.60 s | 0.75 s / 0.60 s |
| 0.1.5 (pre-drawn) | 0.40 s | 0.40 s |

0.2.10 got there by drawing the view before the monsters' turn, and by
making the same work cheaper with identical results: an unrolled division
that takes half the time for divisors under 256, the scan for monsters
that can see you in assembler with a cheap distance test first, and the
status bar rebuilt only when something on it changed. Low detail works
out every other column (walls, scenery, texture fill) and converts each
column pair from one byte instead of two; solid walls fill each wall
column with one colour like the floor. Neither needs any extra data.

0.2.12 moved the whole game (rules, monsters, events, menus) from C to
assembler, which takes about 0.1 s off every step after the picture is
finished. It also fills an adjacent column pair with one byte when both
columns show the same texels (common on near walls), which pays for the
new floor: a dark red that fades to a lighter red towards the front with a
2x2 dither, over a black ceiling (`sms_ceiling_rgb` / `sms_floor_rgb` in
the project; the editor's preview shows it too). The palette is also a
little brighter and more saturated than 0.2.11's.

What is left is mostly the conversion to tiles: a quarter of the drawing
time, the same whatever the picture shows. A full-screen wall costs about
17 frames to fill and 17 to convert at the large size, close to what a
3.6 MHz Z80 can do with this approach. `tools/profile_render.py` shows
where the frames go.

**Tests**: `tools/test_lockstep.py ... --render` plays the whole sector
with the autopilot, keeps the rules in lockstep with the Python model as
before, and after every step compares the view on screen with the
renderer's model pixel for pixel, switching size, update mode, detail
and walls through the pause menu (every 25, 50, 100 and 200 steps): 389
pictures over 400 steps in all eight combinations, all identical.

### Memory

```
$0000-$0FFF  engine            $1000-$3FFF  game logic (ui, core, events)
$4000-$7FBF  game logic (combat, actions)   $7FC0 ROM directory   $7FF0 header
$4000-$7FFF  slot 1: bank 1 (game logic), or the renderer's bank while it runs
$8000-$BFFF  slot 2: bank 2 (the level blob at $A000) while the game logic
             runs, or a data bank (walls, visibility, textures, tables,
             sprites...)
$C000 engine vars ($C0C0 sound queue)   $C100 status text   $C200 dialog text   $C380 status staging
$C400 game state (compared by the test)  $CA00-$D3FF game logic variables
$D400-$D7FF  renderer: per column height, wall, texture column (+ lists in the page tails)
$D800-$DBFF  renderer: chunky strip (8 columns x 96 rows)
$DC00-$DDFF  renderer variables          $DE00 stack
```

ROM banks: 0-1 engine + C, 2 rules + level, 3 walls + visibility index,
4 strings, status bar, fonts, sprite directories, 5 title + conversion
tables, 6 renderer code, 7-8 height tables (large, small), 9-10
visibility lists, 11-16 texture columns, 17-18 billboards, then the
hardware sprites for each size. `docs/rom_format.md` has the details.

### Sound

`tools/build_sfx.py` defines the effects (frequency sweeps, noise bursts,
little arpeggios) and writes them into the engine (`build/gfx/sfx.inc`,
bank 0) plus their ids (`game/gen/sfx.inc`). The game code calls `sfx` with
an `SFX_...` id, which appends the id to an 8-entry queue at `$C0C0`; the frame interrupt
plays one step per frame on tone channel 0 and the noise channel (channel 2
silently clocks the noise for the pitched "whoosh" and explosion sounds).
`python3 tools/build_sfx.py build/gfx game/gen --wav` also writes
`build/gfx/sfx_preview.wav` with every effect, one after another, to listen
to while tweaking them. Sound never touches the game state, so the lockstep
test is unaffected.

## The editor

`../editor/` is the Deimos Editor, a level and art editor (C++, Dear ImGui,
Linux and Windows) for every console's game. It keeps the levels in its own
project files (`.deimos`), made from DoomRPG.jar, and lets you change the
map, the things, the scripts and texts, and every picture (64x64 drawings
with 16 colours, edited whole with their 8x8 tiles, shown with every
palette that uses them and as each console will show them). Making a ROM
writes only the level's data into this game's engine
(`docs/rom_format.md`); for the same level it gives exactly the same bytes
as `build.sh`. See `../editor/README.md`.

## Building

`build.sh` checks for what it needs first (`tools/deps.sh`): Python 3 with
Pillow and numpy, [WLA-DX](https://github.com/vhelin/wla-dx) and
[ZX0](https://github.com/einar-saukas/ZX0). It works out your Linux
distribution (Arch / CachyOS, Debian / Ubuntu, Fedora, openSUSE), says what
is missing and, after one "yes", installs it: Python's modules with the
package manager, WLA-DX and ZX0 built from source into
`~/.local/share/deimos-tools`. `DEIMOS_YES=1 ./build.sh` installs without
asking. (ffmpeg is only for the demo-video tools. Up to 0.2.11 the game
logic was C and also needed z88dk.)

```sh
./build.sh                    # everything (about a minute)
TEST_STEPS=0 ./build.sh       # skip the lockstep test
```

The test needs the [Gearsystem](https://github.com/drhelius/Gearsystem)
libretro core with `tools/gearsystem-debug.patch` applied (it exports the
CPU registers and a trace of recent instruction addresses, which is how the
hangs during development were found): `git apply` it, then
`cd platforms/libretro && make`. Set `DEFAULT_CORE` in
`tools/gearsystem_harness.py`.

The whole sector in lockstep: `python3 tools/test_lockstep.py
rom/deimos.sms 3000 1 --god --autopilot` plays the intended solution
(terminal sequence, the doctor, paying the civilian, door code 225, the
e-mails, override code 1010, the exit) with `tools/autopilot.py` driving
both sides; `--god` has the harness top the player's health up in both the
ROM and the model so the scripted player survives. It ends on the sector's
completion screen after 570 actions with every state identical. Add `--render`
to check every picture against the renderer's model too.
Random play: `test_lockstep.py rom/deimos.sms 500 <seed>`; `--frames DIR`
saves every frame, `--audio FILE.wav` records the sound.

`tools/test_render.py` (random steps, pictures only), `tools/time_steps.py`
(how long steps take) and `tools/sms_demo_video.py` (the demo video: the
whole sector, switching sizes through the pause menu; needs ffmpeg) use
the same harness. `python3 tools/live_data.py ... --preview DIR` draws
sample views with the model next to the floating-point reference renderer
(`tools/view_render.py`).

## Licence and credits

The port's code (game/, rom/, tools/, editor/src/) is GPL-2.0-or-later. It was written
from the behaviour of the original and from reading the GPL-3.0
DoomRPG-RE project; no DoomRPG-RE code is included. The game data (level,
textures, sprites, text, font, face) comes from the user's `DoomRPG.jar`
(id Software / Fountainhead Entertainment / JAMDAT, 2005) and is not
covered by that licence.

- Editor libraries: [Dear ImGui](https://github.com/ocornut/imgui) (MIT), [nlohmann/json](https://github.com/nlohmann/json) (MIT), [stb_image](https://github.com/nothings/stb) (public domain), [SDL2](https://libsdl.org) (zlib).
- ZX0 compressor and Z80 decoder: [Einar Saukas](https://github.com/einar-saukas/ZX0) (BSD-3).
- Toolchain: [WLA-DX](https://github.com/vhelin/wla-dx) (z88dk up to 0.2.11).
- Test emulator: [drhelius/Gearsystem](https://github.com/drhelius/Gearsystem) (GPL-3.0).
- SMS hardware facts: [SMS Power!](https://www.smspower.org/Development/).
