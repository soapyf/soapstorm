// Viewport Avatar Toolset - workspaces (a trial): job-focused dock layouts, each remembering its own arrangement.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// A workspace shows the panels one job needs and a shorter toolbar; All is the full layout. Every command and
// tool window stays reachable from the menus and Find a Tool in any workspace. The data and the layout switching
// live here, apart from App, so they can be tested on a headless ImGui context; workspace_ui.cpp is the App side.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "imgui.h"

namespace vats {

enum class Workspace { All, Pose, Animate, Face, Rig, Export };
inline constexpr int kWorkspaceCount = 6;

// The Timeline's tool buttons. A workspace shows some of them, always in this one order.
enum ToolbarButton : unsigned {
    kTbSelect = 1u << 0,
    kTbMove = 1u << 1,
    kTbRotate = 1u << 2,
    kTbScale = 1u << 3,
    kTbAxes = 1u << 4,
    kTbIkFk = 1u << 5,
    kTbAutoIk = 1u << 6,
    kTbLimits = 1u << 7,
    kTbMirror = 1u << 8,
    kTbRetime = 1u << 9,
    kTbSetKey = 1u << 10,
    kTbBlocking = 1u << 11,
    kTbTween = 1u << 12,
    kTbAll = (1u << 13) - 1,
};

struct WorkspaceDef {
    const char* id;     // in settings.json
    const char* label;  // on its tab
    const char* tip;    // the tab's tooltip: what the workspace is for
    // The windows it docks, by title: the editor's panels and the tool windows it opens on entering.
    std::vector<const char*> windows;
    unsigned toolbar;  // ToolbarButton bits
};

const WorkspaceDef& workspace_def(Workspace w);
bool workspace_from_id(const std::string& id, Workspace& out);

// Properties' sections. Each workspace shows the ones its job needs: the bone and its limits everywhere, the clip's
// settings (Animation) everywhere but Rig, and folded in Pose, where a pose rarely needs them but the tutorials'
// "Properties > Animation" must still be found. The export settings live in the Export panel and dialog, and in
// Properties only in All, which has no Export panel.
enum class PropSection { Bone, JointLimits, Animation, Export };
bool properties_shows(Workspace w, PropSection s);
bool properties_open_at_first(Workspace w, PropSection s);

// The editor's own panels, which have no close box: All draws every one, another workspace only its own and
// the ones the user brought in since (extra, by title).
bool is_editor_panel(const std::string& window);
bool workspace_shows(Workspace w, const std::string& window, const std::vector<std::string>& extra);

// Rebuilds the dockspace as w's first layout (All: build_default_layout). world and host_pane as there.
void build_workspace_layout(ImGuiID dock, Workspace w, bool world, const char* host_pane);
// The panels whose saved place says the layout in the .ini is w's own (else the first frame rebuilds it).
std::vector<const char*> workspace_layout_marks(Workspace w);

// The tab in front of each dock node, by node: (node, tab).
using FrontTabs = std::vector<std::pair<ImGuiID, ImGuiID>>;

// Leaving `from` for `to`, at the start of a frame before the dockspace is submitted: from's arrangement (ImGui's
// whole .ini text) goes into saved, and to's comes back from it. False when to has none yet: build its layout
// with build_workspace_layout once the dockspace is submitted. front: the loaded layout's front tabs, for
// restore_front_tabs.
bool switch_workspace_layout(std::map<std::string, std::string>& saved, Workspace from, Workspace to, FrontTabs& front);
// The frame after a switch, before the dockspace: the panels coming back each took the focus as they appeared, and
// the last one its node's front tab; the loaded layout's front tabs go back. Empties front.
void restore_front_tabs(FrontTabs& front);

}  // namespace vats
