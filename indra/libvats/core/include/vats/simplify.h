// Viewport Avatar Toolset - Simplify Curves: dense (baked, captured, filtered) curves back to few keys.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 15 (SC). The curves are refitted with Bezier segments so every whole frame of
// the range stays within the tolerance of the curve as it was.
#pragma once

#include <string>
#include <vector>

#include "vats/clip.h"

namespace vats {

struct SimplifyOptions {
    double tol_deg = 0.25;  // rot_* channels, degrees
    double tol_mm = 0.5;    // pos_* channels, millimetres
    int from = 0, to = -1;  // whole frames (to = -1: the clip's end)
    std::vector<int> keep;  // frames that keep a key on every curve (where a foot plants or lifts)
};

struct SimplifyResult {
    int before = 0, after = 0;          // keys in the range on the curves simplified
    std::vector<std::string> skipped;   // "mHipLeft: near gimbal lock", one line per track or curve left alone
};

// Simplifies the rot_* and pos_* curves of tracks (empty = every track) over from..to. Keys are kept at the
// range's ends, at `keep`, at the extrema (turns by more than the tolerance) and at the steepest frame between
// two of them (the inflection), then added where the fit misses. Between keys the curve is Auto (clamped) where
// that fits, else Free handles on the curve's own slope. A curve whose fit would not have fewer keys, one with
// stepped keys in the range, and the rotation of a track whose rot_y comes within 5 degrees of +-90 (gimbal
// lock: X and Z swing wildly there) are left as they are. Keys just outside the range hold the curve there, as
// Filter Curves does.
SimplifyResult simplify_curves(Clip& clip, const std::vector<std::string>& tracks, const SimplifyOptions& opt);

}  // namespace vats
