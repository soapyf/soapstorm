// Viewport Avatar Toolset - the dope sheet's rows: tracks grouped by body part, and the keyed frames of a row.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 17 (DS). Editing goes through curve_ops (move_keys, scale_keys, copy_keys, ...) on
// the same KeyRef selection the graph editor uses.
#pragma once

#include <string>
#include <vector>

#include "vats/curve_ops.h"
#include "vats/skeleton.h"

namespace vats {

// A body part and the tracks in it ("mShoulderLeft", "pin:mWristLeft", "ik.ArmLeft").
struct DopeRow {
    std::string label;  // "Left Arm", "Torso", ...; "Other" for a track no part owns
    std::vector<std::string> tracks;
};

// The tracks grouped by body_part_of (pose_ops.h): a pin track goes with its joint, an IK track with the part
// that lists it. Parts come in skeleton order (torso first), tracks in each part too, IK controls after the bones.
std::vector<DopeRow> dope_rows(const Skeleton& skel, const std::vector<std::string>& tracks);

// Every frame that holds a key on any channel of the tracks, sorted, frames within same_frame counted once
// (the summary row, a part's row, a bone's row).
std::vector<double> keyed_frames(const Clip& clip, const std::vector<std::string>& tracks);

// Every key of the tracks from frame f0 to f1 (both included, within same_frame).
std::vector<KeyRef> keys_between(const Clip& clip, const std::vector<std::string>& tracks, double f0, double f1);

}  // namespace vats
