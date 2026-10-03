// Viewport Avatar Toolset - the default dock layout (first run, View > Reset Layout).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#pragma once

#include "imgui.h"

namespace vats {

// Rebuilds the dockspace `dock` as the default layout and docks every panel into it. `world`: the viewer, whose
// centre stays empty for the world; `host_pane`: the viewer's own pane (docked beside the graph), or null.
void build_default_layout(ImGuiID dock, bool world, const char* host_pane);

// The panels around the 3D view, after the dockspace is submitted each frame: k scales each one's size along its
// split (Interface size changed by k, so the panels grow with their text), and the side panels are held at least
// min_w wide while the view keeps as much. The view itself (the central node) takes what is left.
void fit_side_panels(ImGuiID dock, float k, float min_w);

// An Inventory drag (a prop, pose or file) that the 3D view takes. Not a dragged window: that one docks.
bool is_view_drop(const ImGuiPayload* payload);

}  // namespace vats
