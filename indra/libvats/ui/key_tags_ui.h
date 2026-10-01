// Viewport Avatar Toolset - how a tagged key looks, shared by the timeline, the graph and a dope sheet.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 24 (KT-1). Untagged keys keep each view's own mark.
#pragma once

#include "imgui.h"
#include "vats/fcurve.h"

namespace vats {

// Extreme red, Breakdown teal, Hold violet.
ImU32 key_tag_colour(KeyTag tag);
// A tagged key's mark centred at c, r across half its height: Extreme a diamond, Breakdown a circle, Hold a bar twice
// as wide as tall. fill: the tag's colour, or the view's selected-key colour; outline adds a dark rim.
void draw_key_tag_mark(ImDrawList* dl, ImVec2 c, float r, KeyTag tag, ImU32 fill, bool outline = true);

}  // namespace vats
