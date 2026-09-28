// Viewport Avatar Toolset - procedural idle layers: breathing and a loop-safe noise sway, baked to keys.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 11 (IL-1..IL-4). A layer adds a small rotation on top of whatever the curves
// hold: a breath (a smooth pitch on the chest, mTorso rising in phase) or a sway (gradient noise on three
// axes). Periods snap so a whole number of them fits the loop, and the noise is read around a circle that
// closes at the loop length, so loop-out lands exactly where loop-in starts.
#pragma once

#include <string>
#include <vector>

#include "vats/clip.h"

namespace vats {

// A new layer of the kind ("breath" or "sway") with its default bone set and settings.
IdleLayer idle_preset(const std::string& kind);

// Face and eye bones, attachment points and collision volumes never get an idle layer.
bool idle_bone_allowed(const Node& n);

// The period in frames after IL-2's snap: L / n for the whole n nearest to L / (period * fps), where L is
// the length of loop_range (the loop when the clip loops, else the whole clip). Phase 0 is at its start.
double idle_period_frames(const Clip& clip, const IdleLayer& layer);

// The layer's bones that it moves: in the skeleton and allowed.
std::vector<int> idle_nodes(const Skeleton& skel, const IdleLayer& layer);

// Adds the layer's motion at frame to pose (local rotations; mTorso's offset for a breath), skipping the
// nodes in exclude (the live preview passes bones in a limb that uses IK, whose keys IK overrides).
void apply_idle(const Skeleton& skel, const Clip& clip, const IdleLayer& layer, double frame, Pose& pose,
                const std::vector<int>& exclude = {});

// IL-3: bakes layer `which` (every layer when which < 0) onto its bones' curves, keeping the tracks from
// before it in IdleLayer::source. Baked layers stack in list order, so every layer already baked is baked
// again too, from the keys under it. One call is one undo step for the caller.
void bake_idle(Clip& clip, const Skeleton& skel, int which = -1);
// Takes layer `which` out of the stack: the keys go back to what they were before it, with the other
// baked layers baked again on top.
void unbake_idle(Clip& clip, const Skeleton& skel, int which);

}  // namespace vats
