// Viewport Avatar Toolset - key tags (Extreme, Breakdown, Hold) and blocking mode.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 24 (KT-1..KT-4). The tag itself is Key::tag (fcurve.h). Breakdown timing (KT-3) lives
// in curve_ops' move_keys; tween tags its keys (tween.h). All of these edit the clip in place; callers wrap them in an
// undo step.
#pragma once

#include <string>
#include <vector>

#include "vats/clip.h"
#include "vats/curve_ops.h"

namespace vats {

const char* key_tag_name(KeyTag tag);  // "Extreme", "Breakdown", "Hold", "" for None

// KT-1: tags the selected keys. Returns how many changed.
int tag_keys(Clip& clip, const std::vector<KeyRef>& sel, KeyTag tag);
// KT-1: tags every key at frame on the tracks (every channel). Returns how many changed.
int tag_keys_at(Clip& clip, const std::vector<std::string>& tracks, double frame, KeyTag tag);

// KT-2: blocking mode. On each curve of clip with more keys than the same curve in before (or none there), every key
// on a frame where before had no key becomes Stepped (Constant). Curves whose keys only moved (a graph drag, Insert
// Time) are left alone. Returns how many keys it stepped.
int step_new_keys(Clip& clip, const Clip& before);

// The drift a Hold pair gets on conversion, in degrees.
inline constexpr double kHoldDrift = 1.0;

// KT-4: Convert Blocking to Spline. Every key but the IK switches (`blend`) gets the Auto tangent (Bezier, Auto
// Clamped). Then each pair of neighbouring Hold keys on a rotation channel whose second key is within drift of the
// first becomes a moving hold: the second key is set drift degrees from the first, towards the key after the pair
// (or, with none, onwards from the key before it), and never past that key; the pair's facing handles are Plateau, so
// the curve between them stays within the drift. Position, pole and blend channels keep their values. Tags are kept.
// Returns the number of holds that got a drift.
int blocking_to_spline(Clip& clip, double drift = kHoldDrift);

}  // namespace vats
