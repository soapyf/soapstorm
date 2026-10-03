// Viewport Avatar Toolset - the self-penetration check: the ragdoll's capsules against each other.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 27 (SX). A hint, not a guarantee: the capsules (ragdoll.h, RD-2) are not the mesh. The
// Animation Check runs it per frame as its "self_contact" rule; Push Out is that rule's fix.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "vats/ragdoll.h"
#include "vats/rig.h"

namespace vats {

struct Hit {
    double depth = -1e9;
    Vec3 push;  // moves the capsule out of the other
};

Hit capsule_hit(const RagdollCapsule& A, const RagdollCapsule& B);
Hit volume_hit(const RagdollCapsule& A, const Xform& g, const Vec3& s);
std::vector<RagdollCapsule> contact_hull(const Skeleton& skel, const std::vector<Xform>& globals);

struct SelfContact {
    int a = -1, b = -1;  // ragdoll joints (capsules), a < b
    double depth = 0;    // metres the two overlap
    Vec3 push;           // unit direction, world: moving a along it takes it out of b
};

// Which capsules of a pose overlap by more than tol metres. Parent and child are never compared, nor any pair that
// already touches in the rest pose on shape (they meet at a joint, or lie together: the two thighs, the chest and the
// head). volumes: a capsule is also tested against the collision volumes (sized by shape) of the other bodies, and a
// hit counts for the volume's joint; used where a mesh body is imported. The pairs are chosen once, here.
class SelfContactCheck {
public:
    SelfContactCheck(const Skeleton& skel, const Shape* shape, bool volumes, double tol = 0.01);
    // globals posed with the shape given above. Deepest hit per pair, sorted by (a, b).
    std::vector<SelfContact> find(const std::vector<Xform>& globals) const;

private:
    const Skeleton& skel_;
    const Shape* shape_;
    double tol_;
    std::vector<std::pair<int, int>> pairs_;    // capsule indices (ragdoll_capsules order)
    std::vector<std::pair<int, int>> volumes_;  // capsule index, index into skel.volumes()
    std::vector<int> volume_body_;              // per volume: the capsule joint it rides with
};

// Two bodies against each other (a hug, a handshake): every capsule of the check's hull on one pose against every one
// on the other, `other` already moved into the first one's space. The deepest hit of each pair deeper than tol; a is a
// joint of the first body, b of the other, push moves a out of b.
std::vector<SelfContact> cross_contacts(const Skeleton& skel, const std::vector<Xform>& globals,
                                        const std::vector<Xform>& other, double tol = 0.01);

// Push Out: on each of frames where a and b overlap, the arm among them (shoulder, elbow or wrist capsule) is moved
// out through its IK: the arm's target is keyed when it is in IK, else the shoulder, elbow and wrist are keyed with
// the rotations IK gives for the moved target. Repeated while they still overlap (a few passes). Pairs with no arm are
// left alone. evaluate runs with shape; the contacts are measured on mesh_body when given (and its volumes). One undo
// step for the caller.
void push_out(Clip& clip, const Rig& rig, int a, int b, const std::vector<int>& frames, const Shape* shape,
              const Shape* mesh_body);

// A shoulder, elbow or wrist: the joints Push Out moves.
bool push_out_moves(const std::string& joint);

}  // namespace vats
