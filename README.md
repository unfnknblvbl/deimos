# Deimos — Doom RPG on retro consoles

**Deimos** ports *Doom RPG* (id Software / Fountainhead, 2005, J2ME) to
nine retro consoles. It includes the **Deimos Editor**, which makes the ROMs
for all of them from the original game and lets you edit its maps.

To save cartridge space, the Master System, Game Gear, SNES, Super FX and 32X
ROMs are *locked*: the editor cannot open them. Edit an editable ROM (Mega
Drive, 3DS, Game Boy Color or Game Boy Advance) and make the locked ones from
it with **File > Make a ROM for another console** (`editor/README.md`).

Made by AI. Directed by unfnknblvbl.

| Folder | What it builds |
|---|---|
| `mastersystem/` | Sega Master System and Game Gear (`.sms`, `.gg`) |
| `megadrive/` | Mega Drive / Genesis (`.md`); its tools also make the maps' data every other console but the Master System reads |
| `snes/` | SNES (`.sfc`), and the SNES with the Super FX chip (`build_fx.sh`) |
| `32x/` | Sega 32X (`.32x`) |
| `3ds/` | Nintendo 3DS homebrew (`.3dsx`) |
| `gbc/` | Game Boy Color (`.gbc`) |
| `gba/` | Game Boy Advance (`.gba`) |
| `editor/` | The Deimos Editor for Linux and Windows |
| `core/`, `view/`, `tools/` | Shared by the SNES, Super FX, 32X, 3DS, GBC and GBA games: the game rules, the 3D view, the build tools |

## You need your own DoomRPG.jar

No part of the original game is included — not its maps, graphics, sounds,
music or text. Every build reads them from your own copy of the J2ME game,
`DoomRPG.jar`. Put it in `import/` (or `build.sh` asks for its path).

## Reading the source

Every source file starts with an overview of what it does and where it fits,
and every function and routine has a comment saying what it takes, what it
gives back and what it changes. Start with the README in each folder.

## Building

On Linux (or WSL on Windows):

```sh
./build.sh            # asks what to build
./build.sh editor     # just the editor: no DoomRPG.jar, no console compilers
./build.sh gba sms    # or name them: sms md snes snesfx 32x 3ds gbc gba editor engines all
```

The ROMs go to `roms/`. The editor goes to `editor/build/linux/` and/or
`editor/build/windows/` (it asks which), ready to zip up and pass on.

- **editor** builds the Deimos Editor from the engines in `editor/engines/`:
  each console's game code, with nothing of the original game in it. It
  needs only CMake and a C++ compiler — none of the console toolchains and
  no jar. The editor's users give it their own `DoomRPG.jar`, and it makes
  every console's ROM from it, the same bytes as the builds below.
- **engines** remakes those engines from this source, after a change to a
  game's code: every console is built from your jar, then everything that
  came from the jar is cut out of each ROM again. No ROMs are kept.
- **sms**, **md**, ... build a console's ROM into `roms/`.
- **all** builds every ROM, the engines, then the editor.
- The SNES, Super FX, 32X, 3DS, GBC and GBA games are built from parts of
  the Mega Drive and SNES builds, so choosing any of them builds those first
  (that needs the 68000 compiler and cc65 as well).

`build.sh` checks for every tool the chosen targets need first
(`tools/deps.sh`, used by every build script in the repository). It works out
your Linux distribution (Arch / CachyOS / Manjaro, Debian / Ubuntu, Fedora,
openSUSE), lists what is missing and, after one "yes", installs it:

- with your package manager (pacman, apt, dnf, zypper): Python's modules,
  compilers, CMake, cc65 where packaged, the ARM compiler, MinGW-w64, SDL2's
  headers;
- from source, into `~/.local/share/deimos-tools` (no root needed): WLA-DX,
  ZX0, cc65 where not packaged, GBDK-2020, and the 68000 and SH-2 compilers
  (binutils 2.43.1 + gcc 13.3.0, 10-30 minutes each, once);
- for the 3DS, devkitPro's package manager and its `3ds-dev` packages.

`DEIMOS_YES=1 ./build.sh all` installs without asking.

## Toolchains

What each build uses (installed for you as above; the "Used here" column is
what the reference ROMs were built with). All builds need Python 3 with
Pillow and numpy, `unzip`, a host C compiler and the `zx0` compressor.

| For | Tool | Used here | Where to get it |
|---|---|---|---|
| Master System, Game Gear | WLA-DX (`wla-z80`, `wlalink`) | 10.8 | github.com/vhelin/wla-dx |
| Mega Drive (and all the ports) | `m68k-elf-gcc` | GCC 13.3 | `megadrive/tools/build_toolchain.sh` builds it; or set `CROSS=` to another prefix |
| SNES (and all the ports) | cc65 (`cc65`, `ca65`, `ld65`); WLA-DX (`wla-spc700`, `wlalink`) | cc65 2.19 | cc65.github.io; set `CC65_LIB` if `none.lib` is not found |
| SNES Super FX | WLA-DX `wla-superfx` | 10.8 | as above |
| 32X | `m68k-elf-gcc` and `sh-elf-gcc` | GCC 13.3 | set `M68K=`/`SH2=` to their prefixes if not on PATH |
| 3DS | clang/devkitARM, libctru, citro3d, 3dstools, picasso | — | see `3ds/TOOLCHAIN.md`; set `DKP=` |
| Game Boy Color | GBDK-2020 | 4.5.0 | github.com/gbdk-2020/gbdk-2020; set `GBDK=` |
| Game Boy Advance | `arm-none-eabi-gcc` | GCC 13.3 | Arm GNU toolchain, or devkitARM; set `ARM=` to its prefix |
| The editor | CMake 3.16+, a C++17 compiler, git; MinGW-w64 for Windows | | the editor downloads and builds SDL2 itself |

Each folder's own `build.sh` can also be run on its own; their READMEs say
more about how each game works.

## Licence

Deimos is free software: you can redistribute it and/or modify it under the
terms of the **GNU General Public License, version 2 or (at your option) any
later version** (`SPDX-License-Identifier: GPL-2.0-or-later`). The full text
is in `LICENSE`, and every source file starts with this notice. Deimos comes
with NO WARRANTY.

If you pass on a built ROM or editor, the GPL asks you to make the matching
source available too (this repository, or a link to it).
The editor includes third-party code under its own licences (Dear ImGui, MIT;
nlohmann/json, MIT; stb, MIT / public domain; ZX0, BSD-3-Clause; DejaVu fonts;
SDL2, zlib) — see `editor/third_party/` and the licences the editor's build
puts in `docs/`. The 3DS game links devkitPro's libctru and citro3d (zlib)
and newlib's C library (BSD-style licences): their notices are in
`3ds/THIRD_PARTY.md`, which goes with the `.3dsx`. The Game Boy Color game
links GBDK-2020's library (GPL-2.0 with a linking exception).

## Disclaimer

This is an unofficial fan project, not affiliated with or endorsed by id
Software, ZeniMax/Bethesda, Microsoft, Fountainhead Entertainment, EA, Sega,
Nintendo or anyone else. *Doom* and *Doom RPG* are trademarks of their
owners. The original game's content belongs to its owners: you need your own
copy, and the ROMs you build from it are for your own use — please don't
distribute them.
