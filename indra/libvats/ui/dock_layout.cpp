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

bool is_view_drop(const ImGuiPayload* payload) {
    return payload && (payload->IsDataType("VATS_PROP") || payload->IsDataType("VATS_POSE") || payload->IsDataType("VATS_FILE"));
}

}  // namespace vats
