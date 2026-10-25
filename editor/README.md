# Deimos Editor (Master System, Game Gear, Mega Drive, SNES, 3DS, 32X, Game Boy Color and Game Boy Advance)

Version 0.10.0. Part of **Deimos**: Doom RPG on ten retro consoles. Made by AI. Directed by unfnknblvbl. Licence: GPL-2.0-or-later. Version numbers are 0.x.y: x is the number of consoles the editor makes ROMs for (10: the SNES with the SuperFX counts as its own), y the number of releases since the last console was added.

A ROM maker, level editor and art editor for the console versions of Doom RPG (id Software / Fountainhead, 2005, for mobile phones):

- **Doom RPG for the Sega Master System** (`.sms`, up to 1 MB) and the **Game Gear** (`.gg`, up to 1 MB);
- **the Mega Drive game for the Sega Mega Drive / Genesis** (`.md`, up to 4 MB);
- **Doom RPG for the SNES / Super Famicom** (`.sfc`, 4 MB), and the version for a cartridge with the **SuperFX** chip (`.sfc`, 2 MB: an FXPAK Pro or an emulator);
- **Doom RPG in 3D for the Nintendo 3DS** (`.3dsx`, for the Homebrew Launcher or an emulator);
- **Doom RPG for the Sega 32X** (`.32x`, 4 MB, for PicoDrive);
- **Doom RPG for the Game Boy Color** (`.gbc`, 4 MB);
- **Doom RPG for the Game Boy Advance** (`.gba`, about 4 MB).

**The editor contains nothing of the original game.** It carries the consoles' game code only. Give it your own copy of the phone game, `DoomRPG.jar`, and it makes a complete ROM from it: the maps, the wall textures and sprites, the status bar, the fonts, the title screen, the music and sound effects. Without the jar there is nothing to make or edit. (The editor uses the `DoomRPG.jar` in its `import` folder, or asks for yours.)

**Projects.** You edit a **project**: a `.deimos` file holding the maps you change, the whole game or a single level, each with its map, things, scripts, texts and pictures (and each console's own versions of them). **File > New project from the original game** (or **New project from DoomRPG.jar...** on the start page) makes one: tick the maps, in the order they are played (**The whole game**, **UAC Base only**, or any of them; one map makes a single level). **File > Open project** (or drag a `.deimos` file onto the window, or **Open recent**), **Save project** (Ctrl+S) and **Save project as...** do the rest; the editor opens the last project at start. The **Map** menu lists the project's maps: pick the one to edit (switching keeps your changes in the project). Exits keep working: a script that changes map names the target map (for example `/level02.bsp`), so a map you edit still leads to the right place.

**Making ROMs.** **File > Make ROMs of this project...** (or **Make ROMs...** in the Build tab): tick the consoles, and each gets its own ROM in the editor's `export` folder, with its console's usual name (`deimos (2).sms` and so on when the name is taken). Every console but the Master System and the Game Gear gets all of the project's maps, in order; the Master System and the Game Gear hold one map, the one being edited, within their limits (greyed out, with the reason beside them, when it doesn't fit). **F5** (**File > Make a ROM and play**) makes one ROM for the console chosen in the Build tab, `export/play_...`, and starts your emulator on it. **Make ROMs from DoomRPG.jar...** (start page, File menu) makes ROMs of the original maps without a project: tick the maps and the consoles. Building a Master System map takes about 15 seconds, the whole game about a minute per console. The ROMs are a power of two in size (the 3DS's is a `.3dsx` holding one; the Game Boy Advance's is the code and a SNES image, in 64 KB steps) and run in emulators and on flash carts (Mega EverDrive, Master EverDrive, GG EverDrive...).

The ROMs carry no copy of the project (all their room goes to the maps), and the editor doesn't open ROMs: **keep the project** to change the maps again. The game code is never changed, so your maps play by exactly the same rules as the original ones. For the same maps, a ROM made from a project is byte for byte the one made straight from the jar, and the one the consoles' own builds make.

**One console's own art and text.** The Art and Text tabs have an **Edit for** box. With **Every console** a change goes to all of them; pick one console and the pictures or texts you change are that console's own, the others keep the shared ones. On consoles with few colours (the Master System and Game Gear) a picture edited for them is snapped to the colours the console can show, and the level's fixed palette is shown to paint with.

**Button names in texts.** Texts that name the phone's keys (the 7 and * keys, #, 0, 9 and OK) show the console's own buttons in the game, and on consoles where the controls can be changed, the button the player has set. In the Text tab they appear as `{NEXT WEAPON}`, `{PREV WEAPON}`, `{NOTEBOOK}`, `{PAUSE MENU}`, `{PASS TURN}` and `{USE}`; type them the same way.

The maps share the wall textures and sprites. If you change a drawing that other maps also use, only the map you are editing gets the new drawing (saving gives one of the two a key of its own); the others keep theirs. The player's weapon is the same on every map, so a change to it applies to all of them.

**Another level from the original game.** **File > Import a level from the original game** lists every map in your `DoomRPG.jar` and says whether each fits. It converts the one you choose (map, things, scripts, texts and art) in place of the map you're editing, which keeps its place in the project. **File > Export map to folder** / **Import map from folder** write the map as plain files (below) and read them back, to change the pictures in another paint program.

On the Master System the UAC Base, the first two sectors (level01, level02), the Junctions and the Reactor fit by their counts (the Limits tab says what a map needs). Very big open areas can still be too much to see at once, and the build says so. The other consoles hold every map. A few script commands only the phone game had are listed and are skipped by the consoles.

The program is `Deimos Editor` (`Deimos Editor.exe` on Windows); given arguments starting `--`, it is the command-line tool (below). `deimos-editor-cli`, the same tool without a window, is built alongside it in `cmake-build/` for scripts and tests.

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

**The engines.** Each console's game code, with nothing of the original game in it, is one file in `engines/`, named for the console's maker and kind: `sg8.eng` (Master System), `sg8hh.eng` (Game Gear), `sg16.eng` (Mega Drive), `sg32.eng` (32X), `ntnd8hh.eng` (Game Boy Color), `ntnd16.eng` (SNES), `ntnd16fx.eng` (SNES with the Super FX), `ntnd32hh.eng` (Game Boy Advance), `ntnd3Dhh.eng` (3DS) and `ntnd32vr.eng` (Virtual Boy). The editor offers the consoles whose file is there, so an editor can be passed on with only some of them, and a newer engine can be dropped in without a new editor. `../build.sh engines` makes them again from the games' source. Run from its build folder (`cmake-build/<platform>/`), the editor reads `editor/engines/` in the source; `DEIMOS_ENGINES=folder` points it anywhere else. There's no `defaults` folder of ROMs: the editor makes every ROM itself from the jar. CMake's working files stay in `cmake-build/`. Without MinGW-w64, `./build.sh` builds the Linux version and says how to get the Windows one. The first build of each downloads and compiles SDL2, which takes a few minutes.

## 3. Run

```sh
"build/linux/Deimos Editor"          # or with a project: "build/linux/Deimos Editor" path/to/my.deimos
```

At start the editor looks for `DoomRPG.jar` in its `import` folder; without one it asks where it is and copies it there. The first time, press **New project from DoomRPG.jar...** (see the top of this file) and save the project. After that the editor opens the last project at start; **File > Open project** opens another, or drag it onto the window.

Everything is sized to your display's scale (KDE, GNOME and Windows display scaling), so a 4K screen at 200% gets text twice the size. **Help > Settings > text size** makes it bigger or smaller from there. The first time, the editor opens maximised.

**Help > How to use** explains the tools. The tabs:

- **Map.** The tools are on the left (keys 1-9), the map is in the middle and the inspector is on the right. Drag the bar between the map and the inspector to make the inspector wider, up to a third of the window. The view preview grows with it. The map is a grid of 32 x 32 cells, the most the games support. Mouse wheel zooms and right-drag moves the map. Select a cell to see the view from it and to edit what's in it: walls, door, things, props and its script. Pick a console under the view to see it as that console shows it: the Master System and the Game Gear by their own renderer (exactly what they draw, also with the pause menu's low detail and solid walls); the others at their size, in their colours (the Mega Drive's 15 a view, the SNES family's 15 bits, the Game Boy Color's colour families per tile), with their floor and ceiling shading. Tick "doors open" to see through the doors.
- **Art.** A small paint program, described below.
- **Thing types.** What each kind of thing is: monster class, pickup amount, sprite, and so on.
- **Text & scripts.** All the texts (edit them freely) and every scripted cell. A new line in a text box is a new line in the game's dialog box: 18 letters fit on the Master System, 24 on the Mega Drive. A button written as its name in braces, such as `{NEXT WEAPON}` or `{PAUSE MENU}` (the original's "the * and 7 keys" and the like are shown that way), appears in the game as the button that does it on that console, the one the player has chosen where the buttons can be changed; a line with one is re-wrapped to fit when the ROM is made.
- **Limits.** What the map uses, against what the games allow, and whether it fits the Master System and the Game Gear.
- **Build.** Save the project, make ROMs of it, or make one and play it (F5, on the console chosen there). The log shows how much room is left in each ROM.

**F5** makes a ROM and starts your emulator. Set the commands once in **Help > Settings**, for example `mednafen %f` or `blastem %f`.

**Undo/redo:** Ctrl+Z / Ctrl+Y (each map's own; switching maps starts afresh). **Save the project:** Ctrl+S.

### The Art tab

Every picture is 64 x 64 pixels with its own 16 colours. The pictures are listed on the left: wall textures, sprites (things and props), the Master System's border and the Mega Drive's weapon (each weapon twice, still and firing).

- **The whole picture** is shown in the middle, with its 8x8 tile grid. It fits the panel to start with. **Ctrl+mouse wheel** zooms in and out around the mouse (up to 48x), the wheel scrolls, and the middle button drags the picture around. The zoom buttons are also above it, and **Fit** brings the whole picture back. Draw with the left mouse button in the first colour and the right button in the second. The tools: pencil (P), line (L), box (B), filled box (Shift+B), fill (F) and pick colour (K). X swaps the two colours.
- **Tiles.** Ctrl+click a tile to pick it out: it appears large on the right, where you can draw in it, copy and paste it, clear it or flip it. The arrow keys move to the next tile. Shift+fill fills only inside the picked tile.
- **Colours.** The picture's 16 colours are on the right. Click one for the first colour, right-click for the second, and double-click to change it. The colour window also shows the Master System's 64 colours to pick from, and can snap a colour to the nearest one the Mega Drive can show.
- **Palettes.** As in the original game, several pictures can share one drawing with different colours (wall texture 0 is the same drawing as 9 and 10). The panel shows the drawing with each set of colours. Click one to try those colours, or pick any other picture's colours to see how they look. Painting changes the drawing for every picture that shares it. **Give this picture a drawing of its own** separates it first.
- **On the consoles** shows how each console will show the picture. On the Master System, walls and props use the level's 16 colours mixed in pairs. Things are hardware sprites with 15 colours of their own. The "show" menu above the picture shows the whole picture that way.
- **Import PNG / Export PNG** to use another paint program. An imported picture is reduced to 64 x 64 and 16 colours, and gets a drawing of its own.

### Settings

Settings are kept in `deimos-editor.ini` in the editor's own folder (beside the program), so each copy of the editor has its own and never picks up another version's. They cover the recent projects, window size, zoom, emulator commands, the console F5 plays on, the preview and the text size. You can edit the file while the editor is closed.

## Command line

`cmake-build/linux/deimos-editor-cli` (or the editor itself, `"Deimos Editor" --help`) does the same jobs without a window. CONSOLE is one of `sms gg md snes snesfx 3ds 32x gbc gba`; `--map N` anywhere picks a project's map (0 = the first).

```sh
deimos-editor-cli --new-rom DoomRPG.jar sms deimos.sms     # a Master System ROM straight from the game (the UAC Base; or name a map: ... level01)
deimos-editor-cli --new-rom DoomRPG.jar md deimos.md       # the whole game for the Mega Drive (or name the maps: ... intro level01)
deimos-editor-cli --new-project DoomRPG.jar game.deimos   # a project of the whole game
deimos-editor-cli --new-project DoomRPG.jar level01 mine.deimos   # a project of one map (or several, in order)
deimos-editor-cli --make-rom DoomRPG.jar game.deimos gba deimos.gba   # a ROM of the project's maps
deimos-editor-cli --make-rom DoomRPG.jar game.deimos sms deimos.sms --map 2   # the Master System: one map (here level01)
deimos-editor-cli --project-maps game.deimos              # the project's maps, numbered
deimos-editor-cli --selftest game.deimos                  # saving and opening it changes nothing
deimos-editor-cli --export game.deimos mylevel/ --map 2   # a map as project.json + PNGs, to keep in git
deimos-editor-cli --import mylevel/ out.deimos            # such a folder as a new one-map project
deimos-editor-cli --import mylevel/ out.deimos game.deimos --map 2   # ... or in place of a project's map
deimos-editor-cli --render game.deimos 6 2 2 view.ppm     # the Master System view from cell 6,2 facing south
                                                        # (add small, low or solid for the other view settings)
deimos-editor-cli --maps DoomRPG.jar                      # the original game's maps, and which fit
deimos-editor-cli --from-game DoomRPG.jar reactor game.deimos out.deimos --map 2   # one of them in place of a map
```

For the tests there are also `--demo-map PROJECT OUT [JSON]` (a small map made with the editor's own operations, with a new text and changed art, in place of map `--map N`), `--check-forms JAR` (every map read as the Master System's single level equals the same map read in the whole game's form and converted), `--dump-assets JAR CONSOLE VERSION DIR` and `--dump-strings JAR CONSOLE DIR` (the parts made from the jar, to compare with the Python builds').

A ROM made with `--new-rom` is **byte-for-byte identical** to the one the console's own build makes, and `--make-rom` of a project of the same maps gives the same bytes again. The editor's builders and its makers of the status bar, fonts, title screens, music and sounds are exact ports of the builds' tools. They use plain integer arithmetic where it matters, which gives the same answer on every computer, Windows included.

`engines/` holds the consoles' game code without anything of the original game (each port's `tools/make_engine.py` and `../tools/snes_make_engine.py` make them: the parts made from the jar are blanked and listed). Not even the game's own words are in the code: the messages, the menus' and the vendor's words, the weapons' names and the maps' titles are made from the jar's string tables when a ROM is made (`src/gametext.cpp`, the same recipes as `../tools/game_text.py`), into the campaign block's texts table (the Master System's bank 2, `$8000`).

**The project file** (`.deimos`, `src/projectfile.h`): `"DEIMPRJ1"`, its size and a list of entries, each a name and its zlib-packed data: `campaign.json` (the maps, in play order: their names, titles and the original game's map ids), `map/<name>.json` (each map's level, as in an exported `project.json`) and `shape/<key>` (every drawing the maps use, once: 64 x 64 colour numbers).

An exported folder holds `project.json` (the level), `map.json` (its name, title and id in its project), `drawings/` (each drawing as grey levels = colour numbers) and every picture as a PNG in its colours. If you change a picture's PNG in another program and import the folder, that picture gets a drawing of its own.

## Limits

| | Master System, Game Gear | the other consoles, per map |
|---|---|---|
| map | 32 x 32 cells | 32 x 32 cells |
| things (of which monsters) | 192 (56) | 280 (100) |
| doors and sliding walls | 40 | 64 |
| scripted cells, script commands | 96, 320 | 160, 448 |
| thing types | 64 | 64 |
| texts | 255, 8 KB in all | 255, 16 KB in all |
| wall textures (each wall decal adds one) | 255 | 48 |
| different sprites | - | 64 |
| colours | 16 per picture (the level picks 16 in all) | 16 per picture, 32 per texture with decals |
| ROM | a power of two, up to 1 MB | all maps together: up to 4 MB (the Super FX 2 MB) |

Walls must run along cell edges or at 45 degrees. On the Master System at most 96 bytes of walls and props may be visible from one spot (about 45 wall pieces). If a view has too much, the build says which cell.

## Source layout

```
src/main.cpp            window, main loop     src/app.*        editor state, menus, undo, projects, making ROMs
src/ui_map.cpp          map tab               src/ui_art.cpp   the paint program
src/ui_data.cpp         types, text, limits, build, help      src/scripts.*    script editor
src/edit.*              map operations (walls around floor, doors + their scripts, index fix-ups)
src/project.*           a level (its JSON)    src/projectfile.*  the project file (.deimos)
src/projectio.*         level folders (project.json + PNGs)   src/cli.*  the command line
src/rom.*               the consoles; saving a ROM (n3ds.*, mars.*, gbc.*, gba.*: the wrapped ones)
src/engine.*            the engines (engines/*.eng); ROMs from DoomRPG.jar or a project
src/assets.*            status bar, fonts, title screens, music and sounds from DoomRPG.jar
src/geometry.*          wall pieces, decals, what can be seen from a cell (both consoles)
src/sms_build.*         Master System builder  src/sms_model.cpp  the Master System renderer, bit for bit
src/md_build.*          the campaign block (every other console)  src/render.*  true-colour view (previews)
src/colour.*            colour maths (same results as tools/colour_lab.py)
src/jar.*               reading the original game (DoomRPG.jar): maps, entity types, texts, art
src/gametext.*          the game's own texts the consoles' code shows (messages, menus, weapons, maps' titles), from the jar
src/keytext.*           the phone's keys in the texts, as the consoles' buttons
src/levelblob.*         level/strings blobs    src/zx0.*  ZX0 decompression (the engines)    src/settings.*  the .ini file
third_party/            Dear ImGui (MIT), nlohmann/json (MIT), stb_image and stb_image_write (public domain)
```

Licence: GPL-2.0-or-later (the third-party code keeps its own licences).
