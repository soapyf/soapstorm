// Viewport Avatar Toolset - full-body reach: an IK target out of reach pulls the spine and then the hips.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 21 (RC-1). Each arm and leg target has a Pull (Clip::ik_pull, 0..1, default 0).
// When its target is further than the limb reaches, an arm first leans the spine towards it (the spine IK solve,
// up to 30 degrees), then the hips move by the distance still missing x Pull. The result is ordinary mTorso,
// mChest and mPelvis keys at the frame; pinned feet stay where their pins hold them.
#pragma once

#include "vats/rig.h"

namespace vats {

// The limb's Pull, 0 when it has none.
double ik_pull(const Clip& clip, const LimbInfo& limb);

// RC-1: reaches for limb's target at frame. Returns how far the hips moved, metres (0: they did not).
double reach_with_body(Clip& clip, const Rig& rig, double frame, int limb, double pull, const Shape* shape);

}  // namespace vats
