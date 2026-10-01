// Viewport Avatar Toolset - motion paths: where a bone's tip travels over the frames around the playhead.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 17 (MP). Drawing is the app's or the viewer's job (through Host::projector); this picks
// the frames and evaluates the positions, so both show the same path.
#pragma once

#include <vector>

#include "vats/clip.h"
#include "vats/rig.h"

namespace vats {

struct MotionPathSettings {
    int before = 10, after = 10;  // frames each side of the playhead, 1..120
    bool whole_clip = false;      // every frame 0..end_frame instead
};

struct PathPoint {
    double frame = 0;
    Vec3 pos;            // the bone's tip, avatar space
    bool keyed = false;  // a key on the bone, a bone above it, their pins, or an IK control that moves it
};

struct MotionPath {
    int node = -1;
    std::vector<PathPoint> points;  // one per whole frame, in order
};

// The whole frames the path covers: the playhead's frame (rounded down) and before/after frames around it, inside
// 0..end_frame; or all of 0..end_frame.
std::vector<double> motion_path_frames(const Clip& clip, double frame, const MotionPathSettings& s);

// The tracks whose keys mark a path point as keyed for node: its own and its ancestors' bone and pin tracks, and the
// IK tracks of limbs that end on it or above it.
std::vector<std::string> motion_path_tracks(const Rig& rig, int node);

// One path per node, from the full evaluation (IK and pins included) at each frame; the tip is the node's global
// transform applied to its tail vector (skel[node].end, scaled by the shape as the view draws the bone).
std::vector<MotionPath> motion_paths(const Rig& rig, const Clip& clip, const Shape* shape, const std::vector<int>& nodes,
                                     double frame, const MotionPathSettings& s);

}  // namespace vats
