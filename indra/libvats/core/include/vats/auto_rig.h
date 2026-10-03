// Viewport Avatar Toolset - rigging a model from scratch: SL's skeleton placed in a mesh that has none, by markers.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 6 (RG-14 markers, RG-13 weights). The model is stood up (turned to face +X, sized,
// centred over the origin with its lowest point on the ground); markers mark its joints (each named by the SL joint
// it places), guessed from its shape first; the rest of SL's skeleton is fitted from them; bone heat (bone_heat.h)
// weights it. The result is an ordinary rigged DaeModel (binds at the joints, SL's rest rotations), kept in the
// model's mapping file (rig_map.h) so it loads, poses and exports like any mesh body.
#pragma once

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "vats/bone_heat.h"
#include "vats/dae.h"
#include "vats/skeleton.h"

namespace vats {

// A marker the user drags: where one of the model's joints is.
struct RigMarkerDef {
    const char* id;      // "wrist_l"
    const char* name;    // "left wrist"
    const char* joint;   // the SL joint it places, shown beside it ("mWristLeft")
    const char* mirror;  // its partner across the body, "" for one on the middle
    const char* group;   // "" for the body's own; else the optional group it belongs to (rig_group_defs)
};
const std::vector<RigMarkerDef>& rig_marker_defs();
const RigMarkerDef* find_rig_marker(std::string_view id);

// The optional parts of the rig: fingers (on unless the hands are mittens), a groin, a tail, wings, hind limbs, ears and
// the Bento face.
struct RigGroupDef {
    const char* id;    // "tail"
    const char* name;  // "Tail"
    const char* what;  // what it adds, for a tooltip
};
const std::vector<RigGroupDef>& rig_group_defs();

struct RigMarker {
    Vec3 pos;
    int confidence = 100;  // 0..100: how sure the guess is; 100 once you place it yourself
    std::string why;       // how the guess found it
    bool operator==(const RigMarker&) const = default;
};

// A model rigged from scratch, as its mapping file keeps it.
struct ScratchRig {
    std::map<std::string, RigMarker> markers;  // by marker id, in the stood-up model's space
    std::set<std::string> groups{"fingers", "groin"};  // the optional groups in use
    bool fitted = false;                         // fitted mesh: weights shared with SL's collision volumes too
    bool painted = false;                        // the weights were touched up by hand (working them out again loses it)
    std::map<std::string, std::string> transfer;  // part -> the part whose weights it copies (a garment over its body)
    std::map<std::string, Vec3> joints;           // SL joint -> where it sits (fit_scratch_joints), stood-up space
    // SL joint -> where you dragged it, for a joint no marker places (a knuckle, a collar, a spine joint): the fit puts it
    // there and carries the joints below it along.
    std::map<std::string, Vec3> pins;
    // The weights, four per vertex of the file as loaded (SK-40 indices, as DaeModel holds them); empty until computed.
    std::vector<int> wjoints;
    std::vector<float> weights;
    bool active() const { return !markers.empty(); }
    bool weighted(int vertices) const { return vertices > 0 && wjoints.size() == size_t(vertices) * 4 && weights.size() == wjoints.size(); }
};

// How a model as loaded is stood up: p' = scale * turn(p) + offset, turn a quarter turns about Z.
struct ScratchPlacement {
    int turn = 0;
    double scale = 1;
    Vec3 offset;
    Vec3 apply(const Vec3& p) const;
    Vec3 undo(const Vec3& p) const;
};
// height: the size it is made (floor to top), 0 for the file's own.
ScratchPlacement scratch_placement(const DaeModel& as_is, int turn, double height);
// Turns, sizes and moves the model's vertices, normals and shape keys so.
void place_model(DaeModel& model, const ScratchPlacement& p);

// Moves a marker where you put it (confidence 100). mirror: its partner goes to the mirror image across the body's middle
// (y to -y), and a marker on the middle stays on it (y = 0).
void place_marker(ScratchRig& rig, const std::string& id, const Vec3& pos, bool mirror);

// Pins a fitted joint where you put it (place_marker's twin for the joints the markers don't place). mirror: its
// Left/Right partner goes to the mirror image.
void place_pin(ScratchRig& rig, const std::string& joint, const Vec3& pos, bool mirror);

// Where a ray through the model passes through its middle: half way between where it first enters the surface and where
// it next leaves (a marker dropped on a limb lands inside it). False when the ray misses.
bool mesh_middle_on_ray(const DaeModel& placed, const Vec3& origin, const Vec3& dir, Vec3& out);

// Which way an unrigged model faces: the quarter turn about Z that faces it along +X, from where its feet point (toes
// ahead of the ankles), else its widest side (arms out). why says which. A model with nothing to go by stays as it is.
int guess_scratch_turn(const DaeModel& as_is, std::string& why);

// Markers guessed from the stood-up model's shape (cross-sections: the legs part at the crotch, the narrowest points of
// the limbs are wrists, ankles and neck, the ends of the limbs and tails their tips). Every body marker gets a place;
// the groups' only when groups names them. Each says how sure it is and why.
std::map<std::string, RigMarker> guess_markers(const DaeModel& placed, const std::set<std::string>& groups);

// The optional groups a stood-up model's shape asks for, on top of fingers and groin: a tail when something sticks out
// behind the hips, wings when something spreads out to both sides behind the shoulders, hind limbs when a second pair
// of feet stands behind the first, ears when the top of the head parts into two points. why: a line per group found.
std::set<std::string> guess_rig_groups(const DaeModel& placed, std::vector<std::string>* why = nullptr);

// SL's skeleton fitted to the markers: the spine spaced as SL spaces it between the pelvis (above the hips) and the
// neck, the collars between the neck and the shoulders, fingers along each hand's long axis, the face from the face
// markers, and the tail, wings, hind limbs and ears along the centre line of the mesh from their tips. tips: where each
// leaf joint's bone ends (a fingertip, the top of the head), for bone heat.
struct ScratchFit {
    std::map<std::string, Vec3> joints;
    std::map<std::string, Vec3> tips;
    std::vector<std::string> notes;
};
ScratchFit fit_scratch_joints(const Skeleton& skel, const DaeModel& placed, const ScratchRig& rig);

// The joints the weights may go to: every placed joint of the groups in use (no face bones without the face group),
// never the helpers SL has no use for in a body (mSkull, mFaceRoot, the spine's extra joints).
bool scratch_weighted_joint(const Skeleton& skel, const std::string& joint, const std::set<std::string>& groups);
// The bones for bone heat: each weighted joint's segments to its placed child joints, else to its tip.
std::vector<HeatBone> scratch_heat_bones(const Skeleton& skel, const ScratchFit& fit, const std::set<std::string>& groups);

// Makes the stood-up model rigged: SL's rest rotations at the rig's joints (bound), its collision volumes where SL puts
// them against their joints, and the rig's weights; nearest-bone weights while it has none (a preview).
void rig_from_scratch(const Skeleton& skel, DaeModel& placed, const ScratchRig& rig);

// Bone heat on every part (parts that share a seam, as a head and a neck do, are solved as one), or the weights of the
// part rig.transfer names (nearest surface point), then the collision volumes' share when rig.fitted. The model is
// the stood-up one, rigged (rig_from_scratch); the weights go into it and into rig. report gets a line per part.
struct ScratchWeighReport {
    std::vector<std::string> lines;     // per part: how it was weighted, and anything it could not do
    std::vector<BoneHeatStats> stats;   // per solved piece
    double seconds = 0;
};
bool weigh_scratch_rig(const Skeleton& skel, DaeModel& placed, ScratchRig& rig, const BoneHeatOptions& opt,
                       ScratchWeighReport& report);

// The parts best weighted from another part (rig.transfer's suggestion): a part lying nine tenths or more within 2% of
// the model's height of a bigger part's surface (a vest, sleeves, a balaclava over a body), from that part.
std::map<std::string, std::string> suggest_transfers(const DaeModel& placed);

// Fitted mesh: each vertex's weight on a joint shared with that joint's collision volumes by how deep inside each it
// sits (all of it inside the volume, none past twice its size), four at most, summing to 1.
void share_with_volumes(const Skeleton& skel, DaeModel& model);

}  // namespace vats
