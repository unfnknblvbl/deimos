# What's where in the ROM (directory version 4)

The ROM is 1 MB (64 banks of 16 KB, Sega mapper). The game code never
changes between levels. Everything about the level is data, found at run
time through a **level header** in bank 3, so the editor (`../editor/`)
makes a ROM of any level without a compiler. The build tools (`build.sh`, Python)
and the editor write byte-for-byte the same ROM for the same level.

| Bank / address | What | Written by |
|---|---|---|
| 0 `$0000-$0FFF` | display engine (rom/main.asm) | assembler |
| 0 `$1000-$3FFF` | game logic: ui, core, events (game/*.asm) | assembler |
| 1 `$4000-$7FBF` | game logic: combat, actions (game/*.asm) | assembler |
| 1 `$7FC0` | **ROM directory** (below) | rom_dir.py / editor |
| 1 `$7FF0` | SMS header (checksum and size) | rom_size.py / editor |
| 2 `$A000-$BFFF` | **level blob**: the rules' view of the level (below) | gen_cdata.py / editor |
| 3 `$8000` | **level header** (64 bytes, `rom/live_layout.inc`), then the renderer's small tables | live_data.py / editor |
| 4 `$8000-$97FF` | **strings** | gen_cdata.py / editor |
| 4 `$9800-$AFFF` | status bar tiles, fonts | build_ui_gfx.py |
| 4 `$B000` | hardware sprite directory, small view (1 KB) | build_sprites.py / editor |
| 4 `$B800` | hardware sprite directory, large view (1 KB) + palettes (1 KB) | build_sprites.py / editor |
| 5 `$8000` | title screen | build_ui_gfx.py |
| 5 `$A000-$BFFF` | chunky-to-planar tables (row parity, plane, pixel pair) | live_data.py |
| 6 (slot 1) | the renderer (rom/render.asm) | assembler |
| 7, 8 | height tables, large and small view: 1792 x (y0, y1, v0, dv, depth) | live_data.py |
| 9.. | the level's big tables (each starts on a bank): visibility lists, texture columns, billboard sprites | live_data.py / editor |
| then | hardware sprites, large view, then small view | build_sprites.py / editor |

Banks 2 (`$A000`-), 3, 4 (strings and sprite directories) and everything
from bank 9 up belong to the level; the rest is the engine.

## Level header (bank 3, `$8000`)

Little-endian; bank numbers are 16 KB banks paged into slot 2 (`$8000`),
addresses are slot-2 addresses. The offsets are in `rom/live_layout.inc`
(generated from `H_FIELDS` in `tools/live_data.py`).

| field | size | |
|---|---|---|
| magic | 2 | `"LV"` |
| version | 1 | 1 |
| walls bank, addr | 1, 2 | wall pieces |
| pvs_index bank, addr | 1, 2 | visibility index |
| pvs bank | 1 | first bank of the visibility lists |
| tex bank | 1 | first bank of the texture columns |
| texcols bank, addr | 1, 2 | per texture, 64 column ids |
| scenery bank, addr | 1, 2 | props |
| bspr bank | 1 | first bank of the billboard sprites |
| bsprite_dir bank, addr | 1, 2 | billboard directory |
| border bank | 1 | the border's tiles and name tables (one bank) |
| btiles_large, btiles_small, bmap_large, bmap_small addr | 2 each | |
| palette bank, addr | 1, 2 | the level's 16 colours (CRAM bytes) |
| bill_key | 1 | the pair byte that means "transparent" in billboards |
| n_btiles_large, n_btiles_small | 1, 1 | border tiles |
| n_walls | 2 | |

## The renderer's tables

- **Walls**: 16 bytes per wall piece: `s16 ax, az, bx, bz` (world units,
  64 per cell), `u8 texture, line (255 = plain wall; else its door slot),
  ua, ub` (texture column at A and at B, one texel per unit), `u8
  two_sided`, 3 spare. A one-sided wall shows only its front:
  `(A - eye) x (B - A) > 0`.
- **Textures**: per texture 64 x u16 column ids; column id c is at bank
  `tex_bank + c / 256`, address `$8000 + (c % 256) * 64`. Each byte is a
  colour pair: high nibble where x + y is even, low nibble where odd. The
  level has one 16-colour palette (0 = ceiling, 1 = floor).
- **Visibility**: index of u16 per (cell * 4 + facing), 0 = none, else 1 +
  offset into the lists (bank `pvs_bank + offset / 16 KB`). A list: n,
  wall ids (u16), n, scenery ids (u8); at most 96 bytes; never across a
  bank.
- **Scenery** (props): `s16 x, z, u8 billboard sprite, pad`.
- **Billboards**: directory per sprite `u32 offset (from bspr_bank), u8
  first row, u8 end row, u16 0`. A sprite: 64 x u16 offsets (from its
  start) of each column's 64-byte block (one pair byte per canvas row,
  `bill_key` = transparent; 0 = empty column), then the blocks.
- **Border**: 14 tiles per view size, and the name table of screen rows
  2..17 (u16 entries; 0 inside the view).

## ROM directory (bank 1, `$7FC0`, 16 bytes)

```
"DEIMDIR1"  u8 version (4)
u8 9, u8 n            data banks 9 .. 9 + n - 1 (and bank 3)
u8 first sprite bank, u8 number of sprite banks (both sizes)
3 zero bytes
```

The game doesn't read it; it marks the ROM as this port's and says where
the level's banks are. The ROM carries no copy of the level for the
editor: the editor keeps the level in its own project files (`.deimos`)
and makes the ROM from them.

## Level blob (bank 2, `$A000`, max 8 KB)

Header (16 bytes): `"LVL1"`, version 1, then counts: entity types, things on
the map, monsters, doors/lines, scripted cells, script commands, strings;
then spawn x, z, facing, 0. Then one u16 offset (from the blob start) per
table, in this order, then the tables:

| table | size | contents |
|---|---|---|
| def_etype, def_esub | types | kind and subtype of each entity type |
| def_parm | 4 x types | s32 "amount" |
| def_name | 2 x types + text | u16 offsets (from the table start), NUL-terminated upper-case names |
| ent_def0, ent_x0, ent_z0, ent_hidden0 | things | each thing's type, cell, hidden flag |
| ent_mon | things | monster slot (0..55) or 255 |
| line_x, line_z, line_etype, line_locked0 | lines | door cell, kind (0 door, 9 wall, 12 jammed), locked at start |
| ev_x, ev_z, ev_state0, ev_flags, ev_cmd0, ev_ncmd | events | scripted cell, start state, flags, first command, command count |
| ev_at | 1024 | event index per cell, 255 = none |
| cmd_op | commands | opcode |
| cmd_a1, cmd_a2 | 4 x commands | u32 argument, u32 "when" flags |
| wall_bits | 128 | 1 bit per cell: solid |
| static_block | 1024 | blocking prop type per cell (0 = none) |
| line_cells | 128 | 1 bit per cell: has a door/line |
| drop_def | 16 | entity type for each kind of drop (DD_* in game/gen/defs.inc), 255 = none |

Limits: 64 types, 152 things, 56 monsters, 32 lines, 96 scripted cells,
240 commands, 255 strings.

## Strings (bank 4, `$8000`, max 6 KB)

u16 offset per string, then NUL-terminated text; `|` is a line break.

## Hardware sprites (bank 4, `$B000` / `$B800`)

64 types x 4 distances x {u8 bank, u16 address, u8 0}; after the large
view's directory, 64 x 16 CRAM bytes. Records: u8 columns, rows, x, y, 4
bytes pad, then the 8x16 sprite patterns column by column.
