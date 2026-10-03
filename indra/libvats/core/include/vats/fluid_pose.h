// Viewport Avatar Toolset - posing by dragging the body: which bone a point of the skin belongs to, a drag of the
// hips or spine that keeps the feet planted, and follow-through on the loose parts while a drag runs.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 30 (FP-2..FP-4). Builds on Auto IK (rig.h) and the Dynamics solver (dynamics.h); keys
// only what a drag moves, as Auto IK does, and never through the follow-through, which is a preview.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "vats/avatar_mesh.h"
#include "vats/dae.h"
#include "vats/dynamics.h"
#include "vats/rig.h"

namespace vats {

// --- FP-2: the bone under a point of the skin --------------------------------------------------------------------

// The nearest triangle of a mesh (3 floats per vertex, 3 indices per triangle) along a ray, or triangle -1. u and v
// weight the triangle's second and third corners at the hit; the first has 1 - u - v.
struct SurfaceHit {
    double t = 1e30;
    int triangle = -1;
    double u = 0, v = 0;
};
SurfaceHit ray_surface(const Vec3& origin, const Vec3& dir, const std::vector<float>& positions,
                       const std::vector<std::uint32_t>& indices);

// The joint that owns the hit point: the one with the most skin weight there, the three corners' weights blended by
// the hit's u and v. A collision volume's weight counts for the joint it hangs on; the root's for the pelvis. -1 when
// there is no hit or no weight.
int surface_joint(const Skeleton& skel, const DaeModel& model, const SurfaceHit& hit);
int surface_joint(const Skeleton& skel, const AvatarMesh& mesh, const SurfaceHit& hit);

// --- FP-3: dragging the body with the feet planted ------------------------------------------------------------------

// The joints a drag of which moves the whole body: mPelvis, mTorso, mChest and Bento's mSpine1..4 between them.
bool body_drag_joint(const Skeleton& skel, int node);

struct PlantedFoot {
    int limb = -1, node = -1;  // the limb and its end joint (an ankle, a hock)
    Xform at;                  // where it stays, avatar space (a pin's or IK target's place, else the drag start's)
    bool held = false;         // a pin or an IK limb already keeps it there: nothing to key
    AutoIkChain chain;         // the leg's chain when this drag holds it itself
};

struct BodyDrag {
    int node = -1;                   // the dragged joint
    Evaluation start;                // the pose at the press
    std::vector<PlantedFoot> feet;   // the ends held for the drag
    AutoIkChain spine;               // mTorso (and passive spine bones) when the chest is dragged; empty otherwise
    int head = -1;                   // mHead, which keeps its orientation while the spine leans; -1 without a spine
};

// What the shown body stands on. weighted(node): whether the body weights the joint (as follow_through_chains takes
// it; empty: every joint); a leg it weights none of (the SL avatar's hind legs) is never planted or keyed. mesh: a mesh
// body's rigged parts; given, a leg's height is the lowest point of its skin (vertices its end joint or a joint under it
// carries), not its joints', so a creature whose foot joint sits above its sole still stands.
struct BodyGround {
    std::function<bool(int)> weighted;
    std::vector<const DaeModel*> mesh;
};

// Where the body stands: the lowest of the legs (limbs named Leg, the shown body weights) in the rest pose, with shape.
double ground_height(const Rig& rig, const Shape* shape, const BodyGround& body = {});

// The drag of node from the pose at frame. Planted: every limb end held by a pin or in IK (held = true), and every
// leg or hind leg the body weights whose lowest point is within 5 cm of the ground now (held by the drag's own leg IK).
BodyDrag begin_body_drag(const Rig& rig, const Clip& clip, double frame, int node, const Shape* shape,
                         const BodyGround& body = {});

// Keys the drag with the dragged joint at target (avatar space): mPelvis's position (a chest drag first leans mTorso
// halfway there by Auto IK and the hips take the rest), each planted leg solved from the press's pose to keep its
// foot where it was, with the foot's orientation, and mHead's orientation when the spine leaned. A leg joint keyed
// past its limit may stay there for the drag (it can turn back towards its range, not further out), so the foot does
// not jump off its spot. plant = false moves the feet with the body, the legs as they were at the press (the drag's
// own legs only: pins and IK limbs stay held). Limits that stop a joint go into report. Returns the tracks keyed.
struct RigConstraints;
struct ClampReport;

std::vector<std::string> key_body_drag(Clip& clip, const Rig& rig, double frame, const BodyDrag& drag,
                                       const Vec3& target, bool plant, const Shape* shape,
                                       const RigConstraints* constraints = nullptr, ClampReport* report = nullptr);

// --- FP-4: follow-through while a drag runs ------------------------------------------------------------------------

// The chains that swing while the body is dragged: the clip's own unbaked Dynamics chains as they are, then a default
// chain for every loose part the body has that they do not cover: the tail, the wings, the ears (their presets) and
// each soft collision volume (jiggle). weighted(node) says whether the body shown has the node weighted (a mesh
// body's skin weights; the bare skeleton counts every shown joint). Chains that take a bone of `moving` (the dragged
// chain) are left out. Gravity is 0 on every chain, so at rest the follow-through is exactly the keyed pose.
std::vector<DynChain> follow_through_chains(const Skeleton& skel, const Clip& clip, const std::vector<int>& moving,
                                            const std::function<bool(int)>& weighted);

// The live simulation of those chains (a DynSim driven by real time), settling once the drag stops.
class FollowThrough {
public:
    FollowThrough(const Skeleton& skel, const std::vector<DynChain>& chains, const std::vector<Xform>& animated);
    // Advances by dt seconds of real time (sub-stepped at DynSim::kStepsPerSecond) towards the animated globals of the
    // pose as keyed now, then writes the swing into pose. Returns false once everything has come to rest.
    bool step(const std::vector<Xform>& animated, double dt, Pose& pose);
    bool settled() const { return settled_; }
    const std::vector<DynChain>& chains() const { return chains_; }

private:
    std::vector<DynChain> chains_;
    DynSim sim_;
    bool settled_ = false;
    double still_ = 0;  // seconds every point has been within kRestSpeed of standing still
};

}  // namespace vats
