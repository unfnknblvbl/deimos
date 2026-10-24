/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// Importing levels from the original game (DoomRPG.jar, the J2ME phone
// game, supplied by the user): the maps (.bsp + .str), the entity types
// (entities.db/.str) and the art. A port of the ports' tools/bsp_parser.py,
// tools/gamedata.py and the art's extractors: every map comes out exactly
// as the ports' own builds make it.
// Licence: GPL-2.0-or-later.
//
// Called by engine.cpp (new ROMs from the jar), the editor's "Import a
// level from the original game", and the command-line tool. The Jar class
// unzips the whole jar into memory; import_map and import_art turn its
// files into a Project.
#pragma once
#include "project.h"

#include <map>
#include <string>
#include <vector>

// the original game's files, unpacked
class Jar {
public:
    // reads the zip at path; false with err when it can't be read or lacks
    // the game's data files
    bool open(const std::string &path, std::string &err);
    // a file's unpacked bytes, or nullptr when the jar has no such file
    const Bytes *get(const std::string &name) const;
    // the maps in the jar (level01, junction, ...), and a readable name for each
    std::vector<std::string> maps() const;
    static std::string map_title(const std::string &map);    // for lists: "Junction (destroyed)"
    static std::string short_title(const std::string &map);  // as the games show it: "Junction"

private:
    std::map<std::string, Bytes> files_;   // file name in the zip -> its contents
};

// notes: what didn't come across (script commands the consoles don't run, ...)
// campaign: the Mega Drive campaign's form (thin invisible walls, the props'
// cells, the Pain Elementals' lost souls, the original map id); otherwise
// the Master System's (invisible walls block their whole cell)
// Converts map (a name from maps(), such as "level01") into p: the grid,
// walls, props, things, doors, scripts, texts and art. p is replaced only
// on success; false with err otherwise.
bool import_map(const Jar &jar, const std::string &map, Project &p, std::string &err,
                std::vector<std::string> *notes = nullptr, bool campaign = false);
// the jar's art only (the wall textures, sprites, border and weapon of a project)
// Replaces p's textures, sprites, border, weapons and shapes with the
// jar's pictures for the wall textures and sprites p's walls, props and
// thing types use.
bool import_art(const Jar &jar, Project &p, std::string &err);
