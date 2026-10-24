// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Editor: the Map tab (tools, map canvas, cell inspector, preview).
// Licence: GPL-2.0-or-later.
//
// The App member functions that draw the Map tab, called from App::frame()
// each frame (see app.h). The tab has three columns:
//   draw_tools_panel  (left)   the tool list and each tool's options;
//   draw_map_canvas   (middle) the 32 x 32 map, zoom/pan, and the tools' clicks;
//   draw_preview + draw_inspector (right) the first-person view from the
//                     selected cell, and everything in that cell.
// The edits themselves are done by edit.h's functions; each one is wrapped
// in push_undo() before and changed() after.
// Map coordinates: cell (x, z), x east, z south; world units are 64 a cell.
// On screen a cell is cs = set.map_zoom * ui pixels, and (pan_x, pan_y) is
// where cell 0,0's corner is, relative to the canvas's top-left.
#include "app.h"
#include "console_view.h"
#include "edit.h"
#include "colour.h"
#include "render.h"
#include "scripts.h"

#include "../third_party/imgui/imgui.h"
#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

// the tools' names and one-line help, in Tool order (app.h)
static const char *TOOL_NAMES[T_COUNT] = {"Select", "Wall", "Floor", "Paint wall", "Door", "Thing", "Prop", "Start", "Script"};
static const char *TOOL_HELP[T_COUNT] = {
    "Click a cell to see and edit what's in it.",
    "Click or drag to fill cells with solid wall (using the texture below).",
    "Click or drag to dig out floor. Walls appear around it automatically.",
    "Click near the edge of a cell to give that wall face the texture below.",
    "Click a corridor cell to put a door across it (opens when the player uses it).",
    "Click to place the selected thing. Right-click removes the top thing in a cell.",
    "Click to place a prop (lamp, table...). Right-click removes props.",
    "Click to set where the player starts; click again to turn the start direction.",
    "Click a cell to give it a script (or edit its script) in the panel on the right."};

// an ImGui colour from 0..255 components (a: opacity)
static ImU32 col(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

// The average colour of a picture's opaque pixels (grey if it has none);
// the map draws each wall in its texture's average colour.
static RGB average(const Art &a)
{
    long r = 0, g = 0, b = 0, n = 0;
    for (int i = 0; i < 4096; i++)
        if (!a.clear(i)) {
            RGB c = a.at(i);
            r += c.r;
            g += c.g;
            b += c.b;
            n++;
        }
    if (!n) return {128, 128, 128};
    return {(u8)(r / n), (u8)(g / n), (u8)(b / n)};
}

// The Map tab: three child windows side by side, the full height less one
// row for the status line. The right panel's width is set.map_panel (a share
// of the window, at least 260 scaled pixels, at most a third).
void App::draw_map_tab()
{
    float h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("tools", ImVec2(220 * ui, h), true);
    draw_tools_panel();
    ImGui::EndChild();
    ImGui::SameLine();
    // the right-hand panel: drag the bar to resize it (up to a third of the window)
    float win_w = ImGui::GetIO().DisplaySize.x;
    float right = std::max(260 * ui, std::min(win_w / 3.0f, set.map_panel * win_w));
    float split_w = 6 * ui;   // the drag bar's width
    // the canvas takes what is left; no scrolling (the wheel zooms instead)
    ImGui::BeginChild("canvas", ImVec2(ImGui::GetContentRegionAvail().x - right - split_w, h), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    draw_map_canvas();
    ImGui::EndChild();
    ImGui::SameLine(0, 0);
    // the drag bar: an invisible button; dragging it moves the panel's edge
    ImGui::InvisibleButton("split", ImVec2(split_w, h));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive()) {
        right -= ImGui::GetIO().MouseDelta.x;   // dragging left widens the panel
        right = std::max(260 * ui, std::min(win_w / 3.0f, right));
        set.map_panel = right / win_w;
        preview_stale = true;   // the preview is drawn at the panel's width
    }
    {
        // a short grip in the middle of the bar (yellow while dragged)
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + split_w * 0.35f, a.y + h * 0.45f),
                                                  ImVec2(b.x - split_w * 0.35f, a.y + h * 0.55f),
                                                  ImGui::IsItemActive() ? IM_COL32(255, 200, 60, 255) : IM_COL32(120, 120, 130, 255));
    }
    ImGui::SameLine(0, 0);
    ImGui::BeginChild("inspector", ImVec2(0, h), true);
    draw_preview();     // the view from the selected cell
    ImGui::Separator();
    draw_inspector();   // what is in it
    ImGui::EndChild();
}

// The left panel: the tool radio buttons, the tool's help, and its options
// (a texture palette, the door kind, the thing types, the prop sprites), then
// the map's "show" boxes.
void App::draw_tools_panel()
{
    hint("Map: 32 x 32 cells");
    ImGui::TextUnformatted("Tools (keys 1-9)");
    for (int t = 0; t < T_COUNT; t++) {
        char label[64];
        snprintf(label, sizeof(label), "%d  %s", t + 1, TOOL_NAMES[t]);
        if (ImGui::RadioButton(label, tool == t)) tool = t;
    }
    // keys 1..9 pick the tools, unless a text box is being typed in
    // (this runs only while the Map tab is shown)
    if (!ImGui::GetIO().WantTextInput)
        for (int t = 0; t < T_COUNT; t++)
            if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + t))) tool = t;
    ImGui::Separator();
    para("%s", TOOL_HELP[tool]);
    ImGui::Separator();
    float th = 40 * ui;   // thumbnail size
    if (tool == T_WALL || tool == T_FLOOR || tool == T_PAINT || tool == T_DOOR) {
        // the wall textures, three to a row; the current one highlighted
        ImGui::Text("Texture: %d", cur_tex);
        int n = 0;
        for (auto &kv : proj.textures) {
            ImGui::PushID(kv.first);   // a unique ImGui id per button
            SDL_Texture *t = art_img(false, kv.first);
            bool sel = kv.first == cur_tex;
            if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 0.8f, 0.2f, 1));
            if (ImGui::ImageButton("t", (ImTextureID)t, ImVec2(th, th))) cur_tex = kv.first;
            if (sel) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("texture %d", kv.first);
            ImGui::PopID();
            if (++n % 3) ImGui::SameLine();   // a new row after every third
        }
        ImGui::NewLine();
    }
    if (tool == T_DOOR) {
        // door_kind is an edit::DoorKind: 0 door, 1 locked, 2 secret wall
        ImGui::RadioButton("Door", &door_kind, 0);
        ImGui::RadioButton("Locked door", &door_kind, 1);
        ImGui::RadioButton("Secret sliding wall", &door_kind, 2);
        para("Locked doors need an UNLOCK command in a script. Secret walls need OPENLINE.");
    }
    if (tool == T_THING) {
        // the thing types (proj.defs), each with a small sprite
        for (size_t d = 0; d < proj.defs.size(); d++) {
            ImGui::PushID((int)d);
            SDL_Texture *t = art_img(true, proj.defs[d].tile);
            if (t) {
                ImGui::Image((ImTextureID)t, ImVec2(th * 0.6f, th * 0.6f));
                ImGui::SameLine();
            }
            if (ImGui::Selectable(proj.defs[d].name.c_str(), cur_def == (int)d)) cur_def = (int)d;
            ImGui::PopID();
        }
    }
    if (tool == T_PROP) {
        // every sprite can be a prop, three to a row
        ImGui::Checkbox("blocks movement", &prop_blocks);
        int n = 0;
        for (auto &kv : proj.sprites) {
            ImGui::PushID(kv.first);
            bool sel = kv.first == cur_prop;
            if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 0.8f, 0.2f, 1));
            if (ImGui::ImageButton("p", (ImTextureID)art_img(true, kv.first), ImVec2(th, th))) cur_prop = kv.first;
            if (sel) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("sprite %d", kv.first);
            ImGui::PopID();
            if (++n % 3) ImGui::SameLine();
        }
        ImGui::NewLine();
    }
    ImGui::Separator();
    ImGui::Checkbox("grid", &set.show_grid);
    ImGui::Checkbox("things", &set.show_things);
    ImGui::Checkbox("props", &set.show_props);
    ImGui::Checkbox("scripts", &set.show_scripts);
    hint("wheel: zoom, right-drag: move");
}

// The map: draws cells, walls, doors, props, things, script markers, the
// start arrow, the selection and the cell under the mouse, then applies the
// current tool to clicks. Mouse: wheel zooms around the pointer, right or
// middle drag pans, left click/drag uses the tool, a right click (without
// dragging) removes a thing or prop (Thing / Prop tools) or selects the cell.
void App::draw_map_canvas()
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos(), size = ImGui::GetContentRegionAvail();   // the canvas's corner and size
    // one invisible button over the whole canvas catches all three mouse buttons
    ImGui::InvisibleButton("map", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                            ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered = ImGui::IsItemHovered();
    ImGuiIO &io = ImGui::GetIO();
    float cs = set.map_zoom * ui;   // pixels per cell
    // zoom around the mouse
    // (the cell position under the pointer, mx/my, stays under it: 15% a
    // wheel step, between 6 and 80 pixels a cell before scaling)
    if (hovered && io.MouseWheel != 0) {
        float mx = (io.MousePos.x - p0.x - pan_x) / cs, my = (io.MousePos.y - p0.y - pan_y) / cs;
        set.map_zoom = std::max(6.0f, std::min(80.0f, set.map_zoom * (io.MouseWheel > 0 ? 1.15f : 1 / 1.15f)));
        cs = set.map_zoom * ui;
        pan_x = io.MousePos.x - p0.x - mx * cs;
        pan_y = io.MousePos.y - p0.y - my * cs;
    }
    // pan with the right or middle button
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
        pan_x += io.MouseDelta.x;
        pan_y += io.MouseDelta.y;
    }
    // world units (64 a cell) to screen; a cell's top-left corner on screen
    auto to_screen = [&](float wx, float wz) { return ImVec2(p0.x + pan_x + wx * cs / 64.0f, p0.y + pan_y + wz * cs / 64.0f); };
    auto cell_min = [&](int x, int z) { return ImVec2(p0.x + pan_x + x * cs, p0.y + pan_y + z * cs); };
    // nothing drawn outside the canvas; a dark background
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), col(20, 20, 24));
    // cells
    // solid: dark blue-grey; floor: light grey; floor blocked by a prop:
    // a translucent brown on top
    for (int z = 0; z < 32; z++)
        for (int x = 0; x < 32; x++) {
            ImVec2 a = cell_min(x, z), b(a.x + cs, a.y + cs);
            bool s = proj.wall[z * 32 + x];
            dl->AddRectFilled(a, b, s ? col(45, 45, 55) : col(105, 105, 95));
            if (!s && proj.static_block[z * 32 + x]) dl->AddRectFilled(a, b, col(150, 110, 60, 160));
            if (set.show_grid) dl->AddRect(a, b, col(0, 0, 0, 60));
        }
    // walls
    // each in its texture's average colour (light grey if the texture is
    // missing), with a black outline, an eighth of a cell thick (at least 2 px)
    for (size_t i = 0; i < proj.walls.size(); i++) {
        const Wall &w = proj.walls[i];
        RGB c{200, 200, 200};
        auto t = proj.textures.find(w.tex);
        if (t != proj.textures.end()) c = average(proj.flat(t->second));
        float th = std::max(2.0f, cs / 8);
        dl->AddLine(to_screen(w.ax, w.az), to_screen(w.bx, w.bz), col(0, 0, 0), th + 2);
        dl->AddLine(to_screen(w.ax, w.az), to_screen(w.bx, w.bz), col(c.r, c.g, c.b), th);
    }
    // doors
    // drawn over their wall: purple = secret sliding wall (eType 9), red =
    // locked, yellow = plain door
    for (auto &l : proj.lines) {
        if (l.wall < 0 || l.wall >= (int)proj.walls.size()) continue;
        const Wall &w = proj.walls[l.wall];
        ImU32 c = l.eType == 9 ? col(200, 80, 255) : l.locked ? col(255, 60, 60) : col(255, 220, 40);
        dl->AddLine(to_screen(w.ax, w.az), to_screen(w.bx, w.bz), c, std::max(3.0f, cs / 6));
    }
    // props (and wall decals): their sprite, 90% of a cell wide, centred on
    // their position, slightly see-through
    if (set.show_props)
        for (auto &s : proj.scenery) {
            SDL_Texture *t = art_img(true, s.tile);
            ImVec2 c = to_screen(s.x, s.z);
            float r = cs * 0.45f;
            if (t) dl->AddImage((ImTextureID)t, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), ImVec2(0, 0), ImVec2(1, 1), col(255, 255, 255, 170));
        }
    // things: their sprite filling the cell (faint if hidden), monsters
    // (eType 1) with a red frame
    if (set.show_things)
        for (auto &e : proj.entities) {
            if (e.def < 0 || e.def >= (int)proj.defs.size()) continue;   // a bad type number: skipped
            SDL_Texture *t = art_img(true, proj.defs[e.def].tile);
            ImVec2 a = cell_min(e.x, e.z), b(a.x + cs, a.y + cs);
            if (t) dl->AddImage((ImTextureID)t, a, b, ImVec2(0, 0), ImVec2(1, 1), e.hidden ? col(255, 255, 255, 90) : col(255, 255, 255));
            if (proj.defs[e.def].eType == 1) dl->AddRect(a, b, col(255, 60, 60, 200), 0, 0, 1.5f);
        }
    // scripts: a small orange square in the cell's top-left corner
    if (set.show_scripts)
        for (auto &e : proj.events) {
            ImVec2 a = cell_min(e.x, e.z);
            dl->AddRectFilled(ImVec2(a.x + 1, a.y + 1), ImVec2(a.x + cs * 0.3f, a.y + cs * 0.3f), col(255, 140, 0, 220));
        }
    // start
    // a green triangle pointing the way the player faces
    {
        ImVec2 a = cell_min(proj.spawn_x, proj.spawn_z);
        ImVec2 c(a.x + cs / 2, a.y + cs / 2);   // the cell's centre
        static const float DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};   // screen direction of N, E, S, W
        float dx = DX[proj.spawn_f], dy = DY[proj.spawn_f], r = cs * 0.4f;
        // tip ahead of the centre; the two back corners behind it, to either side
        dl->AddTriangleFilled(ImVec2(c.x + dx * r, c.y + dy * r), ImVec2(c.x - dx * r * 0.6f - dy * r * 0.6f, c.y - dy * r * 0.6f + dx * r * 0.6f),
                              ImVec2(c.x - dx * r * 0.6f + dy * r * 0.6f, c.y - dy * r * 0.6f - dx * r * 0.6f), col(60, 255, 60));
    }
    // selection
    // a yellow frame, and a line showing the preview's direction
    if (edit::in_map(sel_x, sel_z)) {
        ImVec2 a = cell_min(sel_x, sel_z);
        dl->AddRect(a, ImVec2(a.x + cs, a.y + cs), col(255, 255, 0), 0, 0, 2.5f);
        static const float DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};
        ImVec2 c(a.x + cs / 2, a.y + cs / 2);
        dl->AddLine(c, ImVec2(c.x + DX[preview_facing] * cs * 0.7f, c.y + DY[preview_facing] * cs * 0.7f), col(255, 255, 0), 2);
    }
    // hover + tools
    // the cell under the mouse (may be off the map)
    int hx = (int)std::floor((io.MousePos.x - p0.x - pan_x) / cs), hz = (int)std::floor((io.MousePos.y - p0.y - pan_y) / cs);
    if (hovered && edit::in_map(hx, hz)) {
        ImVec2 a = cell_min(hx, hz);
        dl->AddRect(a, ImVec2(a.x + cs, a.y + cs), col(255, 255, 255, 160));   // white frame
        char tip[64];
        snprintf(tip, sizeof(tip), "%d,%d", hx, hz);
        dl->AddText(ImVec2(p0.x + 6, p0.y + 4), col(255, 255, 255), tip);   // its coordinates, top-left
        bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);   // pressed this frame
        bool held = ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsItemActive();
        // a right click: released without having moved (a drag pans instead)
        bool rclick = ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).x == 0 &&
                      ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).y == 0;
        int cell = hz * 32 + hx;
        if (click) {
            // a press starts a stroke: one undo step for the whole drag
            // (pushed even if the tool then does nothing)
            dragging = true;
            last_paint_cell = -1;
            if (tool != T_SELECT) push_undo();
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragging = false;
        // Wall, Floor and Paint work on every cell the drag passes; the other
        // tools only on the click. Each cell once per stroke.
        bool paint_tool = tool == T_WALL || tool == T_FLOOR || tool == T_PAINT;
        if ((click || (held && paint_tool && dragging)) && cell != last_paint_cell) {
            last_paint_cell = cell;
            switch (tool) {
            case T_SELECT:
                // clicking the selected cell again turns the view clockwise
                if (sel_x == hx && sel_z == hz) preview_facing = (preview_facing + 1) & 3;
                sel_x = hx;
                sel_z = hz;
                preview_stale = true;
                break;
            case T_WALL:
                edit::make_solid(proj, hx, hz, cur_tex);   // fill the cell, walls fixed up around it
                changed();
                break;
            case T_FLOOR:
                edit::make_floor(proj, hx, hz, cur_tex);   // dig it out, walls put round it
                changed();
                break;
            case T_PAINT: {
                // which edge: the pointer's offset from the cell centre (-0.5..0.5
                // each way); the larger of the two picks E/W or S/N
                float fx = (io.MousePos.x - a.x) / cs - 0.5f, fz = (io.MousePos.y - a.y) / cs - 0.5f;
                int edge = std::fabs(fx) > std::fabs(fz) ? (fx > 0 ? EDGE_E : EDGE_W) : (fz > 0 ? EDGE_S : EDGE_N);
                edit::set_edge_texture(proj, hx, hz, edge, cur_tex);
                changed();
                break;
            }
            case T_DOOR: {
                // the door, its wall and its open/close scripts; or why not
                std::string e = edit::add_door(proj, hx, hz, (edit::DoorKind)door_kind, cur_tex);
                if (!e.empty()) status = e;
                else changed();
                break;
            }
            case T_THING:
                if (proj.wall[cell]) status = "Things go on floor cells";
                else if (cur_def >= 0 && cur_def < (int)proj.defs.size()) {
                    edit::add_entity(proj, cur_def, hx, hz);
                    changed();
                }
                break;
            case T_PROP:
                if (proj.wall[cell]) status = "Props go on floor cells";
                else {
                    edit::add_scenery(proj, cur_prop, hx, hz, prop_blocks);
                    changed();
                }
                break;
            case T_START:
                if (proj.wall[cell]) status = "The start must be on a floor cell";
                else {
                    // the start cell again: turn the start direction clockwise
                    if (proj.spawn_x == hx && proj.spawn_z == hz) proj.spawn_f = (proj.spawn_f + 1) & 3;
                    proj.spawn_x = hx;
                    proj.spawn_z = hz;
                    changed();
                }
                break;
            case T_SCRIPT:
                edit::ensure_event(proj, hx, hz);   // an empty script if it has none
                sel_x = hx;                         // select it: the inspector shows its script
                sel_z = hz;
                changed();
                break;
            }
        }
        if (rclick) {
            if (tool == T_THING) {
                // remove the cell's last-placed thing
                auto v = edit::entities_at(proj, hx, hz);
                if (!v.empty()) {
                    push_undo();
                    edit::delete_entity(proj, v.back());   // also fixes the scripts' thing numbers
                    changed();
                }
            } else if (tool == T_PROP) {
                // remove the cell's last-placed prop
                auto v = edit::scenery_at(proj, hx, hz);
                if (!v.empty()) {
                    push_undo();
                    edit::delete_scenery(proj, v.back());
                    changed();
                }
            } else {
                // the other tools: a right click selects the cell
                sel_x = hx;
                sel_z = hz;
                preview_stale = true;
            }
        }
    }
    dl->PopClipRect();
}

// The first-person view from the selected cell, as the console chosen in the
// menu under it shows it. The picture is only redrawn when preview_stale is
// set or the console changed; it is kept in preview_tex between frames.
void App::draw_preview()
{
    if (!edit::in_map(sel_x, sel_z)) {
        hint("select a cell to see the view from it");
        return;
    }
    static const char *FN[4] = {"north", "east", "south", "west"};
    ImGui::Text("View from %d,%d facing %s", sel_x, sel_z, FN[preview_facing]);
    if (!preview_err.empty()) {   // the level data couldn't be made: say why instead
        ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s", preview_err.c_str());
        return;
    }
    // the console (an index into NEWROM_CONSOLES, kept in range)
    int pc = std::max(0, std::min((int)NEWROM_CONSOLES.size() - 1, set.preview_console));
    Console con = NEWROM_CONSOLES[pc].c;
    bool sms_view = sms_family(con);
    if (preview_stale || preview_shown != pc) {
        preview_shown = pc;
        ConsoleView cv;   // the new picture (cv.w stays 0 if none could be drawn)
        if (sms_view && sms_data) {
            // the console's own renderer, for this one view (the Game Gear: the same picture, full width)
            int W = sms::VIEW_W[set.preview_small], H = sms::VIEW_H[set.preview_small];
            // The renderer draws only the walls and props in the view's
            // visibility list (PVS), which refresh_level_data did not make.
            // Work it out for this one view, as the builder would: the walls
            // seen with every door removed (vo) and with all of them in place
            // (vc), together, plus the props in front of the walls.
            std::vector<char> doors(sms_data->walls.size()), none(sms_data->walls.size(), 0);
            for (size_t i = 0; i < doors.size(); i++) doors[i] = sms_data->walls[i].line != 255;   // door walls
            std::vector<int> vo, vc;
            std::vector<double> colz;   // per screen column, the nearest wall's depth
            visible_walls(sms_data->walls, doors, sel_x, sel_z, preview_facing, vo, &colz);
            visible_walls(sms_data->walls, none, sel_x, sel_z, preview_facing, vc, nullptr);
            std::set<int> u(vo.begin(), vo.end());   // the union, sorted, without repeats
            u.insert(vc.begin(), vc.end());
            int key = (sel_z * 32 + sel_x) * 4 + preview_facing;   // the PVS key: cell * 4 + facing
            sms_data->pvs[key] = {std::vector<int>(u.begin(), u.end()),
                                  visible_scenery(sms_data->scen, colz, sel_x, sel_z, preview_facing)};
            // the picture as colour numbers into the level's 16 colours
            auto img = sms::render(*sms_data, set.preview_small, sel_x, sel_z, preview_facing, preview_open,
                                   set.preview_low, set.preview_solid);
            cv.w = W;
            cv.h = H;
            cv.px.resize((size_t)W * H);
            for (int i = 0; i < W * H; i++) cv.px[i] = sms_data->col.rgb[img[i]];
            sms_data->pvs.erase(key);   // leave sms_data as it was
        } else if (!sms_view && geo)
            console_view(con, proj.for_console(CONSOLE_KEYS[(int)con]), *geo, sel_x, sel_z, preview_facing, preview_open,
                         cv);   // the other consoles (with their own pictures)
        if (cv.w) {
            // a streaming texture of the picture's size (remade if the size changed)
            if (preview_tex && (preview_w != cv.w || preview_h != cv.h)) {
                SDL_DestroyTexture(preview_tex);
                preview_tex = nullptr;
            }
            if (!preview_tex) {
                preview_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, cv.w, cv.h);
                SDL_SetTextureScaleMode(preview_tex, SDL_ScaleModeNearest);   // big square pixels
                preview_w = cv.w;
                preview_h = cv.h;
            }
            // the pixels as R, G, B, A bytes (opaque)
            std::vector<u8> buf((size_t)cv.w * cv.h * 4);
            for (int i = 0; i < cv.w * cv.h; i++) {
                buf[i * 4] = cv.px[i].r;
                buf[i * 4 + 1] = cv.px[i].g;
                buf[i * 4 + 2] = cv.px[i].b;
                buf[i * 4 + 3] = 255;
            }
            SDL_UpdateTexture(preview_tex, nullptr, buf.data(), cv.w * 4);
            preview_note = cv.note;
        }
        preview_stale = false;
    }
    // the picture, as wide as the panel, at the console's aspect ratio
    if (preview_tex) {
        float w = ImGui::GetContentRegionAvail().x;
        ImGui::Image((ImTextureID)preview_tex, ImVec2(w, w * preview_h / preview_w));
    }
    // turn the view: + 3 is a quarter turn anticlockwise (mod 4)
    if (ImGui::Button("< left")) {
        preview_facing = (preview_facing + 3) & 3;
        preview_stale = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("right >")) {
        preview_facing = (preview_facing + 1) & 3;
        preview_stale = true;
    }
    ImGui::SameLine();
    // the console menu (saved in the settings)
    ImGui::SetNextItemWidth(200 * g_ui);
    if (ImGui::BeginCombo("##console", NEWROM_CONSOLES[pc].label)) {
        for (int k = 0; k < (int)NEWROM_CONSOLES.size(); k++)
            if (ImGui::Selectable(NEWROM_CONSOLES[k].label, k == pc)) {
                set.preview_console = k;
                preview_stale = true;
                level_dirty = true;   // (the Master System / Game Gear data follow the console's own pictures)
            }
        ImGui::EndCombo();
    }
    // the Master System's own view settings; the others show their note
    if (sms_view) {
        if (ImGui::Checkbox("small view", &set.preview_small)) preview_stale = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("low detail", &set.preview_low)) preview_stale = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("solid walls", &set.preview_solid)) preview_stale = true;
        hint("Exactly what the %s draws (things appear as sprites in the game). Low detail and solid walls are "
             "the faster settings in the game's pause menu.", NEWROM_CONSOLES[pc].label);
    } else
        hint("%s", preview_note.c_str());
    // "doors open": ticked if any door is drawn open; it opens or closes all of them
    if (!proj.lines.empty()) {
        bool open = false;
        for (u8 o : preview_open) open |= o != 0;
        if (ImGui::Checkbox("doors open", &open)) {
            preview_open.assign(proj.lines.size(), open ? 1 : 0);
            preview_stale = true;
        }
    }
}

// Everything in the selected cell, each part editable: whether a prop blocks
// it, its walls (texture, delete), its door, its things, its props and its
// script (the script editor, scripts.cpp). A delete changes the indices the
// loop is walking, so each delete leaves its loop at once.
void App::draw_inspector()
{
    int x = sel_x, z = sel_z;
    if (!edit::in_map(x, z)) return;
    int cell = z * 32 + x;
    ImGui::Text("Cell %d,%d: %s", x, z, proj.wall[cell] ? "solid" : "floor");
    if (!proj.wall[cell]) {
        bool blocks = proj.static_block[cell] != 0;
        if (ImGui::Checkbox("blocked by a prop", &blocks)) {
            push_undo();
            proj.static_block[cell] = blocks ? 13 : 0;   // 13: the blocking type the editor uses
            changed();
        }
    }
    // walls
    auto ws = edit::walls_touching(proj, x, z);   // on its edges or inside it
    if (!ws.empty() && ImGui::TreeNodeEx("Walls", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (int wi : ws) {
            ImGui::PushID(wi);
            Wall &w = proj.walls[wi];
            SDL_Texture *t = art_img(false, w.tex);
            if (t) {
                ImGui::Image((ImTextureID)t, ImVec2(24 * g_ui, 24 * g_ui));
                ImGui::SameLine();
            }
            ImGui::Text("texture %d, (%d,%d)-(%d,%d)", w.tex, w.ax, w.az, w.bx, w.bz);
            if (ImGui::SmallButton("use current texture")) {
                push_undo();
                w.tex = cur_tex;
                changed();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("delete")) {
                push_undo();
                edit::delete_wall(proj, wi);   // a door's wall takes the door too
                changed();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    // door
    int li = edit::line_at(proj, x, z);
    if (li >= 0 && ImGui::TreeNodeEx("Door / sliding wall", ImGuiTreeNodeFlags_DefaultOpen)) {
        Line &l = proj.lines[li];
        ImGui::Text("slot %d (scripts refer to it by this number)", li);
        // the name, edited in a 64-byte buffer (longer names are cut)
        // (note: the name, "locked" and "hidden" below change the level
        // without push_undo, so they can't be undone on their own)
        char nm[64];
        snprintf(nm, sizeof(nm), "%s", l.name.c_str());
        if (ImGui::InputText("name", nm, sizeof(nm))) {
            l.name = nm;
            changed();
        }
        if (ImGui::Checkbox("locked at start", &l.locked)) changed();
        // kind: index 0 door, 2 secret sliding wall (eType 9).
        // (note: the items string has "\0\0" after "door", which ImGui reads
        // as the end of the list, so only "door" is offered and a sliding
        // wall shows an empty choice)
        int kind = l.eType == 9 ? 2 : 0;
        if (ImGui::Combo("kind", &kind, "door\0\0secret sliding wall\0")) {
            push_undo();
            l.eType = kind == 2 ? 9 : 0;
            changed();
        }
        if (ImGui::Button("delete door")) {
            push_undo();
            edit::delete_line(proj, li);   // its wall and script commands too
            changed();
        }
        ImGui::TreePop();
    }
    // things
    auto es = edit::entities_at(proj, x, z);
    if (!es.empty() && ImGui::TreeNodeEx("Things", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (int i : es) {
            ImGui::PushID(i);
            Entity &e = proj.entities[i];
            ImGui::Text("#%d", i);   // its number, as SHOW in a script names it
            ImGui::SameLine();
            // its type, from a menu of every thing type
            ImGui::SetNextItemWidth(160 * g_ui);
            if (ImGui::BeginCombo("##def", e.def < (int)proj.defs.size() ? proj.defs[e.def].name.c_str() : "?")) {
                for (size_t d = 0; d < proj.defs.size(); d++)
                    if (ImGui::Selectable(proj.defs[d].name.c_str(), (int)d == e.def)) {
                        push_undo();
                        e.def = (int)d;
                        changed();
                    }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Checkbox("hidden", &e.hidden)) changed();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("hidden things appear when a script SHOWs them (by #number)");
            ImGui::SameLine();
            if (ImGui::SmallButton("delete")) {
                push_undo();
                edit::delete_entity(proj, i);   // renumbers the later things and the scripts
                changed();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    // props (0x780000 in info: a wall decal, which has an orientation)
    auto ss = edit::scenery_at(proj, x, z);
    if (!ss.empty() && ImGui::TreeNodeEx("Props", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (int i : ss) {
            ImGui::PushID(i);
            Scenery &s = proj.scenery[i];
            SDL_Texture *t = art_img(true, s.tile);
            if (t) {
                ImGui::Image((ImTextureID)t, ImVec2(24 * g_ui, 24 * g_ui));
                ImGui::SameLine();
            }
            ImGui::Text("sprite %d%s", s.tile, (s.info & 0x780000) ? " (wall decal)" : "");
            ImGui::SameLine();
            if (ImGui::SmallButton("delete")) {
                push_undo();
                edit::delete_scenery(proj, i);
                changed();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    // script
    int ei = edit::event_at(proj, x, z);
    if (ei < 0) {
        if (ImGui::Button("add a script to this cell")) {
            push_undo();
            edit::ensure_event(proj, x, z);
            changed();
        }
        return;
    }
    if (ImGui::TreeNodeEx("Script", ImGuiTreeNodeFlags_DefaultOpen)) {
        draw_script_editor(*this, ei);   // the script's commands (scripts.cpp)
        ImGui::TreePop();
    }
}
