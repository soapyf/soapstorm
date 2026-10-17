// Viewport Avatar Toolset - resizing the skeleton: the body an animation is made for, made bigger or smaller as a whole.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 31 (RS). The size is a factor kept in the clip's export settings ("size"), beside the bake
// shape. The resized body is the base shape scaled about the actor's origin, the ground under the avatar, so the soles
// stay on it; the hip's moves can be scaled with it (scale_position_keys) so a crouch or a sit keeps its depth.
#pragma once

#include "vats/clip.h"
#include "vats/json.h"
#include "vats/skeleton.h"

namespace vats {

// RS-1: the range of the size, and the export settings' "size" (absent, not a number or 1 = as is), clamped to it.
constexpr double kResizeMin = 0.1, kResizeMax = 10;
double body_resize(const Json& export_settings);

// RS-2: base (null = the unshaped skeleton) scaled by s about the point (0, 0, ground_z), the ground under the avatar:
// every node's scale times s, the root-level nodes' offsets moved so each stands s times as far from that point, rig
// tails times s. At rest every node's global position is that point plus s times its offset from it, and a mesh skinned
// with the result is the base's mesh scaled the same way. ground_z is 0 for VATs' own bodies (their soles rest at 0);
// a worn avatar whose legs differ from SL's stands its soles elsewhere (RS-7).
Shape resized_shape(const Skeleton& skel, const Shape* base, double s, double ground_z = 0);

// RS-5: the keys made to fit a body r times the size. Multiplies by r, values and handles, the position channels of
// the root-level nodes, the IK controllers' targets and poles and the pins' offset tracks in every track map
// (for_each_track_map), then the
// positions of pins held in the world (avatar space) and the offsets of pins to the clip's own bones. Rotations,
// cross-actor pins and props are left alone. Bones below the hip need nothing: their moves grow with their parent's
// scale.
struct ResizedKeys {
    int channels = 0;  // position curves scaled
    int pins = 0;
};
ResizedKeys scale_position_keys(const Skeleton& skel, Clip& clip, double r);

}  // namespace vats
