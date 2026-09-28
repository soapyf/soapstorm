// Viewport Avatar Toolset - foot-contact clean-up: planted feet stop sliding (spec 07 RT-9, 08 FC).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// After Kovar, Schreiner and Gleicher, "Footskate cleanup for motion capture editing" (SCA 2002). Each foot
// has two contact points: the heel (the ankle) and the toe (mToeLeft/Right; the ankle only when the skeleton
// has no toe bones or heel_toe is off). A contact is a run of frames where a point is near the ground and
// barely moving. The ground is the lowest sole point over the range: each point's height minus its height
// above the floor at rest. A planted point is snapped onto the ground and held there with the leg's IK: the
// target is the ankle, placed so the held point stays put while the foot turns as animated, and the IK blend
// ramps in and out around each stance, so the fix is ordinary IK data that export bakes like any other. (A pin
// would move the ankle itself and stretch the leg.) Where the leg cannot reach a target with its knee still
// bent kMinKneeBendDeg from rest, the pelvis is lowered by the shortfall on a smoothed curve (mPelvis pos_z
// keys) instead of straightening the knee.
#pragma once

#include <string>
#include <vector>

#include "vats/rig.h"

namespace vats {

// The least knee bend (about its hinge, from the rest pose, where the solver's leg is straightest) a held foot
// may ask for: 180 minus this is the widest knee.
inline constexpr double kMinKneeBendDeg = 3;

struct FootLockOptions {
    double height = 0.05;  // m above the ground; leaving needs 1.5x this
    double speed = 0.3;    // m/s; leaving needs 2x this
    int min_frames = 3;    // shorter contacts are ignored
    int blend = 3;         // frames of IK blend-in and blend-out around each stance
    int from = 0, to = -1;  // frame range (to = -1: the clip's end)
    bool left = true, right = true;
    bool heel_toe = true;    // heel and toe contacts; off = the ankle only
    bool to_ground = false;  // "Put feet on the ground": first move the pelvis so the ground is the rest floor
    const Shape* shape = nullptr;
};

struct FootContact {
    int limb = -1;  // index into Rig::limbs()
    int from = 0, to = 0;
    bool toe = false;  // the toe point; false = the heel (ankle)
};

std::vector<FootContact> find_foot_contacts(const Rig& rig, const Clip& clip, const FootLockOptions& opt);

// The ground over the range, in metres above the rest pose's floor (its lowest ankle, foot or toe).
double foot_ground(const Rig& rig, const Clip& clip, const FootLockOptions& opt);

// Locks every contact. Legs that already use IK are left alone (and reported). Returns report lines: the
// ground, each leg's contacts, and how far the pelvis was lowered.
std::vector<std::string> lock_feet(Clip& clip, const Rig& rig, const FootLockOptions& opt);

}  // namespace vats
