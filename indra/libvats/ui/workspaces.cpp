// Viewport Avatar Toolset - workspaces (a trial): job-focused dock layouts, each remembering its own arrangement.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "workspaces.h"

#include <algorithm>

#include "dock_layout.h"
#include "imgui_internal.h"  // DockBuilder

namespace vats {
namespace {

// Each workspace holds what its job reaches for every few seconds; the rest is a menu or F3 away.
// Pose: the toolbar lives in the Timeline, and a pose is still a key on a frame, so a short Timeline stays.
// Animate: All without the Inventory, the graph and dope sheet given the room.
// Face: the Face sliders and the Picker's face page to pick bones, Motion Capture for face tracking.
// Rig: mapping a model or rigging one from scratch, painting its weights, and joint limits (Properties holds the
// selected bone's). The Joint Offset Inspector is a click away (the Rig menu, the export) rather than docked here: its
// table took half the view on a first visit.
// Export: the export settings and what Second Life will complain about, beside a view to play it in.
const WorkspaceDef kDefs[kWorkspaceCount] = {
    {"all", "All", "Every panel, as before workspaces", {}, kTbAll},
    {"pose", "Pose", "Posing: the bones, the picker and the pose library around the view",
     {"Bones", "Picker", "Inventory", "Viewport", "Properties", "Timeline"},
     kTbSelect | kTbMove | kTbRotate | kTbAxes | kTbIkFk | kTbAutoIk | kTbLimits | kTbMirror | kTbSetKey},
    {"animate", "Animate", "Animating: the graph, the dope sheet and the timing tools",
     {"Bones", "Picker", "Viewport", "Properties", "Graph", "Dope Sheet", "Timeline"},
     kTbSelect | kTbMove | kTbRotate | kTbAxes | kTbIkFk | kTbAutoIk | kTbRetime | kTbSetKey | kTbBlocking | kTbTween},
    {"face", "Face", "Faces: the expression sliders, face tracking and the face bones, framed on the face",
     {"Face", "Picker", "Viewport", "Motion Capture", "Timeline"}, kTbSelect | kTbMove | kTbRotate | kTbSetKey},
    {"rig", "Rig", "Rigging: map a model's bones or rig one from scratch, paint weights, set joint limits",
     {"Bones", "Viewport", "###map-rig", "###rig-scratch", "###paint-weights", "###suggest-limits", "Properties"},
     0},
    {"export", "Export", "Exporting: the export settings and the Animation Check",
     {"Viewport", "Export", "Animation Check", "Timeline"}, 0},
};

const char* const kEditorPanels[] = {"Bones", "Picker", "Inventory", "Viewport", "Properties", "Timeline", "Graph",
                                     "Dope Sheet", "Export"};

}  // namespace

const WorkspaceDef& workspace_def(Workspace w) { return kDefs[std::clamp(int(w), 0, kWorkspaceCount - 1)]; }

bool workspace_from_id(const std::string& id, Workspace& out) {
    for (int i = 0; i < kWorkspaceCount; ++i)
        if (id == kDefs[i].id) return out = Workspace(i), true;
    return false;
}

bool is_editor_panel(const std::string& window) {
    return std::find(std::begin(kEditorPanels), std::end(kEditorPanels), window) != std::end(kEditorPanels);
}

bool properties_shows(Workspace w, PropSection s) {
    switch (s) {
        case PropSection::Bone:
        case PropSection::JointLimits: return true;
        case PropSection::Animation: return w != Workspace::Rig;
        case PropSection::Export: return w == Workspace::All;
    }
    return true;
}

bool properties_open_at_first(Workspace w, PropSection s) { return !(w == Workspace::Pose && s == PropSection::Animation); }

bool workspace_shows(Workspace w, const std::string& window, const std::vector<std::string>& extra) {
    if (w == Workspace::All) return window != "Export";  // All has Export in Properties and its dialog
    const auto& own = workspace_def(w).windows;
    return std::find(own.begin(), own.end(), window) != own.end() ||
           std::find(extra.begin(), extra.end(), window) != extra.end();
}

std::vector<const char*> workspace_layout_marks(Workspace w) {
    if (w == Workspace::All) return {"Graph", "Inventory"};
    std::vector<const char*> marks;
    // Its panels, and Rig's tools by their ids (a layout saved before they had ids is rebuilt, not left floating).
    for (const char* name : workspace_def(w).windows)
        if ((is_editor_panel(name) && std::string(name) != "Viewport") || name[0] == '#') marks.push_back(name);
    return marks;
}

void build_workspace_layout(ImGuiID dock, Workspace w, bool world, const char* host_pane) {
    if (w == Workspace::All) return build_default_layout(dock, world, host_pane);
    ImGui::DockBuilderRemoveNode(dock);
    ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
    const ImVec2 ws = ImGui::GetMainViewport()->WorkSize;
    ImGui::DockBuilderSetNodeSize(dock, ws);
    // The default layout's proportions, widened to what the text needs at larger interface sizes.
    const float fs = ImGui::GetFontSize(), row = ImGui::GetFrameHeightWithSpacing();
    const float timeline_h = std::max(0.36f * 0.38f * ws.y, 3.9f * row);
    const float left_w = std::min(std::max(0.18f * ws.x, 17 * fs), 0.3f * ws.x);
    const float right_w = std::min(std::max(0.24f * ws.x, 22 * fs), 0.32f * ws.x);
    ImGuiID main = dock, left = 0, right = 0, bottom = 0;
    auto split = [&](ImGuiDir dir, float size, float of, ImGuiID& out) {
        ImGui::DockBuilderSplitNode(main, dir, std::min(size / of, 0.6f), &out, &main);
    };
    auto put = [](ImGuiID node, std::initializer_list<const char*> names) {
        for (const char* n : names) ImGui::DockBuilderDockWindow(n, node);
    };
    switch (w) {
        case Workspace::Pose:
            split(ImGuiDir_Down, timeline_h, ws.y, bottom);
            split(ImGuiDir_Left, left_w, ws.x, left);
            split(ImGuiDir_Right, right_w, ws.x - left_w, right);
            put(left, {"Bones", "Picker", "Inventory"});
            put(right, {"Properties"});
            put(bottom, {"Timeline"});
            break;
        case Workspace::Animate: {
            // The graph and dope sheet get half the height or more: at large interface sizes their toolbar takes two
            // rows, and a user test found an 80-pixel curve area under them.
            const float bottom_h = std::clamp(timeline_h + 11 * row, 0.5f * ws.y, 0.6f * ws.y);
            ImGuiID timeline = 0;
            split(ImGuiDir_Down, bottom_h, ws.y, bottom);
            ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Down, std::min(timeline_h / bottom_h, 0.6f), &timeline, &bottom);
            split(ImGuiDir_Left, left_w, ws.x, left);
            split(ImGuiDir_Right, right_w, ws.x - left_w, right);
            put(left, {"Bones", "Picker"});
            put(right, {"Properties"});
            put(bottom, {"Graph", "Dope Sheet"});
            put(timeline, {"Timeline"});
            break;
        }
        case Workspace::Face:
            split(ImGuiDir_Down, timeline_h, ws.y, bottom);
            split(ImGuiDir_Left, std::max(left_w, 22 * fs), ws.x, left);
            split(ImGuiDir_Right, right_w, ws.x - left_w, right);
            put(left, {"Face", "Picker"});
            put(right, {"Motion Capture"});
            put(bottom, {"Timeline"});
            break;
        case Workspace::Rig: {
            // The tools on the right, the selected bone's limits (Properties) under them; the view keeps its height.
            ImGuiID props = 0;
            split(ImGuiDir_Left, std::max(left_w, 19 * fs), ws.x, left);
            split(ImGuiDir_Right, std::max(right_w, 26 * fs), ws.x - left_w, right);
            ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.4f, &props, &right);
            put(left, {"Bones"});
            put(right, {"###map-rig", "###rig-scratch", "###paint-weights", "###suggest-limits"});
            put(props, {"Properties"});
            break;
        }
        case Workspace::Export: {
            ImGuiID check = 0;
            split(ImGuiDir_Down, timeline_h, ws.y, bottom);
            split(ImGuiDir_Right, std::max(right_w, 26 * fs), ws.x, right);
            ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.4f, &check, &right);
            put(right, {"Export"});
            put(check, {"Animation Check"});
            put(bottom, {"Timeline"});
            break;
        }
        case Workspace::All: break;
    }
    // The viewer's own pane (its conversations) beside the panel on the right, the one every workspace keeps.
    if (host_pane) ImGui::DockBuilderDockWindow(host_pane, right ? right : bottom);
    if (!world) ImGui::DockBuilderDockWindow("Viewport", main);
    ImGui::DockBuilderFinish(dock);
}

bool switch_workspace_layout(std::map<std::string, std::string>& saved, Workspace from, Workspace to, FrontTabs& front) {
    saved[workspace_def(from).id] = ImGui::SaveIniSettingsToMemory();
    auto it = saved.find(workspace_def(to).id);
    if (from == to || it == saved.end() || it->second.empty()) return false;
    ImGui::LoadIniSettingsFromMemory(it->second.c_str(), it->second.size());
    front.clear();
    for (const ImGuiStoragePair& p : GImGui->DockContext.Nodes.Data)
        if (auto* node = static_cast<ImGuiDockNode*>(p.val_p); node && node->SelectedTabId) front.push_back({node->ID, node->SelectedTabId});

    return true;
}

void restore_front_tabs(FrontTabs& front) {
    // The focused panel's tab is always its node's front one, so the focus the last one took goes too.
    if (!front.empty()) ImGui::SetWindowFocus(nullptr);
    for (auto [id, tab] : front)
        if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(id)) {
            node->SelectedTabId = tab;  // the tab bar made this frame starts on it
            if (node->TabBar) node->TabBar->NextSelectedTabId = tab;
        }
    front.clear();
}

}  // namespace vats
