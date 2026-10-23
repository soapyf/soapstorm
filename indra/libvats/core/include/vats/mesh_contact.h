// Viewport Avatar Toolset - hands against the body's own mesh: where a hand or forearm sinks into the body shown (a
// mesh body's skinned triangles, not the capsules of self_contact.h), and pushing it back out through the arm.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#pragma once

#include <vector>

#include "vats/dae.h"
#include "vats/rig.h"

namespace vats {

// How deep each hand (and its forearm) is inside the rest of the body on one frame: the model is skinned for the
// pose, every forearm, hand and finger vertex is compared with the nearest vertex of the body around it (within
// 12 cm), and it is inside when it lies behind that vertex's surface (against its normal) and nearer that vertex's
// bone than the surface is (so a thin, two-sided card such as a fur tuft, with no inside, never holds a hand). The
// arm's own vertices
// (collar to fingertips) never count as body for it, so an elbow bending is not a hit. push is the unit direction
// out (the body's normal there, averaged over the deepest hits), depth in metres; 0 when the hand is clear.
struct HandDepth {
    double depth = 0;
    Vec3 push;
};
struct HandContacts {
    HandDepth side[2];  // left, right
};
class MeshContactCheck {
public:
    // parts: the body as shown (shown_model of each part; the rigged ones count), shape: the shape it is posed on.
    MeshContactCheck(const Skeleton& skel, const std::vector<const DaeModel*>& parts, const Shape* shape);
    HandContacts measure(const std::vector<Xform>& globals) const;
    bool usable() const { return usable_; }

private:
    const Skeleton& skel_;
    std::vector<const DaeModel*> parts_;
    const Shape* shape_;
    std::vector<signed char> arm_;  // per vertex: 0 left forearm/hand, 1 right, -1 neither
    std::vector<signed char> own_;  // per vertex: the arm chain it belongs to (collar to fingers), -1 none
    std::vector<int> node_;         // per vertex: the joint carrying most of it (its bone is the vertex's axis)
    bool usable_ = false;
};

// Push Hands Out of the Body: every frame from..to is measured; where a hand is in deeper than tol, it is moved out
// along push by the depth plus a margin, through the arm's IK (its target keyed when the arm is in IK, else the
// shoulder, elbow and wrist keyed with the rotations IK finds). The pushes are smoothed over time (eased in and out
// over ease_s seconds around each contact) so the arm glides clear rather than jumping, and measured again (a few
// passes). Returns how many frames each hand was moved on and the deepest contact found first.
struct PushOutReport {
    int frames[2] = {0, 0};
    double deepest = 0;
    int deepest_frame = -1;
    int deepest_side = -1;
};
// constraints: the joint limits the view poses with (evaluate's), so the arm is measured and keyed as it shows.
PushOutReport push_hands_out(Clip& clip, const Rig& rig, const std::vector<const DaeModel*>& parts, const Shape* shape,
                             int from, int to, const RigConstraints* constraints = nullptr, double tol = 0.005,
                             double ease_s = 0.15);

}  // namespace vats
