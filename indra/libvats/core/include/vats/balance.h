// Viewport Avatar Toolset - balance: the centre of mass over the planted feet, and hips that keep it there.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 21 (CM-1..CM-3). The whole-body centre of mass is the ragdoll's bodies with their
// de Leva segment masses (ragdoll.h). The support polygon is the convex hull of the planted feet's footprints on
// the ground; Auto-Balance moves mPelvis sideways and back or forth until the centre of mass is over it, the
// planted feet held by leg IK as Clean Up Foot Sliding holds them (footlock.h).
#pragma once

#include <string>
#include <vector>

#include "vats/rig.h"

namespace vats {

struct Balance {
    bool contact = false;       // a foot is planted; nothing below is set when false
    Vec3 com;                   // centre of mass, avatar space
    Vec3 ground;                // com dropped onto the ground
    std::vector<Vec3> support;  // support polygon on the ground, convex, counter-clockwise seen from above
    double margin = 0;          // distance from the polygon's nearest edge, + inside, metres
    bool inside() const { return contact && margin >= 0; }
};

// CM-1: the balance of a posed body. A foot is planted when its lowest joint (ankle, foot or toe) is within
// 5 cm of the ground, the lowest of those joints in the rest pose.
Balance balance_of(const Skeleton& skel, const std::vector<Xform>& globals, const Shape* shape);

struct AutoBalanceOptions {
    int from = 0, to = -1;      // frame range (to = -1: the clip's end)
    double margin = 0.02;       // m: the centre of mass goes this far inside the polygon's edges
    int smooth = 2;             // frames each side of the moving average the hip offsets are smoothed with
    bool counter_lean = false;  // mTorso also tilts towards the support, so the hips move less
    const Shape* shape = nullptr;
};

// CM-2: keys mPelvis position (and mTorso with counter_lean) on every frame of the range, and the frames on
// either side with their own values so the rest of the clip stays as it is. Returns a report line.
std::string auto_balance(Clip& clip, const Rig& rig, const AutoBalanceOptions& opt);

}  // namespace vats
