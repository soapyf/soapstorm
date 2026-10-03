// Viewport Avatar Toolset - IK rigs, pins, follow bake and full pose evaluation.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/02 sections 2.6-2.8, 3.7-3.9 and AM-120.
// IK controllers live in "ik.<Limb>" tracks (blend, pos_*, rot_*, pole_*), pin offsets in "pin:<joint>"
// tracks. Arm, leg, hind-leg, wing and spine controllers are in avatar space; finger controllers are
// in the space of the finger root's parent (the wrist). When a controller has no pos/rot keys its
// target is the evaluated end bone, and with no pole keys its pole is derived from the pose (AM-57;
// VATs clips use switch_to_ik's pole).
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "vats/clip.h"
#include "vats/skeleton.h"

namespace vats {

struct LimbInfo {
    std::string name;   // "ArmLeft", ... (track = "ik." + name)
    std::string label;  // "Left Arm"
    int root = -1, mid = -1, end = -1;  // spine: mTorso, mChest, mNeck (mNeck only places the target)
    Vec3 hinge, fallback_pole;          // hinge in the mid bone's frame, pole in the root's parent frame
    bool finger = false, spine = false;
};

class Rig {
public:
    explicit Rig(const Skeleton& skel);  // limbs whose bones are missing from skel are left out

    const Skeleton& skeleton() const { return skel_; }
    const std::vector<LimbInfo>& limbs() const { return limbs_; }
    // The limb that rotates node (spine: mTorso and mChest), or -1.
    int limb_of_bone(int node) const;
    int find_limb(std::string_view name) const;

    // Resolves cross-actor pin targets (GR-4) for every evaluation through this rig; empty = those pins
    // are skipped.
    ExternalTarget external;

private:
    const Skeleton& skel_;
    std::vector<LimbInfo> limbs_;
    std::vector<int> limb_of_;
};

// A limb uses IK (for export) when any of its blend keys is > 0 (AM-53).
bool uses_ik(const Clip& clip, const LimbInfo& limb);

struct LimbState {
    bool uses_ik = false;   // any blend key > 0 (AM-53)
    bool ik_on = false;     // blend >= 0.5 at this frame
    double blend = 0;
    Xform target;           // world (avatar space) target transform
    Vec3 pole;              // world pole position (unused for the spine)
};

struct Evaluation {
    Pose pose;                     // final local pose after IK and pins
    std::vector<Xform> globals;    // global transforms of every node
    std::vector<LimbState> limbs;  // one per Rig::limbs()
};

struct RigConstraints;
struct ClampReport;

// curves -> FK -> IK (Spine, then arms/legs/hind legs/wings, then fingers) -> pins (AM-58, AM-81).
Evaluation evaluate(const Rig& rig, const Clip& clip, double frame, const Shape* shape,
                    const RigConstraints* constraints = nullptr);

// The pole AM-57 derives from a pose, in world space.
Vec3 derive_pole(const Rig& rig, int limb, const std::vector<Xform>& globals);

// Keys blend as a stepped key; on an empty blend curve at frame > 0 first keys 1 - value at 0 (AM-54).
void key_blend(Clip& clip, const Rig& rig, double frame, int limb, double value);
// AM-55, no jump (exact when the mid bone is bent about its hinge only; a fully straight arm or finger moves
// by AM-59's reach clamp). The pole is AM-57's without its straight-chain fallback, and under
// IkSolve::Literal turned about the root-end line to suit section 3.7's frame. A VATs controller with no
// pole keys uses the same pole.
void switch_to_ik(Clip& clip, const Rig& rig, double frame, int limb, const Shape* shape);
void switch_to_fk(Clip& clip, const Rig& rig, double frame, int limb, const Shape* shape);  // AM-56, no jump
// Keys the 9 controller channels (6 for the spine); the pole keeps its evaluated value (AM-61). The limb's joints
// its IK would turn past their limits (evaluate holds them there) go into report.
void key_limb_target(Clip& clip, const Rig& rig, double frame, int limb, const Xform& world_target,
                     const Shape* shape, const RigConstraints* constraints = nullptr, ClampReport* report = nullptr);
// Keys the 9 controller channels; the target keeps its evaluated value.
void key_limb_pole(Clip& clip, const Rig& rig, double frame, int limb, const Vec3& world_pole, const Shape* shape);

// Pins (AM-82..86). target_bone = -1 for a world pin. Return false and set why when refused.
bool pin_here(Clip& clip, const Rig& rig, double frame, int node, int target_bone, const Shape* shape,
              std::string& why);
// GR-4: pins node, from frame on, to `bone` of another actor, resolved through rig.external.
bool pin_to_actor(Clip& clip, const Rig& rig, double frame, int node, const std::string& actor, const std::string& bone,
                  const Shape* shape, std::string& why);
bool unpin_here(Clip& clip, const Rig& rig, double frame, int node, const Shape* shape, std::string& why);
void delete_pin(Clip& clip, const Rig& rig, size_t pin);
// Band drags, clamped against the neighbouring pins on the same joint (E-18).
void move_pin_start(Clip& clip, const Rig& rig, size_t pin, int frame, const Shape* shape);  // AM-84
void move_pin_end(Clip& clip, const Rig& rig, size_t pin, int frame, const Shape* shape);    // end_frame+1 = open
// The limb a pin drives through IK: the pin holds that limb's end (wrist, ankle, fingertip, hind foot, wing
// tip), so the whole chain bends to reach it. -1 when the pin moves its via bone itself (attachment points,
// the spine, other bones).
int pin_limb(const Rig& rig, const Pin& p);
// Index of the pin holding node at frame, or -1.
int pin_at(const Clip& clip, const Rig& rig, int node, double frame);
// A gizmo edit of a pinned point keys its "pin:" offset track instead (AM-86). False when node is not
// pinned at frame (nothing keyed).
bool key_pinned_point(Clip& clip, const Rig& rig, double frame, int node, const Xform& world, const Shape* shape);

// Follow Target bake over integer frames f0..f1 (AM-70/71).
bool follow_bake(Clip& clip, const Rig& rig, int target, int follower, int f0, int f1, bool keep_offset,
                 const Shape* shape, std::string& why);

// Auto IK (spec 08 AI): a joint dragged in the view pulls the bones above it, solved by IK on every frame of the drag
// and keyed as plain rotations, with no IK controller, as a Move drag keys a bone.
struct AutoIkChain {
    std::vector<int> bones;  // from the chain's root down to the dragged joint's parent; Bento's mSpine1..4 among them
                             // are passed through, never turned
    int end = -1;            // the dragged joint: its head follows the pointer
    int turning = 0;         // how many of bones turn
    int longest = 0;         // the most bones that turn a chain from this joint can have now
    std::string why;         // why there is no chain, when bones is empty
    // FP-2: a point grabbed on the body, in end's own frame. With grab_on, end is the last of bones and turns too,
    // and this point, not end's head, follows the pointer.
    bool grab_on = false;
    Vec3 grab;
};
// AI-2: how many bones a drag of node turns by default: an arm to its collar, a leg to its hip, a hind leg, wing, tail
// or finger to its first bone; any other joint 2 (fewer when it has fewer above it). 0: none (the pelvis, attachment
// points, collision volumes, face bones). Bento's mSpine1..4 do not count: their offsets cancel in pairs.
int auto_ik_default_length(const Skeleton& skel, int node);
// AI-3: the chain for dragging node at frame with length bones (<= 0: the default), cut short where it would take the
// pelvis or a bone an IK limb or a pin drives at the frame, which stay as they are. node itself must turn freely: none
// (why says so) for the pelvis, attachment points, face bones, a joint with position keys (Move moves those), a
// pinned joint and one an IK limb drives.
AutoIkChain auto_ik_chain(const Rig& rig, const Clip& clip, double frame, int node, int length = 0);
// AI-4: the hinge each joint bends about, in its parent's frame, zero for a ball joint: elbows, knees, the hind legs'
// second and third joints, the wings' second joint and the fingers' second and third joints, about their limb's
// hinge. Where shape has rig axes for the joint, the rig axis nearest that hinge (never the one along the bone); else
// where shape bends the joint at rest by 20 degrees or more, the normal of that bend (a creature rigged bent).
std::vector<Vec3> auto_ik_hinges(const Rig& rig, const Shape* shape);
// AI-4: turns chain's bones in pose so the head of chain.end reaches target (avatar space), or as near as they reach,
// keeping the chain's root joint where it is. Damped least squares from pose: the smallest turn that gets there,
// hinges bend only about their hinge and never through straight to the other side. A chain longer than the joint's
// default (auto_ik_default_length) reaches with those bones first, and the ones above only take what is left.
// IK local rotations (rest included) of root, mid and end (02 section 3.7).
void solve_two_bone(const Skeleton& skel, const LimbInfo& l, IkSolve mode, const Pose& pose,
                    const std::vector<Xform>& g, const Shape* shape, const Xform& target, const Vec3& pole,
                    Quat out[3], const RigConstraints* constraints = nullptr);

// Limb solve for VATs rigs, using Auto IK and pole swivel.
void solve_limb_vats(const Rig& rig, int limb_index, const Shape* shape, const Xform& target, const Vec3& pole,
                     const Pose& input_pose, const std::vector<Xform>& g, Quat out[3],
                     const RigConstraints* constraints = nullptr, ClampReport* report = nullptr);

void solve_auto_ik(const Skeleton& skel, const Shape* shape, const AutoIkChain& chain, const std::vector<Vec3>& hinges,
                   const Vec3& target, Pose& pose, const RigConstraints* constraints = nullptr,
                   ClampReport* report = nullptr);
// Keys chain's bones at frame with the solve for target from start's pose, and gives the solved pose in solved. A drag
// solves each step from the last one's pose (solved), so the chain follows the pointer's path; from the evaluation at
// the press, a target where the joint was keys the pose it had.
std::vector<std::string> key_auto_ik(Clip& clip, const Rig& rig, double frame, const AutoIkChain& chain,
                                     const Evaluation& start, const Vec3& target, const Shape* shape,
                                     Pose* solved = nullptr, const RigConstraints* constraints = nullptr,
                                     ClampReport* report = nullptr);

}  // namespace vats
