// Viewport Avatar Toolset - bakeable dynamics ("dynabones"): spring chains driven by the animation.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 4 (DY-1..DY-4). A chain of joints (tail, wings, hair, ears) swings behind
// its animated parents; a collision volume (BELLY, BUTT, LEFT_PEC...) jiggles as a point spring. The
// simulation runs at a fixed sub-step, collides with the body's collision volumes and bakes to keys.
// The sampling and baking half (simulate_frames, bake_samples) is independent of the spring model so a
// ragdoll (08 section 5) can reuse it with a different simulator.
#pragma once

#include <string>
#include <vector>

#include "vats/clip.h"
#include "vats/rig.h"

namespace vats {

// Presets for DY-1's common cases.
DynChain dyn_preset(const std::string& kind, const std::string& root, int length);  // "tail", "ears", "jiggle"

// Nodes the chain simulates, in order: root, then the first joint child, length times. A collision
// volume root is a chain of one.
std::vector<int> dyn_nodes(const Skeleton& skel, const DynChain& chain);

// The spring simulation state for a set of chains.
class DynSim {
public:
    static constexpr int kStepsPerSecond = 120;

    DynSim(const Skeleton& skel, const std::vector<DynChain>& chains);
    // Puts every point at the animated pose, at rest.
    void reset(const std::vector<Xform>& animated);
    // One sub-step of dt seconds towards the animated globals.
    void step(const std::vector<Xform>& animated, double dt);
    // Writes the simulated chains into pose (rotations; offsets for volumes), given the animated pose
    // and globals it came from.
    void apply(const std::vector<Xform>& animated, Pose& pose) const;
    bool empty() const { return chains_.empty(); }

private:
    struct Chain {
        DynChain def;
        std::vector<int> nodes;
        std::vector<Vec3> p, prev, target_prev;  // one point per node: bone tail, or volume centre
    };
    // The simulated global of every chain node for the current points, from the animated globals.
    void solve(const Chain& c, const std::vector<Xform>& animated, std::vector<Xform>& out, bool with_points) const;
    Vec3 collide(const Vec3& p, const Vec3& target, double radius, const std::vector<Xform>& animated) const;

    const Skeleton& skel_;
    std::vector<Chain> chains_;
};

// Runs sim over the clip and returns the simulated local pose of every integer frame 0..end_frame.
// The driver evaluates the clip (FK, IK, pins) at every sub-step. For a looping clip the loop range is
// pre-rolled twice so the recorded loop ends where it starts (DY-2).
std::vector<Pose> simulate_frames(const Rig& rig, const Clip& clip, const Shape* shape, DynSim& sim);

// Replaces the rotation keys (position keys for volumes) of nodes with per-frame samples, reduced to
// tol_deg / tol_m, with linear keys. Nodes in with_position also get their position keys replaced (a
// ragdoll's pelvis).
void bake_samples(Clip& clip, const Skeleton& skel, const std::vector<int>& nodes, const std::vector<Pose>& frames,
                  double tol_deg = 0.1, double tol_m = 0.0002, const std::vector<int>& with_position = {});

// DY-3: simulates the chains (all when which < 0) from their pre-bake tracks and bakes them, keeping
// those tracks in DynChain::source. One call is one undo step for the caller.
void bake_dynamics(Clip& clip, const Rig& rig, const Shape* shape, int which = -1);
// Puts the pre-bake tracks back and clears source.
void unbake_dynamics(Clip& clip, const Skeleton& skel, int which);

}  // namespace vats
