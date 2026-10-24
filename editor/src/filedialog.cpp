// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Licence: GPL-2.0-or-later.
//
// The editor's own file chooser (declared in filedialog.h), drawn with Dear
// ImGui so the program needs no system dialog library. It lists the current
// folder's subfolders (in [brackets]) and the files with a matching
// extension, both sorted by name ignoring case; hidden entries (names
// starting with '.') are left out.
#include "filedialog.h"

#include "../third_party/imgui/imgui.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

// s in lower case (ASCII letters only)
static std::string lower(std::string s)
{
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// Remembers the request and shows the dialog from the next draw() on.
void FileDialog::open(const std::string &title, Mode mode, const std::string &ext, const std::string &start_dir,
                      std::function<void(const std::string &)> on_ok, const std::string &default_name)
{
    title_ = title;
    mode_ = mode;
    ext_ = ext;
    on_ok_ = on_ok;
    std::error_code ec;
    dir_ = start_dir;
    // no usable start folder: the home folder (USERPROFILE on Windows), else the current one
    if (dir_.empty() || !fs::is_directory(dir_, ec)) {
        const char *h = std::getenv(
#ifdef _WIN32
            "USERPROFILE"
#else
            "HOME"
#endif
        );
        dir_ = h ? h : fs::current_path().string();
    }
    snprintf(name_, sizeof(name_), "%s", default_name.c_str());   // (cut to 511 bytes)
    error_.clear();
    visible_ = true;
    just_opened_ = true;
}

// Draws the dialog (inside the main window's ImGui frame). On OK it closes
// the popup and calls the callback, which may itself open() a new request.
void FileDialog::draw()
{
    if (!visible_) return;
    if (just_opened_) {
        ImGui::OpenPopup(title_.c_str());   // ImGui modals must be opened before they are begun
        just_opened_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(720 * g_ui, 480 * g_ui), ImGuiCond_Appearing);
    bool open = true;   // the popup's close box sets it to false
    if (!ImGui::BeginPopupModal(title_.c_str(), &open)) {
        if (!open) visible_ = false;
        return;
    }
    std::error_code ec;
    fs::path cur(dir_);
    // ---- the top row: Up, Home, (Windows) the drive letters, the current folder
    if (ImGui::Button("Up")) {
        // (a root's parent is itself: stay there)
        if (cur.has_parent_path() && cur.parent_path() != cur) dir_ = cur.parent_path().string();
    }
    ImGui::SameLine();
    if (ImGui::Button("Home")) {
        const char *h = std::getenv(
#ifdef _WIN32
            "USERPROFILE"
#else
            "HOME"
#endif
        );
        if (h) dir_ = h;
    }
#ifdef _WIN32
    // a button for each drive that exists
    for (char d = 'A'; d <= 'Z'; d++) {
        std::string root = std::string(1, d) + ":\\";
        if (fs::exists(root, ec)) {
            ImGui::SameLine();
            if (ImGui::SmallButton(root.c_str())) dir_ = root;
        }
    }
#endif
    ImGui::SameLine();
    ImGui::TextUnformatted(dir_.c_str());
    // ---- the folder's contents (read again every frame, so it is always current)
    std::vector<fs::directory_entry> dirs, files;
    // (errors, e.g. a folder that can't be read, end the listing early)
    for (auto it = fs::directory_iterator(dir_, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const auto &e = *it;
        std::string n = e.path().filename().string();
        if (n.empty() || n[0] == '.') continue;   // hidden
        if (e.is_directory(ec)) dirs.push_back(e);
        // a file whose lower-case extension is one of the words in ext_
        // (no files at all in FOLDER mode)
        else if (mode_ != FOLDER && (ext_.empty() || (" " + ext_ + " ").find(" " + lower(e.path().extension().string()) + " ") != std::string::npos))
            files.push_back(e);
    }
    auto by_name = [](const fs::directory_entry &a, const fs::directory_entry &b) {
        return lower(a.path().filename().string()) < lower(b.path().filename().string());
    };
    std::sort(dirs.begin(), dirs.end(), by_name);
    std::sort(files.begin(), files.end(), by_name);
    // the list fills the window except for the two rows at the bottom
    float bottom = ImGui::GetFrameHeightWithSpacing() * 2.5f;
    ImGui::BeginChild("files", ImVec2(0, -bottom), true);
    // a double click on a folder goes into it
    for (auto &d : dirs) {
        std::string label = "[" + d.path().filename().string() + "]";
        if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(0))
            dir_ = d.path().string();
    }
    // a click on a file puts its name in the box; a double click in OPEN mode picks it at once
    for (auto &f : files) {
        std::string n = f.path().filename().string();
        bool sel = n == name_;
        if (ImGui::Selectable(n.c_str(), sel, ImGuiSelectableFlags_AllowDoubleClick)) {
            snprintf(name_, sizeof(name_), "%s", n.c_str());
            if (ImGui::IsMouseDoubleClicked(0) && mode_ == OPEN) {
                // close everything this function began before calling back
                visible_ = false;
                ImGui::CloseCurrentPopup();
                ImGui::EndChild();
                ImGui::EndPopup();
                auto cb = on_ok_;   // the callback may open the dialog again
                cb(f.path().string());
                return;
            }
        }
    }
    ImGui::EndChild();
    // ---- the bottom row: the name box (not for folders), OK, Cancel, any error
    if (mode_ != FOLDER) {
        ImGui::SetNextItemWidth(-200);   // leaves 200 pixels (not scaled by g_ui) for the buttons
        ImGui::InputText("##name", name_, sizeof(name_));
        ImGui::SameLine();
    }
    bool ok = ImGui::Button(mode_ == SAVE ? "Save" : mode_ == FOLDER ? "Use this folder" : "Open", ImVec2(110 * g_ui, 0));
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80 * g_ui, 0)) || !open) {   // Cancel or the close box
        visible_ = false;
        ImGui::CloseCurrentPopup();
    }
    if (!error_.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", error_.c_str());
    if (ok) {
        std::string result;   // the chosen path ("" = not yet)
        if (mode_ == FOLDER) result = dir_;
        else {
            std::string n = name_;
            if (n.empty()) error_ = "type a file name";
            else {
                // saving: add the first listed extension if the name lacks it
                // (an existing file is overwritten without asking)
                if (mode_ == SAVE && !ext_.empty() && lower(fs::path(n).extension().string()) != ext_.substr(0, ext_.find(' ')))
                    n += ext_.substr(0, ext_.find(' '));
                result = (fs::path(dir_) / n).string();
                if (mode_ == OPEN && !fs::exists(result, ec)) {
                    error_ = "no such file";
                    result.clear();
                }
            }
        }
        if (!result.empty()) {
            visible_ = false;
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            auto cb = on_ok_;   // a copy: the callback may call open() and replace on_ok_
            cb(result);
            return;
        }
    }
    ImGui::EndPopup();
}
