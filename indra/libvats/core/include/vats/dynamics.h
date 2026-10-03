// Viewport Avatar Toolset - bakeable dynamics ("dynabones"): spring chains driven by the animation.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 4 (DY-1..DY-4). A chain of joints (tail, wings, hair, ears) swings behind
// its animated parents; a collision volume (BELLY, BUTT, LEFT_PEC...) jiggles as a point spring, or as SL's avatar
// physics moves it (DY-6, DynChain::physics). The
// simulation runs at a fixed sub-step, collides with the body's collision volumes and bakes to keys.
// The sampling and baking half (simulate_frames, bake_samples) is independent of the spring model so a
// ragdoll (08 section 5) can reuse it with a different simulator.
#pragma once

#include <string>
#include <vector>

#include "vats/clip.h"
#include "vats/rig.h"

namespace vats {

// Presets for DY-1's common cases, and for a part on a spare chain (rig_map.h RM-8): "scarf", "hair", "cape".
DynChain dyn_preset(const std::string& kind, const std::string& root, int length);  // "tail", "ears", "jiggle", "overlap", ...

// RM-10: SL's avatar physics (the Physics wearable) for the parts it moves.
struct AvatarPhysics {
    PhysicsPart breasts, belly, butt;
    bool operator==(const AvatarPhysics&) const = default;
};
// Presets in SL's own units, from gentle to bouncy: "Subtle", "Natural" and "Bouncy". A new Physics wearable in SL moves
// nothing (every max effect 0); these are what people set.
const std::vector<std::pair<std::string, AvatarPhysics>>& avatar_physics_presets();
// The volumes SL's avatar physics moves: BELLY, BUTT, LEFT_PEC and RIGHT_PEC.
const std::vector<std::string>& avatar_physics_volumes();
// A chain for one of them with physics' settings for its part (DynChain::physics): a chain of one, no gravity of its own.
DynChain avatar_physics_chain(const std::string& volume, const AvatarPhysics& physics);
// Bakes SL's bounce on these volumes into their position keys, as Dynamics bakes a chain (one chain per volume, kept in
// clip.dynamics, so it bakes again from the keys it had before and unbakes): keys on every other track stay. Returns
// how many it baked.
int bake_avatar_physics(Clip& clip, const Rig& rig, const Shape* shape, const AvatarPhysics& physics,
                        const std::vector<std::string>& volumes, bool match_loop = true);

// Nodes the chain simulates, parents before children: root, then down the first joint child, `length` bones deep,
// with the children that start where the first one does (a fan: mWing4Fan beside mWing4) as branches. A collision
// volume root is a chain of one. branches = false, or a chain without fans: the first-child path alone (Overlap's chain).
std::vector<int> dyn_nodes(const Skeleton& skel, const DynChain& chain, bool branches = true);

// The spring simulation state for a set of chains.
class DynSim {
public:
    static constexpr int kStepsPerSecond = 120;

    // shape: the body's, whose bone tails (a mesh body's own) a chain's last bone swings by; null for SL's.
    DynSim(const Skeleton& skel, const std::vector<DynChain>& chains, const Shape* shape = nullptr);
    // Puts every point at the animated pose, at rest.
    void reset(const std::vector<Xform>& animated);
    // One sub-step of dt seconds towards the animated globals.
    void step(const std::vector<Xform>& animated, double dt);
    // Writes the simulated chains into pose (rotations; offsets for volumes), given the animated pose
    // and globals it came from.
    void apply(const std::vector<Xform>& animated, Pose& pose) const;
    bool empty() const { return chains_.empty(); }
    double max_speed() const { return max_speed_; }

private:
    struct Chain {
        DynChain def;
        std::vector<int> nodes;
        std::vector<int> up, child;  // per node: its parent's and its first child's index in nodes, -1 for none
        std::vector<Vec3> p, prev, target_prev;  // one point per node: bone tail, or volume centre
        std::vector<Vec3> origin_prev, body_prev;  // the step before: where the node was, its animated direction
        // RM-10, a chain with physics: SL's motions on its volume (LLPhysicsMotion), one per direction, driven by a joint.
        struct Motion {
            PhysicsAxis axis;
            Vec3 dir;                 // in the joint's frame
            double lo = 0, hi = 0;    // the driven param's range
            Vec3 pos;                 // its volume_morph pos at weight 1
            double at = 0.5, speed = 0, joint_speed = 0, joint_accel = 0;  // param space, as SL keeps them
        };
        int driver = -1;
        std::vector<Motion> motions;
        Vec3 driver_prev, offset;  // the driver's place at the last update; the offset the motions give the volume
        double pending = 0;        // seconds not yet simulated (SL updates once a frame: at 60 Hz here)
        double moving = 0;         // how fast the offset changed at the last update, m/s
    };
    void step_physics(Chain& c, const std::vector<Xform>& animated, double dt);
    Vec3 tail_local(const Chain& c, size_t i, const std::vector<Xform>& animated) const;
    Vec3 collide(const Vec3& p, const Vec3& target, double radius, const std::vector<Xform>& animated) const;

    const Skeleton& skel_;
    const Shape* shape_;
    std::vector<Chain> chains_;
    double max_speed_ = 0;
};

// Runs sim over the clip and returns the simulated local pose of every integer frame 0..end_frame.
// The driver evaluates the clip (FK, IK, pins) at every sub-step. For a looping clip the loop range is
// pre-rolled twice so the recorded loop ends where it starts (DY-2).
std::vector<Pose> simulate_frames(const Rig& rig, const Clip& clip, const Shape* shape, DynSim& sim);

// Replaces the rotation keys (position keys for volumes) of nodes with per-frame samples, reduced to
// tol_deg / tol_m, with linear keys. Nodes in with_position also get their position keys replaced (a
// ragdoll's pelvis). position_only leaves every rotation key alone (an idle breath's mTorso rise).
void bake_samples(Clip& clip, const Skeleton& skel, const std::vector<int>& nodes, const std::vector<Pose>& frames,
                  double tol_deg = 0.1, double tol_m = 0.0002, const std::vector<int>& with_position = {},
                  bool position_only = false);

// DY-3: simulates the chains (all when which < 0) from their pre-bake tracks and bakes them, keeping
// those tracks in DynChain::source. One call is one undo step for the caller.
// match_loop = false: a looping clip is simulated straight through from frame 0, as a clip played once.
void bake_dynamics(Clip& clip, const Rig& rig, const Shape* shape, int which = -1, bool match_loop = true);
// RM-8 follow-through in one step: the chain from root, length joints deep without fans, swinging as kind's preset
// ("scarf", "hair", "cape", "tail") from the body's motion, baked. A chain already on root is set up again and
// rebaked from the keys it had before its bake. Returns its index in clip.dynamics.
int bake_follow_through(Clip& clip, const Rig& rig, const Shape* shape, const std::string& root, int length,
                        const std::string& kind, bool match_loop = true);
// Puts the pre-bake tracks back and clears source.
void unbake_dynamics(Clip& clip, const Skeleton& skel, int which);

}  // namespace vats
