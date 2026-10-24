# Deimos Editor @VERSION@

Part of **Deimos**: Doom RPG on nine retro consoles. Made by AI. Directed by unfnknblvbl. Free software under the GNU GPL, version 2 or later (`docs/GPL-2.0.txt`).

A ROM maker, level editor and art editor for the console versions of Doom RPG (id Software / Fountainhead, 2005, for mobile phones):

- **Doom RPG for the Sega Master System** (`.sms`, up to 1 MB) and the **Game Gear** (`.gg`, up to 1 MB);
- **the Mega Drive game for the Sega Mega Drive / Genesis** (`.md`, up to 4 MB);
- **Doom RPG for the SNES / Super Famicom** (`.sfc`, 4 MB), and the version for a cartridge with the **SuperFX** chip (`.sfc`, 2 MB: an FXPAK Pro or an emulator);
- **Doom RPG in 3D for the Nintendo 3DS** (`.3dsx`, for the Homebrew Launcher or an emulator);
- **Doom RPG for the Sega 32X** (`.32x`, 4 MB, for PicoDrive);
- **Doom RPG for the Game Boy Color** (`.gbc`, 4 MB);
- **Doom RPG for the Game Boy Advance** (`.gba`, about 4 MB).

## What's in this folder

| | |
|---|---|
| `Deimos Editor` (`Deimos Editor.exe` on Windows) | the editor: double-click it |
| `README.md` | this file |
| `import/` | put your `DoomRPG.jar` here |
| `export/` | the ROMs the editor makes go here |
| `docs/` | the licences |

The editor needs nothing else installed. On Linux, if double-clicking doesn't start it, make it executable first (right-click > Properties > Permissions, or `chmod +x "Deimos Editor"`).

## Your copy of Doom RPG

**The editor contains nothing of the original game.** It carries the consoles' game code only (the files in the `engines` folder beside it: keep that folder with the editor), and makes everything else from your own copy of the phone game, `DoomRPG.jar`: the maps, the wall textures and sprites, the status bar, the fonts, the title screen, the music and the sound effects.

Put `DoomRPG.jar` in the `import` folder. When the editor starts, it looks there. If there's no valid one, it asks where yours is and copies it into `import`, so it finds it the next time.

## Projects

You edit a **project**: a `.deimos` file that holds the maps you change, the whole game or a single level, each with its map, the things in it, the scripts and texts, and every picture (with each console's own versions, if you give it any). The ROMs are made from the project; they carry no copy of it, so **keep the project** to change your maps again.

**A new project.** Press **New project from DoomRPG.jar** (or **File > New project from the original game**) and tick the maps, in the order they are played: **The whole game** ticks all of them, **UAC Base only** just the first; one map makes a single level. Leaving a map for one that isn't in the project ends the game. Each map says whether it fits the Master System. The project opens on its first map; **File > Save project as...** gives it a file.

**Opening and saving.** **File > Open project** (or drag a `.deimos` file onto the window, or pick one from **Open recent** or the start page), **File > Save project** (Ctrl+S) and **Save project as...**. The editor opens the last project at start.

**The maps.** The **Map** menu lists the project's maps; pick the one to edit. Switching keeps your changes in the project (Save writes all of them). Exits keep working: a script that changes map names the target map (for example `/level02.bsp`), so a map you edit still leads to the right place. The maps can share the wall textures and sprites: if you change a drawing that other maps also use, only the map you changed gets the new drawing (saving stores both); the others keep theirs. The player's weapon is the same on every map, so a change to it applies to all of them.

**Another level from the original game.** **File > Import a level from the original game** lists every map in your `DoomRPG.jar` and says whether each fits. It converts the one you choose (map, things, scripts, texts and art) in place of the map you're editing; that map keeps its place in the project. **File > New map** empties the map to a small room, keeping its art, thing types and texts.

**Editing the art outside.** **File > Export map to folder** writes the map as `project.json`, `map.json`, its drawings and every picture as a PNG; change the PNGs in another paint program and **File > Import map from folder** brings them back (each changed picture gets a drawing of its own).

## Making ROMs

**Of your project.** **File > Make ROMs of this project** (or **Make ROMs...** in the Build tab): tick the consoles, and each gets its own ROM in the `export` folder, with its console's usual name (`deimos.sms`, `deimos.md`, `deimos.gba`...; if that name is taken, `deimos (2).sms` and so on). Every console but the Master System and the Game Gear gets every map of the project, in order; the Master System and the Game Gear hold one map, the one you are editing, within their limits (they are greyed out, with the reason beside them, when it doesn't fit). **F5** (**File > Make a ROM and play**) makes one ROM, for the console chosen in the Build tab, as `export/play_...`, and starts your emulator on it.

**Straight from the game.** **Make ROMs from DoomRPG.jar** (on the start page or in the File menu) makes ROMs of the original maps without a project: tick the maps and the consoles, as above.

The editor builds everything that depends on the maps; the game code is never changed, so your maps play by exactly the same rules as the original ones. A Master System map takes about 15 seconds, the whole game about a minute per console. The ROMs run in emulators and on flash carts.

On the Master System the UAC Base, Sector 1, Sector 2, the Junctions and the Reactor fit by their counts (the Limits tab says what a map needs). Very big open areas can still be too much to see at once, and the build says so. The other consoles hold every map. A few script commands only the phone game had are listed and are skipped by the consoles.

The editor also works from the command line: `"Deimos Editor" --help` lists what it does (new projects, ROMs, level folders).

## Using the editor

Everything is sized to your display's scale (KDE, GNOME and Windows display scaling), so a 4K screen at 200% gets text twice the size. **Help > Settings > text size** makes it bigger or smaller from there. The first time, the editor opens maximised.

**Help > How to use** explains the tools. The tabs:

- **Map.** The tools are on the left (keys 1-9), the map is in the middle and the inspector is on the right. Drag the bar between the map and the inspector to make the inspector wider, up to a third of the window. The view preview grows with it. The map is a grid of 32 x 32 cells, the most the games support. Mouse wheel zooms and right-drag moves the map. Select a cell to see the view from it and to edit what's in it: walls, door, things, props and its script. Pick a console under the view to see it as that console shows it: the Master System and the Game Gear by their own renderer (exactly what they draw, also with the pause menu's low detail and solid walls); the others at their size, in their colours (the Mega Drive's 15 a view, the SNES family's 15 bits, the Game Boy Color's colour families per tile), with their floor and ceiling shading. Tick "doors open" to see through the doors.
- **Art.** A small paint program, described below.
- **Thing types.** What each kind of thing is: monster class, pickup amount, sprite, and so on.
- **Text & scripts.** All the texts (edit them freely) and every scripted cell. A new line in a text box is a new line in the game's dialog box: 18 letters fit on the Master System, 24 on the Mega Drive.
- **Limits.** What the map uses, against what the games allow, and whether it fits the Master System.
- **Build.** Save the project, make ROMs of it, or make one and play it (F5, on the console chosen there). The log shows how much room is left in each ROM.

**F5** makes a ROM and starts your emulator. Set the commands once in **Help > Settings**, for example `mednafen %f` or `blastem %f`.

**Undo/redo:** Ctrl+Z / Ctrl+Y (per map). **Save the project:** Ctrl+S.

### The Art tab

Every picture is 64 x 64 pixels with its own 16 colours. The pictures are listed on the left: wall textures, sprites (things and props), the Master System's border and the Mega Drive's weapon (each weapon twice, still and firing).

- **The whole picture** is shown in the middle, with its 8x8 tile grid. It fits the panel to start with. **Ctrl+mouse wheel** zooms in and out around the mouse (up to 48x), the wheel scrolls, and the middle button drags the picture around. The zoom buttons are also above it, and **Fit** brings the whole picture back. Draw with the left mouse button in the first colour and the right button in the second. The tools: pencil (P), line (L), box (B), filled box (Shift+B), fill (F) and pick colour (K). X swaps the two colours.
- **Tiles.** Ctrl+click a tile to pick it out: it appears large on the right, where you can draw in it, copy and paste it, clear it or flip it. The arrow keys move to the next tile. Shift+fill fills only inside the picked tile.
- **Colours.** The picture's 16 colours are on the right. Click one for the first colour, right-click for the second, and double-click to change it. The colour window also shows the Master System's 64 colours to pick from, and can snap a colour to the nearest one the Mega Drive can show.
- **Palettes.** As in the original game, several pictures can share one drawing with different colours (wall texture 0 is the same drawing as 9 and 10). The panel shows the drawing with each set of colours. Click one to try those colours, or pick any other picture's colours to see how they look. Painting changes the drawing for every picture that shares it. **Give this picture a drawing of its own** separates it first.
- **On the consoles** shows how each console will show the picture. On the Master System, walls and props use the level's 16 colours mixed in pairs. Things are hardware sprites with 15 colours of their own. The "show" menu above the picture shows the whole picture that way.
- **Import PNG / Export PNG** to use another paint program. An imported picture is reduced to 64 x 64 and 16 colours, and gets a drawing of its own.

## Settings

Settings are kept in `deimos-editor.ini` in the editor's own folder (beside the program), so each copy of the editor has its own and never picks up another version's. They cover the recent projects, window size, zoom, emulator commands, the preview and the text size. You can edit the file while the editor is closed.

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

## Licence

The editor is free software under the GNU General Public License, version 2 or later (`docs/GPL-2.0.txt`). It uses Dear ImGui, SDL2, nlohmann/json, stb and the DejaVu fonts; their licences are in `docs/`.
