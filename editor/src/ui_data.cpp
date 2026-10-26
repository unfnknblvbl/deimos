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
//   draw_build_tab   Build: saving the project, making ROMs, the progress and log;
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
    case 1: return "class: 0 zombie, 1 hellhound, 2 troop, 3 imp, 4 phantom, 5 pinky, 6 cacodemon, 7 beholder, "
                   "8 ghoul, 9 behemoth, 10 archvile, 11 ogre, 12 cyberdemon, 13 kronos. Strength scale: 294 weak, 467, 640 strong.";
    case 3: return "20 health (+amount), 21 armour (+amount), 22/23 credits (+amount), 24 key card (0 green 1 yellow 2 blue 3 red)";
    case 4: return "25 small medkit, 26 large medkit, 27 soulsphere, 28-29 other items";
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
        for (int c = 0; c < N_CONSOLES; c++)
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

// The Build tab: saving the project and making ROMs of it (or, during a
// build, its progress and a Cancel button), the console F5 plays on, the
// thread count, and the build log.
void App::draw_build_tab()
{
    para("The project (a %s file) holds every map you edit. Making a ROM builds everything that depends on the "
         "maps (the renderer's data, sprites, texts) into the console's game code, which stays exactly as it "
         "is; the graphics and sounds come from your DoomRPG.jar. The ROMs go to the export folder. A ROM "
         "carries no copy of the project: keep the project to change the maps again.", PROJECT_EXT);
    if (building) {
        ImGui::ProgressBar(progress, ImVec2(-1, 0 * g_ui));   // full width, default height
        if (ImGui::Button("Cancel")) cancel = true;           // the builder checks it and stops
    } else {
        // the same actions as the File menu (app.cpp draw_menu)
        if (ImGui::Button("Save project")) save_or_ask();
        ImGui::SameLine();
        if (ImGui::Button("Save project as...")) save_as();
        ImGui::SameLine();
        if (ImGui::Button("Make ROMs...")) ask_new_rom(NR_PROJECT_ROMS);
        ImGui::SameLine();
        if (ImGui::Button("Make a ROM and play (F5)")) make_and_play();
    }
    // F5's console (saved with the settings)
    ImGui::SetNextItemWidth(260 * g_ui);
    int pc = std::max(0, std::min((int)NEWROM_CONSOLES.size() - 1, set.play_console));
    if (ImGui::BeginCombo("F5 plays on", NEWROM_CONSOLES[pc].label)) {
        for (size_t k = 0; k < NEWROM_CONSOLES.size(); k++)
            if (ImGui::Selectable(NEWROM_CONSOLES[k].label, (int)k == pc)) set.play_console = (int)k;
        ImGui::EndCombo();
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
// red over it or unknown) for a map of the whole game (every console but
// the Master System and Game Gear), or for a single level (the Master
// System's), and whether the map fits the Master System. Recounted every
// frame.
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
    bool camp = proj.campaign();
    try {
        level_bytes = camp ? (int)make_level_blob_md(proj, {}, 0).size() : (int)make_level_blob(proj).size();
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
    // a single level: the Master System's limits, and the Mega Drive
    // renderer's that every map must keep (md::R_MAX_*; "both": the two)
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
    // a map of the whole game: the Mega Drive game's own (larger) limits, which the other consoles share
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
        para("This is one map of the whole game (%s). The Mega Drive game, and every console but the Master "
             "System and Game Gear, allows more on a map than the Master System one.", map_label(cur_map).c_str());
    }
    // the Master System and Game Gear take the map as a single level
    std::string sms = check_limits(proj.single_level());
    if (sms.empty()) ImGui::TextColored(ImVec4(0.6f, 1, 0.6f, 1), "It fits the Master System and the Game Gear.");
    else ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Too big for the Master System and the Game Gear: %s", sms.c_str());
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
    para("Pictures: 64 x 64 pixels, 16 colours each (sprites: 15 + transparent). On the Mega Drive a "
                       "wall texture may have at most 32 colours with its decals. On the Master System at most 96 "
                       "bytes of walls and props can be visible from one spot; the build says where if a view has "
                       "too much. The ROM grows as needed: up to 1 MB on the Master System, 4 MB on the Mega Drive "
                       "for all the maps together.");
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
            "PROJECTS\n"
            "You edit a project (a .deimos file): File > New project from the original game makes one of the "
            "maps you tick, the whole game or a single level. The Map menu lists its maps: pick the one to edit. "
            "File > Save project keeps all of them.\n\n"
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
            "MAKING ROMS\n"
            "File > Make ROMs of this project makes a ROM for every console you tick, in the export folder: every "
            "map, in order (the Master System and Game Gear: the map you are editing). F5 makes one for the "
            "console chosen in the Build tab and starts your emulator (set it in Help > Settings). The ROMs carry "
            "no copy of the project: keep the project.\n\n"
            "THE ORIGINAL GAME\n"
            "File > Import a level from the original game reads your copy of DoomRPG.jar, lists its maps and "
            "converts the one you pick (map, things, scripts, texts and art) in place of the map you are "
            "editing.\n\n"
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
        // edit buffers for the emulator commands, one per console, filled
        // from the settings when the window opens or a Browse... picks a
        // program (reload; init is cleared again on Close)
        static char emu[Settings::N_EMULATORS][512];
        static bool init = false, reload = false;
        if (!init || reload) {
            for (int c = 0; c < Settings::N_EMULATORS; c++)
                snprintf(emu[c], sizeof(emu[c]), "%s", set.emulators[c].c_str());
            init = true;
            reload = false;
        }
        ImGui::Text("Emulator commands (%%f = the ROM file), e.g.  mednafen %%f   or   blastem %%f");
        hint("Browse... picks the emulator's program; add any options it needs after it.");
        for (int c = 0; c < Settings::N_EMULATORS; c++) {
            ImGui::PushID(c);
            ImGui::SetNextItemWidth(460 * g_ui);
            if (ImGui::InputText("##emu", emu[c], sizeof(emu[c]))) set.emulators[c] = emu[c];
            ImGui::SameLine();
            if (ImGui::Button("Browse...")) {
                const std::string name = console_name((Console)c);
                fd.open("The " + name + " emulator", FileDialog::OPEN, "", set.last_dir,
                        [this, c](const std::string &f) {
                            // the program, quoted (its path may have spaces), then the ROM
                            set.emulators[c] = "\"" + f + "\" %f";
                            reload = true;
                        });
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(console_name((Console)c));
            ImGui::PopID();
        }
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
        para("Master System, Game Gear, Mega Drive, SNES, SNES SuperFX, 3DS, 32X, Game Boy Color, Game Boy Advance, "
             "Virtual Boy. "
             "GPL-2.0-or-later. Uses Dear ImGui (MIT), SDL2 (zlib), nlohmann/json (MIT), stb (public domain) "
             "and the DejaVu fonts. An unofficial fan project, not made or endorsed by "
             "id Software, ZeniMax or Microsoft; it needs your own copy of Doom RPG.");
        if (ImGui::Button("Close")) {
            set.save();   // write the settings file now
            init = false;
            ImGui::CloseCurrentPopup();
        }
        fd.draw();   // (Browse...'s file dialog, over this window)
        ImGui::EndPopup();
    }
}
