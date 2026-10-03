// Viewport Avatar Toolset - workspaces (a trial): the tabs, switching, View > Workspaces and the Export panel.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// The data and the layout switching are in workspaces.cpp. A switch keeps the document, the selection and the
// camera (Face frames the face, and leaving it puts the camera back); only the docked windows change.
#include <algorithm>
#include <cmath>
#include <string>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui_internal.h"
#include "theme.h"
#include "workspaces.h"

namespace vats {
namespace {

// The tabs' order: the jobs in the order work goes, then All, the full layout, apart at the end.
constexpr Workspace kTabOrder[kWorkspaceCount] = {Workspace::Pose, Workspace::Animate, Workspace::Face,
                                                  Workspace::Rig,  Workspace::Export,  Workspace::All};
// Their actions, by Workspace (Find a Tool, Keyboard Shortcuts).
const char* const kActionIds[kWorkspaceCount] = {"workspace_all", "workspace_pose", "workspace_animate",
                                                 "workspace_face", "workspace_rig", "workspace_export"};
// The editor panels a workspace may leave out: their titles and their show_window names.
constexpr std::pair<const char*, const char*> kPanels[] = {
    {"Bones", "bones"},       {"Picker", "picker"}, {"Inventory", "inventory"},   {"Properties", "properties"},
    {"Timeline", "timeline"}, {"Graph", "graph"},   {"Dope Sheet", "dope-sheet"}};

}  // namespace

Workspace App::workspace() const {
    Workspace w = Workspace::All;
    return settings_.workspaces && workspace_from_id(settings_.workspace, w) ? w : Workspace::All;
}

bool App::cli_workspace(const std::string& id) {
    Workspace w;
    if (!workspace_from_id(id, w)) return false;
    switch_workspace(w, true);
    return true;
}

bool* App::workspace_tool_flag(const std::string& window) {
    if (window == "Face") return &show_face_;
    if (window == "Motion Capture") return &show_mocap_;
    if (window == "###map-rig") return &show_rig_map_;
    if (window == "###rig-scratch") return &show_rig_scratch_;
    if (window == "###paint-weights") return &show_paint_;
    if (window == "###suggest-limits") return &show_suggest_limits_;
    if (window == "Joint Offset Inspector") return &show_joint_inspector_;
    if (window == "Animation Check") return &show_check_;
    return nullptr;
}

void App::open_workspace_tools(Workspace w) {
    for (const char* name : workspace_def(w).windows)
        if (bool* shown = workspace_tool_flag(name); shown && !*shown) {
            *shown = true;
            ws_opened_.push_back(name);
        }
}

void App::switch_workspace(Workspace to, bool on) {
    const Workspace from = workspace();
    settings_.workspaces = on;
    if (on) settings_.workspace = workspace_def(to).id;
    const Workspace now = workspace();
    save_settings();
    if (now == from) return;

    // Leaving: the tool windows it opened close (a model being mapped stays), and the face's framing goes.
    for (const std::string& name : std::exchange(ws_opened_, {}))
        if (bool* shown = workspace_tool_flag(name); shown && !(name == "###map-rig" && rig_map_ui_)) *shown = false;
    if (from == Workspace::Face) {
        if (ws_face_camera_) {
            Camera back = *std::exchange(ws_face_camera_, std::nullopt);
            back.fov = camera_.fov, back.ortho = camera_.ortho, back.min_eye_z = camera_.min_eye_z;
            cam_glide_.target_cam = back, cam_glide_.active = true, cam_glide_.fixed_eye = false;
        }
        cli_picker("body"), pending_tab_.clear();
    }
    if (!ws_switch_from_) ws_switch_from_ = from;  // two switches in one frame: the first one's layout is the one left

    // Entering: its tool windows, and Face looks at the face from the front, on the Picker's face page.
    open_workspace_tools(now);
    if (now == Workspace::Face) {
        ws_face_camera_ = camera_;
        ws_frame_face_ = true;  // next frame, once the pose is evaluated
        cli_picker("face"), pending_tab_ = "Face";
    }
    status(std::string(workspace_def(now).label) + " workspace");
}

void App::set_workspaces_on(bool on) {
    Workspace last = Workspace::All;
    workspace_from_id(settings_.workspace, last);
    switch_workspace(last, on);
}

void App::workspace_frame_start() {
    if (std::exchange(ws_frame_face_, false)) {  // Face: the head and shoulders, from the front and a little aside
        const int head = skel_.find("mHead"), skull = skel_.find("mSkull");
        if (head >= 0 && head < int(globals_.size())) {
            const Vec3 at = skull >= 0 && skull < int(globals_.size()) ? (globals_[head].pos + globals_[skull].pos) * 0.5
                                                                        : globals_[head].pos;
            cam_glide_.look_from(camera_, Vec3{1, 0.25, 0.08});
            cam_glide_.apply_input(camera_, [&](Camera& c) {
                c.target = at;
                c.distance = 0.95 * std::tan(Camera::kFov / 2) / std::tan(c.fov / 2);
            });
            if (headless_) update_camera_animation(1);  // a screenshot: there, not on the way
        }
    }
    restore_front_tabs(ws_front_tabs_);
    update_maximised();
    if (!ws_switch_from_) return;
    // Its own arrangement back, or its first layout when it has none yet (Reset Layout rebuilds it).
    const Workspace from = *std::exchange(ws_switch_from_, std::nullopt);
    if (!switch_workspace_layout(settings_.workspace_layouts, from, workspace(), ws_front_tabs_)) reset_layout_ = true;
    if (!maximised_.empty()) settings_.workspace_layouts[workspace_def(from).id] = maximised_ini_, end_maximised(false);
    save_settings();
}

// View > Maximise Panel (Ctrl+Space, as in Blender): the docked panel under the pointer comes out of the dock and
// fills the window over the others; again puts the whole layout back as it was (the way a workspace switch does).
void App::toggle_maximised_panel() {
    if (!maximised_.empty()) return void(restore_maximised_ = true);
    ImGuiWindow* w = GImGui->HoveredWindow ? GImGui->HoveredWindow->RootWindow : nullptr;
    if (!w || !w->DockIsActive)
        return status("Point at a docked panel, then " + key_hint("maximise_panel") + " fills the window with it");
    maximised_ini_ = ImGui::SaveIniSettingsToMemory();
    maximised_ = w->Name;
    maximised_placed_ = false;
    maximised_ini_file_ = std::exchange(ImGui::GetIO().IniFilename, nullptr);
    ImGui::DockContextQueueUndockWindow(GImGui, w);
    const std::string name = w->Name;
    status(name.substr(0, name.find("##")) + " fills the window; " + key_hint("maximise_panel") + " again puts the panels back");
}

void App::end_maximised(bool restore) {
    if (restore) ImGui::LoadIniSettingsFromMemory(maximised_ini_.c_str(), maximised_ini_.size()), status("Every panel back in its place");
    ImGui::GetIO().IniFilename = maximised_ini_file_;
    maximised_.clear(), maximised_ini_.clear();
    restore_maximised_ = false;
}

void App::update_maximised() {
    if (maximised_.empty()) return;
    if (restore_maximised_) return end_maximised(true);
    ImGuiWindow* w = ImGui::FindWindowByName(maximised_.c_str());
    if (!w || w->DockIsActive || w->DockId) return;  // not out of the dock yet (next frame), or docked again by hand
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetWindowPos(w, vp->WorkPos);
    ImGui::SetWindowSize(w, vp->WorkSize);
    if (!std::exchange(maximised_placed_, true)) ImGui::FocusWindow(w);
}

bool App::workspace_layout_known() const {
    for (const char* name : workspace_layout_marks(workspace()))
        if (!ImGui::FindWindowSettingsByID(ImHashStr(name))) return false;
    return true;
}

bool App::panel_shown(const char* window) const {
    static const std::vector<std::string> none;
    auto it = settings_.workspace_extra.find(workspace_def(workspace()).id);
    return workspace_shows(workspace(), window, it == settings_.workspace_extra.end() ? none : it->second);
}

void App::reveal_panel(const std::string& name) {
    for (auto [title, id] : kPanels)
        if (name == id && !panel_shown(title)) {
            settings_.workspace_extra[workspace_def(workspace()).id].push_back(title);
            save_settings();
        }
}

void App::add_workspace_actions(const std::function<void(const char*, Action)>& add) {
    auto step = [this](int dir) {
        int at = 0;
        while (kTabOrder[at] != workspace()) ++at;
        switch_workspace(kTabOrder[(at + dir + kWorkspaceCount) % kWorkspaceCount], true);
    };
    auto off = [this]() -> const char* { return settings_.workspaces ? nullptr : "Workspaces are off (View > Workspaces)"; };
    // No keys of their own: Blender's Ctrl+Page Up / Down are ImGui's for the tabs of a docked panel. Keyboard
    // Shortcuts can give them some.
    add("workspace_next", {"Next Workspace", 0, 0, false, [step] { step(+1); }, off});
    add("workspace_prev", {"Previous Workspace", 0, 0, false, [step] { step(-1); }, off});
    static const char* const labels[kWorkspaceCount] = {"All Panels Workspace", "Pose Workspace", "Animate Workspace",
                                                        "Face Workspace", "Rig Workspace", "Export Workspace"};
    for (int w = 0; w < kWorkspaceCount; ++w)
        add(kActionIds[w], {labels[w], 0, 0, false, [this, w] { switch_workspace(Workspace(w), true); }, {}});
}

namespace {

// Each tab's icon and colour, by Workspace: the Commodore 64 badge stripes in work order, All in the text colour.
const char* const kTabIcons[kWorkspaceCount] = {icon::kAllPanels, icon::kWalkTest, icon::kClips,
                                                icon::kFace,      icon::kIkFk,     icon::kUpload};
constexpr ImU32 kTabHues[kWorkspaceCount] = {0,                             IM_COL32(74, 158, 234, 255),
                                             IM_COL32(76, 192, 90, 255),    IM_COL32(242, 220, 74, 255),
                                             IM_COL32(245, 137, 42, 255),   IM_COL32(229, 72, 77, 255)};
ImU32 tab_hue(Workspace w) { return kTabHues[int(w)] ? kTabHues[int(w)] : ImGui::GetColorU32(ImGuiCol_Text); }

}  // namespace

// The tabs sit in the menu bar after the menus, as Blender's do in its top bar, so they cost no row of their own.
// Icon and name; the current one lights its icon in the workspace's colour over a short bar at the bar's foot.
void App::draw_workspace_tabs() {
    if (!settings_.workspaces) return;
    const float fs = ImGui::GetFontSize(), pad = 0.55f * fs, gap = 0.1f * fs, icon_w = ImGui::CalcTextSize(icon::kFace).x,
                icon_gap = 0.35f * fs;
    const Workspace current = workspace();
    // Narrower bars drop the names but the current one's, then fall back to a menu.
    auto tab_w = [&](Workspace w, bool named) {
        return icon_w + (named ? icon_gap + ImGui::CalcTextSize(workspace_def(w).label).x : 0) + 2 * (named ? pad : 0.6f * pad);
    };
    auto row_w = [&](bool all_named) {
        float total = gap * 7;  // the hairline before All
        for (Workspace w : kTabOrder) total += tab_w(w, all_named || w == current) + gap;
        return total;
    };
    ImGuiWindow* bar = ImGui::GetCurrentWindow();
    const float start = ImGui::GetCursorPosX() + 1.5f * fs, room = bar->Size.x - start - 1.5f * fs;
    const bool names = row_w(true) <= room;
    if (!names && row_w(false) > room) {
        // Too narrow: one button with the current workspace, the others in its menu.
        auto label_of = [](Workspace w) { return std::string(kTabIcons[int(w)]) + "  " + workspace_def(w).label; };
        ImGui::SetCursorPosX(start);
        const std::string label = label_of(current) + " " + icon::kDown + "###ws_menu";
        if (begin_menu_icon(nullptr, label.c_str())) {
            for (Workspace w : kTabOrder)
                if (ImGui::MenuItem(label_of(w).c_str(), nullptr, w == current)) switch_workspace(w, true);
            ImGui::EndMenu();
        }
        return;
    }
    ImGui::SetCursorPosX(start);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = bar->MenuBarHeight, y = bar->Pos.y, foot = y + h;
    for (Workspace w : kTabOrder) {
        const WorkspaceDef& d = workspace_def(w);
        if (w == Workspace::All) {  // a hairline between the jobs and the full layout
            const float x = ImGui::GetCursorScreenPos().x + gap * 3;
            dl->AddLine(ImVec2(x, y + h * 0.3f), ImVec2(x, y + h * 0.7f), ImGui::GetColorU32(ImGuiCol_Border));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap * 7);
        }
        const bool on = w == current, named = names || on;
        const ImVec2 text = ImGui::CalcTextSize(d.label);
        const float w_tab = tab_w(w, named), left = named ? pad : 0.6f * pad;
        const ImVec2 p0(ImGui::GetCursorScreenPos().x, y);
        ImGui::SetCursorScreenPos(p0);
        ImGui::PushID(d.id);
        const bool pressed = ImGui::InvisibleButton("##tab", ImVec2(w_tab, h));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::SetItemTooltip("%s%s%s", named ? "" : d.label, named ? "" : ": ", d.tip);
        key_badge(kActionIds[int(w)]);
        ImGui::PopID();
        if (hovered && !on) dl->AddRectFilled(p0, ImVec2(p0.x + w_tab, foot), ImGui::GetColorU32(ImGuiCol_FrameBg, 0.5f));
        const ImU32 fg = ImGui::GetColorU32(on || hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        const float ty = y + (h - text.y) * 0.5f;
        dl->AddText(ImVec2(p0.x + left, ty), on ? tab_hue(w) : fg, kTabIcons[int(w)]);
        if (named) dl->AddText(ImVec2(p0.x + left + icon_w + icon_gap, ty), fg, d.label);
        if (on) {  // the short bar: inset from the tab's ends, rounded on top
            const float t = std::max(2.f, 0.14f * fs);
            dl->AddRectFilled(ImVec2(p0.x + 0.6f * left, foot - t), ImVec2(p0.x + w_tab - 0.6f * left, foot), tab_hue(w), t,
                              ImDrawFlags_RoundCornersTop);
        }
        if (pressed && !on) switch_workspace(w, true);
        ImGui::SameLine(0, gap);
    }
}

// Paint Weights, Rig a Model from Scratch and Map Rig are the Rig workspace's modes, as Blender's weight painting is a
// mode its workspaces switch. Opened from a menu, F3 or the pie in another job's workspace, they take you to Rig; leaving
// Rig for another job stops the brush, so a drag meant to pose never paints. All keeps everything together, as before.
// The one just asked for (a menu, F3, the pie, --window) comes to the front of the Rig tabs, not whichever the workspace
// opened last.
void App::follow_tool_workspaces() {
    bool App::* const tools[] = {&App::show_paint_, &App::show_rig_scratch_, &App::show_rig_map_};
    const char* const ids[] = {"###paint-weights", "###rig-scratch", "###map-rig"};
    const Workspace now = workspace();
    int asked = -1;  // newly shown, and not by a workspace opening its tools
    for (size_t i = 0; i < std::size(tools); ++i)
        if (this->*tools[i] && !tools_were_shown_[i] && std::find(ws_opened_.begin(), ws_opened_.end(), ids[i]) == ws_opened_.end())
            asked = int(i);
    if (settings_.workspaces && asked >= 0 && now != Workspace::All && now != Workspace::Rig) switch_workspace(Workspace::Rig, true);
    for (size_t i = 0; i < std::size(tools); ++i) tools_were_shown_[i] = this->*tools[i];  // with the ones Rig opened
    if (asked >= 0) pending_tab_ = ids[asked];
    if (settings_.workspaces) {
        if (last_workspace_ == Workspace::Rig && now != Workspace::Rig && now != Workspace::All)
            stop_painting("Painting stopped in " + std::string(workspace_def(now).label) + ": back in Rig, the brush is there");
    }
    last_workspace_ = workspace();
}

void App::draw_workspace_menu() {
    if (!begin_menu_icon(icon::kAddLayer, "Workspaces")) return;
    const Workspace current = workspace();
    for (Workspace w : kTabOrder) {
        const WorkspaceDef& d = workspace_def(w);
        if (w == Workspace::All) ImGui::Separator();
        if (ImGui::MenuItem(d.label, key_hint(kActionIds[int(w)]).c_str(), settings_.workspaces && w == current)) switch_workspace(w, true);
        ImGui::SetItemTooltip("%s", d.tip);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Workspace Tabs (Trial)", nullptr, settings_.workspaces)) set_workspaces_on(!settings_.workspaces);
    ImGui::SetItemTooltip("Tabs that show only the panels one job needs. Off: every panel, as before. Every tool "
                          "stays in the menus and Find a Tool (F3) either way.");
    if (current != Workspace::All) {  // a panel this workspace leaves out, brought in
        bool any = false;
        for (auto [title, name] : kPanels)
            if (!panel_shown(title)) {
                if (!any) subheading("Show in this workspace");
                any = true;
                if (ImGui::MenuItem(title)) show_window(name);
            }
    }
    ImGui::EndMenu();
}

void App::draw_export_panel() {
    if (!panel_shown("Export")) return;
    if (!ImGui::Begin("Export")) return ImGui::End();
    help_button("export-to-second-life");
    draw_export_section(-1);
    ImGui::End();
}

}  // namespace vats
