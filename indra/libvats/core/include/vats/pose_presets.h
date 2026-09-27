// Viewport Avatar Toolset - the built-in starter poses: hand shapes and a few body poses.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Hand poses are written as per-finger curls and spreads and turned into Euler keys with the hand
// poser's axes (docs/spec/02 AM-38), so they read the same as poses made by dragging. They are
// left-hand items; the library mirrors them onto the right hand like any saved hand pose.
#pragma once

#include <vector>

#include "vats/pose_ops.h"

namespace vats {

// The built-in library items, ids "builtin:<slug>". Built once from skel (the finger directions
// come from its `end` vectors); later calls return the same list.
const std::vector<LibraryItem>& builtin_poses(const Skeleton& skel);

}  // namespace vats
