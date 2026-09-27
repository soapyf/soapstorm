// Viewport Avatar Toolset - foot-contact clean-up: planted feet stop sliding (spec 07 RT-9).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// A contact is a run of frames where an ankle is near its lowest height and barely moving. Each
// contact is held with the leg's IK: the target is keyed at the ankle's transform where the contact
// starts, and the IK blend ramps in and out around it, so the fix is ordinary IK data that export
// bakes like any other. (A pin would move the ankle itself and stretch the leg.)
#pragma once

#include <string>
#include <vector>

#include "vats/rig.h"

namespace vats {

struct FootLockOptions {
    double height = 0.05;  // m above the ankle's lowest point in the range; leaving needs 1.5x this
    double speed = 0.3;    // m/s; leaving needs 2x this
    int min_frames = 3;    // shorter contacts are ignored
    int blend = 3;         // frames of IK blend-in and blend-out around each contact
    int from = 0, to = -1;  // frame range (to = -1: the clip's end)
    bool left = true, right = true;
    const Shape* shape = nullptr;
};

struct FootContact {
    int limb = -1;  // index into Rig::limbs()
    int from = 0, to = 0;
};

std::vector<FootContact> find_foot_contacts(const Rig& rig, const Clip& clip, const FootLockOptions& opt);

// Locks every contact. Legs that already use IK are left alone (and reported). Returns report lines.
std::vector<std::string> lock_feet(Clip& clip, const Rig& rig, const FootLockOptions& opt);

}  // namespace vats
