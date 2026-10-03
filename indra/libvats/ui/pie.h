// Viewport Avatar Toolset - the Tab pie: a ring of the common tools at the pointer, picked by direction.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// The slots are data, so the rings can be made customisable later; the picking is plain geometry, apart from
// App so it can be tested. pie_menu_ui.cpp draws the pie and runs the slots.
#pragma once

#include <vector>

namespace vats {

// The eight directions, clockwise from straight up.
enum class PieDir { N, NE, E, SE, S, SW, W, NW };
inline constexpr int kPieSlots = 8;

enum class PieRing { Main, More };

struct PieSlot {
    const char* id;     // an editor action (run_action), a tool ("tool_move"), or "more" / "back" for the rings
    const char* label;
    const char* icon;
    PieDir dir;
    bool needs_selection;  // greyed while no bone or IK control is selected
};

// The slots of a ring, at most one per direction.
const std::vector<PieSlot>& pie_ring(PieRing ring);

struct PieItem {
    const PieSlot* slot;
    bool enabled;
};
// What the ring offers now: its slots, with those that need a selection greyed while there is none.
std::vector<PieItem> pie_items(PieRing ring, bool has_selection);

// The direction the pointer points from the pie's centre (dx right, dy down, in pixels), or -1 while it is
// still inside the dead zone of that radius around the centre. Each direction owns a 45-degree sector.
int pie_dir_at(float dx, float dy, float dead_zone);

}  // namespace vats
