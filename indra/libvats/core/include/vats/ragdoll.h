// Viewport Avatar Toolset - ragdoll: the body, or the selected limbs, falling limp from the animation.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 5 (RD-1..RD-4). An XPBD rigid-body solver (Mueller et al. 2020, "Detailed
// rigid body simulation with extended position based dynamics"): one capsule body per major joint, ball
// joints with swing and twist limits from a per-joint table, collisions with the ground, static prop boxes
// and the body's own parts. The result bakes to keys through dynamics.h's bake_samples.
#pragma once

#include <string>
#include <vector>

#include "vats/clip.h"
#include "vats/rig.h"

namespace vats {

// A static prop the ragdoll collides with (RD-2): an oriented box.
struct RagdollBox {
    Xform frame;  // centre and orientation
    Vec3 half;    // half extents, metres
};

// The solver, with the same reset / step / apply shape as DynSim so another backend (Jolt) could take its
// place behind simulate_ragdoll.
class RagdollSolver {
public:
    struct Joint;  // joint-table row, see ragdoll.cpp
    RagdollSolver(const Skeleton& skel, const Ragdoll& settings, std::vector<RagdollBox> boxes);
    // Bodies at the animated pose; prev_animated (may equal animated), dt earlier, gives the starting velocity.
    void reset(const std::vector<Xform>& animated, const std::vector<Xform>& prev_animated, double dt);
    // One sub-step of dt seconds; blend 1 pins the simulated joints to the animation, 0 lets them go (RD-3).
    void step(const std::vector<Xform>& animated, double dt, double blend);
    // Writes the simulated joints into pose (the animated local pose that animated came from).
    void apply(const std::vector<Xform>& animated, Pose& pose) const;

    const std::vector<int>& nodes() const { return sim_nodes_; }  // joints whose rotation it sets
    bool moves_pelvis() const { return moves_pelvis_; }
    double lowest_surface() const;  // lowest capsule bottom (z - radius), for tests

private:
    // One rigid body per ragdoll joint: the bone from the joint to its child (or a rigid group: the hips,
    // the chest, the foot). Its orientation is the joint's global rotation.
    struct Body {
        int node = -1;
        int parent = -1;            // parent body, -1 for the pelvis
        bool dynamic = false;       // false: follows the animation (infinite mass)
        double inv_mass = 0, inv_inertia = 0, radius = 0.04;  // inertia taken as isotropic
        Vec3 com;                   // centre of mass in the joint's frame
        std::vector<Vec3> ends;     // capsule ends in the joint's frame; each capsule runs from the joint
        Vec3 x, x_prev, v, w;       // centre of mass (world), linear and angular velocity
        Quat q, q_prev;             // the joint's global rotation
        bool touching = false;      // in contact with the ground, a prop or another part this sub-step
    };

    Vec3 joint_world(const Body& b) const { return b.x - b.q.rotate(b.com); }
    Vec3 point_world(const Body& b, const Vec3& local) const { return b.x + b.q.rotate(local - b.com); }
    void place_kinematic(Body& b, const std::vector<Xform>& g) const;
    // XPBD corrections between bodies a and b (-1 = the static world) at world points pa / pb.
    void positional(int a, int b, const Vec3& pa, const Vec3& pb, const Vec3& correction);
    void angular(int a, int b, const Vec3& rotation);
    void solve_joints(const std::vector<Xform>& g, double drive);
    void solve_contacts(double friction);
    void damp_joints(double h);

    const Skeleton& skel_;
    Ragdoll settings_;
    std::vector<RagdollBox> boxes_;
    std::vector<Body> bodies_;
    std::vector<int> body_of_;                   // per skeleton node: its body, or -1
    std::vector<const Joint*> row_;              // per skeleton node: its table row, or null
    std::vector<Vec3> axis_;                     // per skeleton node: limit axis (table joints)
    std::vector<std::pair<int, int>> self_pairs_;  // body pairs that collide (not neighbours, not touching at start)
    void nudge();  // the push that starts a limp standing body falling (fall_direction)
    std::vector<int> sim_nodes_;
    bool moves_pelvis_ = false;
    double last_blend_ = 1;
};

// The joints a ragdoll can move (whole body), for the UI.
std::vector<std::string> ragdoll_joint_names();

// RD-1..RD-3: simulates clip.ragdoll over its range and returns the local pose of every integer frame
// 0..end_frame (the animation outside the range). Empty when the clip has no ragdoll.
std::vector<Pose> simulate_ragdoll(const Rig& rig, const Clip& clip, const Shape* shape,
                                   const std::vector<RagdollBox>& boxes = {});

// RD-4: bakes the simulation into the moved joints' tracks (and the pelvis position when it falls),
// keeping the pre-bake tracks in Ragdoll::source; re-baking starts from those. One undo step for the caller.
void bake_ragdoll(Clip& clip, const Rig& rig, const Shape* shape, const std::vector<RagdollBox>& boxes = {});
// Puts the pre-bake tracks back.
void unbake_ragdoll(Clip& clip, const Skeleton& skel);

// How far a local pose goes past the ragdoll's joint limits: the worst joint, in degrees (0 = inside).
double ragdoll_limit_excess(const Skeleton& skel, const Pose& pose);
// Each joint of the table that a local pose takes more than tol_deg past its limits: the node, how far (degrees),
// and its local rotation (relative to rest, as Pose::rot) brought back inside them. Used by the Animation Check.
struct LimitExcess {
    int node = -1;
    double deg = 0;
    Quat inside;
};
std::vector<LimitExcess> ragdoll_limit_excesses(const Skeleton& skel, const Pose& pose, double tol_deg);

}  // namespace vats
