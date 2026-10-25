// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Editor: the Art tab, a small paint program for the level's pictures
// (wall textures, sprites, the Master System border, the Mega Drive weapon).
// Each picture is a 64x64 drawing (palette indices) + its own 16 colours.
// Pictures can share a drawing with different colours, as in the original
// game; the panel on the right shows the drawing with each of them, and
// how the consoles will show it. The drawing is edited whole, with its 8x8
// tile grid shown; a tile can be picked out and edited, copied, flipped.
// Licence: GPL-2.0-or-later.
//
// Called from App::frame() (draw_art_tab) every frame while the Art tab is
// shown. Layout: the picture list (left), the paint area with its tool bar
// and the whole-drawing buttons (middle), and the side panel (right): the
// 16 colours, the picked tile, the drawing with other pictures' colours, and
// how the consoles show it. State kept between frames is in App::art
// (ArtEditState, app.h) and the art_* settings.
// Words used here: a "drawing" (Drawing, a "shape" in the Project) is 64x64
// palette indices 0..15 (255 = CLEAR, transparent, sprites only); a
// "picture" (ArtRef) is a drawing plus 16 colours. Painting changes the
// drawing, so every picture sharing it changes too.
// Undo: each stroke or operation pushes one undo step first.
#include "app.h"
#include "colour.h"
#include "md_build.h"
#include "projectio.h"

#include "../third_party/imgui/imgui.h"
#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

// the art tools' names and keys, in ArtTool order (app.h)
static const char *TOOL_NAMES[A_COUNT] = {"Pencil", "Line", "Box", "Filled box", "Fill", "Pick colour"};
static const char *TOOL_KEYS[A_COUNT] = {"P", "L", "B", "Shift+B", "F", "K"};

// ---------------------------------------------------------------- pictures
// The picture a PicId names, or nullptr if there is no such texture,
// sprite or weapon frame (the border always exists).
ArtRef *App::pic_ref(const PicId &id)
{
    if (art.console >= 0)   // "Edit for" a console: its own version when it has one
        if (ArtRef *o = own_ref(CONSOLE_KEYS[art.console], id)) return o;
    switch (id.kind) {
    case P_TEXTURE: {
        auto it = proj.textures.find(id.id);
        return it == proj.textures.end() ? nullptr : &it->second;
    }
    case P_SPRITE: {
        auto it = proj.sprites.find(id.id);
        return it == proj.sprites.end() ? nullptr : &it->second;
    }
    case P_BORDER: return &proj.border;
    default: return id.id >= 0 && id.id < (int)proj.weapons.size() ? &proj.weapons[id.id] : nullptr;
    }
}

// A console's own version of a texture or sprite (Project::per_console), or
// nullptr when it has none (the border and the weapon have no per-console
// versions).
ArtRef *App::own_ref(const std::string &console, const PicId &id)
{
    auto pc = proj.per_console.find(console);
    if (pc == proj.per_console.end()) return nullptr;
    auto &m = id.kind == P_TEXTURE ? pc->second.textures : pc->second.sprites;
    if (id.kind != P_TEXTURE && id.kind != P_SPRITE) return nullptr;
    auto it = m.find(id.id);
    return it == m.end() ? nullptr : &it->second;
}

// What the picture list shows for picture id: with "Edit for" a console,
// its own version when it has one, else the shared one.
const ArtRef &App::shown_ref(const PicId &id, const ArtRef &shared)
{
    if (art.console >= 0)
        if (ArtRef *o = own_ref(CONSOLE_KEYS[art.console], id)) return *o;
    return shared;
}

// A picture's name for the list's tooltip and the paint area's title. A
// sprite also lists what uses it: thing types (up to about 60 letters of
// names), "prop", "wall decal".
std::string App::pic_name(const PicId &id) const
{
    switch (id.kind) {
    case P_TEXTURE: return "Wall texture " + std::to_string(id.id);
    case P_SPRITE: {
        std::string s = "Sprite " + std::to_string(id.id);
        std::string who;
        for (auto &d : proj.defs)
            if (d.tile == id.id && who.size() < 60) who += (who.empty() ? "" : ", ") + d.name;
        bool prop = false, decal = false;
        // a scenery item with orientation bits (0x780000) is a wall decal, else a prop
        for (auto &sc : proj.scenery)
            if (sc.tile == id.id) (sc.info & 0x780000 ? decal : prop) = true;
        if (prop) who += who.empty() ? "prop" : ", prop";
        if (decal) who += who.empty() ? "wall decal" : ", wall decal";
        return who.empty() ? s : s + " (" + who + ")";
    }
    case P_BORDER: return "Border around the view (Master System)";
    default: {   // weapon frame i is the player's weapon i / 2 (odd i: firing)
        // its name: the map's thing type that gives that weapon (eType 5,
        // eSub the weapon's number), as the game names it; else its number
        std::string w = "weapon " + std::to_string(id.id / 2);
        for (auto &d : proj.defs)
            if (d.eType == 5 && d.eSub == id.id / 2) {
                w = d.name;
                break;
            }
        return "Weapon: " + w + (id.id & 1 ? ", firing" : "") + " (Mega Drive)";
    }
    }
}

namespace {
// Flood fill: from pixel (x, y), every pixel of the same value reachable
// through its 4 neighbours, within the box x0..x1-1, y0..y1-1, becomes `to`.
// Breadth first with a queue (no recursion, so no stack overflow).
void flood(Drawing &d, int x, int y, u8 to, int x0, int y0, int x1, int y1)
{
    u8 from = d.px[y * 64 + x];
    if (from == to) return;   // nothing to do (and it would never end)
    std::deque<std::pair<int, int>> q = {{x, y}};
    // a pixel already changed no longer equals `from`, so it is not done twice
    while (!q.empty()) {
        auto [cx, cy] = q.front();
        q.pop_front();
        if (cx < x0 || cy < y0 || cx >= x1 || cy >= y1 || d.px[cy * 64 + cx] != from) continue;
        d.px[cy * 64 + cx] = to;
        q.push_back({cx + 1, cy});
        q.push_back({cx - 1, cy});
        q.push_back({cx, cy + 1});
        q.push_back({cx, cy - 1});
    }
}

// A line of value v from (x0, y0) to (x1, y1), both ends included
// (Bresenham's algorithm, all octants; err tracks the error of both axes).
// Pixels outside the drawing are skipped.
void line(Drawing &d, int x0, int y0, int x1, int y1, u8 v)
{
    int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        if (x0 >= 0 && y0 >= 0 && x0 < 64 && y0 < 64) d.px[y0 * 64 + x0] = v;
        // step in x, in y, or both, whichever keeps closest to the true line
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

// Draws the finished shape of a Line, Box or Filled box stroke from corner
// (x0, y0) to (x1, y1) (both inside the drawing). A box is its outline,
// one pixel wide; a filled box every pixel.
void shape_op(Drawing &d, int tool, int x0, int y0, int x1, int y1, u8 v)
{
    if (tool == A_LINE) return line(d, x0, y0, x1, y1, v);
    int ax = std::min(x0, x1), bx = std::max(x0, x1), ay = std::min(y0, y1), by = std::max(y0, y1);
    for (int y = ay; y <= by; y++)
        for (int x = ax; x <= bx; x++)
            if (tool == A_FILLRECT || x == ax || x == bx || y == ay || y == by) d.px[y * 64 + x] = v;
}

// an RGB as an ImGui packed colour (with opacity a) or as a float vector
ImU32 imcol(RGB c, int a = 255) { return IM_COL32(c.r, c.g, c.b, a); }
ImVec4 imvec(RGB c) { return ImVec4(c.r / 255.f, c.g / 255.f, c.b / 255.f, 1); }

// A grey checkerboard (squares of `step` pixels) over w x h at p0: what
// shows through a sprite's transparent pixels.
void checker(ImDrawList *dl, ImVec2 p0, float w, float h, float step)
{
    for (float y = 0; y < h; y += step)
        for (float x = 0; x < w; x += step) {
            bool o = ((int)(x / step) + (int)(y / step)) & 1;
            dl->AddRectFilled(ImVec2(p0.x + x, p0.y + y), ImVec2(p0.x + std::min(w, x + step), p0.y + std::min(h, y + step)),
                              o ? IM_COL32(95, 95, 100, 255) : IM_COL32(65, 65, 70, 255));
        }
}
}  // namespace

// ---------------------------------------------------------------- the tab
// The whole Art tab, one frame. Long, in parts marked "----": the picture
// list, the paint area (tools, zoom, the picture, mouse handling, the
// whole-drawing buttons), and the right-hand panel.
// ---------------------------------------------------------------- per console
// The colour the console `c` would show for rgb (each channel to the
// console's levels): the Master System's 4 levels a channel, the Game Gear's
// 16, the Mega Drive's 8, the 15-bit consoles' 32; the 3DS shows them as
// they are.
static RGB console_colour(Console c, RGB rgb)
{
    auto snap = [](int v, const int *lev, int n) {
        int best = lev[0];
        for (int i = 1; i < n; i++)
            if (std::abs(lev[i] - v) < std::abs(best - v)) best = lev[i];
        return (u8)best;
    };
    static const int SMS[4] = {0, 85, 170, 255};
    static const int MD[8] = {0, 52, 87, 116, 144, 172, 206, 255};
    switch (c) {
    case Console::SMS: return {snap(rgb.r, SMS, 4), snap(rgb.g, SMS, 4), snap(rgb.b, SMS, 4)};
    case Console::GG: return {(u8)((rgb.r + 8) / 17 * 17), (u8)((rgb.g + 8) / 17 * 17), (u8)((rgb.b + 8) / 17 * 17)};
    case Console::MD: return {snap(rgb.r, MD, 8), snap(rgb.g, MD, 8), snap(rgb.b, MD, 8)};
    case Console::N3DS: return rgb;
    default: {   // 5 bits a channel, shown as 8 (31 -> 255)
        auto f = [](int v) { int q = std::min(31, (v + 4) / 8); return (u8)(q << 3 | q >> 2); };
        return {f(rgb.r), f(rgb.g), f(rgb.b)};
    }
    }
}

// the nearest of the 16 colours pal to c (squared RGB distance, first wins)
static RGB nearest_of(const RGB *pal, RGB c)
{
    int best = 0;
    long bd = -1;
    for (int i = 0; i < 16; i++) {
        long d = (long)(c.r - pal[i].r) * (c.r - pal[i].r) + (long)(c.g - pal[i].g) * (c.g - pal[i].g) +
                 (long)(c.b - pal[i].b) * (c.b - pal[i].b);
        if (bd < 0 || d < bd) {
            bd = d;
            best = i;
        }
    }
    return pal[best];
}

// The Art tab: the "Edit for" bar (every console, or one console's own
// versions of the pictures), then the tab itself (draw_art_tab_body).
// With a console chosen, a change to a picture that console has no version
// of yet is moved, at the end of the frame, into a new version for that
// console only (the shared picture is put back as it was; the undo step
// the change pushed undoes both). The console's versions keep to the
// colours it can show; the Master System and Game Gear can also fix the
// level's 16 colours, and their pictures then use only those.
void App::draw_art_tab()
{
    ArtEditState &st = art;
    // ---- the bar
    ImGui::SetNextItemWidth(220 * ui);
    const char *now = st.console < 0 ? "Every console (shared)" : console_name((Console)st.console);
    if (ImGui::BeginCombo("Edit for", now)) {
        if (ImGui::Selectable("Every console (shared)", st.console < 0)) st.console = -1;
        for (int c = 0; c < N_CONSOLES; c++)
            if (ImGui::Selectable(console_name((Console)c), st.console == c)) st.console = c;
        ImGui::EndCombo();
    }
    std::string key = st.console >= 0 ? CONSOLE_KEYS[st.console] : "";
    bool own_able = st.console >= 0 && (st.pic.kind == P_TEXTURE || st.pic.kind == P_SPRITE);
    bool has_own = own_able && own_ref(key, st.pic);
    ImGui::SameLine();
    if (st.console < 0)
        ImGui::TextDisabled("changes apply to every console (unless a console has its own version)");
    else if (!own_able)
        ImGui::TextDisabled("the border and the weapon are the same on every console");
    else if (has_own) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "this picture has its own %s version", console_name((Console)st.console));
        ImGui::SameLine();
        if (ImGui::SmallButton("Use the shared picture again")) {
            push_undo();
            auto &own = proj.per_console[key];
            (st.pic.kind == P_TEXTURE ? own.textures : own.sprites).erase(st.pic.id);
            if (own.empty()) proj.per_console.erase(key);
            proj.drop_unused_shapes();
            changed();
            has_own = false;
        }
    } else
        ImGui::TextDisabled("a change gives the %s its own version; the other consoles keep the shared one",
                            console_name((Console)st.console));
    // the Master System and Game Gear: the level's 16 colours, fixed or chosen
    bool sms_like = st.console == (int)Console::SMS || st.console == (int)Console::GG;
    if (sms_like) {
        ConsoleOwn *own = proj.per_console.count(key) ? &proj.per_console[key] : nullptr;
        bool fixed = own && own->has_palette;
        if (ImGui::Checkbox("Fixed level palette", &fixed)) {
            push_undo();
            ConsoleOwn &o = proj.per_console[key];
            o.has_palette = fixed;
            if (fixed) {   // start from the 16 colours the builder chooses now
                if (sms_data)
                    for (int i = 0; i < 16; i++) o.palette[i] = sms_data->col.rgb[i];
                else
                    for (int i = 0; i < 16; i++) o.palette[i] = console_colour((Console)st.console, {(u8)(i * 17), (u8)(i * 17), (u8)(i * 17)});
            } else if (o.empty())
                proj.per_console.erase(key);
            changed();
        }
        hint("The %s shows the whole level with 16 colours. Fixed: you choose them here (0 is the ceiling, 1 the "
             "floor), and this console's pictures are drawn with only these. Otherwise the builder picks the 16 "
             "that suit the level's pictures best.",
             console_name((Console)st.console));
        if (fixed) {
            ConsoleOwn &o = proj.per_console[key];
            for (int i = 0; i < 16; i++) {
                ImGui::PushID(1000 + i);
                float c[3] = {o.palette[i].r / 255.f, o.palette[i].g / 255.f, o.palette[i].b / 255.f};
                ImGui::SameLine();
                if (ImGui::ColorEdit3("##lp", c, ImGuiColorEditFlags_NoInputs)) {
                    push_undo();
                    o.palette[i] = console_colour((Console)st.console, {(u8)(c[0] * 255 + 0.5f), (u8)(c[1] * 255 + 0.5f), (u8)(c[2] * 255 + 0.5f)});
                    changed();
                }
                ImGui::PopID();
            }
        }
    }
    // ---- snapshot of the shared picture, to move a change into a console version
    bool redirect = own_able && !has_own;
    ArtRef before;
    Drawing before_dr;
    bool snap = false;
    if (redirect)
        if (ArtRef *r = pic_ref(st.pic))
            if (const Drawing *d = proj.drawing(*r)) {
                before = *r;
                before_dr = *d;
                snap = true;
            }
    draw_art_tab_body();
    if (snap) {
        ArtRef *r = pic_ref(st.pic);   // (still the shared picture: no version yet)
        const Drawing *d = r ? proj.drawing(*r) : nullptr;
        bool pal_changed = r && memcmp(r->pal, before.pal, sizeof before.pal) != 0;
        bool shape_changed = r && r->shape != before.shape;
        bool px_changed = d && !shape_changed && memcmp(d->px, before_dr.px, sizeof before_dr.px) != 0;
        if (r && (pal_changed || shape_changed || px_changed)) {
            ArtRef nr = *r;
            if (px_changed) {   // the edited drawing becomes the console's own
                std::string k = proj.new_shape_key(key + "_");
                proj.shapes[k] = *d;
                nr.shape = k;
            }
            *r = before;                          // the shared picture as it was
            proj.shapes[before.shape] = before_dr;
            auto &own = proj.per_console[key];
            (st.pic.kind == P_TEXTURE ? own.textures : own.sprites)[st.pic.id] = nr;
            changed();
        }
    }
    // ---- the console's version keeps to its colours
    if (st.console >= 0)
        if (ArtRef *o = own_ref(key, st.pic)) {
            const ConsoleOwn &co = proj.per_console[key];
            bool snapped = false;
            for (int i = 0; i < 16; i++) {
                RGB c = co.has_palette ? nearest_of(co.palette, o->pal[i]) : console_colour((Console)st.console, o->pal[i]);
                if (memcmp(&c, &o->pal[i], sizeof c)) {
                    o->pal[i] = c;
                    snapped = true;
                }
            }
            if (snapped) changed();
        }
}

void App::draw_art_tab_body()
{
    ArtEditState &st = art;
    float h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();   // all but the status line
    float s = ui;   // the size scale

    // ---- the list of pictures
    ImGui::BeginChild("artlist", ImVec2(240 * s, h), true);
    float th = 44 * s;   // thumbnail size
    // one thumbnail button: selects the picture (dropping the picked tile and
    // any colours being tried); as many to a row as fit, n counts them
    auto entry = [&](PicId id, const ArtRef &r, bool sprite, int &n) {
        ImGui::PushID(id.kind * 100000 + id.id);   // a unique ImGui id per picture
        bool sel = st.pic == id;
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 0.8f, 0.2f, 1));
        if (ImGui::ImageButton("p", (ImTextureID)picture_img(r, sprite), ImVec2(th, th))) {
            st.pic = id;
            st.tile = -1;
            st.view_pal.clear();
        }
        if (sel) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", pic_name(id).c_str());
        ImGui::PopID();
        // the width one button takes, and how many fit across
        float step = th + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
        int per_row = std::max(1, (int)((ImGui::GetContentRegionAvail().x + ImGui::GetStyle().ItemSpacing.x) / step));
        if (++n % per_row) ImGui::SameLine();
    };
    // the four groups, each a collapsible header (border and weapon closed at first)
    if (ImGui::CollapsingHeader(("Wall textures (" + std::to_string(proj.textures.size()) + ")").c_str(),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        int n = 0;
        for (auto &kv : proj.textures) entry({P_TEXTURE, kv.first}, shown_ref({P_TEXTURE, kv.first}, kv.second), false, n);
        ImGui::NewLine();
    }
    if (ImGui::CollapsingHeader(("Sprites (" + std::to_string(proj.sprites.size()) + ")").c_str(),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        int n = 0;
        for (auto &kv : proj.sprites) entry({P_SPRITE, kv.first}, shown_ref({P_SPRITE, kv.first}, kv.second), true, n);
        ImGui::NewLine();
    }
    if (ImGui::CollapsingHeader("Border (Master System)")) {
        int n = 0;
        entry({P_BORDER, 0}, proj.border, false, n);
        ImGui::NewLine();
    }
    if (ImGui::CollapsingHeader("Weapon (Mega Drive)")) {
        int n = 0;
        for (int i = 0; i < (int)proj.weapons.size(); i++) entry({P_WEAPON, i}, proj.weapons[i], true, n);
        ImGui::NewLine();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // the picture being edited; if it is gone (e.g. after an undo or another
    // level), fall back to the first texture and draw nothing this frame
    ArtRef *ref = pic_ref(st.pic);
    if (!ref || !proj.drawing(*ref)) {
        st.pic = {P_TEXTURE, proj.textures.empty() ? 0 : proj.textures.begin()->first};
        hint("pick a picture on the left");
        return;
    }
    Drawing &dr = *proj.drawing(*ref);   // painting writes here
    bool sprite = pic_sprite(st.pic);    // has transparency
    auto users = proj.users(ref->shape); // the pictures sharing this drawing
    // the colours shown: this picture's, or another picture's (to try them)
    // (st.view_pal names the other picture, as "texture 5", "sprite 12",
    // "border" or "weapon 3 firing"; if it no longer exists it is forgotten)
    ArtRef view = *ref;
    if (!st.view_pal.empty()) {
        bool found = false;
        auto try_ref = [&](const ArtRef &r, const std::string &name) {
            if (name == st.view_pal) {
                for (int i = 0; i < 16; i++) view.pal[i] = r.pal[i];
                found = true;
            }
        };
        for (auto &kv : proj.textures) try_ref(kv.second, "texture " + std::to_string(kv.first));
        for (auto &kv : proj.sprites) try_ref(kv.second, "sprite " + std::to_string(kv.first));
        try_ref(proj.border, "border");
        for (size_t i = 0; i < proj.weapons.size(); i++)
            try_ref(proj.weapons[i], "weapon " + std::to_string(i / 2) + (i & 1 ? " firing" : ""));
        if (!found) st.view_pal.clear();
    }

    // ---- the canvas
    float right_w = 330 * s;   // the right-hand panel's width
    ImGui::BeginChild("paint", ImVec2(ImGui::GetContentRegionAvail().x - right_w, h), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(pic_name(st.pic).c_str());
    // the tool buttons (the current one orange)
    for (int t = 0; t < A_COUNT; t++) {
        if (t) ImGui::SameLine();
        bool on = st.tool == t;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f, 0.6f, 0.1f, 1));
        if (ImGui::Button(TOOL_NAMES[t])) st.tool = t;
        if (on) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s  (key %s)", TOOL_NAMES[t], TOOL_KEYS[t]);
    }
    ImGuiIO &io = ImGui::GetIO();
    // tool keys, while the mouse is over the paint area and no text box has the keys
    if (!io.WantTextInput && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        if (ImGui::IsKeyPressed(ImGuiKey_P)) st.tool = A_PENCIL;
        if (ImGui::IsKeyPressed(ImGuiKey_L)) st.tool = A_LINE;
        if (ImGui::IsKeyPressed(ImGuiKey_B)) st.tool = io.KeyShift ? A_FILLRECT : A_RECT;
        if (ImGui::IsKeyPressed(ImGuiKey_F)) st.tool = A_FILL;
        if (ImGui::IsKeyPressed(ImGuiKey_K)) st.tool = A_PICK;
        if (ImGui::IsKeyPressed(ImGuiKey_X)) std::swap(st.fg, st.bg);
    }
    // the zoom steps the -/+ buttons and Ctrl+wheel move between (13 of them)
    static const int ZOOMS[] = {2, 3, 4, 6, 8, 10, 12, 16, 20, 24, 32, 40, 48};
    // the next step above (dir > 0) or below the current zoom, which need not
    // be a step itself (the slider and Fit give any value)
    auto zoom_step = [&](int dir) {
        int i = 0;
        while (i < 12 && ZOOMS[i] < set.art_zoom) i++;   // the first step >= the zoom
        if (dir > 0) {
            while (i < 12 && ZOOMS[i] <= set.art_zoom) i++;   // the first step > the zoom
        } else if (i > 0)
            i--;
        return ZOOMS[i];
    };
    // the zoom row: - slider + Fit, the grid boxes and the "show" menu
    // (new_zoom is applied below, once the view's size is known)
    int new_zoom = set.art_zoom;
    if (ImGui::Button("-")) new_zoom = zoom_step(-1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * s);
    ImGui::SliderInt("##zoom", &new_zoom, 2, 48, "zoom %dx");
    ImGui::SameLine();
    if (ImGui::Button("+")) new_zoom = zoom_step(1);
    ImGui::SameLine();
    bool fit = ImGui::Button("Fit");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("zoom so the whole picture fits (Ctrl+wheel zooms, middle-drag moves)");
    ImGui::SameLine();
    ImGui::Checkbox("8x8 tiles", &set.art_grid);
    ImGui::SameLine();
    ImGui::Checkbox("pixels", &set.art_pixel_grid);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150 * s);
    // st.look: 0 own colours, 1 Master System, 2 Mega Drive (picture_img's look)
    ImGui::Combo("show", &st.look, "its own colours\0as the Master System shows it\0as the Mega Drive shows it\0");

    // the picture, in its own scrolling view: Ctrl+wheel zooms around the
    // mouse, the wheel scrolls (Shift: sideways), middle-drag moves it
    // (the view leaves room below for the hint and the operation buttons)
    float bottom = ImGui::GetFrameHeightWithSpacing() * 2 + ImGui::GetTextLineHeightWithSpacing() * 3;
    ImGui::BeginChild("artview", ImVec2(0, std::max(64.0f * s, ImGui::GetContentRegionAvail().y - bottom)), false,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 view_size = ImGui::GetContentRegionAvail();
    // Fit: the largest whole zoom at which 64 pixels fit both ways (2..48);
    // any other zoom change turns it off
    if (fit) set.art_fit = true;
    if (new_zoom != set.art_zoom) set.art_fit = false;
    if (set.art_fit) new_zoom = std::max(2, std::min(48, (int)(std::min(view_size.x, view_size.y) / (64 * s))));
    bool view_hov = ImGui::IsWindowHovered();
    // the wheel: Ctrl zooms, Shift scrolls sideways, else up and down (60 px a notch, scaled)
    if (view_hov && io.MouseWheel != 0) {
        if (io.KeyCtrl) {
            new_zoom = zoom_step(io.MouseWheel > 0 ? 1 : -1);
            set.art_fit = false;
        }
        else if (io.KeyShift) ImGui::SetScrollX(ImGui::GetScrollX() - io.MouseWheel * 60 * s);
        else ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseWheel * 60 * s);
    }
    if (view_hov && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        ImGui::SetScrollX(ImGui::GetScrollX() - io.MouseDelta.x);
        ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
    }
    if (new_zoom != set.art_zoom) {
        // keep the pixel under the mouse (or the middle of the view) where it is
        // c0: where the picture's corner is now; ux, uy: the picture pixel
        // under the anchor; the scroll moves by that pixel's shift in size
        ImVec2 c0 = ImGui::GetCursorScreenPos();
        ImVec2 anchor = view_hov ? io.MousePos : ImVec2(ImGui::GetWindowPos().x + view_size.x / 2, ImGui::GetWindowPos().y + view_size.y / 2);
        float oz = (float)set.art_zoom * s, nz = (float)new_zoom * s;   // old and new screen pixels per picture pixel
        float ux = (anchor.x - c0.x) / oz, uy = (anchor.y - c0.y) / oz;
        ImGui::SetScrollX(std::max(0.0f, ImGui::GetScrollX() + ux * (nz - oz)));
        ImGui::SetScrollY(std::max(0.0f, ImGui::GetScrollY() + uy * (nz - oz)));
        set.art_zoom = new_zoom;
    }
    float z = (float)set.art_zoom * s;   // screen pixels per picture pixel
    ImVec2 p0 = ImGui::GetCursorScreenPos();   // the picture's top-left on screen
    ImDrawList *dl = ImGui::GetWindowDrawList();
    checker(dl, p0, 64 * z, 64 * z, 4 * z);   // behind it, for the transparent pixels
    ImGui::Image((ImTextureID)picture_img(view, sprite, st.look), ImVec2(64 * z, 64 * z));
    bool hov = ImGui::IsItemHovered();
    // the pixel grid (faint black, only from 6 screen pixels a pixel) and the
    // 8x8 tile grid (faint white)
    if (set.art_pixel_grid && z >= 6)
        for (int i = 1; i < 64; i++) {
            dl->AddLine(ImVec2(p0.x + i * z, p0.y), ImVec2(p0.x + i * z, p0.y + 64 * z), IM_COL32(0, 0, 0, 40));
            dl->AddLine(ImVec2(p0.x, p0.y + i * z), ImVec2(p0.x + 64 * z, p0.y + i * z), IM_COL32(0, 0, 0, 40));
        }
    if (set.art_grid)
        for (int i = 0; i <= 64; i += 8) {
            dl->AddLine(ImVec2(p0.x + i * z, p0.y), ImVec2(p0.x + i * z, p0.y + 64 * z), IM_COL32(255, 255, 255, 70));
            dl->AddLine(ImVec2(p0.x, p0.y + i * z), ImVec2(p0.x + 64 * z, p0.y + i * z), IM_COL32(255, 255, 255, 70));
        }
    if (st.pic.kind == P_SPRITE) {   // where it stands: the floor at the bottom, the middle of the cell
        dl->AddLine(ImVec2(p0.x, p0.y + 64 * z - 1), ImVec2(p0.x + 64 * z, p0.y + 64 * z - 1), IM_COL32(0, 255, 0, 140), 2);
        dl->AddLine(ImVec2(p0.x + 32 * z, p0.y), ImVec2(p0.x + 32 * z, p0.y + 64 * z), IM_COL32(0, 255, 0, 50));
    }
    // the picked tile's yellow frame
    if (st.tile >= 0) {
        int tx = st.tile % 8, ty = st.tile / 8;
        dl->AddRect(ImVec2(p0.x + tx * 8 * z, p0.y + ty * 8 * z), ImVec2(p0.x + (tx + 1) * 8 * z, p0.y + (ty + 1) * 8 * z),
                    IM_COL32(255, 220, 0, 255), 0, 0, 2.5f);
    }
    // the picture pixel under the mouse (may be outside it)
    int mx = (int)std::floor((io.MousePos.x - p0.x) / z), my = (int)std::floor((io.MousePos.y - p0.y) / z);
    bool inside = mx >= 0 && my >= 0 && mx < 64 && my < 64;
    // the value a stroke paints: the first (left button) or second colour;
    // "transparent" only on sprites (elsewhere CLEAR & 15 = colour 15)
    auto paint_value = [&](bool right) -> u8 {
        int c = right ? st.bg : st.fg;
        return (c == CLEAR && sprite) ? CLEAR : (u8)(c & 15);
    };
    // Pick colour: the pixel's value becomes the first or second colour
    auto pick = [&](int x, int y, bool right) {
        u8 v = dr.px[y * 64 + x];
        (right ? st.bg : st.fg) = v == CLEAR ? CLEAR : v;
    };
    // the button that started the current stroke (static: kept between frames)
    static bool right_stroke = false;
    if (hov && inside) {
        // a white frame round the pixel and a tooltip saying where and what it is
        dl->AddRect(ImVec2(p0.x + mx * z, p0.y + my * z), ImVec2(p0.x + (mx + 1) * z, p0.y + (my + 1) * z),
                    IM_COL32(255, 255, 255, 220));
        u8 v = dr.px[my * 64 + mx];
        char tip[96];
        if (v == CLEAR) snprintf(tip, sizeof tip, "x %d  y %d   tile %d,%d   transparent", mx, my, mx / 8, my / 8);
        else snprintf(tip, sizeof tip, "x %d  y %d   tile %d,%d   colour %d", mx, my, mx / 8, my / 8, v & 15);
        ImGui::SetTooltip("%s", tip);
        bool lclick = ImGui::IsMouseClicked(ImGuiMouseButton_Left), rclick = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        if (io.KeyCtrl && lclick) {   // ctrl+click: pick out this 8x8 tile
            st.tile = (my / 8) * 8 + mx / 8;
        } else if (lclick || rclick) {
            // a press starts a stroke (one undo step); Pick colour doesn't paint
            right_stroke = rclick;
            if (st.tool == A_PICK) pick(mx, my, rclick);
            else {
                push_undo();
                st.drawing = true;
                st.sx = mx;   // the stroke's start (the line / box corner, or the pencil's last point)
                st.sy = my;
                if (st.tool == A_FILL) {   // a fill happens at once, on the press
                    int x0 = 0, y0 = 0, x1 = 64, y1 = 64;
                    if (io.KeyShift && st.tile >= 0) {   // shift: only inside the picked tile
                        x0 = (st.tile % 8) * 8;
                        y0 = (st.tile / 8) * 8;
                        x1 = x0 + 8;
                        y1 = y0 + 8;
                    }
                    if (mx >= x0 && mx < x1 && my >= y0 && my < y1) flood(dr, mx, my, paint_value(rclick), x0, y0, x1, y1);
                }
            }
        }
    }
    // the stroke in progress: while its button is held, and once more when
    // it is released (the mouse may have left the picture: the position is
    // clamped to the edge)
    bool down = ImGui::IsMouseDown(right_stroke ? ImGuiMouseButton_Right : ImGuiMouseButton_Left);
    if (st.drawing) {
        int cx = std::max(0, std::min(63, mx)), cy = std::max(0, std::min(63, my));
        if (st.tool == A_PENCIL && down) {
            // join to the last point so fast strokes have no gaps
            line(dr, st.sx, st.sy, cx, cy, paint_value(right_stroke));
            st.sx = cx;
            st.sy = cy;
        } else if (st.tool != A_PENCIL && st.tool != A_FILL && down) {
            // line / box: only a preview on screen until the button is released
            // (from pixel centre to pixel centre, in the paint colour; magenta for transparent)
            ImVec2 a(p0.x + (st.sx + 0.5f) * z, p0.y + (st.sy + 0.5f) * z), b(p0.x + (cx + 0.5f) * z, p0.y + (cy + 0.5f) * z);
            RGB pc = paint_value(right_stroke) == CLEAR ? RGB{255, 0, 255} : view.pal[paint_value(right_stroke) & 15];
            if (st.tool == A_LINE) dl->AddLine(a, b, imcol(pc), std::max(2.0f, z * 0.5f));
            else if (st.tool == A_RECT) dl->AddRect(a, b, imcol(pc), 0, 0, std::max(2.0f, z * 0.5f));
            else dl->AddRectFilled(a, b, imcol(pc, 180));
        }
        if (!down) {
            // released: draw the line / box into the drawing; the stroke ends
            if (st.tool == A_LINE || st.tool == A_RECT || st.tool == A_FILLRECT)
                shape_op(dr, st.tool, st.sx, st.sy, cx, cy, paint_value(right_stroke));
            st.drawing = false;
            changed(true);   // all images remade, the level unsaved
        } else if (st.tool == A_PENCIL || st.tool == A_FILL) {
            // pencil and fill changed the drawing already: remake only this
            // drawing's images, each frame of the stroke
            forget_shape_images(ref->shape);   // live update while drawing
            unsaved = true;
        }
    }
    ImGui::EndChild();
    hint("Left: draw with the first colour. Right: with the second. Ctrl+click: pick out a tile. X: swap the "
         "colours. Ctrl+wheel: zoom.");
    // whole-drawing operations
    // op: a button that, when pressed, pushes undo and calls fn(old copy,
    // drawing) to rewrite the drawing from the copy
    auto op = [&](const char *label, auto fn) {
        if (ImGui::Button(label)) {
            push_undo();
            Drawing o = dr;
            fn(o, dr);
            changed(true);
        }
        ImGui::SameLine();
    };
    op("Flip left-right", [](const Drawing &o, Drawing &d) {
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) d.px[y * 64 + x] = o.px[y * 64 + 63 - x];
    });
    op("Flip up-down", [](const Drawing &o, Drawing &d) {
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) d.px[y * 64 + x] = o.px[(63 - y) * 64 + x];
    });
    // shifts wrap round: the column (row) pushed off one edge comes back on the other
    op("Shift left", [](const Drawing &o, Drawing &d) {
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) d.px[y * 64 + x] = o.px[y * 64 + (x + 1) % 64];
    });
    op("Shift up", [](const Drawing &o, Drawing &d) {
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) d.px[y * 64 + x] = o.px[((y + 1) % 64) * 64 + x];
    });
    ImGui::NewLine();
    // Import PNG: the file (reduced to 64x64 and 16 colours by art_from_png,
    // projectio.h) becomes this picture's own new drawing and colours; the
    // old drawing goes if nothing else uses it. The callback runs later, so
    // it looks the picture up again by its id.
    if (ImGui::Button("Import PNG...")) {
        PicId id = st.pic;
        fd.open("Import PNG", FileDialog::OPEN, ".png", set.last_dir, [this, id, sprite](const std::string &f) {
            Art na;
            std::string err;
            if (!art_from_png(f, sprite, na, err)) {
                status = err;
                return;
            }
            ArtRef *r = pic_ref(id);
            if (!r) return;
            push_undo();
            std::string key = proj.new_shape_key(sprite ? "s" : "t");   // a new drawing name ("s..." sprite, "t..." texture)
            memcpy(proj.shapes[key].px, na.px, 4096);
            r->shape = key;
            for (int i = 0; i < 16; i++) r->pal[i] = na.pal[i];
            proj.drop_unused_shapes();
            changed(true);
            status = "Imported " + f + " (64x64, 16 colours) as this picture's own drawing";
        });
    }
    ImGui::SameLine();
    // Export PNG: the picture in the colours shown (the tried colours, if
    // any), named after the picture with every non-letter/digit as '_'
    if (ImGui::Button("Export PNG...")) {
        Art a = proj.flat(view);
        std::string name = pic_name(st.pic);
        for (auto &c : name)
            if (!isalnum((unsigned char)c)) c = '_';
        fd.open("Export PNG", FileDialog::SAVE, ".png", set.last_dir,
                [this, a, sprite](const std::string &f) {
                    std::string err;
                    status = art_to_png(a, sprite, f, err) ? "Saved " + f : err;
                },
                name + ".png");
    }
    // Copy as a new picture (textures and sprites): a copy with its own
    // drawing, under the next free number after the highest; it becomes
    // the picture being edited
    if (st.pic.kind == P_TEXTURE || st.pic.kind == P_SPRITE) {
        ImGui::SameLine();
        if (ImGui::Button("Copy as a new picture")) {
            push_undo();
            auto &m = st.pic.kind == P_TEXTURE ? proj.textures : proj.sprites;
            int id = m.rbegin()->first + 1;   // (m holds at least this picture)
            ArtRef nr = *ref;
            nr.shape = proj.new_shape_key(sprite ? "s" : "t");
            proj.shapes[nr.shape] = dr;
            m[id] = nr;
            st.pic.id = id;
            changed(true);
            status = std::string(st.pic.kind == P_TEXTURE ? "Wall texture " : "Sprite ") + std::to_string(id) +
                     " added: use it on the map or for a thing type";
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ---- right panel: colours, tile, sharing, consoles
    ImGui::BeginChild("artside", ImVec2(0, h), true);
    // colours
    // the 16 colours shown (the tried ones, if any), two rows of 8: the
    // first colour framed yellow, the second blue; each swatch has its own
    // "change colour" popup (the ImGui id includes i)
    ImGui::TextUnformatted("Colours");
    hint("click: first colour, right-click: second, double-click: change");
    float sw = 30 * s;   // swatch size
    for (int i = 0; i < 16; i++) {
        ImGui::PushID(i);
        bool f = st.fg == i, b = st.bg == i;
        if (f || b) {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 3);
            ImGui::PushStyleColor(ImGuiCol_Border, f ? ImVec4(1, 1, 0, 1) : ImVec4(0, 0.8f, 1, 1));
        }
        RGB c = view.pal[i];
        if (ImGui::ColorButton("##c", imvec(c), ImGuiColorEditFlags_NoTooltip, ImVec2(sw, sw))) st.fg = i;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) st.bg = i;
        if (f || b) {
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }
        if (ImGui::IsItemHovered()) {
            RGB a = look_sms(c), m = look_md(c);
            ImGui::SetTooltip("colour %d: %d, %d, %d\nMaster System: %d, %d, %d\nMega Drive: %d, %d, %d", i, c.r, c.g,
                              c.b, a.r, a.g, a.b, m.r, m.g, m.b);
            if (ImGui::IsMouseDoubleClicked(0)) {
                st.edit_colour = i;
                ImGui::OpenPopup("colour");
            }
        }
        // the colour window: a free picker, the Master System's 64 colours,
        // and a snap to the Mega Drive's; it edits this picture's own colour
        // (not the tried ones)
        if (ImGui::BeginPopup("colour")) {
            RGB &pc = ref->pal[i];
            float fc[3] = {pc.r / 255.f, pc.g / 255.f, pc.b / 255.f};   // 0..1 for ImGui's picker
            ImGui::Text("Colour %d of %s", i, pic_name(st.pic).c_str());
            // one undo step for all the changes while the window is open
            static bool undo_pushed = false;
            if (ImGui::IsWindowAppearing()) undo_pushed = false;
            auto set_col = [&](RGB nc) {
                if (!undo_pushed) {
                    push_undo();
                    undo_pushed = true;
                }
                proj.drawing(*pic_ref(st.pic));   // (keeps the reference valid)
                pic_ref(st.pic)->pal[i] = nc;
                changed(true);
            };
            if (ImGui::ColorPicker3("##pick", fc, ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_DisplayRGB))
                set_col({(u8)(fc[0] * 255 + 0.5f), (u8)(fc[1] * 255 + 0.5f), (u8)(fc[2] * 255 + 0.5f)});   // rounded to 0..255
            ImGui::TextUnformatted("The Master System's 64 colours:");
            for (int k = 0; k < 64; k++) {   // 4 rows of 16 (sms_rgb numbering: r*16 + g*4 + b)
                ImGui::PushID(1000 + k);
                if (ImGui::ColorButton("##s", imvec(sms_rgb(k)), 0, ImVec2(16 * s, 16 * s))) set_col(sms_rgb(k));
                ImGui::PopID();
                if (k % 16 != 15) ImGui::SameLine();
            }
            ImGui::TextUnformatted("Mega Drive: 512 colours (8 levels per channel)");
            if (ImGui::Button("snap to the Mega Drive's nearest")) set_col(look_md(pc));
            ImGui::SameLine();
            if (ImGui::Button("close")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopID();
        if (i % 8 != 7) ImGui::SameLine();
    }
    // sprites can also paint "transparent" (CLEAR): click for the first, right-click for the second
    if (sprite) {
        bool f = st.fg == CLEAR, b = st.bg == CLEAR;
        if (ImGui::Button(f ? "[transparent] (first)" : b ? "[transparent] (second)" : "transparent")) st.fg = CLEAR;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) st.bg = CLEAR;
    }
    // while trying another picture's colours: go back, or keep them for this picture (undoable)
    if (!st.view_pal.empty()) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Trying the colours of %s", st.view_pal.c_str());
        if (ImGui::SmallButton("back to its own colours")) st.view_pal.clear();
        ImGui::SameLine();
        if (ImGui::SmallButton("use these colours")) {
            push_undo();
            for (int i = 0; i < 16; i++) ref->pal[i] = view.pal[i];
            st.view_pal.clear();
            changed(true);
        }
    }
    ImGui::Separator();

    // the picked tile
    // shown large (18 scaled pixels a pixel), drawn rectangle by rectangle in
    // the look chosen (Master System here: the nearest colour, not the pairs)
    if (st.tile >= 0) {
        int tx = st.tile % 8, ty = st.tile / 8;   // the tile's column and row (0..7)
        ImGui::Text("Tile %d,%d (pixels %d-%d, %d-%d)", tx, ty, tx * 8, tx * 8 + 7, ty * 8, ty * 8 + 7);
        float tz = 18 * s;
        ImVec2 q0 = ImGui::GetCursorScreenPos();
        ImDrawList *tdl = ImGui::GetWindowDrawList();
        checker(tdl, q0, 8 * tz, 8 * tz, tz);
        Art fa = proj.flat(view);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int i = (ty * 8 + y) * 64 + tx * 8 + x;
                if (sprite && fa.clear(i)) continue;
                RGB c = fa.at(i);
                if (st.look == 1) c = look_sms(c);
                if (st.look == 2) c = look_md(c);
                tdl->AddRectFilled(ImVec2(q0.x + x * tz, q0.y + y * tz), ImVec2(q0.x + (x + 1) * tz, q0.y + (y + 1) * tz), imcol(c));
            }
        // painting in the large tile: any tool but Pick colour acts as a pencil
        // (one pixel at a time, left = first colour, right = second)
        ImGui::InvisibleButton("tile", ImVec2(8 * tz, 8 * tz), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        if (ImGui::IsItemHovered()) {
            int x = (int)((io.MousePos.x - q0.x) / tz), y = (int)((io.MousePos.y - q0.y) / tz);   // pixel in the tile
            if (x >= 0 && y >= 0 && x < 8 && y < 8) {
                bool l = ImGui::IsMouseDown(ImGuiMouseButton_Left), r = ImGui::IsMouseDown(ImGuiMouseButton_Right);
                static bool tile_undo = false;   // a stroke has pushed its undo step
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    if (st.tool == A_PICK) pick(tx * 8 + x, ty * 8 + y, !l);   // right button if left isn't down
                    else {
                        push_undo();
                        tile_undo = true;
                    }
                }
                if ((l || r) && tile_undo && st.tool != A_PICK) {
                    u8 v = paint_value(!l);
                    u8 &px = dr.px[(ty * 8 + y) * 64 + tx * 8 + x];
                    if (px != v) {   // only on a change: changed(true) drops every cached image
                        px = v;
                        changed(true);
                    }
                }
                if (!l && !r) tile_undo = false;   // stroke over
                // (if the button is let go outside the tile, tile_undo stays set
                // and the next stroke there adds no undo step of its own)
            }
        }
        // tile_op: a button that copies the tile out (o, and t to change),
        // lets fn rewrite t from o, and copies t back (one undo step)
        auto tile_op = [&](const char *label, auto fn) {
            if (ImGui::Button(label)) {
                push_undo();
                u8 t[64], o[64];
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) t[y * 8 + x] = o[y * 8 + x] = dr.px[(ty * 8 + y) * 64 + tx * 8 + x];
                fn(o, t);
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) dr.px[(ty * 8 + y) * 64 + tx * 8 + x] = t[y * 8 + x];
                changed(true);
            }
        };
        if (ImGui::Button("Copy tile")) {
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) st.tile_clip[y * 8 + x] = dr.px[(ty * 8 + y) * 64 + tx * 8 + x];
            st.tile_clip_full = true;
        }
        ImGui::SameLine();
        if (st.tile_clip_full) {
            tile_op("Paste tile", [&](const u8 *, u8 *t) { memcpy(t, st.tile_clip, 64); });
            ImGui::SameLine();
        }
        tile_op("Clear", [&](const u8 *, u8 *t) { memset(t, paint_value(true), 64); });   // to the second colour
        tile_op("Flip left-right", [](const u8 *o, u8 *t) {
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) t[y * 8 + x] = o[y * 8 + 7 - x];
        });
        ImGui::SameLine();
        tile_op("Flip up-down", [](const u8 *o, u8 *t) {
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) t[y * 8 + x] = o[(7 - y) * 8 + x];
        });
        if (ImGui::SmallButton("done with this tile")) st.tile = -1;
        hint("arrow keys: next tile");
        // the arrow keys move to the neighbouring tile, wrapping round the 8x8 grid
        if (!io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) st.tile = ty * 8 + (tx + 7) % 8;
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) st.tile = ty * 8 + (tx + 1) % 8;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) st.tile = ((ty + 7) % 8) * 8 + tx;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) st.tile = ((ty + 1) % 8) * 8 + tx;
        }
    } else
        hint("Ctrl+click the picture to pick out one 8x8 tile.");
    ImGui::Separator();

    // the same drawing with other colours
    ImGui::TextUnformatted("This drawing with other colours");
    if (users.size() > 1)
        para("The drawing is shared by %d pictures: painting here changes all of them. Colours "
                           "belong to each picture.", (int)users.size());
    float mini = 34 * s;
    // one row: this drawing in other's colours (click to try them), other's
    // 16 colours as small squares, and other's name
    auto show_with = [&](const ArtRef &other, const std::string &name) {
        ArtRef v = *ref;
        for (int i = 0; i < 16; i++) v.pal[i] = other.pal[i];
        ImGui::PushID(name.c_str());
        if (ImGui::ImageButton("v", (ImTextureID)picture_img(v, sprite, st.look), ImVec2(mini, mini))) st.view_pal = name;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("show it with the colours of %s", name.c_str());
        ImGui::SameLine();
        ImDrawList *l = ImGui::GetWindowDrawList();
        ImVec2 c0 = ImGui::GetCursorScreenPos();
        for (int i = 0; i < 16; i++)
            l->AddRectFilled(ImVec2(c0.x + (i % 8) * 10 * s, c0.y + (i / 8) * 10 * s),
                             ImVec2(c0.x + (i % 8 + 1) * 10 * s, c0.y + (i / 8 + 1) * 10 * s), imcol(other.pal[i]));
        ImGui::Dummy(ImVec2(80 * s, 20 * s));
        ImGui::SameLine();
        bool mine = &other == ref;
        ImGui::TextUnformatted(mine ? (name + " (this one)").c_str() : name.c_str());
        ImGui::PopID();
    };
    // a row for every picture that shares this drawing
    for (auto &kv : proj.textures)
        if (kv.second.shape == ref->shape) show_with(kv.second, "texture " + std::to_string(kv.first));
    for (auto &kv : proj.sprites)
        if (kv.second.shape == ref->shape) show_with(kv.second, "sprite " + std::to_string(kv.first));
    if (proj.border.shape == ref->shape) show_with(proj.border, "border");
    for (size_t i = 0; i < proj.weapons.size(); i++)
        if (proj.weapons[i].shape == ref->shape)
            show_with(proj.weapons[i], "weapon " + std::to_string(i / 2) + (i & 1 ? " firing" : ""));
    // (other_kind is not used: dead code)
    static int other_kind = 0;
    // any texture's or sprite's colours, to try on this drawing
    ImGui::SetNextItemWidth(170 * s);
    if (ImGui::BeginCombo("try other colours", "pick a picture...")) {
        for (auto &kv : proj.textures)
            if (ImGui::Selectable(("texture " + std::to_string(kv.first)).c_str()))
                st.view_pal = "texture " + std::to_string(kv.first);
        for (auto &kv : proj.sprites)
            if (ImGui::Selectable(("sprite " + std::to_string(kv.first)).c_str()))
                st.view_pal = "sprite " + std::to_string(kv.first);
        ImGui::EndCombo();
    }
    (void)other_kind;
    // a shared drawing: copy it for this picture alone
    if (users.size() > 1 && ImGui::Button("Give this picture a drawing of its own")) {
        push_undo();
        std::string key = proj.new_shape_key(sprite ? "s" : "t");
        proj.shapes[key] = dr;   // a copy of the shared drawing under a new name
        pic_ref(st.pic)->shape = key;
        changed(true);
        status = "This picture now has its own drawing; the others keep theirs";
    }
    ImGui::Separator();

    // the consoles
    // the Master System look (not for the weapon, which it doesn't use), the
    // Mega Drive look (not for the border, the Master System's only), and for
    // a thing's sprite the Master System's hardware-sprite colours
    ImGui::TextUnformatted("On the consoles");
    float pv = 96 * s;   // preview size
    if (st.pic.kind != P_WEAPON) {
        ImGui::BeginGroup();
        ImGui::Image((ImTextureID)picture_img(view, sprite, 1), ImVec2(pv, pv));
        hint("Master System");
        ImGui::EndGroup();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(st.pic.kind == P_SPRITE ? "as a prop: the level's 16 colours, mixed in pairs"
                                                      : "the level's 16 colours, mixed in pairs (dithered)");
        ImGui::SameLine();
    }
    if (st.pic.kind != P_BORDER) {
        ImGui::BeginGroup();
        ImGui::Image((ImTextureID)picture_img(view, sprite, 2), ImVec2(pv, pv));
        hint("Mega Drive");
        ImGui::EndGroup();
    }
    if (st.pic.kind == P_SPRITE) {
        bool thing = false;   // a thing type uses this sprite
        for (auto &d : proj.defs) thing |= d.tile == st.pic.id;
        if (thing) {
            // as a thing: a hardware sprite with its own 15 console colours
            // (cached under "thing:<drawing>:<colours>")
            std::string key = "thing:" + ref->shape + ":";
            for (auto &c : view.pal) key += (char)c.r, key += (char)c.g, key += (char)c.b;
            SDL_Texture *t = nullptr;
            auto it = img_cache.find(key);
            if (it != img_cache.end()) t = it->second;
            else {
                Art a = proj.flat(view);
                // the 15 colours the Master System builder picks for this
                // sprite (pal[0] is the transparent slot, unused here)
                RGB pal[16];
                sms::sprite_colours(a, pal);
                Lab P[15];
                for (int i = 0; i < 15; i++) P[i] = lab(pal[i + 1]);
                std::vector<RGB> px(4096);
                std::vector<u8> al(4096, 0);
                // each opaque pixel: the nearest of the 15 in CIELAB
                for (int i = 0; i < 4096; i++) {
                    if (a.clear(i)) continue;
                    Lab l = lab(a.at(i));
                    int bi = 0;
                    double bd = 1e300;
                    for (int k = 0; k < 15; k++)
                        if (dist2(l, P[k]) < bd) {
                            bd = dist2(l, P[k]);
                            bi = k;
                        }
                    px[i] = pal[bi + 1];
                    al[i] = 255;
                }
                t = upload(key, px, &al, 64, 64);
            }
            ImGui::BeginGroup();
            ImGui::Image((ImTextureID)t, ImVec2(pv, pv));
            hint("SMS thing");
            ImGui::EndGroup();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("as a thing (monster, item...): a hardware sprite with its own 15 colours");
        }
    }
    if (st.pic.kind == P_WEAPON) {
        // where it sits on the Mega Drive's screen
        // (md::weapon_frame, md_build.h, places the frame in the 256 x 160
        // view as the game does: idx = colour numbers, 0 = nothing there;
        // pal = Mega Drive CRAM words. The empty part shows the ceiling
        // colour in the top 80 lines, the floor colour below.)
        std::string key = "weapon:" + std::to_string(st.pic.id) + ":" + ref->shape + ":";
        for (auto &c : ref->pal) key += (char)c.r, key += (char)c.g, key += (char)c.b;
        SDL_Texture *t = nullptr;
        auto it = img_cache.find(key);
        if (it != img_cache.end()) t = it->second;
        else {
            std::vector<u8> idx;
            u16 pal[16];
            md::weapon_frame(proj, st.pic.id, idx, pal);
            std::vector<RGB> px(256 * 160);
            for (int i = 0; i < 256 * 160; i++) px[i] = idx[i] ? md_rgb(pal[idx[i]]) : (i / 256 < 80 ? proj.ceiling : proj.floor);
            t = upload(key, px, nullptr, 256, 160);   // opaque
        }
        ImGui::Image((ImTextureID)t, ImVec2(256 * s, 160 * s));
        hint("On the Mega Drive's screen (the view is 256 x 160)");
    }
    ImGui::EndChild();
}
