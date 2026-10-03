// Viewport Avatar Toolset - the Tab pie: a ring of the common tools at the pointer, picked by direction.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "pie.h"

#include <cmath>

#include "icons.h"

namespace vats {

// Placed for the hand: the tools used most sit on the quick horizontal flicks (Move left, Rotate right), Set Key
// straight up, and More straight down, where Blender's pies keep their way to more. Toggles pair across the
// bottom. The More ring keeps Back where More was, so a flick down and down again returns.
const std::vector<PieSlot>& pie_ring(PieRing ring) {
    static const std::vector<PieSlot> main = {
        {"key", "Set Key", icon::kSetKey, PieDir::N, true},
        {"ik_toggle", "IK / FK", icon::kIkFk, PieDir::NE, true},
        {"tool_rotate", "Rotate", icon::kRotate, PieDir::E, false},
        {"respect_joint_limits", "Limits", icon::kLocked, PieDir::SE, false},
        {"more", "More", icon::kMore, PieDir::S, false},
        {"auto_ik", "Auto IK", icon::kPull, PieDir::SW, false},
        {"tool_move", "Move", icon::kMove, PieDir::W, false},
        {"tool_select", "Select", icon::kSelect, PieDir::NW, false},
    };
    static const std::vector<PieSlot> more = {
        {"frame_selected", "Frame", icon::kFrameSelected, PieDir::N, true},
        {"relax", "Relax", icon::kRelax, PieDir::NE, true},
        {"reset_bone", "Reset Bone", icon::kUnbake, PieDir::E, true},
        {"edit_limits", "Edit Limits", icon::kEditLimits, PieDir::SE, true},
        {"back", "Back", icon::kUndo, PieDir::S, false},
        {"tool_scale", "Scale", icon::kScale, PieDir::SW, false},
        {"mirror_bone", "Mirror", icon::kMirror, PieDir::W, true},
        {"tween", "Tween", icon::kTween, PieDir::NW, true},
    };
    return ring == PieRing::Main ? main : more;
}

std::vector<PieItem> pie_items(PieRing ring, bool has_selection) {
    std::vector<PieItem> items;
    for (const PieSlot& s : pie_ring(ring)) items.push_back({&s, has_selection || !s.needs_selection});
    return items;
}

int pie_dir_at(float dx, float dy, float dead_zone) {
    if (dx * dx + dy * dy < dead_zone * dead_zone) return -1;
    // Clockwise from up, in eighths, each sector centred on its direction.
    const float a = std::atan2(dx, -dy);  // 0 up, pi/2 right
    const int sector = int(std::floor(a / (2 * 3.14159265f) * kPieSlots + 0.5f));
    return (sector % kPieSlots + kPieSlots) % kPieSlots;
}

}  // namespace vats
