// Viewport Avatar Toolset - loop tools: seamless loops, seam checks, in-place travel, cycle offset.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 7.4 (LP-1..LP-4). All of them edit the clip in place; callers wrap them in
// an undo step.
#pragma once

#include <string>
#include <vector>

#include "vats/clip.h"

namespace vats {

struct LoopRange {
    int in = 0, out = 0;
};

// loop_in..loop_out when the clip loops, else the whole clip.
LoopRange loop_range(const Clip& clip);

// LP-1: every animated channel ends its loop where it starts, with the same slope. blend_frames > 0 eases
// the last frames into the start value instead of changing only the end key. Rotations may end a whole
// turn away from where they start. Returns the number of channels changed.
int make_loop_seamless(Clip& clip, int blend_frames);

// LP-2: channels whose value at loop-out differs from loop-in by more than the tolerance
// (degrees for rot_*, metres for pos_*, the raw value for anything else).
struct SeamJump {
    std::string track, channel;
    double jump = 0;
};
std::vector<SeamJump> loop_seam_jumps(const Clip& clip, double tol_deg = 0.5, double tol_m = 0.001);

// LP-3: removes the hip's travel along the ground over the loop range (X and Y; height untouched), keeping
// its sway. Returns the travel removed, in metres per second (vx forward, vy left).
struct Travel {
    double vx = 0, vy = 0;
    double speed() const;
};
Travel remove_travel(Clip& clip, const std::string& hip = "mPelvis");
// The reverse: the hip moves along the ground at this speed.
void add_travel(Clip& clip, const Travel& t, const std::string& hip = "mPelvis");

// LP-4: the loop starts at `start` instead of loop-in; values move, the loop range stays. The loop should
// be seamless first. Returns false when start is not inside the loop range.
bool cycle_offset(Clip& clip, int start);

}  // namespace vats
