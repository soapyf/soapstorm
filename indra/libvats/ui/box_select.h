// Viewport Avatar Toolset - box (drag) selection of bones in the viewport: what falls in the box, and what the
// modifiers do per control preset.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#pragma once

#include <algorithm>
#include <vector>

#include "settings.h"
#include "view_math.h"

namespace vats {

enum class BoxMode { Replace, Add, Remove, Toggle };

// Shift and Ctrl as held at the release. b_key: a box Blender's B started. QAvimator starts the box with Ctrl, so Ctrl
// means nothing more there.
inline BoxMode box_mode(Preset p, bool shift, bool ctrl, bool b_key) {
    if (p == Preset::Industry) return shift && ctrl ? BoxMode::Add : shift ? BoxMode::Toggle : ctrl ? BoxMode::Remove : BoxMode::Replace;
    if (p == Preset::QAvimator) ctrl = false;
    if (ctrl && !shift) return BoxMode::Remove;  // as the graph editor's box: Ctrl removes, Shift (or both) adds
    return shift || b_key ? BoxMode::Add : BoxMode::Replace;
}

// The nodes (0..points.size()-1, those keep() accepts) whose point projects inside the screen rectangle with corners
// (ax, ay) and (bx, by), in either order. Points behind the camera never count.
template <class Keep>
std::vector<int> points_in_rect(const Projector& pr, const std::vector<Xform>& points, double ax, double ay, double bx,
                                double by, Keep keep) {
    const double x0 = std::min(ax, bx), x1 = std::max(ax, bx), y0 = std::min(ay, by), y1 = std::max(ay, by);
    std::vector<int> out;
    for (int i = 0; i < int(points.size()); ++i) {
        double x, y;
        if (keep(i) && pr.to_screen(points[i].pos, x, y) && x >= x0 && x <= x1 && y >= y0 && y <= y1) out.push_back(i);
    }
    return out;
}

// Applies a box's hits to a selection, keeping its order (the primary is last); new nodes go to the end.
inline void apply_box(std::vector<int>& sel, const std::vector<int>& hits, BoxMode mode) {
    if (mode == BoxMode::Replace) {
        sel = hits;
        return;
    }
    for (int n : hits) {
        auto it = std::find(sel.begin(), sel.end(), n);
        if (it == sel.end()) {
            if (mode != BoxMode::Remove) sel.push_back(n);
        } else if (mode != BoxMode::Add) {
            sel.erase(it);
        }
    }
}

}  // namespace vats
