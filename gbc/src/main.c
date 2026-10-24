/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/* Doom RPG for the Game Boy Color: the start (bank 0). Licence: GPL-2.0-or-later.
 *
 * GBDK's start-up code calls main() after clearing RAM. main sets the
 * machine up and hands over to ui.c's ui_main, which never returns. */
#include <gb/gb.h>
#include <gb/cgb.h>
#include "plat.h"

void ui_main(void) BANKED;              /* (ui.c: the title, the game, forever) */

/* the entry point: double speed, the cartridge RAM on (bank 1: the level,
   which must be in place whenever the core runs), work RAM bank 1 at
   $D000, the resources found, the screen on, then the game */
void main(void)
{
    cpu_fast();                         /* the double speed: 8 MHz */
    ENABLE_RAM;                         /* (MBC5: cartridge RAM enabled, $0A to $0000-$1FFF) */
    SWITCH_RAM(1);                      /* cartridge RAM bank 1 at $A000: the level (plat.c) */
    SVBK_REG = 1;                       /* (SVBK, $FF70: work RAM bank 1 at $D000-$DFFF) */
    plat_init();                        /* find the resources' bank in ROM */
    DISPLAY_ON;                         /* LCDC bit 7: the LCD on */
    ui_main();                          /* the game (never returns) */
}
