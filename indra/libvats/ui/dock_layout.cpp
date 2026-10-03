// Viewport Avatar Toolset - the default dock layout (first run, View > Reset Layout).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "dock_layout.h"

#include <algorithm>

#include "imgui_internal.h"  // DockBuilder

namespace vats {

void build_default_layout(ImGuiID dock, bool world, const char* host_pane) {
    ImGui::DockBuilderRemoveNode(dock);
    ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dock, ImGui::GetMainViewport()->WorkSize);
    // Default proportions, widened to what the text needs at larger interface sizes.
    const ImVec2 ws = ImGui::GetMainViewport()->WorkSize;
    const float fs = ImGui::GetFontSize(), row = ImGui::GetFrameHeightWithSpacing();
    const float timeline_h = std::max(0.36f * 0.38f * ws.y, 3.9f * row);
    const float bottom_h = std::min(std::max(0.38f * ws.y, timeline_h + 7 * row), 0.55f * ws.y);
    const float left_w = std::min(std::max(0.18f * ws.x, 17 * fs), 0.3f * ws.x);
    const float right_w = std::min(std::max(0.24f * ws.x, 22 * fs), 0.32f * ws.x);
    ImGuiID main = dock, left, right, bottom, timeline;
    ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, bottom_h / ws.y, &bottom, &main);
    ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Down, std::min(timeline_h / bottom_h, 0.6f), &timeline, &bottom);
    ImGui::DockBuilderSplitNode(main, ImGuiDir_Left, left_w / ws.x, &left, &main);
    ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, right_w / (ws.x - left_w), &right, &main);
    ImGui::DockBuilderDockWindow("Bones", left);
    ImGui::DockBuilderDockWindow("Picker", left);
    ImGui::DockBuilderDockWindow("Inventory", left);
    ImGui::DockBuilderDockWindow("Properties", right);
    ImGui::DockBuilderDockWindow("Graph", bottom);
    ImGui::DockBuilderDockWindow("Dope Sheet", bottom);  // spec 08 DS: a tab beside the graph
    if (host_pane) ImGui::DockBuilderDockWindow(host_pane, bottom);
    ImGui::DockBuilderDockWindow("Timeline", timeline);
    if (!world) ImGui::DockBuilderDockWindow("Viewport", main);
    ImGui::DockBuilderFinish(dock);
}

namespace {

void fit_node(ImGuiDockNode* node, float k, float min_w) {
    if (!node || node->IsLeafNode()) return;
    ImGuiDockNode* kids[2] = {node->ChildNodes[0], node->ChildNodes[1]};
    const int axis = node->SplitAxis;
    for (int i = 0; i < 2; ++i) {
        ImGuiDockNode *side = kids[i], *centre = kids[1 - i];
        if (!side || !centre || side->HasCentralNodeChild || side->IsCentralNode() ||
            !(centre->HasCentralNodeChild || centre->IsCentralNode()) || side->SizeRef[axis] <= 0)
            continue;
        const float avail = node->Size[axis];
        float s = side->SizeRef[axis] * k;
        if (axis == ImGuiAxis_X && avail >= 2 * min_w) s = std::max(s, min_w);  // a side panel, if the view keeps min_w
        if (axis == ImGuiAxis_X) s = std::min(s, std::max(avail - min_w, side->SizeRef[axis]));
        else if (k != 1) s = std::min(s, std::max(0.6f * avail, side->SizeRef[axis]));  // grown, the view keeps 40%
        if (s != side->SizeRef[axis]) side->SizeRef[axis] = side->Size[axis] = s;
    }
    fit_node(kids[0], k, min_w);
    fit_node(kids[1], k, min_w);
}

}  // namespace

void fit_side_panels(ImGuiID dock, float k, float min_w) { fit_node(ImGui::DockBuilderGetNode(dock), k, min_w); }

bool is_view_drop(const ImGuiPayload* payload) {
    return payload && (payload->IsDataType("VATS_PROP") || payload->IsDataType("VATS_POSE") || payload->IsDataType("VATS_FILE"));
}

}  // namespace vats
