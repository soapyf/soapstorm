// Viewport Avatar Toolset - onion skinning: ghost poses around the current frame.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 7.1 (ON-1). Drawing is the app's or the viewer's job; this picks the frames
// and evaluates the poses, so both show the same ghosts.
#pragma once

#include <vector>

#include "vats/clip.h"
#include "vats/pose_ops.h"
#include "vats/rig.h"

namespace vats {

struct OnionSettings {
    int before = 2, after = 2;  // ghosts on each side, 0..5
    int step = 1;               // frames between ghosts (ignored when keyed_only)
    bool keyed_only = false;    // ghosts only at frames that hold a key
};

struct OnionFrame {
    double frame = 0;
    int offset = 0;     // -1, -2, ... before the current frame; 1, 2, ... after
    float weight = 1;   // 1 nearest, falling towards 0 with distance (for opacity)
};

// The frames to show, nearest first on each side, inside 0..end_frame, never the current frame itself.
std::vector<OnionFrame> onion_frames(const Clip& clip, double frame, const OnionSettings& s);

struct OnionGhost {
    OnionFrame at;
    std::vector<Xform> globals;  // the full evaluation (IK and pins included) at that frame
};

std::vector<OnionGhost> onion_ghosts(const Rig& rig, const Clip& clip, double frame, const Shape* shape,
                                     const OnionSettings& s);

// ON-5: a library pose drawn in place as a target to match: the clip's pose at frame with the pose applied there
// (a part pose over the rest of the body as it is), fully evaluated. The clip is not changed.
std::vector<Xform> pose_ghost(const Rig& rig, const Clip& clip, double frame, const Shape* shape, const LibraryItem& pose);

// The target ghost: another animation drawn over the avatar as a pose to match by eye. Its frame at the edited
// clip's frame: the same frame number, held at 0 before its start and at its last frame after its end.
double target_frame(const Clip& target, double frame);
// How far a bone is from where the target has it, in degrees (0..180): the angle between its rotations relative to
// its parent in two poses of one skeleton (globals), so a bone matches once its own rotation does, whatever its
// parent does. The root bone's is its world rotation.
double bone_angle_apart(const Skeleton& skel, const std::vector<Xform>& a, const std::vector<Xform>& b, int bone);

}  // namespace vats
