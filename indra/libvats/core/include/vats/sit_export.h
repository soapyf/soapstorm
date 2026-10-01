// Viewport Avatar Toolset - sit-system lines for couples and groups (AVsitter2 AVpos, nPose V4).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 GR-2 (sit-system lines). Both systems seat each avatar by setting its PRIM_POS_LOCAL and
// PRIM_ROT_LOCAL from a notecard line: a position in metres in the root prim's frame, and a rotation as
// Euler degrees that the script turns into a rotation with llEuler2Rot(v * DEG_TO_RAD) (X, then Y, then Z
// about fixed axes; the same order as euler_to_quat). Only the text format is taken from them, no code.
#pragma once

#include <string>
#include <vector>

#include "vats/math.h"

namespace vats {

// Where the shared sit target (the scene's origin) is in the furniture root prim's frame.
struct SitRoot {
    Vec3 pos;  // metres
    Vec3 rot;  // Euler degrees, as in the build window and llEuler2Rot
    Xform xform() const { return {euler_to_quat(rot), pos}; }
};

struct Sitter {
    std::string name;   // the actor: AVsitter's SITTER label
    std::string anim;   // the animation's inventory name (the export name without ".anim")
    Xform place;        // from the sit target (Actor::placement)
};

// Where a sitter goes in the root's frame, as those scripts want it: position and Euler degrees.
void sitter_in_root(const Sitter& s, const SitRoot& root, Vec3& pos, Vec3& euler_deg);

// AVsitter2 AVpos notecard lines, one SITTER section per sitter, each with a SYNC pose named `pose` and its
// {pose}<pos><rot> line. `pose` is cut to AVsitter's 23-character button limit.
std::string avsitter_lines(const std::string& pose, const std::vector<Sitter>& sitters, const SitRoot& root);

// nPose V4 lines for a SET card, one XANIM per seat (seats count from 1). The .init card needs SEAT_INIT|<count>.
std::string npose_lines(const std::vector<Sitter>& sitters, const SitRoot& root);

}  // namespace vats
