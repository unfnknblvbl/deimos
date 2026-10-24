// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Editor: thing types, text & scripts list, build, help/settings.
// Licence: GPL-2.0-or-later.
//
// App member functions drawing four of the six tabs and two windows, called
// from App::frame() (app.cpp):
//   draw_types_tab   the Thing types tab: the table of Project::defs;
//   draw_text_tab    Text & scripts: the texts (Project::strings) and the
//                    list of scripted cells (Project::events);
//   draw_build_tab   Build: save buttons, the build's progress and log;
//   draw_limits      Limits: what the level uses against each game's limits;
//   draw_help        the "How to use" and "Settings" windows.
// Texts: in a Project a '|' in a string is a line break in the game's dialog
// box; the editor shows it as a real new line.
#include "app.h"
#include "edit.h"
#include "keytext.h"
#include "levelblob.h"
#include "md_build.h"
#include "scripts.h"

#include "../third_party/imgui/imgui.h"
#include <SDL.h>

#include <algorithm>
#include <cstring>

// the names of the original game's thing kinds (EntityDef::eType 0..16);
// 8 and 9 have no name here ("-")
static const char *ETYPES[] = {"line", "monster", "person", "pickup", "inventory item", "weapon", "ammo", "lamp/furniture",
                               "-", "-", "fire", "big fire", "barrel/crate", "table/chair", "blocker", "blocker", "ammo pack"};

// The tooltip explaining eSub and parm ("amount") for a kind of thing, ""
// for kinds without one.
static const char *subtype_help(int et)
{
    switch (et) {
    case 1: return "class: 0 zombie, 1 hellhound, 2 troop, 3 imp, 4 phantom, 5 bull demon, 6 cacodemon, 7 beholder, "
                   "8 ghoul, 9 behemoth, 10 archvile, 11 ogre, 12 cyberdemon, 13 kronos. Strength scale: 294 weak, 467, 640 strong.";
    case 3: return "20 health (+amount), 21 armour (+amount), 22/23 credits (+amount), 24 key card (0 green 1 yellow 2 blue 3 red)";
    case 4: return "25 small medkit, 26 large medkit, 27 soul sphere, 28-29 other items";
    case 5: return "weapon number: 0 axe 1 extinguisher 2 pistol 3 shotgun 4 chaingun 5 super shotgun 6 plasma 7 rockets 8 BFG; amount = ammo";
    case 6: case 16: return "ammo kind: 0 halon 1 bullets 2 shells 3 rockets 4 cells; amount = how many";
    case 12: return "1 barrel (explodes), 2 crate (random loot), 3/4 jammed doors; amount = which weapons can break it (bit mask)";
    default: return "";
    }
}

// The Thing types tab: one table row per EntityDef, every field editable:
// number, sprite id (with its picture), name, kind (eType), subtype (eSub),
// amount (parm), and copy / delete buttons.
// (Typing in the sprite, name, subtype and amount boxes calls changed()
// without push_undo, so those edits are not undoable on their own; the kind
// menu, copy and delete are.)
void App::draw_types_tab()
{
    para("Each thing on the map is one of these types. The type decides what it does (monster, pickup...), "
                       "its numbers, and its sprite. Changing a type changes every thing of that type.");
    float h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("types", ImVec2(0, h), true);
    int del = -1;   // the type to delete, done after the table (not while looping over it)
    if (ImGui::BeginTable("t", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30 * g_ui);
        ImGui::TableSetupColumn("sprite", ImGuiTableColumnFlags_WidthFixed, 110 * g_ui);
        ImGui::TableSetupColumn("name");
        ImGui::TableSetupColumn("kind", ImGuiTableColumnFlags_WidthFixed, 150 * g_ui);
        ImGui::TableSetupColumn("subtype", ImGuiTableColumnFlags_WidthFixed, 90 * g_ui);
        ImGui::TableSetupColumn("amount", ImGuiTableColumnFlags_WidthFixed, 90 * g_ui);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 110 * g_ui);
        ImGui::TableHeadersRow();
        for (int d = 0; d < (int)proj.defs.size(); d++) {
            EntityDef &e = proj.defs[d];
            ImGui::PushID(d);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d", d);
            ImGui::TableNextColumn();
            SDL_Texture *t = art_img(true, e.tile);
            if (t) ImGui::Image((ImTextureID)t, ImVec2(32 * g_ui, 32 * g_ui));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60 * g_ui);
            if (ImGui::InputInt("##tile", &e.tile, 0)) changed();
            ImGui::TableNextColumn();
            char nm[64];
            snprintf(nm, sizeof(nm), "%s", e.name.c_str());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##name", nm, sizeof(nm))) {
                e.name = nm;
                changed();
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            // the kind menu offers only the kinds a thing on the map can be
            // (not 0 line, 7 furniture, 13 table, 14/15 blockers: those are
            // props and doors in the editor's level)
            if (ImGui::BeginCombo("##et", e.eType >= 0 && e.eType <= 16 ? ETYPES[e.eType] : "?")) {
                for (int k : {1, 2, 3, 4, 5, 6, 16, 10, 11, 12})
                    if (ImGui::Selectable(ETYPES[k], e.eType == k)) {
                        push_undo();
                        e.eType = k;
                        changed();
                    }
                ImGui::EndCombo();
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##sub", &e.eSub, 0)) changed();
            if (ImGui::IsItemHovered() && *subtype_help(e.eType)) ImGui::SetTooltip("%s", subtype_help(e.eType));
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##parm", &e.parm, 0)) changed();
            if (ImGui::IsItemHovered() && *subtype_help(e.eType)) ImGui::SetTooltip("%s", subtype_help(e.eType));
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("copy")) {
                // a new type at the end, named "<name> 2"
                // (push_back may move the vector, so e must not be used after it)
                push_undo();
                proj.defs.push_back(e);
                proj.defs.back().name += " 2";
                changed();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("delete")) del = d;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (del >= 0) {
        push_undo();
        edit::delete_def(proj, del);   // its things go too; later types renumbered
        changed();
        status = "Type deleted (and every thing of that type)";
    }
    ImGui::EndChild();
}

// Text & scripts: the texts on the left (list, then an editor for the one
// selected), the scripted cells on the right (click one to select that cell
// and go to the Map tab).
void App::draw_text_tab()
{
    float h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();
    float w = ImGui::GetContentRegionAvail().x * 0.5f;   // half the width each
    ImGui::BeginChild("strings", ImVec2(w, h), true);
    para("Texts used by scripts (dialogs, notes, door codes). A new line in the box becomes a new line "
                       "in the game. Dialog lines hold 18 letters on the Master System and 24 on the Mega Drive. "
                       "A button's name in braces, {USE}, {PASS TURN}, {MEDKIT}, {PREV WEAPON}, {NEXT WEAPON}, "
                       "{NOTEBOOK} or {PAUSE MENU}, is shown in the game as the button that does it on that "
                       "console (as the player has set them); a line with one is re-wrapped to fit when the ROM "
                       "is made. (The original's \"the * and 7 keys\" and the like are shown this way too.)");
    static int sel = 0;   // the text selected (kept between frames; checked against the count below)
    // "Edit for" (the same setting as the Art tab's): every console, or one
    // console's own versions of the texts (Project::per_console)
    ImGui::SetNextItemWidth(220 * ui);
    if (ImGui::BeginCombo("Edit for", art.console < 0 ? "Every console (shared)" : console_name((Console)art.console))) {
        if (ImGui::Selectable("Every console (shared)", art.console < 0)) art.console = -1;
        for (int c = 0; c < 9; c++)
            if (ImGui::Selectable(console_name((Console)c), art.console == c)) art.console = c;
        ImGui::EndCombo();
    }
    std::string ckey = art.console >= 0 ? CONSOLE_KEYS[art.console] : "";
    // text i as the chosen console has it (its own version, else the shared one), and whether it is its own
    auto text_of = [&](int i, bool *own) -> std::string {
        if (own) *own = false;
        if (art.console >= 0) {
            auto pc = proj.per_console.find(ckey);
            if (pc != proj.per_console.end()) {
                auto it = pc->second.strings.find(i);
                if (it != pc->second.strings.end()) {
                    if (own) *own = true;
                    return it->second;
                }
            }
        }
        return proj.strings[i];
    };
    // each text on one line: its number and its first 50 letters ('|' shown
    // as a space, the buttons as {NAME}: keytext::key_markup); a console's
    // own version is marked with *
    for (int i = 0; i < (int)proj.strings.size(); i++) {
        bool own;
        std::string s = keytext::key_markup(text_of(i, &own));
        if (own) s = "* " + s;
        for (auto &c : s)
            if (c == '|') c = ' ';
        if (s.size() > 50) s = s.substr(0, 50) + "...";
        if (ImGui::Selectable((std::to_string(i) + ": " + s).c_str(), sel == i)) sel = i;
    }
    if (ImGui::Button("+ new text")) {   // added at the end, so no script's text number changes
        push_undo();
        proj.strings.push_back("New text");
        sel = (int)proj.strings.size() - 1;
        changed();
    }
    if (sel >= 0 && sel < (int)proj.strings.size()) {
        ImGui::Separator();
        // the selected text in a box, '|' shown as a new line and the
        // buttons (the phone's keys the original names, and the tokens) as
        // {NAME} (keytext::key_markup)
        bool own;
        std::string s = keytext::key_markup(text_of(sel, &own));
        for (auto &c : s)
            if (c == '|') c = '\n';
        // filled from the text every frame (ImGui keeps its own copy while
        // the box is being typed in); texts longer than 2047 bytes are cut
        static char buf[2048];
        snprintf(buf, sizeof(buf), "%s", s.c_str());
        if (art.console < 0) ImGui::Text("Text %d (every console):", sel);
        else if (own) {
            ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "Text %d: the %s's own version", sel, console_name((Console)art.console));
            ImGui::SameLine();
            if (ImGui::SmallButton("Use the shared text again")) {
                push_undo();
                proj.per_console[ckey].strings.erase(sel);
                if (proj.per_console[ckey].empty()) proj.per_console.erase(ckey);
                changed();
            }
        } else
            ImGui::Text("Text %d: typing gives the %s its own version", sel, console_name((Console)art.console));
        // typing: store it back with new lines as '|' and each {NAME} as
        // its button's token (keytext::key_unmarkup)
        // (no push_undo: typing is not undoable on its own)
        if (ImGui::InputTextMultiline("##txt", buf, sizeof(buf), ImVec2(-1, 220 * g_ui))) {
            std::string ns = buf;
            for (auto &c : ns)
                if (c == '\n') c = '|';
            ns = keytext::key_unmarkup(ns);
            if (art.console < 0) proj.strings[sel] = ns;
            else proj.per_console[ckey].strings[sel] = ns;   // (this console's own version)
            changed();
        }
        // the longest line without a button (those are re-wrapped when the
        // ROM is made), in letters, against the dialog box widths (18 / 24)
        int longest = keytext::widest_line(keytext::key_unmarkup(buf));
        if (longest > 18)
            ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "a line has %d letters: %s", longest,
                               longest > 24 ? "too long for both consoles" : "too long for the Master System (18 fit)");
        // deleting: only the last text, and only if no script uses it.
        // The commands that take a text number in a1: 4 MESSAGE, 8 DIALOG,
        // 10 PASSWORD, 24 FORCEMESSAGE, 26 DIALOGNOBACK, 40 NOTE.
        if (sel < (int)proj.strings.size() && ImGui::Button("delete this text")) {
            bool used = false;
            for (auto &e : proj.events)
                for (auto &c : e.cmds) used |= (c.op == 4 || c.op == 8 || c.op == 10 || c.op == 24 || c.op == 26 || c.op == 40) && (int)c.a1 == sel;
            if (used) status = "Text " + std::to_string(sel) + " is used by a script; change the script first";
            else if (sel != (int)proj.strings.size() - 1) status = "Only the last text can be deleted (scripts refer to texts by number)";
            else {
                push_undo();
                proj.strings.pop_back();
                sel--;
                changed();
            }
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("scripts", ImVec2(0, h), true);
    para("Every cell with a script. Click one to select it on the map.");
    // each: "x,z: " and its first two commands in words (describe_cmd, scripts.h)
    for (int i = 0; i < (int)proj.events.size(); i++) {
        auto &e = proj.events[i];
        std::string head = std::to_string(e.x) + "," + std::to_string(e.z) + ": ";
        for (size_t k = 0; k < e.cmds.size() && k < 2; k++) head += (k ? "; " : "") + describe_cmd(proj, e.cmds[k]);
        if (e.cmds.size() > 2) head += "; ...";
        // "##i" keeps the ImGui ids unique when two lines read the same
        if (ImGui::Selectable((head + "##" + std::to_string(i)).c_str(), sel_x == e.x && sel_z == e.z)) {
            sel_x = e.x;
            sel_z = e.z;
            preview_stale = true;
            requested_tab = 0;   // go to the Map tab, where the inspector shows the script
        }
    }
    ImGui::EndChild();
}

// The Build tab: the save buttons (or, during a build, its progress and a
// Cancel button), the thread count, and the build log.
void App::draw_build_tab()
{
    para("Saving writes a new game ROM image: the editor rebuilds everything that depends on the level (the "
         "renderer's data, sprites, texts, and the copy of the level the editor reads back) and keeps the "
         "game code exactly as it is. It takes about ten seconds.");
    if (building) {
        ImGui::ProgressBar(progress, ImVec2(-1, 0 * g_ui));   // full width, default height
        if (ImGui::Button("Cancel")) cancel = true;           // the builder checks it and stops
    } else {
        // the same actions as the File menu (app.cpp draw_menu)
        if (ImGui::Button("Save ROM")) start_build(rom.path, false);
        ImGui::SameLine();
        if (ImGui::Button("Save ROM as...")) {
            fd.open("Save ROM as", FileDialog::SAVE, rom_ext(), export_start(),
                    [this](const std::string &f) { start_build(f, false); }, "my_level" + rom_ext());
        }
        ImGui::SameLine();
        if (ImGui::Button("Save and play (F5)")) start_build(rom.path, true);
        ImGui::SameLine();
        // (unlike the File menu's version, this one does not ask which map a
        // whole-game ROM should lose: the ROM's selected map, map 0 after
        // loading, is replaced)
        if (ImGui::Button("Put this level into the other console's ROM...")) {
            fd.open("Pick the other console's ROM (it is not changed)", FileDialog::OPEN, ROM_EXTS,
                    set.last_dir, [this](const std::string &f) {
                        auto other = std::make_shared<Rom>();
                        std::string err;
                        if (!other->load(f, err)) {
                            status = err;
                            return;
                        }
                        std::string ext = console_ext(other->console);
                        fd.open("Save the new ROM as", FileDialog::SAVE, ext, export_start(),
                                [this, other](const std::string &out) { start_build(out, false, other.get()); },
                                "my_level" + ext);
                    });
        }
    }
    // the thread count for builds (saved with the settings; 0 = one per core)
    ImGui::SetNextItemWidth(100 * g_ui);
    ImGui::InputInt("CPU threads (0 = all)", &set.build_threads);
    ImGui::Separator();
    // the log, at reading width, filling the rest of the tab
    ImGui::BeginChild("log", ImVec2(std::min(ImGui::GetContentRegionAvail().x, reading_width()),
                                    ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing()), true);
    {
        std::lock_guard<std::mutex> l(log_mu);   // the build thread may be adding lines
        for (auto &s : log) para("%s", s.c_str());
    }
    if (building) ImGui::SetScrollHereY(1.0f);   // keep the newest line in view
    ImGui::EndChild();
}

// The Limits tab: a table of counts (green; orange above 90% of the limit;
// red over it or unknown) for the Master System and single-level Mega Drive
// ROMs, or for a whole-game ROM's map. Recounted every frame.
void App::draw_limits()
{
    para("What the level uses, against what the games allow. The map is a grid of 32 x 32 cells "
                       "(x and z from 0 to 31); the outer cells should stay solid. Walls run along cell edges or at "
                       "45 degrees. The texts can be edited freely within these limits.");
    // monsters (things of kind 1), script commands, and the texts' size:
    // 2 bytes each (taken here as each text's offset) plus its bytes and a terminating 0
    int n_mon = 0, n_cmd = 0, text_bytes = 2 * (int)proj.strings.size();
    for (auto &e : proj.entities)
        if (e.def >= 0 && e.def < (int)proj.defs.size() && proj.defs[e.def].eType == 1) n_mon++;
    for (auto &e : proj.events) n_cmd += (int)e.cmds.size();
    for (auto &t : proj.strings) text_bytes += (int)t.size() + 1;
    // the size of the level blob the builder would make (levelblob.h): the
    // campaign's format or the single-level one; -1 if it can't be made
    int level_bytes = 0;
    bool camp = rom.is_campaign();
    try {
        level_bytes = camp ? (int)make_level_blob_md(proj, {}, 0).size() : (int)make_level_blob(proj, false).size();
    } catch (...) {
        level_bytes = -1;
    }
    int open = 0;   // floor cells
    for (u8 w : proj.wall) open += !w;
    // wall texture images after decals are baked in (from geo; -1 if the geometry failed)
    int tex_imgs = geo ? (int)geo->tex_imgs.size() : -1;
    // different sprites: thing types' and props' / decals' left as sprites
    std::set<int> tiles;
    for (auto &d : proj.defs) tiles.insert(d.tile);
    if (geo)
        for (auto &sc : geo->scenery) tiles.insert(sc.tile);
    int props = 0;   // props, not counting wall decals
    if (geo)
        for (auto &sc : geo->scenery) props += !(sc.info & 0x780000);
    // one table row: what, how many, the limit, which game it is for
    struct Row {
        const char *what;
        int n, max;
        const char *who;
    };
    // the Master System and the Mega Drive game's single-level ROMs (the rows marked
    // "both" hold for both; md::R_MAX_* are the Mega Drive renderer's limits)
    Row rows[] = {{"thing types", (int)proj.defs.size(), 64, "both"},
                  {"things on the map", (int)proj.entities.size(), 192, "Master System"},
                  {"monsters", n_mon, 56, "both"},
                  {"doors and sliding walls", (int)proj.lines.size(), 40, "Master System"},
                  {"cells with a script", (int)proj.events.size(), 96, "both"},
                  {"script commands", n_cmd, 320, "Master System"},
                  {"texts", (int)proj.strings.size(), 255, "both"},
                  {"bytes of text", text_bytes, 8192, "Master System"},
                  {"bytes of level data", level_bytes, 8192, "both"},
                  {"open cells (each gets its views)", open, 1024, "both"},
                  {"wall textures (each decal makes one more)", tex_imgs, 255, "Master System"},
                  {"props", props, 255, "Master System"},
                  {"wall textures (each decal makes one more)", tex_imgs, md::R_MAX_TEX, "Mega Drive"},
                  {"different sprites (things + props)", (int)tiles.size(), md::R_MAX_SPR, "Mega Drive"}};
    // a map of the Mega Drive campaign: that game's own (larger) limits
    Row camp_rows[] = {{"thing types", (int)proj.defs.size(), 64, "Mega Drive"},
                       {"things on the map", (int)proj.entities.size(), 280, "Mega Drive"},
                       {"monsters", n_mon, 100, "Mega Drive"},
                       {"doors and sliding walls", (int)proj.lines.size(), 64, "Mega Drive"},
                       {"cells with a script", (int)proj.events.size(), 160, "Mega Drive"},
                       {"script commands", n_cmd, 448, "Mega Drive"},
                       {"texts", (int)proj.strings.size(), 255, "Mega Drive"},
                       {"bytes of text", text_bytes, 16384, "Mega Drive"},
                       {"bytes of level data", level_bytes, 32768, "Mega Drive"},
                       {"open cells (each gets its views)", open, 1024, "Mega Drive"},
                       {"wall textures (each decal makes one more)", tex_imgs, md::R_MAX_TEX, "Mega Drive"},
                       {"different sprites (things + props)", (int)tiles.size(), md::R_MAX_SPR, "Mega Drive"}};
    if (camp) {
        ImGui::Spacing();
        para("This is one map of the whole game (%s). The Mega Drive game allows more on a map than the Master "
             "System one, so this map may not fit the Master System.", map_label(rom.map).c_str());
    }
    float tw = std::min(ImGui::GetContentRegionAvail().x, reading_width());   // the table's width
    if (ImGui::BeginTable("lim", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit,
                          ImVec2(tw, 0))) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("used");
        ImGui::TableSetupColumn("allowed");
        ImGui::TableSetupColumn("for");
        ImGui::TableHeadersRow();
        int n_rows = camp ? (int)(sizeof camp_rows / sizeof camp_rows[0]) : (int)(sizeof rows / sizeof rows[0]);
        for (int ri = 0; ri < n_rows; ri++) {
            const Row &r = camp ? camp_rows[ri] : rows[ri];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", r.what);
            ImGui::TableNextColumn();
            // red: over the limit or unknown; orange: over 90% of it; else green
            ImVec4 c = r.n < 0 || r.n > r.max ? ImVec4(1, 0.35f, 0.3f, 1)
                       : r.n > r.max * 9 / 10 ? ImVec4(1, 0.8f, 0.3f, 1)
                                              : ImVec4(0.6f, 1, 0.6f, 1);
            if (r.n < 0) ImGui::TextColored(c, "?");
            else ImGui::TextColored(c, "%d", r.n);
            ImGui::TableNextColumn();
            ImGui::Text("%d", r.max);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.who);
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    // (note: the ROM sizes in this text, 4 MB Master System and 2 MB Mega
    // Drive, differ from the README's: 1 MB, and 2 MB / 4 MB for a whole-game ROM)
    para("Pictures: 64 x 64 pixels, 16 colours each (sprites: 15 + transparent). On the Mega Drive a "
                       "wall texture may have at most 32 colours with its decals. On the Master System at most 96 "
                       "bytes of walls and props can be visible from one spot; the build says where if a view has "
                       "too much. The ROM grows as needed: up to 4 MB on the Master System, 2 MB on the Mega Drive.");
    if (!preview_err.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "Problem: %s", preview_err.c_str());
}

// Two modal windows, opened from the Help menu: "How to use" (fixed text)
// and "Settings" (emulator commands, text size, the settings file's place,
// and the about text). Each draws nothing unless open.
void App::draw_help()
{
    if (show_help) {
        ImGui::OpenPopup("How to use");
        show_help = false;
    }
    ImGui::SetNextWindowSize(ImVec2(640 * g_ui, 520 * g_ui), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("How to use", nullptr)) {
        para(
            "WHOLE-GAME ROMS\n"
            "Mega Drive version 0.0.6 and later hold all ten maps. The Map menu lists them: pick the one to edit. "
            "Saving writes that map back and keeps the others.\n\n"
            "MAP\n"
            "The level is a 32x32 grid. Dark cells are solid, light cells are floor. Use the tools on the left:\n"
            " - Wall / Floor: click or drag to fill or dig out cells. The walls around floor cells are made for you, "
            "with the texture chosen below the tools.\n"
            " - Paint wall: click near the edge of a cell to change that wall's texture.\n"
            " - Door: click a corridor cell (walls on both sides). The scripts that open and close it are added too.\n"
            " - Thing: monsters, items, people, barrels. Right-click removes one.\n"
            " - Prop: lamps, tables, decorations drawn into the pictures.\n"
            " - Start: where the player begins; click again to turn.\n"
            " - Script: give a cell commands - dialogs, door codes, unlocking doors, showing hidden monsters...\n"
            "Select a cell to see the view from it, as any of the consoles shows it (pick one under the view), "
            "and to edit what's in it.\n\n"
            "ART\n"
            "Every wall texture, sprite, the Master System's border and the Mega Drive's weapon is a 64x64 drawing "
            "with 16 colours of its own. Several pictures can share one drawing with different colours (as in the "
            "original game). The Art tab shows the whole picture with its 8x8 tiles; Ctrl+click picks out a tile to "
            "edit, copy or flip it. The right-hand panel shows the drawing with every set of colours that uses it, "
            "and how the Master System and the Mega Drive will show it. Double-click a colour to change it.\n\n"
            "TEXT\n"
            "Text & scripts lists every text; edit them freely. The | character (a new line in the box) breaks a line.\n\n"
            "SAVING\n"
            "File > Save ROM as... writes a new ROM. Keep the original ROM safe. F5 saves and starts your emulator "
            "(set it in Help > Settings). File > Put this level into the other console's ROM makes the Master "
            "System level play on the Mega Drive, or the other way round.\n\n"
            "THE ORIGINAL GAME\n"
            "File > Import a level from the original game reads your copy of DoomRPG.jar, lists its maps and "
            "converts the one you pick (map, things, scripts, texts and art).\n\n"
            "LIMITS\n"
            "The Limits tab shows what the level uses against what the games allow.");
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (show_about) {
        ImGui::OpenPopup("Settings");
        show_about = false;
    }
    if (ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        // edit buffers for the two emulator commands, filled from the
        // settings when the window opens (init is cleared again on Close)
        static char emu[512], emu_md[512];
        static bool init = false;
        if (!init) {
            snprintf(emu, sizeof(emu), "%s", set.emulator.c_str());
            snprintf(emu_md, sizeof(emu_md), "%s", set.emulator_md.c_str());
            init = true;
        }
        ImGui::Text("Emulator commands (%%f = the ROM file), e.g.  mednafen %%f   or   blastem %%f");
        ImGui::SetNextItemWidth(500 * g_ui);
        if (ImGui::InputText("Master System##emu", emu, sizeof(emu))) set.emulator = emu;
        ImGui::SetNextItemWidth(500 * g_ui);
        if (ImGui::InputText("Mega Drive##emumd", emu_md, sizeof(emu_md))) set.emulator_md = emu_md;
        // the text size: the font is remade (main.cpp setup_fonts) only once
        // the slider is let go, not while it is dragged
        ImGui::SetNextItemWidth(200 * g_ui);
        if (ImGui::SliderFloat("text size", &set.ui_scale, 0.5f, 3.0f, "%.2f x"))
            ;
        if (ImGui::IsItemDeactivatedAfterEdit()) fonts_dirty = true;
        hint("Your display asks for %.2f x; the text size multiplies that.", dpi_scale);
        ImGui::Text("Settings file: %s", set.file().c_str());
        ImGui::Separator();
        para("Deimos Editor %s", EDITOR_VERSION);
        para("Made by AI");
        para("Directed by unfnknblvbl");
        para("Master System, Game Gear, Mega Drive, SNES, SNES SuperFX, 3DS, 32X, Game Boy Color, Game Boy Advance. "
             "GPL-2.0-or-later. Uses Dear ImGui (MIT), SDL2 (zlib), nlohmann/json (MIT), stb (public domain), "
             "ZX0 by Einar Saukas (BSD-3) and the DejaVu fonts. An unofficial fan project, not made or endorsed by "
             "id Software, ZeniMax or Microsoft; it needs your own copy of Doom RPG.");
        if (ImGui::Button("Close")) {
            set.save();   // write the settings file now
            init = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
