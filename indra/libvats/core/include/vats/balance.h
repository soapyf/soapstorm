// Viewport Avatar Toolset - balance: the centre of mass over the planted feet, and hips that keep it there.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 21 (CM-1..CM-3). The whole-body centre of mass is the ragdoll's bodies with their
// de Leva segment masses (ragdoll.h). The support polygon is the convex hull of the planted feet's footprints on
// the ground; Auto-Balance moves mPelvis sideways and back or forth until the centre of mass is over it, the
// planted feet held by leg IK as Clean Up Foot Sliding holds them (footlock.h).
#pragma once

#include <functional>
#include <span>
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

struct MeshContactFloor {
    bool has_mesh = false;
    double lowest_z = 0.0;
    std::vector<Vec3> contact_points;
};

// Computes the lowest vertex and contact points (within threshold of lowest vertex) from skinned mesh parts.
MeshContactFloor compute_mesh_contact_floor(const std::vector<std::span<const float>>& mesh_parts,
                                            double threshold = 0.02);

inline MeshContactFloor compute_mesh_contact_floor(const std::vector<float>& mesh_positions,
                                                   double threshold = 0.02) {
    if (mesh_positions.empty()) return MeshContactFloor{};
    return compute_mesh_contact_floor(std::vector<std::span<const float>>{mesh_positions}, threshold);
}

// CM-1: the balance of a posed body. When mesh parts/positions are provided, contact is taken
// from the skinned mesh vertices (within ~2 cm of the lowest vertex). When omitted or empty,
// falls back to the rest-pose bone heights.
Balance balance_of(const Skeleton& skel, const std::vector<Xform>& globals, const Shape* shape = nullptr,
                   const std::function<bool(int)>* is_weighted = nullptr,
                   const std::vector<std::span<const float>>& mesh_parts = {},
                   const MeshContactFloor* cached_floor = nullptr);

inline Balance balance_of(const Skeleton& skel, const std::vector<Xform>& globals, const Shape* shape,
                          const std::vector<std::span<const float>>& mesh_parts,
                          const MeshContactFloor* cached_floor = nullptr) {
    return balance_of(skel, globals, shape, nullptr, mesh_parts, cached_floor);
}

inline Balance balance_of(const Skeleton& skel, const std::vector<Xform>& globals, const Shape* shape,
                          const std::function<bool(int)>* is_weighted,
                          const std::vector<float>& mesh_positions,
                          const MeshContactFloor* cached_floor = nullptr) {
    if (mesh_positions.empty() && !cached_floor) return balance_of(skel, globals, shape, is_weighted);
    return balance_of(skel, globals, shape, is_weighted,
                      mesh_positions.empty() ? std::vector<std::span<const float>>{} : std::vector<std::span<const float>>{mesh_positions},
                      cached_floor);
}

struct AutoBalanceOptions {
    int from = 0, to = -1;      // frame range (to = -1: the clip's end)
    double margin = 0.02;       // m: the centre of mass goes this far inside the polygon's edges
    int smooth = 2;             // frames each side of the moving average the hip offsets are smoothed with
    bool counter_lean = false;  // mTorso also tilts towards the support, so the hips move less
    const Shape* shape = nullptr;
    // The floor the view's balance uses (CM-1): the shown body's mesh posed by these globals; empty = the bones'.
    std::function<MeshContactFloor(const std::vector<Xform>&)> mesh_floor;
    std::function<bool(int)> is_weighted;  // the shown body's weighted joints (balance_of's); empty = none given
};

// CM-2: keys mPelvis position (and mTorso with counter_lean) on every frame of the range, and the frames on
// either side with their own values so the rest of the clip stays as it is. Returns a report line.
std::string auto_balance(Clip& clip, const Rig& rig, const AutoBalanceOptions& opt);

}  // namespace vats
