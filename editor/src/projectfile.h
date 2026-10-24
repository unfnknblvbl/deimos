/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// The project file (.deimos, "DEIMPRJ1"): what the editor opens, edits and
// saves, and makes ROMs from. It holds a whole campaign (every map, in the
// order they are played) or a single level (a campaign of one map), each
// map with every console's own pictures and texts. The ROMs carry no copy
// of it: keep the project to change a level again.
// Licence: GPL-2.0-or-later.
//
// Layout (little-endian):
//   "DEIMPRJ1"   magic, the last character the format version
//   u32          the file's total size, header included
//   u16          number of entries
//   per entry:   u8 name length, the name, u32 unpacked size, u32 stored
//                size, u8 method (2 = zlib's deflate, with its header; 0 =
//                stored as it is), the data
// Entries, in this order:
//   "campaign.json"     {"maps": [{"name", "title", "id"}, ...]}, play order
//   "map/<name>.json"   each map's level (Project::to_json)
//   "shape/<key>"       every drawing the maps use, once (4096 bytes of
//                       palette indices), sorted by key
// Two maps can share a drawing by its key. Saving gives a drawing that
// differs between two maps but has the same key a key of its own in the
// later map (<key>_<map name>, then a number if needed).
// The App (app.cpp) and the command-line tool (cli.cpp) read and write
// projects with these; engine.h makes ROMs from them.
#pragma once
#include "project.h"

#include <map>
#include <string>
#include <vector>

// the project files' extension
#define PROJECT_EXT ".deimos"

// one named entry of the file
struct SrcEntry {
    std::string name;
    Bytes data;      // unpacked
    Bytes packed;    // as stored (zlib); empty = pack when writing
};

// reads every entry of the file's bytes at data (size bytes available);
// false with err when they are not a project file, of another version or
// damaged
bool read_entries(const u8 *data, size_t size, std::vector<SrcEntry> &out, std::string &err);
// the file's bytes for entries, each packed with zlib (an entry's own
// packed bytes are used when it has them; those it gets are kept in it)
Bytes write_entries(std::vector<SrcEntry> &entries);
// a level from its JSON entry (json_name) and the drawings it uses; the
// drawings the other maps use are dropped
bool project_from_entries(const std::vector<SrcEntry> &entries, const std::string &json_name, Project &p,
                          std::string &err);

// one map of a project: how the games know it, and the level
struct ProjectMap {
    MapInfo info;
    Project level;
};
// a project: its maps, and the entries as last read or written (so an
// unchanged map or drawing is not compressed again when saving)
struct ProjectFile {
    std::vector<ProjectMap> maps;   // in the order they are played
    std::map<std::string, SrcEntry> stored;   // entry name -> its data and packed bytes
};

// a project from a file's bytes; false with err when it can't be read
bool read_project_file(const Bytes &file, ProjectFile &pf, std::string &err);
// The file's bytes for pf. Every map's unused drawings are dropped, and a
// drawing that two maps have under one key but drawn differently gets a
// new key in the later map (pf changes accordingly). False with err when
// two maps have the same name or there is no map.
bool write_project_file(ProjectFile &pf, Bytes &out, std::string &err);
// read_project_file / write_project_file with the file at path
bool load_project_file(const std::string &path, ProjectFile &pf, std::string &err);
bool save_project_file(const std::string &path, ProjectFile &pf, std::string &err);
