/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// The source container (DRPGSRC2): the editable level stored inside a
// ROM, the same for both consoles (tools/source_container.py). The Mega
// Drive campaign keeps all its maps in one: campaign.json, map/<name>.json
// and the drawings they share.
// Licence: GPL-2.0-or-later.
//
// Layout (little-endian):
//   "DRPGSRC2"   magic, the last character the format version
//   u32          the container's total size, header included
//   u16          number of entries
//   per entry:   u8 name length, the name, u32 unpacked size, u32 stored
//                size, u8 method (1 = ZX0, else stored as it is), the data
// Entries: "project.json" (Project::to_json) and "shape/<key>" (each
// drawing in use, 4096 bytes, palette indices). rom.cpp finds the
// container through the ROM's directory and calls these.
#pragma once
#include "project.h"

#include <string>
#include <vector>

// one named file inside the container
struct SrcEntry {
    std::string name;
    Bytes data;      // unpacked
    Bytes packed;    // as read (ZX0); empty = pack when writing
};

// reads every entry of the container at data (size bytes available);
// false with err when it is missing, of another version or damaged
bool read_entries(const u8 *data, size_t size, std::vector<SrcEntry> &out, std::string &err);
// the container for entries, each packed with ZX0 (or as it was read)
Bytes write_entries(const std::vector<SrcEntry> &entries);
// a project from its JSON entry and the drawings it uses
bool project_from_entries(const std::vector<SrcEntry> &entries, const std::string &json_name, Project &p,
                          std::string &err);

// a single-level container -> its Project ("project.json" + the drawings)
bool read_container(const u8 *data, size_t size, Project &p, std::string &err);
// a Project -> a single-level container (project.json + the drawings in use)
Bytes make_container(const Project &p);
// A locked ROM's container: a single entry named "locked" in place of the
// level's editable copy (BuildOptions::locked), so the ROM is smaller and
// read_entries refuses it with an explanation.
Bytes locked_container();
