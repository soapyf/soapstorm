// Viewport Avatar Toolset - key reduction by the world-space error it causes anywhere on the body.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 19 (WR, idea 11). export_anim uses these when AnimExportOptions::reduce_world_m > 0
// ("Reduce keys: N mm anywhere on the body"); the per-bone tolerances (reduce_rotation_keys) are the other mode.
#pragma once

#include <functional>
#include <vector>

#include "vats/math.h"
#include "vats/skeleton.h"

namespace vats {

constexpr double kReduceWorldDefault = 0.001;  // metres: "Reduce keys: 1.00 mm anywhere on the body"

// WR-1: Ramer-Douglas-Peucker over frames [0, n). Frame 0, frame n - 1 and every anchor are kept; each run
// between kept frames is split at its worst frame while err(a, b, k) exceeds tol there, and at its middle while
// it is longer than max_gap frames (0 = no limit). tol <= 0 keeps every frame. Sorted.
std::vector<int> rdp_keys(int n, double tol, int max_gap, const std::vector<char>& anchors,
                          const std::function<double(int a, int b, int k)>& err);

// WR-2: reach[node][frame], how far the node's farthest descendant (collision volumes aside) can be from it: the
// longest bone path down from it, on the frame's global pose, and at least its own tip or 0.1 m. It bounds the
// distance to every descendant.
// globals[frame] is Skeleton::global_pose of that frame, computed once per frame (the cache).
std::vector<std::vector<double>> world_reach(const Skeleton& skel, const std::vector<std::vector<Xform>>& globals);

// WR-3: the error in metres each node's reduced channels may cause: rot[i] / pos[i] say which channels are written.
// A node gets tol_m / L, L the most channels on any root-to-leaf path through it, so the errors of every channel
// above a joint add up to at most tol_m. 0 for nodes with nothing written.
std::vector<double> world_budgets(const Skeleton& skel, const std::vector<char>& rot, const std::vector<char>& pos,
                                  double tol_m);

// WR-4: rotation keys (the file's local rotations) whose nlerp, as SL plays them, moves no descendant further than
// tol_m: a turn error of angle a moves a point at distance r by at most 2 r sin(a / 2); reach is that node's
// world_reach row.
std::vector<int> reduce_rotation_keys_world(const std::vector<Quat>& samples, const std::vector<double>& reach,
                                            double tol_m, int max_gap, const std::vector<char>& anchors = {});
// Position keys (local, metres) whose lerp is off by at most tol_m in the world; scale is the largest component of
// the parent's shape scale (local positions are scaled by it), 1 without a shape.
std::vector<int> reduce_position_keys_world(const std::vector<Vec3>& samples, double scale, double tol_m, int max_gap,
                                            const std::vector<char>& anchors = {});

}  // namespace vats
