// Viewport Avatar Toolset - Jump Arc: the hips fly a ballistic arc between takeoff and landing.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 21 (JA-1). mPelvis's height follows z0 + v t - g t^2 / 2 through the takeoff and
// landing heights; forward (X) and, unless kept, lateral (Y) travel at constant speed, as a body in the air does.
#pragma once

#include <string>

#include "vats/clip.h"

namespace vats {

struct JumpArcOptions {
    int takeoff = 0, landing = 0;  // frames; both keep their values
    double gravity = 9.81;         // m/s^2
    bool forward = true;           // X travels at constant speed (off: its keys stay)
    bool keep_lateral = true;      // Y keeps its keys (off: constant speed like X)
};

// The apex above the takeoff height, metres, for a flight of t seconds rising dz.
double jump_apex(double t, double dz, double gravity);

// JA-1: keys mPelvis position on every frame from takeoff to landing. Returns false and the reason in msg when
// refused; on success msg is the status line.
bool jump_arc(Clip& clip, const JumpArcOptions& opt, std::string& msg);

}  // namespace vats
