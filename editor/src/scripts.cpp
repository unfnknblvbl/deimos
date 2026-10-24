// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Script editor: Doom RPG's tile-event byte code, with plain-English labels.
// Command meanings follow tools/game_model.py exec_cmd (the rules the ROM runs).
// Licence: GPL-2.0-or-later.
//
// How a command's arguments are packed (as the game reads them):
//   a1 low byte "lo", next byte "hi", third byte "b2": a door slot, a text
//   id, a cell (lo = x, hi = z), an amount... depending on the op (see
//   describe_cmd); CHANGEMAP's bit 31 = "completes the sector".
//   a2: when the command runs. Bits 0..3 arriving while moving S, W, N, E;
//   4..7 leaving moving N, E, S, W; 8 the player uses the cell; 9 only
//   once; 16..24 only in script state 1..9 (none: state 0); 28..31
//   turning to face N, E, W, S.
#include "scripts.h"
#include "app.h"
#include "edit.h"
#include "keytext.h"

#include "../third_party/imgui/imgui.h"

#include <cstring>

namespace {
// a command the menu offers: its byte code, menu label and help text
struct OpInfo {
    int op;
    const char *label;
    const char *help;
};
// the commands the port runs (others are accepted but do nothing)
const OpInfo OPS[] = {
    {15, "open door", "Opens a door (if it isn't locked)."},
    {16, "close door", "Closes a door."},
    {6, "toggle door", "Opens a closed door / closes an open one."},
    {13, "unlock door", "Unlocks a door so it can be opened."},
    {12, "lock door", "Locks a door."},
    {14, "toggle lock", "Locks/unlocks a door."},
    {8, "dialog", "Shows a text box; the script carries on when it's closed."},
    {26, "dialog (no back)", "Same as dialog."},
    {4, "message", "Shows a message on the top line."},
    {24, "status message", "Shows a message on the top line (used for area names)."},
    {40, "note", "Adds a text to the player's notebook."},
    {10, "door code", "Asks for a code; the rest of the script runs only if it's right."},
    {7, "show thing", "Makes a hidden thing appear (kills whatever stands on its cell)."},
    {18, "hide things", "Removes the non-monster things standing on a cell."},
    {11, "set script state", "Sets the state of the script on a cell (commands can be limited to a state)."},
    {19, "next script state", "Adds 1 to the state of the script on a cell."},
    {20, "previous script state", "Subtracts 1 from the state of the script on a cell."},
    {21, "give", "Gives health, armour, credits or XP."},
    {22, "take", "Takes health, armour, credits or XP."},
    {23, "require", "Stops the script unless the player has enough health/armour/credits/XP."},
    {41, "require key", "Stops the script unless the player has a key card."},
    {5, "hurt player", "Damages the player."},
    {9, "give map", "Marks the map as given (no automap in this port yet)."},
    {2, "exit level", "Leaves the sector. Tick 'completes the sector' for the real exit."},
    {37, "wait", "(no effect in this port)"},
};
const int N_OPS = sizeof(OPS) / sizeof(OPS[0]);   // (not used)

// the menu entry for op, or nullptr when the menu doesn't offer it
const OpInfo *op_info(int op)
{
    for (auto &o : OPS)
        if (o.op == op) return &o;
    return nullptr;
}

// names of the stat numbers (give/take/require) and key numbers (require key)
const char *STATS[4] = {"health", "armour", "credits", "XP"};
const char *KEYS[4] = {"green", "yellow", "blue", "red"};

// text id as a short quoted preview: '|' (the games' line break) as a
// space, the buttons as {NAME} (keytext::key_markup), cut at 40 characters
std::string short_str(const Project &p, int id)
{
    if (id < 0 || id >= (int)p.strings.size()) return "<no text " + std::to_string(id) + ">";
    std::string s = keytext::key_markup(p.strings[id]);
    for (auto &c : s)
        if (c == '|') c = ' ';
    if (s.size() > 40) s = s.substr(0, 40) + "...";
    return "\"" + s + "\"";
}

// a door slot as "<name> at x,z"
std::string line_desc(const Project &p, int slot)
{
    if (slot < 0 || slot >= (int)p.lines.size()) return "door #" + std::to_string(slot) + " (missing)";
    auto &l = p.lines[slot];
    return l.name + " at " + std::to_string(l.x) + "," + std::to_string(l.z);
}

// a combo box choosing a text by id; true when the choice changed
bool string_picker(App &app, const char *label, int &id)
{
    Project &p = app.proj;
    bool ch = false;
    std::string cur = short_str(p, id);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo(label, cur.c_str())) {
        for (int i = 0; i < (int)p.strings.size(); i++) {
            std::string s = std::to_string(i) + ": " + short_str(p, i);
            if (ImGui::Selectable(s.c_str(), i == id)) {
                id = i;
                ch = true;
            }
        }
        ImGui::EndCombo();
    }
    return ch;
}

// a combo box choosing a door slot; true when the choice changed
bool line_picker(App &app, int &slot)
{
    Project &p = app.proj;
    bool ch = false;
    std::string cur = line_desc(p, slot);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("door", cur.c_str())) {
        for (int i = 0; i < (int)p.lines.size(); i++)
            if (ImGui::Selectable((std::to_string(i) + ": " + line_desc(p, i)).c_str(), i == slot)) {
                slot = i;
                ch = true;
            }
        ImGui::EndCombo();
    }
    return ch;
}

// two number fields for a cell, clamped to 0..31; true when changed
bool cell_input(int &x, int &z)
{
    int v[2] = {x, z};
    ImGui::SetNextItemWidth(120 * g_ui);
    bool ch = ImGui::InputInt2("cell x,z", v);
    if (ch) {
        x = std::max(0, std::min(31, v[0]));
        z = std::max(0, std::min(31, v[1]));
    }
    return ch;
}
}  // namespace

// see scripts.h. Unknown or unsupported ops read "<NAME> (not used by this port)".
std::string describe_cmd(const Project &p, const Cmd &c)
{
    // the first three bytes of a1
    int lo = c.a1 & 0xFF, hi = (c.a1 >> 8) & 0xFF, b2 = (c.a1 >> 16) & 0xFF;
    switch (c.op) {
    case 15: return "open " + line_desc(p, lo);
    case 16: return "close " + line_desc(p, lo);
    case 6: case 17: return "toggle " + line_desc(p, lo);
    case 13: return "unlock " + line_desc(p, lo);
    case 12: return "lock " + line_desc(p, lo);
    case 14: return "toggle lock of " + line_desc(p, lo);
    case 8: case 26: return "dialog " + short_str(p, lo);
    case 4: case 24: return "message " + short_str(p, lo);
    case 40: return "note " + short_str(p, lo);
    case 10: return "door code " + short_str(p, lo) + " (prompt " + std::to_string(hi) + ")";
    case 7: return lo == 0xFF ? "show nothing" : "show thing #" + std::to_string(lo);
    case 18: return "hide things at " + std::to_string(lo) + "," + std::to_string(hi);
    case 11: return "set state of " + std::to_string(lo) + "," + std::to_string(hi) + " to " + std::to_string(b2);
    case 19: return "next state of " + std::to_string(lo) + "," + std::to_string(hi);
    case 20: return "previous state of " + std::to_string(lo) + "," + std::to_string(hi);
    case 21: return "give " + std::to_string(hi) + " " + STATS[lo & 3];
    case 22: return "take " + std::to_string(hi) + " " + STATS[lo & 3];
    case 23: return "require " + std::to_string(hi) + " " + STATS[lo & 3];
    case 41: return std::string("require ") + KEYS[lo & 3] + " key";
    case 5: return "hurt player by " + std::to_string(lo);
    case 9: return "give map";
    case 2: return (c.a1 & 0x80000000u) ? "exit: sector complete" : "exit back to Junction (sealed in this port)";
    }
    return std::string(cmd_name(c.op)) + " (not used by this port)";
}

// see scripts.h. Per command: an "up" and a delete button, then a tree
// node with the command menu, its arguments (by op) and its "when" flags.
// Moves and deletes are applied after the loop, so the list doesn't change
// while it is drawn.
void draw_script_editor(App &app, int ei)
{
    Project &p = app.proj;
    Event &ev = p.events[ei];
    ImGui::Text("Script on %d,%d", ev.x, ev.z);
    ImGui::SetNextItemWidth(80 * g_ui);
    // (the start state and the flag below are changed without an undo step)
    if (ImGui::InputInt("start state", &ev.state)) {
        ev.state = std::max(0, std::min(9, ev.state));   // states 0..9
        app.changed();
    }
    bool nomove = ev.flags & 1;   // flags bit 0: ignore moves
    if (ImGui::Checkbox("only when used", &nomove)) {
        ev.flags = (ev.flags & ~1) | (nomove ? 1 : 0);
        app.changed();
    }
    hint("Ticked: walking onto the cell or turning on it does nothing; only 'use' and other scripts set it off.");
    hint("Commands run top to bottom when their 'when' matches.");
    int del = -1, up = -1;   // the command to delete / move up this frame
    for (int k = 0; k < (int)ev.cmds.size(); k++) {
        Cmd &c = ev.cmds[k];
        ImGui::PushID(k);
        std::string desc = describe_cmd(p, c);
        std::string head = std::to_string(k + 1) + ". " + desc + "###cmd";
        if (ImGui::SmallButton("up")) up = k;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("move this command up");
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) del = k;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("delete this command");
        ImGui::SameLine();
        bool open = ImGui::TreeNode(head.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", desc.c_str());
        if (open) {
            const OpInfo *oi = op_info(c.op);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("command", oi ? oi->label : cmd_name(c.op))) {
                for (auto &o : OPS)
                    if (ImGui::Selectable(o.label, o.op == c.op)) {
                        app.push_undo();
                        c.op = o.op;
                        app.changed();
                    }
                ImGui::EndCombo();
            }
            if (oi) para("%s", oi->help);
            int lo = c.a1 & 0xFF, hi = (c.a1 >> 8) & 0xFF, b2 = (c.a1 >> 16) & 0xFF;
            bool ch = false;   // an argument changed (no undo step is pushed for these)
            // the argument fields for the op; each case rebuilds a1 from them
            switch (c.op) {
            case 15: case 16: case 6: case 17: case 12: case 13: case 14:   // door commands: lo = door slot
                if (line_picker(app, lo)) { c.a1 = lo; ch = true; }
                break;
            case 8: case 26: case 4: case 24: case 40:   // texts: lo = text id
                if (string_picker(app, "text", lo)) { c.a1 = lo; ch = true; }
                break;
            case 10:   // PASSWORD: lo = the code's text, hi = the question's text
                ImGui::Text("the code:");
                if (string_picker(app, "##code", lo)) ch = true;
                ImGui::Text("the question:");
                if (string_picker(app, "##prompt", hi)) ch = true;
                if (ch) c.a1 = lo | (hi << 8);
                break;
            case 7:   // SHOW: lo = thing index (255 = none)
                ImGui::SetNextItemWidth(100 * g_ui);
                if (ImGui::InputInt("thing #", &lo)) { c.a1 = std::max(0, std::min(255, lo)); ch = true; }
                hint("the #number is shown in the cell inspector");
                break;
            case 18: case 19: case 20: case 11: {   // a cell (lo, hi); CHANGESTATE: b2 = the new state
                int x = lo, z = hi;
                if (cell_input(x, z)) ch = true;
                if (c.op == 11) {
                    ImGui::SetNextItemWidth(100 * g_ui);
                    if (ImGui::InputInt("new state", &b2)) { b2 = std::max(0, std::min(9, b2)); ch = true; }
                }
                if (ch) c.a1 = x | (z << 8) | (b2 << 16);
                break;
            }
            case 21: case 22: case 23: {   // lo = stat, hi = amount; REQSTAT: b2 = text if short
                int s = lo & 3;
                ImGui::SetNextItemWidth(120 * g_ui);
                if (ImGui::Combo("what", &s, "health\0armour\0credits\0XP\0")) ch = true;
                ImGui::SetNextItemWidth(100 * g_ui);
                if (ImGui::InputInt("amount", &hi)) { hi = std::max(0, std::min(255, hi)); ch = true; }
                if (c.op == 23) {
                    ImGui::Text("text shown when it's not enough:");
                    if (string_picker(app, "##req", b2)) ch = true;
                }
                if (ch) c.a1 = s | (hi << 8) | (b2 << 16);
                break;
            }
            case 41: {   // CHECKKEY: lo = key colour
                int s = lo & 3;
                ImGui::SetNextItemWidth(120 * g_ui);
                if (ImGui::Combo("key", &s, "green\0yellow\0blue\0red\0")) { c.a1 = s; ch = true; }
                break;
            }
            case 5:   // PAIN: lo = damage
                ImGui::SetNextItemWidth(100 * g_ui);
                if (ImGui::InputInt("damage", &lo)) { c.a1 = std::max(0, std::min(255, lo)); ch = true; }
                break;
            case 2: {   // CHANGEMAP: bit 31 = the sector is completed
                bool done = c.a1 & 0x80000000u;
                if (ImGui::Checkbox("completes the sector", &done)) {
                    c.a1 = done ? (c.a1 | 0x80000000u) : (c.a1 & 0x7FFFFFFFu);
                    ch = true;
                }
                break;
            }
            default: {   // anything else: a1 as raw hex
                unsigned v = c.a1;
                ImGui::SetNextItemWidth(140 * g_ui);
                if (ImGui::InputScalar("argument (hex)", ImGuiDataType_U32, &v, nullptr, nullptr, "%08X",
                                       ImGuiInputTextFlags_CharsHexadecimal)) {
                    c.a1 = v;
                    ch = true;
                }
            }
            }
            // ---- when ----
            ImGui::Text("When:");
            u32 a2 = c.a2;
            // a checkbox for one bit of a2 (ImGui's "##" part only keeps labels unique)
            auto flag = [&](const char *label, u32 bit) {
                bool v = (a2 & bit) != 0;
                if (ImGui::Checkbox(label, &v)) a2 = v ? (a2 | bit) : (a2 & ~bit);
            };
            flag("the player uses this cell (button 1)", 0x100);
            ImGui::SameLine();
            flag("only once", 0x200);
            ImGui::Text("walking onto it, moving:");
            ImGui::SameLine();
            flag("N##a", 4);
            ImGui::SameLine();
            flag("E##a", 8);
            ImGui::SameLine();
            flag("S##a", 1);
            ImGui::SameLine();
            flag("W##a", 2);
            ImGui::Text("walking off it, moving:");
            ImGui::SameLine();
            flag("N##l", 16);
            ImGui::SameLine();
            flag("E##l", 32);
            ImGui::SameLine();
            flag("S##l", 64);
            ImGui::SameLine();
            flag("W##l", 128);
            ImGui::Text("turning to face:");
            ImGui::SameLine();
            flag("N##f", 0x10000000);
            ImGui::SameLine();
            flag("E##f", 0x20000000);
            ImGui::SameLine();
            flag("S##f", 0x80000000u);
            ImGui::SameLine();
            flag("W##f", 0x40000000);
            ImGui::Text("only in script state:");
            for (int s = 1; s <= 9; s++) {
                ImGui::SameLine();
                char lb[8];
                snprintf(lb, sizeof(lb), "%d##s", s);
                flag(lb, 0x10000u << (s - 1));
            }
            hint("(no state ticked = only in state 0)");
            if (a2 != c.a2) {
                app.push_undo();
                c.a2 = a2;
                ch = true;
            }
            if (ch) app.changed();
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (up > 0) {
        app.push_undo();
        std::swap(ev.cmds[up], ev.cmds[up - 1]);
        app.changed();
    }
    if (del >= 0) {
        app.push_undo();
        ev.cmds.erase(ev.cmds.begin() + del);
        app.changed();
    }
    if (ImGui::Button("+ add command")) {
        app.push_undo();
        ev.cmds.push_back({8, 0, 0x100});   // a new command: dialog with text 0, when used
        app.changed();
    }
    ImGui::SameLine();
    if (ImGui::Button("delete script")) {
        app.push_undo();
        edit::delete_event(p, ei);   // removes the script (ei is invalid afterwards)
        app.changed();
    }
}
