# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

# The build machine's test programs (run by hand: make libcore.so view_host ...):
#   libcore.so    the portable core as a shared library, for lockstep_host.py
#   view_host     the 3D view as a program (the reference the consoles' views
#                 are compared with: tools/snes_view_test.py)
#   view_host_fx  the same with the SuperFX build's view (VIEW_FX)
#   view_host_snes, view_host_snes_fx: the two at the SNES builds' view
#                 size (SNES_VIEW, as snes/build.sh passes it)
SNES_VIEW = -DVIEW_W=104 -DVIEW_H=72
CFLAGS = -O2 -g -Wall -Wextra -std=c99 -fPIC
libcore.so: ../core/rules.c ../core/gen/data.c host.c ../core/rules.h ../core/types.h ../core/gen/state.h
	$(CC) $(CFLAGS) -shared -o $@ ../core/rules.c ../core/gen/data.c host.c
view_host: view_host.c ../view/view.c ../view/view.h ../core/rules.c ../core/gen/data.c
	$(CC) -O2 -Wall -std=c99 -o $@ view_host.c ../view/view.c ../core/rules.c ../core/gen/data.c
view_host_fx: view_host.c ../view/view.c ../view/view.h ../core/rules.c ../core/gen/data.c
	$(CC) -O2 -Wall -std=c99 -DVIEW_FX -o $@ view_host.c ../view/view.c ../core/rules.c ../core/gen/data.c
view_host_snes: view_host.c ../view/view.c ../view/view.h ../core/rules.c ../core/gen/data.c
	$(CC) -O2 -Wall -std=c99 $(SNES_VIEW) -o $@ view_host.c ../view/view.c ../core/rules.c ../core/gen/data.c
view_host_snes_fx: view_host.c ../view/view.c ../view/view.h ../core/rules.c ../core/gen/data.c
	$(CC) -O2 -Wall -std=c99 $(SNES_VIEW) -DVIEW_FX -o $@ view_host.c ../view/view.c ../core/rules.c ../core/gen/data.c
