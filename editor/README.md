# Deimos Editor (Master System, Game Gear, Mega Drive, SNES, 3DS, 32X, Game Boy Color and Game Boy Advance)

Version 0.9.9. Part of **Deimos**: Doom RPG on nine retro consoles. Made by AI. Directed by unfnknblvbl. Licence: GPL-2.0-or-later. Version numbers are 0.x.y: x is the number of consoles the editor makes ROMs for (9: the SNES with the SuperFX counts as its own), y the number of releases since the last console was added.

A ROM maker, level editor and art editor for the console versions of Doom RPG (id Software / Fountainhead, 2005, for mobile phones):

- **Doom RPG for the Sega Master System** (`.sms`, up to 1 MB) and the **Game Gear** (`.gg`, up to 1 MB);
- **the Mega Drive game for the Sega Mega Drive / Genesis** (`.md`, up to 4 MB);
- **Doom RPG for the SNES / Super Famicom** (`.sfc`, 4 MB), and the version for a cartridge with the **SuperFX** chip (`.sfc`, 2 MB: an FXPAK Pro or an emulator);
- **Doom RPG in 3D for the Nintendo 3DS** (`.3dsx`, for the Homebrew Launcher or an emulator);
- **Doom RPG for the Sega 32X** (`.32x`, 4 MB, for PicoDrive);
- **Doom RPG for the Game Boy Color** (`.gbc`, 4 MB);
- **Doom RPG for the Game Boy Advance** (`.gba`, about 4 MB).

**The editor contains nothing of the original game.** It carries the consoles' game code only. Give it your own copy of the phone game, `DoomRPG.jar`, and it makes a complete ROM from it: the maps, the wall textures and sprites, the status bar, the fonts, the title screen, and on the Mega Drive the music and sound effects too. Without the jar there is nothing to make or edit.

**Making ROMs.** Start the editor and press **Make ROMs from DoomRPG.jar...** (or **File > New ROMs from the original game**). (The editor uses the `DoomRPG.jar` in its `import` folder, or asks for yours.) Then tick:

- **the maps**, in the order they are played. **The whole game** ticks all of them, **UAC Base only** just the first. Leaving a map for one that isn't in the ROM ends the game. Each map says whether it fits the Master System.
- **the consoles**: every one you tick gets its own ROM. The Master System and the Game Gear hold one map, within their limits, so they are greyed out (with the reason beside them) unless just one map that fits is ticked.

Press **Make the ROMs**: the editor builds them into its `export` folder, each with its console's usual name (`deimos (2).sms` and so on when the name is taken), and opens the first. Building a Master System map takes about 15 seconds, the whole game about a minute per console. The ROMs are a power of two in size (the 3DS's is a `.3dsx` holding one; the Game Boy Advance's is the code and a SNES image, in 64 KB steps) and run in emulators and on flash carts (Mega EverDrive, Master EverDrive, GG EverDrive...).

**Editing.** Open a ROM made here (**File > Open ROM**, or drag it onto the window). Change the map, the things in it, the scripts and texts, and every picture, then **File > Save ROM as...** with a new name. The level is stored inside the ROM, so the ROM is all you need. When you save, the editor rebuilds everything that depends on the level. The game code is never changed, so your level plays by exactly the same rules as the original maps. The editor also opens the Mega Drive ROMs of the Mega Drive game's own releases (0.0.4 on).

Both games keep the level in the same format. **File > Put this level into the other console's ROM** turns a Master System level into a Mega Drive one, or the other way round.

**Which ROMs can be edited.** To save cartridge space, the Master System, Game Gear, SNES, SNES with the Super FX chip and 32X ROMs are made *locked*: they carry no copy of the level for the editor, and **File > Open ROM** refuses them. Keep the level in an editable ROM (Mega Drive, 3DS, Game Boy Color or Game Boy Advance) and use **File > Make a ROM for another console** to make a locked ROM of it for any console. **File > Save a locked copy...** saves a locked copy of an editable ROM too.

**One console's own art and text.** The Art and Text tabs have an **Edit for** box. With **Every console** a change goes to all of them; pick one console and the pictures or texts you change are that console's own, the others keep the shared ones. On consoles with few colours (the Master System and Game Gear) a picture edited for them is snapped to the colours the console can show, and the level's fixed palette is shown to paint with.

**Button names in texts.** Texts that name the phone's keys (the 7 and * keys, #, 0, 9 and OK) show the console's own buttons in the game, and on consoles where the controls can be changed, the button the player has set. In the Text tab they appear as `{NEXT WEAPON}`, `{PREV WEAPON}`, `{NOTEBOOK}`, `{PAUSE MENU}`, `{PASS TURN}` and `{USE}`; type them the same way.

**The whole game on the Mega Drive.** Mega Drive versions 0.0.6 and later hold the whole game: UAC Base, the Junction, Sectors 1 to 7 and the destroyed Junction, and from 0.0.9 the Reactor, where the game ends. When you open such a ROM, the **Map** menu lists them; pick the one to edit. **Save ROM** writes that map back and leaves the others as they are. Exits keep working: a script that changes map names the target map (for example `/level02.bsp`), so a map you edit still leads to the right place. A level from elsewhere (a Master System ROM, a folder, the original game) takes the place of the map you have picked; when you put one into a whole-game ROM, the editor asks which map it replaces.

The maps share the wall textures and sprites. If you change a drawing that other maps also use, only the map you are editing gets the new drawing; the others keep theirs. The player's weapon is the same on every map, so a change to it applies to all of them.

**Another level from the original game into an open ROM.** **File > Import a level from the original game** lists every map in your `DoomRPG.jar` and says whether each fits. It converts the one you choose (map, things, scripts, texts and art) into the level you're editing.

On the Master System the UAC Base, Sector 1, Sector 2, the Junctions and the Reactor fit by their counts (the Limits tab says what a map needs). Very big open areas can still be too much to see at once, and the build says so. The Mega Drive holds every map. A few script commands only the phone game had are listed and are skipped by the Master System.

The program is `Deimos Editor` (`Deimos Editor.exe` on Windows). The command-line tool, `deimos-editor-cli`, is built alongside it in `cmake-build/` for scripts and tests.

## 1. The tools

`build.sh` checks for them first (`deps.sh`) and offers to install what is missing with your distribution's package manager (pacman on Arch / CachyOS, apt on Debian / Ubuntu, dnf on Fedora, zypper on openSUSE), after one "yes": CMake, a C++ compiler, git, the X11/Wayland/sound headers SDL2 is built against, and MinGW-w64 for the Windows version. `DEIMOS_YES=1 ./build.sh` installs without asking.

The build downloads SDL2 and compiles it into the editor, so the editor runs on other Linux machines without SDL2 installed.

## 2. Build

In the `editor` folder:

```sh
./build.sh              # asks: Linux, Windows or both (just Enter: both)
./build.sh linux        # Linux only
./build.sh windows      # Windows only
./build.sh both         # both, without asking
```

Each platform's editor goes to its own folder, emptied first, holding only what is passed on: zip up the folder and give it to someone.

```
build/linux/  (build/windows/ the same, with Deimos Editor.exe)
    Deimos Editor     the editor
    engines/           the consoles' games (sg8.eng ... ntnd3Dhh.eng, below)
    README.md          how to use it (from dist/README.md)
    docs/              the licences
    import/            where DoomRPG.jar goes ("Doom RPG jar file here.txt" says so)
    export/            where the editor puts the ROMs it makes
```

Nothing else is needed to run it: SDL2 is built into the program, the consoles' games are the files in `engines/` beside it, and the person only needs their own `DoomRPG.jar`.

**The engines.** Each console's game code, with nothing of the original game in it, is one file in `engines/`, named for the console's maker and kind: `sg8.eng` (Master System), `sg8hh.eng` (Game Gear), `sg16.eng` (Mega Drive), `sg32.eng` (32X), `ntnd8hh.eng` (Game Boy Color), `ntnd16.eng` (SNES), `ntnd16fx.eng` (SNES with the Super FX), `ntnd32hh.eng` (Game Boy Advance) and `ntnd3Dhh.eng` (3DS). The editor offers the consoles whose file is there, so an editor can be passed on with only some of them, and a newer engine can be dropped in without a new editor. `../build.sh engines` makes them again from the games' source. Run from its build folder (`cmake-build/<platform>/`), the editor reads `editor/engines/` in the source; `DEIMOS_ENGINES=folder` points it anywhere else. There's no `defaults` folder of ROMs: the editor makes every ROM itself from the jar. CMake's working files stay in `cmake-build/`. Without MinGW-w64, `./build.sh` builds the Linux version and says how to get the Windows one. The first build of each downloads and compiles SDL2, which takes a few minutes.

## 3. Run

```sh
"build/linux/Deimos Editor"          # or with a ROM: "build/linux/Deimos Editor" path/to/deimos.sms
```

At start the editor looks for `DoomRPG.jar` in its `import` folder; without one it asks where it is and copies it there. The first time, press **Make ROMs from DoomRPG.jar...** (see the top of this file). After that, **File > Open ROM** opens a ROM, or drag it onto the window. Use **File > Save ROM as...** with a new name, so you always keep the one you started from.

Everything is sized to your display's scale (KDE, GNOME and Windows display scaling), so a 4K screen at 200% gets text twice the size. **Help > Settings > text size** makes it bigger or smaller from there. The first time, the editor opens maximised.

**Help > How to use** explains the tools. The tabs:

- **Map.** The tools are on the left (keys 1-9), the map is in the middle and the inspector is on the right. Drag the bar between the map and the inspector to make the inspector wider, up to a third of the window. The view preview grows with it. The map is a grid of 32 x 32 cells, the most the games support. Mouse wheel zooms and right-drag moves the map. Select a cell to see the view from it and to edit what's in it: walls, door, things, props and its script. Pick a console under the view to see it as that console shows it: the Master System and the Game Gear by their own renderer (exactly what they draw, also with the pause menu's low detail and solid walls); the others at their size, in their colours (the Mega Drive's 15 a view, the SNES family's 15 bits, the Game Boy Color's colour families per tile), with their floor and ceiling shading. Tick "doors open" to see through the doors.
- **Art.** A small paint program, described below.
- **Thing types.** What each kind of thing is: monster class, pickup amount, sprite, and so on.
- **Text & scripts.** All the texts (edit them freely) and every scripted cell. A new line in a text box is a new line in the game's dialog box: 18 letters fit on the Master System, 24 on the Mega Drive. A button written as its name in braces, such as `{NEXT WEAPON}` or `{PAUSE MENU}` (the original's "the * and 7 keys" and the like are shown that way), appears in the game as the button that does it on that console, the one the player has chosen where the buttons can be changed; a line with one is re-wrapped to fit when the ROM is made.
- **Limits.** What the level uses, against what each game allows.
- **Build.** Save the game ROM image, save and play, or copy the level to another console's ROM. Shows how much room is left in the ROM.

**F5** saves and starts your emulator. Set the commands once in **Help > Settings**, for example `mednafen %f` or `blastem %f`.

**Undo/redo:** Ctrl+Z / Ctrl+Y. **Save:** Ctrl+S.

### The Art tab

Every picture is 64 x 64 pixels with its own 16 colours. The pictures are listed on the left: wall textures, sprites (things and props), the Master System's border and the Mega Drive's weapon (each weapon twice, still and firing).

- **The whole picture** is shown in the middle, with its 8x8 tile grid. It fits the panel to start with. **Ctrl+mouse wheel** zooms in and out around the mouse (up to 48x), the wheel scrolls, and the middle button drags the picture around. The zoom buttons are also above it, and **Fit** brings the whole picture back. Draw with the left mouse button in the first colour and the right button in the second. The tools: pencil (P), line (L), box (B), filled box (Shift+B), fill (F) and pick colour (K). X swaps the two colours.
- **Tiles.** Ctrl+click a tile to pick it out: it appears large on the right, where you can draw in it, copy and paste it, clear it or flip it. The arrow keys move to the next tile. Shift+fill fills only inside the picked tile.
- **Colours.** The picture's 16 colours are on the right. Click one for the first colour, right-click for the second, and double-click to change it. The colour window also shows the Master System's 64 colours to pick from, and can snap a colour to the nearest one the Mega Drive can show.
- **Palettes.** As in the original game, several pictures can share one drawing with different colours (wall texture 0 is the same drawing as 9 and 10). The panel shows the drawing with each set of colours. Click one to try those colours, or pick any other picture's colours to see how they look. Painting changes the drawing for every picture that shares it. **Give this picture a drawing of its own** separates it first.
- **On the consoles** shows how each console will show the picture. On the Master System, walls and props use the level's 16 colours mixed in pairs. Things are hardware sprites with 15 colours of their own. The "show" menu above the picture shows the whole picture that way.
- **Import PNG / Export PNG** to use another paint program. An imported picture is reduced to 64 x 64 and 16 colours, and gets a drawing of its own.

### Settings

Settings are kept in `deimos-editor.ini` in the editor's own folder (beside the program), so each copy of the editor has its own and never picks up another version's. They cover recent files, window size, zoom, emulator commands, the preview and the text size. You can edit the file while the editor is closed.

## Command line

`cmake-build/linux/deimos-editor-cli` does the same jobs without a window:

```sh
deimos-editor-cli --new-rom DoomRPG.jar sms deimos.sms    # a Master System ROM (the UAC Base; or name a map: ... level01)
deimos-editor-cli --new-rom DoomRPG.jar md deimos.md     # the whole game for the Mega Drive (or name the maps: ... intro)
deimos-editor-cli --selftest deimos.sms                   # unpack + repack check (a whole-game ROM: every map)
deimos-editor-cli --rom-maps deimos.md                   # a whole-game ROM's maps, numbered
deimos-editor-cli --transfer deimos.sms deimos.md out.md --map 2   # the SMS level in place of Sector 1
                                                        # (--map N picks the map of a whole-game ROM to work on)
deimos-editor-cli --rebuild deimos.sms out.sms            # rebuild from the ROM's own level
deimos-editor-cli --transfer deimos.sms deimos.md out.md   # the SMS level in the Mega Drive game
deimos-editor-cli --export deimos.sms mylevel/            # project.json + PNGs, to keep in git
deimos-editor-cli --import mylevel/ deimos.sms out.sms    # build a ROM from such a folder
deimos-editor-cli --render deimos.sms 6 2 2 view.ppm      # the Master System view from cell 6,2 facing south
                                                        # (add small, low or solid for the other view settings)
deimos-editor-cli --maps DoomRPG.jar                       # the original game's maps, and which fit
deimos-editor-cli --from-game DoomRPG.jar reactor deimos.sms out.sms   # build a ROM with one of them
```

`--selftest` proves that unpacking and repacking doesn't change the game. It reads the level out of the ROM, rebuilds the ROM from it and compares the two. For both consoles the rebuilt ROM is **byte-for-byte identical** to the one the port's own build tools make, and so is a ROM made with `--new-rom` from the jar. The editor's builders and its makers of the status bar, fonts, title screens, music and sounds are exact ports of those tools. They use plain integer arithmetic where it matters, which gives the same answer on every computer, Windows included.

`engines/` holds the two games' code without anything of the original game (each port's `tools/make_engine.py` makes them: the parts made from the jar are blanked and listed). The build embeds them in the editor.

An exported folder holds `project.json` (the level), `drawings/` (each drawing as grey levels = colour numbers) and every picture as a PNG in its colours. If you change a picture's PNG in another program and import the folder, that picture gets a drawing of its own.

## Limits

| | Master System | Mega Drive (one level, 0.0.4-0.0.5) | Mega Drive (whole game, 0.0.6+), per map |
|---|---|---|---|
| map | 32 x 32 cells | 32 x 32 cells | 32 x 32 cells |
| things (of which monsters) | 192 (56) | 152 (56) | 280 (100) |
| doors and sliding walls | 40 | 32 | 64 |
| scripted cells, script commands | 96, 320 | 96, 240 | 160, 448 |
| thing types | 64 | 64 | 64 |
| texts | 255, 8 KB in all | 255, 6 KB in all | 255, 16 KB in all |
| wall textures (each wall decal adds one) | 255 | 48 | 48 |
| different sprites | - | 64 | 64 |
| colours | 16 per picture (the level picks 16 in all) | 16 per picture, 32 per texture with decals | the same |
| ROM | a power of two, up to 1 MB | a power of two, up to 2 MB | a power of two, up to 4 MB, all maps together |

Walls must run along cell edges or at 45 degrees. On the Master System at most 96 bytes of walls and props may be visible from one spot (about 45 wall pieces). If a view has too much, the build says which cell.

## Source layout

```
src/main.cpp            window, main loop     src/app.*        editor state, menus, undo, saving
src/ui_map.cpp          map tab               src/ui_art.cpp   the paint program
src/ui_data.cpp         types, text, limits, build, help      src/scripts.*    script editor
src/edit.*              map operations (walls around floor, doors + their scripts, index fix-ups)
src/project.*           the level (project.json)              src/container.*  the copy inside the ROM
src/rom.*               opening/saving either console's ROM
src/engine.*            the engines (engines/*.eng) and new ROMs from DoomRPG.jar
src/assets.*            status bar, fonts, title screens, music and sounds from DoomRPG.jar
src/geometry.*          wall pieces, decals, what can be seen from a cell (both consoles)
src/sms_build.*         Master System builder  src/sms_model.cpp  the Master System renderer, bit for bit
src/md_build.*          Mega Drive builder     src/render.*       true-colour view (Mega Drive preview)
src/colour.*            colour maths (same results as tools/colour_lab.py)
src/jar.*               reading the original game (DoomRPG.jar): maps, entity types, texts, art
src/levelblob.*         level/strings blobs    src/zx0.*  ZX0 compression    src/settings.*  the .ini file
third_party/            Dear ImGui (MIT), nlohmann/json (MIT), stb_image (public domain),
                        ZX0 compressor (BSD-3, made thread-safe; changes marked)
```

Licence: GPL-2.0-or-later (the third-party code keeps its own licences).
