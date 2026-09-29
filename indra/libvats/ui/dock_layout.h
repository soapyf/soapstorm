// Viewport Avatar Toolset - the default dock layout (first run, View > Reset Layout).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#pragma once

#include "imgui.h"

namespace vats {

// Rebuilds the dockspace `dock` as the default layout and docks every panel into it. `world`: the viewer, whose
// centre stays empty for the world; `host_pane`: the viewer's own pane (docked beside the graph), or null.
void build_default_layout(ImGuiID dock, bool world, const char* host_pane);

// An Inventory drag (a prop, pose or file) that the 3D view takes. Not a dragged window: that one docks.
bool is_view_drop(const ImGuiPayload* payload);

}  // namespace vats
