// Viewport Avatar Toolset - the body picker's layout: clickable regions of an avatar outline and their bones.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 24 (PK-1). Data only: the UI (ui/picker_ui.cpp) draws the regions and selects their bones.
#pragma once

#include <string>
#include <vector>

#include "vats/skeleton.h"

namespace vats {

// Front and Back show the whole body (Back mirrored, with the wings and tail); the hands and the face are sub-pickers.
enum class PickerView { Front, Back, LeftHand, RightHand, Face };
inline constexpr int kPickerViewCount = 5;
const char* picker_view_name(PickerView v);  // "Front", "Back", "Left Hand", "Right Hand", "Face"

struct PickerRegion {
    std::string label;               // "Left Forearm"
    std::vector<std::string> bones;  // selected together
    float x = 0, y = 0, w = 0, h = 0;  // in a canvas kPickerWidth wide and picker_height(view) tall
    bool round = false;              // an ellipse, else a rounded box
};

inline constexpr float kPickerWidth = 1.0f;
float picker_height(PickerView v);

// The regions of a view, drawn in this order (a later one wins a click where they overlap).
const std::vector<PickerRegion>& picker_regions(PickerView v);

// The skeleton nodes of a region's bones that the skeleton has.
std::vector<int> region_nodes(const Skeleton& skel, const PickerRegion& r);

}  // namespace vats
