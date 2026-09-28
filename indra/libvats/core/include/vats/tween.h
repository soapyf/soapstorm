// Viewport Avatar Toolset - tween (breakdown) keys, pose blend on apply and easing presets.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 7.6 (TW-1..TW-3). All of them edit the clip in place; callers wrap them in
// an undo step.
#pragma once

#include <string>
#include <vector>

#include "vats/clip.h"
#include "vats/curve_ops.h"
#include "vats/rig.h"

namespace vats {

// TW-1: the tracks a tween at frame keys. Each of tracks (bone, "pin:" and "ik." names, as the selection
// gives them) is kept, except that a bone of a limb that is IK-on at frame (blend >= 0.5) is replaced by
// that limb's "ik." track, the way Set Key keys an IK limb's target and pole (AM-31).
std::vector<std::string> tween_tracks(const Rig& rig, const Clip& clip, double frame,
                                      const std::vector<std::string>& tracks);

enum class TweenMode {
    Breakdown,  // t of the way from the previous key's pose to the next key's
    Relax,      // t of the way from the track's key at frame towards the curve its neighbours make without it
};

// Tween limits: t may run 20% past either key.
inline constexpr double kTweenMin = -0.2, kTweenMax = 1.2;

// TW-1: keys each track at frame. Rotations slerp, then take the Euler triple nearest the curve (AM-30);
// positions and IK poles lerp; IK blend is never touched. Breakdown skips a track without a key on both
// sides of frame; Relax skips a track without a key at frame. Returns the number of tracks keyed.
int tween(Clip& clip, const std::vector<std::string>& tracks, double frame, double t, TweenMode mode);

// TW-2: blends the pose a library pose, hand pose or Paste Pose keyed at frame. clip is after (the clip
// with the pose applied) or an earlier blend of the same two; every track that differs between before and
// after is reset to after's and keyed at frame amount of the way from before's pose there to after's
// (0 = as before, 1 = as applied, 1.5 = pushed 50% further), with TW-1's rules. Returns the number of
// tracks keyed.
int blend_pose(Clip& clip, const Clip& before, const Clip& after, double frame, double amount);

// TW-3: easing presets for the segments between selected graph keys.
enum class EaseShape { Quad, Cubic, Sine, Back, Elastic, Bounce };
enum class EaseDir { In, Out, InOut };

// The eased fraction of the way at t in [0, 1]: 0 at 0, 1 at 1. Back and Elastic overshoot.
double ease(EaseShape shape, EaseDir dir, double t);
// Quad, Cubic and Sine become Bezier handles; Back, Elastic and Bounce are baked to a Linear key per frame.
bool ease_is_baked(EaseShape shape);

// Shapes the segment between each pair of neighbouring selected keys on a curve; a curve with one selected
// key shapes the segment after it. Handles: the segment's facing handles become Free and are set to the
// shape (Quad and Cubic exactly, Sine and In-Out a close cubic fit). Baked: the segment's start key turns
// Linear and gets a Linear key on every whole frame inside the segment. sel is updated to the same keys'
// new indices. Returns the number of segments shaped.
int apply_ease(Clip& clip, std::vector<KeyRef>& sel, EaseShape shape, EaseDir dir);

}  // namespace vats
